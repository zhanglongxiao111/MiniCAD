#include "DiffEngine.h"
#include "CpuRasterizer.h"
#include "JsonWriter.h"
#include "PathGuard.h"
#include "PngWriter.h"
#include "SceneTessellator.h"
#include "SqliteExporter.h"
#include "SqliteGuard.h"
#include "TchSemanticExport.h"
#include "Core/PathUtils.h"
#include "Core/Entity/InsertEntity.hpp"
#include "Core/Entity/LineEntity.hpp"
#include "Import/CadExchange.h"
#include "Import/CadSemanticSnapshot.h"
#include "Scene/Scene.h"
#include "sqlite3.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace MiniCAD::SaTool
{
    namespace
    {
        constexpr int64_t     kHardMaxTotalPx        = 40'000'000LL;
        constexpr const char* kDefaultAlignLayerRe   = "DOTE|AXIS|轴线|轴网|GRID";
        constexpr double      kSubBoxMinEdgeMm       = 250.0;
        constexpr size_t      kMaxSubBoxesPerRegion  = 8;

        // ── 文本与几何辅助 ────────────────────────────────────────────────

        std::string NormalizeText(const std::string& raw)
        {
            std::string out;
            out.reserve(raw.size());
            bool inWs = true;
            for (unsigned char c : raw)
            {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                {
                    if (!inWs)
                    {
                        out.push_back(' ');
                        inWs = true;
                    }
                }
                else
                {
                    out.push_back(static_cast<char>(c));
                    inWs = false;
                }
            }
            if (!out.empty() && out.back() == ' ')
                out.pop_back();
            return out;
        }

        bool PointInBox(double x, double y, const std::array<double, 4>& box, double pad = 0.0)
        {
            return x >= box[0] - pad && x <= box[2] + pad
                && y >= box[1] - pad && y <= box[3] + pad;
        }

        bool BoxIntersectsBox(const std::array<double, 4>& a, const std::array<double, 4>& b, double pad = 0.0)
        {
            return !(a[2] < b[0] - pad || a[0] > b[2] + pad || a[3] < b[1] - pad || a[1] > b[3] + pad);
        }

        int FindMatchingRegionId(const std::vector<DiffRegionItem>& regions,
                                 const std::array<double, 4>& box,
                                 double pad = 100.0)
        {
            for (const auto& r : regions)
            {
                if (BoxIntersectsBox(r.BboxDrawing, box, pad))
                    return r.Id;
            }
            return 0;
        }

        int CountDiagnosticsKind(const CadSemanticSnapshot& snap, const char* kind)
        {
            int total = 0;
            for (const auto& d : snap.Diagnostics)
            {
                if (d.Kind == kind)
                    total += std::max(1, d.Count);
            }
            return total;
        }

        bool WriteUtf8TextFile(const std::string& utf8Path, const std::string& content, std::string* error)
        {
            const std::filesystem::path p = MiniCAD::Utf8ToPath(utf8Path);
            std::error_code ec;
            if (p.has_parent_path())
                std::filesystem::create_directories(p.parent_path(), ec);

            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            if (!f.is_open())
            {
                if (error) *error = "无法写入文件：" + utf8Path;
                return false;
            }
            f.write(content.data(), static_cast<std::streamsize>(content.size()));
            f.close();
            if (!f)
            {
                if (error) *error = "写入文件失败：" + utf8Path;
                return false;
            }
            return true;
        }

        // ── 自动对齐：1D 共识投票（与 align_dxf.py 同算法）────────────────

        std::vector<double> DeduplicateSortedCoords(std::vector<double> vals, double mergeEps)
        {
            if (vals.empty())
                return vals;
            std::sort(vals.begin(), vals.end());
            std::vector<double> out;
            out.reserve(vals.size());
            double sum = vals[0];
            int count  = 1;
            double prev = vals[0];
            for (size_t i = 1; i < vals.size(); ++i)
            {
                if (vals[i] - prev <= mergeEps)
                {
                    sum += vals[i];
                    ++count;
                    prev = vals[i];
                }
                else
                {
                    out.push_back(sum / static_cast<double>(count));
                    sum   = vals[i];
                    count = 1;
                    prev  = vals[i];
                }
            }
            out.push_back(sum / static_cast<double>(count));
            return out;
        }

        struct Vote1DResult
        {
            bool                        HasSolution = false;
            double                      Delta       = 0.0;
            int                         Support     = 0;
            std::vector<AlignCandidate> TopCandidates;
        };

        Vote1DResult Vote1D(const std::vector<double>& valsA,
                            const std::vector<double>& valsB,
                            double tolMm)
        {
            Vote1DResult res;
            if (valsA.empty() || valsB.empty() || !(tolMm > 0.0))
                return res;

            std::unordered_map<int64_t, int> bins;
            bins.reserve(std::min<size_t>(valsA.size() * valsB.size(), 65536u));

            for (double va : valsA)
            {
                for (double vb : valsB)
                {
                    const double d = va - vb;
                    const int64_t key = static_cast<int64_t>(std::llround(d / tolMm));
                    ++bins[key];
                }
            }
            if (bins.empty())
                return res;

            std::vector<std::pair<int64_t, int>> ranked(bins.begin(), bins.end());
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return std::abs(a.first) < std::abs(b.first);
            });

            const size_t topN = std::min<size_t>(5, ranked.size());
            res.TopCandidates.reserve(topN);
            for (size_t i = 0; i < topN; ++i)
            {
                res.TopCandidates.push_back({
                    static_cast<double>(ranked[i].first) * tolMm,
                    ranked[i].second
                });
            }

            // 对最高票箱 ±tolMm 内的两两差值取中位数，得精确平移量
            const double center = static_cast<double>(ranked[0].first) * tolMm;
            std::vector<double> inliers;
            for (double va : valsA)
            {
                for (double vb : valsB)
                {
                    const double d = va - vb;
                    if (std::abs(d - center) <= tolMm)
                        inliers.push_back(d);
                }
            }
            if (inliers.empty())
                return res;

            std::sort(inliers.begin(), inliers.end());
            const size_t mid = inliers.size() / 2;
            res.Delta = (inliers.size() % 2 == 1)
                          ? inliers[mid]
                          : 0.5 * (inliers[mid - 1] + inliers[mid]);
            res.Support     = static_cast<int>(inliers.size());
            res.HasSolution = true;
            if (!res.TopCandidates.empty())
            {
                res.TopCandidates[0].Delta = res.Delta;
                res.TopCandidates[0].Votes = res.Support;
            }
            return res;
        }

        void CollectTchAxisCoords(const CadSemanticSnapshot& snap,
                                  const std::string& space,
                                  const std::string& layoutName,
                                  const std::array<double, 4>* cropFilter,
                                  std::vector<double>& xs,
                                  std::vector<double>& ys)
        {
            for (const auto& ax : snap.TchAxes)
            {
                if (!ax.BlockName.empty())
                    continue;
                if (ax.Space != space)
                    continue;
                if (space == "paper" && !layoutName.empty() && ax.LayoutName != layoutName)
                    continue;
                if (cropFilter)
                {
                    const std::array<double, 4> abox{ ax.MinX, ax.MinY, ax.MaxX, ax.MaxY };
                    if (!BoxIntersectsBox(abox, *cropFilter, 1000.0))
                        continue;
                }
                if (!ax.Coord.has_value())
                    continue;
                if (ax.Orientation == "vertical")
                    xs.push_back(*ax.Coord);
                else if (ax.Orientation == "horizontal")
                    ys.push_back(*ax.Coord);
            }
        }

        struct AxisLineLayerStats
        {
            int    EntitiesOnLayer = 0;
            int    LinesSeen       = 0;
            int    LinesQualified  = 0;
            double MaxLineSpan     = 0.0;
        };

        void CollectOrthoLinesRecursive(const Entity& e,
                                        const Math::Mat4& xform,
                                        const LayerManager& lm,
                                        const std::string& layerPattern,
                                        double minSpanMm,
                                        const std::array<double, 4>* cropFilter,
                                        std::vector<double>& xs,
                                        std::vector<double>& ys,
                                        std::map<std::string, AxisLineLayerStats>& layerStats,
                                        int depth)
        {
            if (depth > 2 || !e.GetAttr().Visible)
                return;

            if (const auto* ins = dynamic_cast<const InsertEntity*>(&e))
            {
                if (!ins->GetBlock() || ins->GetBlock()->IsUnresolvedXRef())
                    return;
                const auto& children = ins->GetResolvedChildren();
                const int rows = std::max(1, ins->GetRowCount());
                const int cols = std::max(1, ins->GetColumnCount());
                if (rows * cols > 64)
                    return;
                for (int r = 0; r < rows; ++r)
                {
                    for (int c = 0; c < cols; ++c)
                    {
                        const Math::Mat4 nextXf = ins->CellMatrix(c, r) * xform;
                        for (const auto& ch : children)
                        {
                            if (ch)
                                CollectOrthoLinesRecursive(*ch, nextXf, lm, layerPattern, minSpanMm,
                                                           cropFilter, xs, ys, layerStats, depth + 1);
                        }
                    }
                }
                return;
            }

            const Layer* layer = lm.GetLayer(e.GetAttr().LayerId);
            if (layer && !layer->IsVisible())
                return;
            const std::string layerName = layer ? layer->GetName() : std::string("0");
            if (!RegexSearchUtf8Icase(layerName, layerPattern))
                return;

            auto& st = layerStats[layerName];
            ++st.EntitiesOnLayer;

            const auto* line = dynamic_cast<const LineEntity*>(&e);
            if (!line)
                return;
            ++st.LinesSeen;

            const Math::Point3 p0 = xform * line->GetLine().Start;
            const Math::Point3 p1 = xform * line->GetLine().End;
            if (cropFilter)
            {
                const std::array<double, 4> lbox{
                    std::min(p0.x, p1.x), std::min(p0.y, p1.y),
                    std::max(p0.x, p1.x), std::max(p0.y, p1.y)
                };
                if (!BoxIntersectsBox(lbox, *cropFilter, 1000.0))
                    return;
            }

            const double dx = std::abs(p1.x - p0.x);
            const double dy = std::abs(p1.y - p0.y);
            const double span = std::hypot(dx, dy);
            if (span > st.MaxLineSpan)
                st.MaxLineSpan = span;

            if (dx < 1.0 && dy >= minSpanMm)
            {
                xs.push_back(0.5 * (p0.x + p1.x));
                ++st.LinesQualified;
            }
            else if (dy < 1.0 && dx >= minSpanMm)
            {
                ys.push_back(0.5 * (p0.y + p1.y));
                ++st.LinesQualified;
            }
        }

        void CollectAxisLinesFromScene(const Scene& scene,
                                       int layoutIdx,
                                       const std::string& layerPattern,
                                       double minSpanMm,
                                       const std::array<double, 4>* cropFilter,
                                       std::vector<double>& xs,
                                       std::vector<double>& ys,
                                       std::map<std::string, AxisLineLayerStats>& layerStats)
        {
            const DrawingLayout& layout = scene.Layouts()[static_cast<size_t>(layoutIdx)];
            const LayerManager& lm = scene.GetLayerManager();
            const Math::Mat4 ident = Math::Mat4::Identity();

            scene.ForEachObject([&](const Object& obj) {
                if (!obj.IsKindOf<Entity>())
                    return;
                const auto& e = static_cast<const Entity&>(obj);
                if (layout.Model)
                {
                    if (e.GetAttr().InPaperSpace)
                        return;
                }
                else
                {
                    if (!e.GetAttr().InPaperSpace)
                        return;
                    if (layout.OwnerKey != 0 && e.GetAttr().LayoutKey != 0 && e.GetAttr().LayoutKey != layout.OwnerKey)
                        return;
                }
                CollectOrthoLinesRecursive(e, ident, lm, layerPattern, minSpanMm,
                                           cropFilter, xs, ys, layerStats, 0);
            });
        }

        AlignmentInfo ComputeAutoAlignment(const Scene& sceneA, const CadSemanticSnapshot& snapA, int layoutIdxA,
                                           const Scene& sceneB, const CadSemanticSnapshot& snapB, int layoutIdxB,
                                           const std::array<double, 4>* cropFilterA,
                                           const DiffOptions& opts,
                                           std::vector<std::string>& warnings)
        {
            AlignmentInfo info;
            info.Mode = "auto";

            const DrawingLayout& layA = sceneA.Layouts()[static_cast<size_t>(layoutIdxA)];
            const DrawingLayout& layB = sceneB.Layouts()[static_cast<size_t>(layoutIdxB)];
            const std::string spaceA  = layA.Model ? "model" : "paper";
            const std::string spaceB  = layB.Model ? "model" : "paper";

            std::vector<double> axA, ayA, axB, ayB;
            CollectTchAxisCoords(snapA, spaceA, layA.Model ? "Model" : layA.Name, cropFilterA, axA, ayA);
            CollectTchAxisCoords(snapB, spaceB, layB.Model ? "Model" : layB.Name, nullptr, axB, ayB);

            axA = DeduplicateSortedCoords(std::move(axA), 1.0);
            ayA = DeduplicateSortedCoords(std::move(ayA), 1.0);
            axB = DeduplicateSortedCoords(std::move(axB), 1.0);
            ayB = DeduplicateSortedCoords(std::move(ayB), 1.0);

            if (axA.size() >= 2 && ayA.size() >= 2 && axB.size() >= 2 && ayB.size() >= 2)
            {
                info.Source = "tch_axes";
            }
            else
            {
                // 次选轨：轴线层正交长 LINE
                axA.clear(); ayA.clear(); axB.clear(); ayB.clear();
                const std::string patA = opts.AlignLayersA.empty() ? kDefaultAlignLayerRe : opts.AlignLayersA;
                const std::string patB = opts.AlignLayersB.empty() ? kDefaultAlignLayerRe : opts.AlignLayersB;
                std::map<std::string, AxisLineLayerStats> statsA, statsB;
                CollectAxisLinesFromScene(sceneA, layoutIdxA, patA, opts.AlignMinSpanMm, cropFilterA, axA, ayA, statsA);
                CollectAxisLinesFromScene(sceneB, layoutIdxB, patB, opts.AlignMinSpanMm, nullptr,     axB, ayB, statsB);
                axA = DeduplicateSortedCoords(std::move(axA), 1.0);
                ayA = DeduplicateSortedCoords(std::move(ayA), 1.0);
                axB = DeduplicateSortedCoords(std::move(axB), 1.0);
                ayB = DeduplicateSortedCoords(std::move(ayB), 1.0);
                info.Source = "layer_lines";
            }

            if (axA.empty() || ayA.empty() || axB.empty() || ayB.empty())
            {
                info.Confidence = "failed";
                info.Dx = 0.0;
                info.Dy = 0.0;
                warnings.push_back("自动对齐未找到足够的正交轴网线，已回落至偏移 (0, 0)；如需精确对齐请显式传 --offset 或指定 --align-layers-a/b");
                return info;
            }

            const Vote1DResult vx = Vote1D(axA, axB, opts.AlignTolMm);
            const Vote1DResult vy = Vote1D(ayA, ayB, opts.AlignTolMm);
            info.CandidatesX = vx.TopCandidates;
            info.CandidatesY = vy.TopCandidates;

            if (!vx.HasSolution || !vy.HasSolution)
            {
                info.Confidence = "failed";
                info.Dx = 0.0;
                info.Dy = 0.0;
                warnings.push_back("自动对齐投票未能收敛，已回落至偏移 (0, 0)");
                return info;
            }

            info.Dx       = vx.Delta;
            info.Dy       = vy.Delta;
            info.SupportX = vx.Support;
            info.SupportY = vy.Support;

            const bool uniqueX = vx.TopCandidates.size() < 2 || vx.TopCandidates[0].Votes > vx.TopCandidates[1].Votes;
            const bool uniqueY = vy.TopCandidates.size() < 2 || vy.TopCandidates[0].Votes > vy.TopCandidates[1].Votes;

            if (vx.Support >= 3 && vy.Support >= 3 && uniqueX && uniqueY)
            {
                info.Confidence = "high";
            }
            else
            {
                info.Confidence = "low";
                warnings.push_back("自动对齐置信度偏低（support_x=" + std::to_string(vx.Support)
                                   + ", support_y=" + std::to_string(vy.Support)
                                   + "），建议人工核对偏移或改用 --offset");
            }
            return info;
        }

        // ── 轨 1：可分离一维滑动窗口膨胀（O(W*H)）──────────────────────────

        void DilateMaskSeparable(const std::vector<uint8_t>& src,
                                 int w, int h, int radiusPx,
                                 std::vector<uint8_t>& dst)
        {
            const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
            dst.assign(total, 0u);
            if (w <= 0 || h <= 0)
                return;
            if (radiusPx <= 0)
            {
                dst = src;
                return;
            }

            // 第 1 趟：水平滑动窗口
            std::vector<uint8_t> tmp(total, 0u);
            for (int y = 0; y < h; ++y)
            {
                const uint8_t* sRow = src.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
                uint8_t*       tRow = tmp.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
                int sum = 0;
                const int initEnd = std::min(w - 1, radiusPx);
                for (int x = 0; x <= initEnd; ++x)
                    sum += sRow[x];

                for (int x = 0; x < w; ++x)
                {
                    tRow[x] = (sum > 0) ? 1u : 0u;
                    const int addX = x + radiusPx + 1;
                    if (addX < w) sum += sRow[addX];
                    const int remX = x - radiusPx;
                    if (remX >= 0) sum -= sRow[remX];
                }
            }

            // 第 2 趟：垂直滑动窗口（逐行累加列和，保持顺序内存访问）
            std::vector<int> colSum(static_cast<size_t>(w), 0);
            const int initRowEnd = std::min(h - 1, radiusPx);
            for (int y = 0; y <= initRowEnd; ++y)
            {
                const uint8_t* tRow = tmp.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
                for (int x = 0; x < w; ++x)
                    colSum[static_cast<size_t>(x)] += tRow[x];
            }

            for (int y = 0; y < h; ++y)
            {
                uint8_t* dRow = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
                for (int x = 0; x < w; ++x)
                    dRow[x] = (colSum[static_cast<size_t>(x)] > 0) ? 1u : 0u;

                const int addY = y + radiusPx + 1;
                if (addY < h)
                {
                    const uint8_t* addRow = tmp.data() + static_cast<size_t>(addY) * static_cast<size_t>(w);
                    for (int x = 0; x < w; ++x)
                        colSum[static_cast<size_t>(x)] += addRow[x];
                }
                const int remY = y - radiusPx;
                if (remY >= 0)
                {
                    const uint8_t* remRow = tmp.data() + static_cast<size_t>(remY) * static_cast<size_t>(w);
                    for (int x = 0; x < w; ++x)
                        colSum[static_cast<size_t>(x)] -= remRow[x];
                }
            }
        }

        // ── 轨 1：粗网格 8 连通 BFS + 紧缩 + 递归中位数切分 + 细子框 ──────

        struct PxBox
        {
            int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
            bool Valid() const { return x1 >= x0 && y1 >= y0; }
        };

        bool TightenPxBox(const std::vector<uint8_t>& mask, int w, int h, PxBox& box)
        {
            int nx0 = w, ny0 = h, nx1 = -1, ny1 = -1;
            const int xStart = std::max(0, box.x0);
            const int xEnd   = std::min(w - 1, box.x1);
            const int yStart = std::max(0, box.y0);
            const int yEnd   = std::min(h - 1, box.y1);

            for (int y = yStart; y <= yEnd; ++y)
            {
                const uint8_t* row = mask.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
                for (int x = xStart; x <= xEnd; ++x)
                {
                    if (row[x])
                    {
                        if (x < nx0) nx0 = x;
                        if (x > nx1) nx1 = x;
                        if (y < ny0) ny0 = y;
                        if (y > ny1) ny1 = y;
                    }
                }
            }
            if (nx1 < nx0 || ny1 < ny0)
            {
                box = {};
                return false;
            }
            box = { nx0, ny0, nx1, ny1 };
            return true;
        }

        std::vector<PxBox> ClusterMaskGrid(const std::vector<uint8_t>& mask,
                                           int w, int h,
                                           const PxBox& roi,
                                           int stepPx)
        {
            std::vector<PxBox> out;
            if (!roi.Valid() || stepPx < 1)
                return out;

            const int roiW = roi.x1 - roi.x0 + 1;
            const int roiH = roi.y1 - roi.y0 + 1;
            const int gw   = (roiW + stepPx - 1) / stepPx;
            const int gh   = (roiH + stepPx - 1) / stepPx;
            std::vector<uint8_t> grid(static_cast<size_t>(gw) * static_cast<size_t>(gh), 0u);

            for (int y = roi.y0; y <= roi.y1; ++y)
            {
                const int gy = (y - roi.y0) / stepPx;
                const uint8_t* row = mask.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
                uint8_t* gRow      = grid.data() + static_cast<size_t>(gy) * static_cast<size_t>(gw);
                for (int x = roi.x0; x <= roi.x1; ++x)
                {
                    if (row[x])
                        gRow[(x - roi.x0) / stepPx] = 1u;
                }
            }

            std::vector<uint8_t> visited(grid.size(), 0u);
            std::queue<std::pair<int, int>> q;

            for (int gy = 0; gy < gh; ++gy)
            {
                for (int gx = 0; gx < gw; ++gx)
                {
                    const size_t idx = static_cast<size_t>(gy) * static_cast<size_t>(gw) + static_cast<size_t>(gx);
                    if (!grid[idx] || visited[idx])
                        continue;

                    visited[idx] = 1u;
                    q.push({ gx, gy });
                    int minGx = gx, maxGx = gx, minGy = gy, maxGy = gy;

                    while (!q.empty())
                    {
                        const auto [cx, cy] = q.front();
                        q.pop();
                        minGx = std::min(minGx, cx);
                        maxGx = std::max(maxGx, cx);
                        minGy = std::min(minGy, cy);
                        maxGy = std::max(maxGy, cy);

                        for (int dy = -1; dy <= 1; ++dy)
                        {
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                if (dx == 0 && dy == 0) continue;
                                const int nx = cx + dx;
                                const int ny = cy + dy;
                                if (nx < 0 || nx >= gw || ny < 0 || ny >= gh)
                                    continue;
                                const size_t nIdx = static_cast<size_t>(ny) * static_cast<size_t>(gw) + static_cast<size_t>(nx);
                                if (grid[nIdx] && !visited[nIdx])
                                {
                                    visited[nIdx] = 1u;
                                    q.push({ nx, ny });
                                }
                            }
                        }
                    }

                    PxBox box;
                    box.x0 = roi.x0 + minGx * stepPx;
                    box.y0 = roi.y0 + minGy * stepPx;
                    box.x1 = std::min(roi.x1, roi.x0 + (maxGx + 1) * stepPx - 1);
                    box.y1 = std::min(roi.y1, roi.y0 + (maxGy + 1) * stepPx - 1);
                    if (TightenPxBox(mask, w, h, box))
                        out.push_back(box);
                }
            }
            return out;
        }

        void SplitOversizedRegionRecursive(const std::vector<uint8_t>& mask,
                                           int w, int h,
                                           const PxBox& box,
                                           double mmPerPx,
                                           double maxRegionMm,
                                           std::vector<PxBox>& out,
                                           int depth = 0)
        {
            if (!box.Valid())
                return;
            const double wMm = static_cast<double>(box.x1 - box.x0 + 1) * mmPerPx;
            const double hMm = static_cast<double>(box.y1 - box.y0 + 1) * mmPerPx;
            if (maxRegionMm <= 0.0 || std::max(wMm, hMm) <= maxRegionMm || depth >= 10)
            {
                out.push_back(box);
                return;
            }

            const bool splitX = (wMm >= hMm);
            if (splitX)
            {
                const int span = box.x1 - box.x0 + 1;
                if (span < 2) { out.push_back(box); return; }
                std::vector<int64_t> hist(static_cast<size_t>(span), 0);
                int64_t total = 0;
                for (int y = box.y0; y <= box.y1; ++y)
                {
                    const uint8_t* row = mask.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
                    for (int x = box.x0; x <= box.x1; ++x)
                    {
                        if (row[x]) { ++hist[static_cast<size_t>(x - box.x0)]; ++total; }
                    }
                }
                int cutOffset = span / 2;
                int64_t acc = 0;
                const int64_t half = total / 2;
                for (int i = 0; i < span - 1; ++i)
                {
                    acc += hist[static_cast<size_t>(i)];
                    if (acc >= half) { cutOffset = i; break; }
                }
                if (cutOffset < 0) cutOffset = 0;
                if (cutOffset >= span - 1) cutOffset = span - 2;

                PxBox left  = { box.x0, box.y0, box.x0 + cutOffset, box.y1 };
                PxBox right = { box.x0 + cutOffset + 1, box.y0, box.x1, box.y1 };
                const bool okL = TightenPxBox(mask, w, h, left);
                const bool okR = TightenPxBox(mask, w, h, right);
                if (!okL || !okR)
                {
                    out.push_back(box);
                    return;
                }
                SplitOversizedRegionRecursive(mask, w, h, left,  mmPerPx, maxRegionMm, out, depth + 1);
                SplitOversizedRegionRecursive(mask, w, h, right, mmPerPx, maxRegionMm, out, depth + 1);
            }
            else
            {
                const int span = box.y1 - box.y0 + 1;
                if (span < 2) { out.push_back(box); return; }
                std::vector<int64_t> hist(static_cast<size_t>(span), 0);
                int64_t total = 0;
                for (int y = box.y0; y <= box.y1; ++y)
                {
                    const uint8_t* row = mask.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
                    int64_t rowCnt = 0;
                    for (int x = box.x0; x <= box.x1; ++x)
                        rowCnt += row[x];
                    hist[static_cast<size_t>(y - box.y0)] = rowCnt;
                    total += rowCnt;
                }
                int cutOffset = span / 2;
                int64_t acc = 0;
                const int64_t half = total / 2;
                for (int i = 0; i < span - 1; ++i)
                {
                    acc += hist[static_cast<size_t>(i)];
                    if (acc >= half) { cutOffset = i; break; }
                }
                if (cutOffset < 0) cutOffset = 0;
                if (cutOffset >= span - 1) cutOffset = span - 2;

                PxBox top = { box.x0, box.y0, box.x1, box.y0 + cutOffset };
                PxBox bot = { box.x0, box.y0 + cutOffset + 1, box.x1, box.y1 };
                const bool okT = TightenPxBox(mask, w, h, top);
                const bool okB = TightenPxBox(mask, w, h, bot);
                if (!okT || !okB)
                {
                    out.push_back(box);
                    return;
                }
                SplitOversizedRegionRecursive(mask, w, h, top, mmPerPx, maxRegionMm, out, depth + 1);
                SplitOversizedRegionRecursive(mask, w, h, bot, mmPerPx, maxRegionMm, out, depth + 1);
            }
        }

        std::array<double, 4> PxBoxToWorld(const PxBox& b, const CanvasLayout& canvas)
        {
            return {
                canvas.Bbox[0] + static_cast<double>(b.x0) * canvas.MmPerPx,
                canvas.Bbox[3] - static_cast<double>(b.y1 + 1) * canvas.MmPerPx,
                canvas.Bbox[0] + static_cast<double>(b.x1 + 1) * canvas.MmPerPx,
                canvas.Bbox[3] - static_cast<double>(b.y0) * canvas.MmPerPx
            };
        }

        std::vector<DiffRegionItem> ExtractDiffRegions(const std::vector<uint8_t>& aOnly,
                                                       const std::vector<uint8_t>& bOnly,
                                                       const CanvasLayout& canvas,
                                                       double minRegionMm,
                                                       double regionGapMm,
                                                       double maxRegionMm)
        {
            std::vector<DiffRegionItem> result;
            if (minRegionMm <= 0.0 || canvas.WidthPx <= 0 || canvas.HeightPx <= 0)
                return result;

            const size_t total = static_cast<size_t>(canvas.WidthPx) * static_cast<size_t>(canvas.HeightPx);
            std::vector<uint8_t> diffMask(total, 0u);
            for (size_t i = 0; i < total; ++i)
                diffMask[i] = (aOnly[i] || bOnly[i]) ? 1u : 0u;

            const int stepPx = std::max(1, static_cast<int>(std::lround(regionGapMm / canvas.MmPerPx)));
            const PxBox fullRoi{ 0, 0, canvas.WidthPx - 1, canvas.HeightPx - 1 };
            const std::vector<PxBox> rawClusters = ClusterMaskGrid(diffMask, canvas.WidthPx, canvas.HeightPx, fullRoi, stepPx);

            std::vector<PxBox> splitBoxes;
            for (const auto& b : rawClusters)
                SplitOversizedRegionRecursive(diffMask, canvas.WidthPx, canvas.HeightPx, b, canvas.MmPerPx, maxRegionMm, splitBoxes);

            const int subStepPx = std::max(1, static_cast<int>(std::lround((regionGapMm * 0.25) / canvas.MmPerPx)));

            for (const auto& b : splitBoxes)
            {
                const double wMm = static_cast<double>(b.x1 - b.x0 + 1) * canvas.MmPerPx;
                const double hMm = static_cast<double>(b.y1 - b.y0 + 1) * canvas.MmPerPx;
                if (std::max(wMm, hMm) < minRegionMm)
                    continue;

                int64_t aCnt = 0, bCnt = 0;
                for (int y = b.y0; y <= b.y1; ++y)
                {
                    const size_t rowOff = static_cast<size_t>(y) * static_cast<size_t>(canvas.WidthPx);
                    for (int x = b.x0; x <= b.x1; ++x)
                    {
                        aCnt += aOnly[rowOff + static_cast<size_t>(x)];
                        bCnt += bOnly[rowOff + static_cast<size_t>(x)];
                    }
                }
                if (aCnt + bCnt == 0)
                    continue;

                DiffRegionItem item;
                item.BboxPx      = { b.x0, b.y0, b.x1, b.y1 };
                item.BboxDrawing = PxBoxToWorld(b, canvas);
                item.AOnlyPixels = aCnt;
                item.BOnlyPixels = bCnt;

                const double tot = static_cast<double>(aCnt + bCnt);
                if (static_cast<double>(aCnt) / tot >= 0.7)
                    item.DominantSide = "a_only";
                else if (static_cast<double>(bCnt) / tot >= 0.7)
                    item.DominantSide = "b_only";
                else
                    item.DominantSide = "both";

                // 细网格子框（gap_mm / 4，长边 >= 250mm，最多 8 个）
                const std::vector<PxBox> subClusters = ClusterMaskGrid(diffMask, canvas.WidthPx, canvas.HeightPx, b, subStepPx);
                struct ScoredSub { PxBox box; int64_t px; };
                std::vector<ScoredSub> subs;
                for (const auto& sb : subClusters)
                {
                    const double swMm = static_cast<double>(sb.x1 - sb.x0 + 1) * canvas.MmPerPx;
                    const double shMm = static_cast<double>(sb.y1 - sb.y0 + 1) * canvas.MmPerPx;
                    if (std::max(swMm, shMm) < kSubBoxMinEdgeMm)
                        continue;
                    int64_t cnt = 0;
                    for (int y = sb.y0; y <= sb.y1; ++y)
                    {
                        const size_t off = static_cast<size_t>(y) * static_cast<size_t>(canvas.WidthPx);
                        for (int x = sb.x0; x <= sb.x1; ++x)
                            cnt += diffMask[off + static_cast<size_t>(x)];
                    }
                    subs.push_back({ sb, cnt });
                }
                std::sort(subs.begin(), subs.end(), [](const ScoredSub& l, const ScoredSub& r) {
                    return l.px > r.px;
                });
                const size_t keepSubs = std::min(kMaxSubBoxesPerRegion, subs.size());
                item.SubBoxesDrawing.reserve(keepSubs);
                for (size_t k = 0; k < keepSubs; ++k)
                    item.SubBoxesDrawing.push_back(PxBoxToWorld(subs[k].box, canvas));

                result.push_back(std::move(item));
            }

            std::sort(result.begin(), result.end(), [](const DiffRegionItem& l, const DiffRegionItem& r) {
                const int64_t ls = l.AOnlyPixels + l.BOnlyPixels;
                const int64_t rs = r.AOnlyPixels + r.BOnlyPixels;
                if (ls != rs) return ls > rs;
                return l.BboxDrawing[0] < r.BboxDrawing[0];
            });
            for (size_t i = 0; i < result.size(); ++i)
                result[i].Id = static_cast<int>(i + 1);
            return result;
        }

        // ── 轨 2：独立空间文字与属性比对 ──────────────────────────────────

        void RunTrack2TextDiff(const Scene& sceneA, const CadSemanticSnapshot& snapA,
                               const std::string& spaceA, const std::string& layoutA,
                               const Scene& sceneB, const CadSemanticSnapshot& snapB,
                               const std::string& spaceB, const std::string& layoutB,
                               const std::array<double, 4>& cropInA,
                               double dx, double dy,
                               double tolMm,
                               const std::vector<DiffRegionItem>& regions,
                               DiffReport& report)
        {
            std::vector<ExtractedText> allA, allB;
            CollectAllSceneTexts(sceneA, snapA, allA);
            CollectAllSceneTexts(sceneB, snapB, allB);

            struct TextEntry
            {
                ExtractedText Raw;
                std::string   Norm;
                bool          InStrictCrop = false;
            };

            std::vector<TextEntry> listA, listB;
            for (auto& t : allA)
            {
                if (t.Space != spaceA) continue;
                if (spaceA == "paper" && t.LayoutName != layoutA) continue;
                std::string norm = NormalizeText(t.Content);
                if (norm.empty()) continue;
                const bool strictIn = PointInBox(t.X, t.Y, cropInA, 0.0)
                                   || BoxIntersectsBox({ t.MinX, t.MinY, t.MaxX, t.MaxY }, cropInA, 0.0);
                if (!strictIn)
                    continue;
                listA.push_back({ std::move(t), std::move(norm), true });
            }

            for (auto& t : allB)
            {
                if (t.Space != spaceB) continue;
                if (spaceB == "paper" && t.LayoutName != layoutB) continue;
                std::string norm = NormalizeText(t.Content);
                if (norm.empty()) continue;
                // B 平移至 A 坐标系
                t.X += dx;    t.Y += dy;
                t.MinX += dx; t.MaxX += dx;
                t.MinY += dy; t.MaxY += dy;
                const bool looseIn = PointInBox(t.X, t.Y, cropInA, tolMm)
                                  || BoxIntersectsBox({ t.MinX, t.MinY, t.MaxX, t.MaxY }, cropInA, tolMm);
                if (!looseIn)
                    continue;
                const bool strictIn = PointInBox(t.X, t.Y, cropInA, 0.0)
                                   || BoxIntersectsBox({ t.MinX, t.MinY, t.MaxX, t.MaxY }, cropInA, 0.0);
                listB.push_back({ std::move(t), std::move(norm), strictIn });
            }

            std::vector<bool> usedA(listA.size(), false);
            std::vector<bool> usedB(listB.size(), false);

            // 阶段 1：精确文本优先（用哈希桶按规整文本分组，避免 O(N^2)）
            std::unordered_map<std::string, std::vector<size_t>> bByNorm;
            bByNorm.reserve(listB.size());
            for (size_t j = 0; j < listB.size(); ++j)
                bByNorm[listB[j].Norm].push_back(j);

            struct PairMatch { size_t ia, ib; double score, dist; };
            std::vector<PairMatch> exactPairs;
            for (size_t i = 0; i < listA.size(); ++i)
            {
                auto it = bByNorm.find(listA[i].Norm);
                if (it == bByNorm.end()) continue;
                for (size_t j : it->second)
                {
                    const double d = std::hypot(listA[i].Raw.X - listB[j].Raw.X,
                                                listA[i].Raw.Y - listB[j].Raw.Y);
                    if (d <= tolMm)
                        exactPairs.push_back({ i, j, d, d });
                }
            }
            std::sort(exactPairs.begin(), exactPairs.end(), [](const PairMatch& l, const PairMatch& r) {
                return l.dist < r.dist;
            });
            for (const auto& m : exactPairs)
            {
                if (usedA[m.ia] || usedB[m.ib]) continue;
                usedA[m.ia] = true;
                usedB[m.ib] = true;
                ++report.TextUnchangedCount;
            }

            // 阶段 2：同位改字匹配（空间网格加速，优先同 source_kind 与同 tag）
            const double cellSize = std::max(1.0, tolMm);
            auto cellKey = [](int64_t cx, int64_t cy) -> uint64_t {
                return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32)
                     | static_cast<uint64_t>(static_cast<uint32_t>(cy));
            };
            std::unordered_map<uint64_t, std::vector<size_t>> gridB;
            for (size_t j = 0; j < listB.size(); ++j)
            {
                if (usedB[j]) continue;
                const int64_t cx = static_cast<int64_t>(std::floor(listB[j].Raw.X / cellSize));
                const int64_t cy = static_cast<int64_t>(std::floor(listB[j].Raw.Y / cellSize));
                gridB[cellKey(cx, cy)].push_back(j);
            }

            std::vector<PairMatch> modPairs;
            for (size_t i = 0; i < listA.size(); ++i)
            {
                if (usedA[i]) continue;
                const int64_t cx = static_cast<int64_t>(std::floor(listA[i].Raw.X / cellSize));
                const int64_t cy = static_cast<int64_t>(std::floor(listA[i].Raw.Y / cellSize));
                for (int64_t oy = -1; oy <= 1; ++oy)
                {
                    for (int64_t ox = -1; ox <= 1; ++ox)
                    {
                        auto it = gridB.find(cellKey(cx + ox, cy + oy));
                        if (it == gridB.end()) continue;
                        for (size_t j : it->second)
                        {
                            if (usedB[j]) continue;
                            const double d = std::hypot(listA[i].Raw.X - listB[j].Raw.X,
                                                        listA[i].Raw.Y - listB[j].Raw.Y);
                            if (d > tolMm) continue;
                            double score = d;
                            if (listA[i].Raw.SourceKind == listB[j].Raw.SourceKind)
                                score -= 50.0;
                            if (!listA[i].Raw.Tag.empty() && listA[i].Raw.Tag == listB[j].Raw.Tag)
                                score -= 100.0;
                            modPairs.push_back({ i, j, score, d });
                        }
                    }
                }
            }
            std::sort(modPairs.begin(), modPairs.end(), [](const PairMatch& l, const PairMatch& r) {
                if (l.score != r.score) return l.score < r.score;
                return l.dist < r.dist;
            });

            for (const auto& m : modPairs)
            {
                if (usedA[m.ia] || usedB[m.ib]) continue;
                usedA[m.ia] = true;
                usedB[m.ib] = true;

                const auto& a = listA[m.ia].Raw;
                const auto& b = listB[m.ib].Raw;
                DiffTextItem item;
                item.ChangeType   = "modified";
                item.SourceKind   = a.SourceKind;
                item.Tag          = !a.Tag.empty() ? a.Tag : b.Tag;
                item.X            = a.X;
                item.Y            = a.Y;
                item.AText        = listA[m.ia].Norm;
                item.BText        = listB[m.ib].Norm;
                item.ALayer       = a.Layer;
                item.BLayer       = b.Layer;
                item.AOwnerHandle = a.OwnerHandle;
                item.BOwnerHandle = b.OwnerHandle;
                item.HasDistMm    = true;
                item.DistMm       = m.dist;
                const std::array<double, 4> tbox{
                    std::min(a.MinX, b.MinX), std::min(a.MinY, b.MinY),
                    std::max(a.MaxX, b.MaxX), std::max(a.MaxY, b.MaxY)
                };
                item.RegionId = FindMatchingRegionId(regions, tbox, 100.0);
                report.TextChanges.push_back(std::move(item));
                ++report.TextModifiedCount;
            }

            // 阶段 3：未配对项
            for (size_t i = 0; i < listA.size(); ++i)
            {
                if (usedA[i] || !listA[i].InStrictCrop) continue;
                const auto& a = listA[i].Raw;
                DiffTextItem item;
                item.ChangeType   = "a_only";
                item.SourceKind   = a.SourceKind;
                item.Tag          = a.Tag;
                item.X            = a.X;
                item.Y            = a.Y;
                item.AText        = listA[i].Norm;
                item.ALayer       = a.Layer;
                item.AOwnerHandle = a.OwnerHandle;
                item.RegionId     = FindMatchingRegionId(regions, { a.MinX, a.MinY, a.MaxX, a.MaxY }, 100.0);
                report.TextChanges.push_back(std::move(item));
                ++report.TextAOnlyCount;
            }
            for (size_t j = 0; j < listB.size(); ++j)
            {
                if (usedB[j] || !listB[j].InStrictCrop) continue;
                const auto& b = listB[j].Raw;
                DiffTextItem item;
                item.ChangeType   = "b_only";
                item.SourceKind   = b.SourceKind;
                item.Tag          = b.Tag;
                item.X            = b.X;
                item.Y            = b.Y;
                item.BText        = listB[j].Norm;
                item.BLayer       = b.Layer;
                item.BOwnerHandle = b.OwnerHandle;
                item.RegionId     = FindMatchingRegionId(regions, { b.MinX, b.MinY, b.MaxX, b.MaxY }, 100.0);
                report.TextChanges.push_back(std::move(item));
                ++report.TextBOnlyCount;
            }

            // 按 region_id（有区域优先）再按坐标排序，保证输出确定性
            std::sort(report.TextChanges.begin(), report.TextChanges.end(),
                      [](const DiffTextItem& l, const DiffTextItem& r) {
                          const int lr = l.RegionId > 0 ? l.RegionId : 1'000'000;
                          const int rr = r.RegionId > 0 ? r.RegionId : 1'000'000;
                          if (lr != rr) return lr < rr;
                          if (l.ChangeType != r.ChangeType) return l.ChangeType < r.ChangeType;
                          if (l.X != r.X) return l.X < r.X;
                          return l.Y < r.Y;
                      });
        }

        // ── 轨 3：原生天正构件语义比对 ────────────────────────────────────

        std::string JoinParts(const std::vector<std::string>& parts, const char* sep = "，")
        {
            std::string out;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i > 0) out += sep;
                out += parts[i];
            }
            return out;
        }

        std::string WallParamJson(const CadSemanticSnapshot::TchWallRecord& w)
        {
            JsonWriter j(false);
            j.BeginObject();
            j.KeyString("dxf_name", w.DxfName);
            j.KeyDouble("thickness", w.TotalThickness, 1);
            if (w.HasHeight) j.KeyDouble("height", w.Height, 1);
            else             j.KeyNull("height");
            j.KeyString("material", w.MaterialName);
            j.KeyDouble("length", w.Length, 1);
            j.KeyInt("opening_count", w.OpeningCount);
            j.EndObject();
            return j.Take();
        }

        std::string OpeningParamJson(const CadSemanticSnapshot::TchOpeningRecord& o)
        {
            JsonWriter j(false);
            j.BeginObject();
            j.KeyString("kind", o.KindName);
            j.KeyOptString("label", o.Label);
            j.KeyDouble("width", o.Width, 1);
            j.KeyDouble("height", o.Height, 1);
            j.KeyDouble("sill_z", o.SillZ, 1);
            j.KeyInt("mirror", o.Mirror);
            j.EndObject();
            return j.Take();
        }

        std::string ColumnParamJson(const CadSemanticSnapshot::TchColumnRecord& c)
        {
            JsonWriter j(false);
            j.BeginObject();
            j.KeyString("material", c.MaterialName);
            if (c.HasArea) j.KeyDouble("area_m2", c.AreaM2, 4);
            j.KeyDouble("height", c.Height, 1);
            j.EndObject();
            return j.Take();
        }

        std::string SpaceParamJson(const CadSemanticSnapshot::TchSpaceRecord& s)
        {
            JsonWriter j(false);
            j.BeginObject();
            j.KeyOptString("name", s.Name);
            j.KeyDouble("area_m2", s.AreaM2, 2);
            j.KeyDouble("perimeter_mm", s.PerimeterMm, 1);
            j.EndObject();
            return j.Take();
        }

        std::string StairParamJson(const CadSemanticSnapshot::TchStairRecord& s)
        {
            JsonWriter j(false);
            j.BeginObject();
            j.KeyString("stair_kind", s.StairKind);
            if (s.StepHeight)   j.KeyDouble("step_height", *s.StepHeight, 1);
            if (s.TreadWidth)   j.KeyDouble("tread_width", *s.TreadWidth, 1);
            if (s.FlightWidth)  j.KeyDouble("flight_width", *s.FlightWidth, 1);
            if (s.Steps1)       j.KeyInt("steps1", *s.Steps1);
            if (s.Steps2)       j.KeyInt("steps2", *s.Steps2);
            if (!s.FloorTypeName.empty()) j.KeyString("floor_type", s.FloorTypeName);
            if (s.Mirror)       j.KeyBool("mirror", *s.Mirror);
            j.EndObject();
            return j.Take();
        }

        // 通用两级配对器：1) 同句柄且中心距 <= 3*tol；2) 空间最近邻 <= tol（可附加语义偏好减分）
        template <class Rec, class CenterFn, class HandleFn, class BonusFn>
        void MatchTwoStage(const std::vector<Rec>& listA,
                           const std::vector<Rec>& listB,
                           double tolMm,
                           CenterFn&& centerOf,
                           HandleFn&& handleOf,
                           BonusFn&& bonusOf,
                           std::vector<std::pair<size_t, size_t>>& matched,
                           std::vector<bool>& usedA,
                           std::vector<bool>& usedB)
        {
            usedA.assign(listA.size(), false);
            usedB.assign(listB.size(), false);

            // 级 1：同句柄
            std::unordered_map<uint64_t, std::vector<size_t>> bByHandle;
            for (size_t j = 0; j < listB.size(); ++j)
            {
                const uint64_t h = handleOf(listB[j]);
                if (h != 0) bByHandle[h].push_back(j);
            }
            for (size_t i = 0; i < listA.size(); ++i)
            {
                const uint64_t h = handleOf(listA[i]);
                if (h == 0) continue;
                auto it = bByHandle.find(h);
                if (it == bByHandle.end()) continue;
                const auto [ax, ay] = centerOf(listA[i]);
                size_t bestJ = SIZE_MAX;
                double bestD = 3.0 * tolMm + 1.0;
                for (size_t j : it->second)
                {
                    if (usedB[j]) continue;
                    const auto [bx, by] = centerOf(listB[j]);
                    const double d = std::hypot(ax - bx, ay - by);
                    if (d <= 3.0 * tolMm && d < bestD)
                    {
                        bestD = d;
                        bestJ = j;
                    }
                }
                if (bestJ != SIZE_MAX)
                {
                    usedA[i]     = true;
                    usedB[bestJ] = true;
                    matched.push_back({ i, bestJ });
                }
            }

            // 级 2：空间最近邻（含同类/同编号加权优先）
            struct Cand { size_t ia, ib; double score, dist; };
            std::vector<Cand> cands;
            for (size_t i = 0; i < listA.size(); ++i)
            {
                if (usedA[i]) continue;
                const auto [ax, ay] = centerOf(listA[i]);
                for (size_t j = 0; j < listB.size(); ++j)
                {
                    if (usedB[j]) continue;
                    const auto [bx, by] = centerOf(listB[j]);
                    const double d = std::hypot(ax - bx, ay - by);
                    if (d <= tolMm)
                        cands.push_back({ i, j, d - bonusOf(listA[i], listB[j]), d });
                }
            }
            std::sort(cands.begin(), cands.end(), [](const Cand& l, const Cand& r) {
                if (l.score != r.score) return l.score < r.score;
                return l.dist < r.dist;
            });
            for (const auto& c : cands)
            {
                if (usedA[c.ia] || usedB[c.ib]) continue;
                usedA[c.ia] = true;
                usedB[c.ib] = true;
                matched.push_back({ c.ia, c.ib });
            }
        }

        void RunTrack3TchDiffImpl(const CadSemanticSnapshot& snapA, const std::string& spaceA, const std::string& layoutA,
                                  const CadSemanticSnapshot& snapB, const std::string& spaceB, const std::string& layoutB,
                                  const std::array<double, 4>& cropInA,
                                  double dx, double dy,
                                  double tolMm,
                                  const std::vector<DiffRegionItem>& regions,
                                  DiffReport& report)
        {
            auto filterLayout = [](const std::string& sp, const std::string& lay,
                                   const std::string& targetSp, const std::string& targetLay) -> bool {
                if (sp != targetSp) return false;
                if (targetSp == "paper" && !targetLay.empty() && lay != targetLay) return false;
                return true;
            };

            // 收集五类天正对象（B 侧平移 +(dx, dy)）
            std::vector<CadSemanticSnapshot::TchWallRecord> wallsA, wallsB;
            for (const auto& w : snapA.TchWalls)
            {
                if (!w.BlockName.empty() || !filterLayout(w.Space, w.LayoutName, spaceA, layoutA)) continue;
                if (BoxIntersectsBox({ w.MinX, w.MinY, w.MaxX, w.MaxY }, cropInA, 0.0))
                    wallsA.push_back(w);
            }
            for (auto w : snapB.TchWalls)
            {
                if (!w.BlockName.empty() || !filterLayout(w.Space, w.LayoutName, spaceB, layoutB)) continue;
                w.StartX += dx; w.StartY += dy; w.EndX += dx; w.EndY += dy;
                w.ArcCx += dx;  w.ArcCy += dy;
                w.MinX += dx;   w.MaxX += dx;   w.MinY += dy; w.MaxY += dy;
                if (BoxIntersectsBox({ w.MinX, w.MinY, w.MaxX, w.MaxY }, cropInA, 0.0))
                    wallsB.push_back(std::move(w));
            }

            std::vector<CadSemanticSnapshot::TchOpeningRecord> openA, openB;
            for (const auto& o : snapA.TchOpenings)
            {
                if (!o.BlockName.empty() || !filterLayout(o.Space, o.LayoutName, spaceA, layoutA)) continue;
                if (BoxIntersectsBox({ o.MinX, o.MinY, o.MaxX, o.MaxY }, cropInA, 0.0))
                    openA.push_back(o);
            }
            for (auto o : snapB.TchOpenings)
            {
                if (!o.BlockName.empty() || !filterLayout(o.Space, o.LayoutName, spaceB, layoutB)) continue;
                o.X += dx; o.Y += dy; o.LabelX += dx; o.LabelY += dy;
                o.MinX += dx; o.MaxX += dx; o.MinY += dy; o.MaxY += dy;
                if (BoxIntersectsBox({ o.MinX, o.MinY, o.MaxX, o.MaxY }, cropInA, 0.0))
                    openB.push_back(std::move(o));
            }

            std::vector<CadSemanticSnapshot::TchColumnRecord> colA, colB;
            for (const auto& c : snapA.TchColumns)
            {
                if (!c.BlockName.empty() || !filterLayout(c.Space, c.LayoutName, spaceA, layoutA)) continue;
                if (BoxIntersectsBox({ c.MinX, c.MinY, c.MaxX, c.MaxY }, cropInA, 0.0))
                    colA.push_back(c);
            }
            for (auto c : snapB.TchColumns)
            {
                if (!c.BlockName.empty() || !filterLayout(c.Space, c.LayoutName, spaceB, layoutB)) continue;
                c.X += dx; c.Y += dy;
                c.MinX += dx; c.MaxX += dx; c.MinY += dy; c.MaxY += dy;
                if (BoxIntersectsBox({ c.MinX, c.MinY, c.MaxX, c.MaxY }, cropInA, 0.0))
                    colB.push_back(std::move(c));
            }

            std::vector<CadSemanticSnapshot::TchSpaceRecord> spA, spB;
            for (const auto& s : snapA.TchSpaces)
            {
                if (!s.BlockName.empty() || !filterLayout(s.Space, s.LayoutName, spaceA, layoutA)) continue;
                if (BoxIntersectsBox({ s.MinX, s.MinY, s.MaxX, s.MaxY }, cropInA, 0.0))
                    spA.push_back(s);
            }
            for (auto s : snapB.TchSpaces)
            {
                if (!s.BlockName.empty() || !filterLayout(s.Space, s.LayoutName, spaceB, layoutB)) continue;
                s.CentroidX += dx; s.CentroidY += dy;
                s.MinX += dx; s.MaxX += dx; s.MinY += dy; s.MaxY += dy;
                if (BoxIntersectsBox({ s.MinX, s.MinY, s.MaxX, s.MaxY }, cropInA, 0.0))
                    spB.push_back(std::move(s));
            }

            std::vector<CadSemanticSnapshot::TchStairRecord> stA, stB;
            for (const auto& s : snapA.TchStairs)
            {
                if (!s.BlockName.empty() || !filterLayout(s.Space, s.LayoutName, spaceA, layoutA)) continue;
                if (BoxIntersectsBox({ s.MinX, s.MinY, s.MaxX, s.MaxY }, cropInA, 0.0))
                    stA.push_back(s);
            }
            for (auto s : snapB.TchStairs)
            {
                if (!s.BlockName.empty() || !filterLayout(s.Space, s.LayoutName, spaceB, layoutB)) continue;
                s.X += dx; s.Y += dy;
                s.MinX += dx; s.MaxX += dx; s.MinY += dy; s.MaxY += dy;
                if (BoxIntersectsBox({ s.MinX, s.MinY, s.MaxX, s.MaxY }, cropInA, 0.0))
                    stB.push_back(std::move(s));
            }

            // CAD-18：按源图天正解码状态判定天正轨可用性，而非按裁剪后构件数量判定，
            // 使原生天正图纸在裁剪窗口内单侧全删/全增或空侧新增均可正常产出差异项。
            auto isNativeTch = [](const CadSemanticSnapshot& s) -> bool {
                return s.TchDecodedCount > 0
                    || !s.TchWalls.empty() || !s.TchOpenings.empty() || !s.TchColumns.empty()
                    || !s.TchSpaces.empty() || !s.TchStairs.empty() || !s.TchDimensions.empty()
                    || !s.TchAxes.empty() || !s.TchSheets.empty() || !s.TchTexts.empty();
            };
            const bool nativeA = isNativeTch(snapA);
            const bool nativeB = isNativeTch(snapB);
            const bool failedA = !nativeA && snapA.TchFailedCount > 0;
            const bool failedB = !nativeB && snapB.TchFailedCount > 0;
            const bool emptyA  = !nativeA && !failedA && snapA.EntityOrigins.empty();
            const bool emptyB  = !nativeB && !failedB && snapB.EntityOrigins.empty();

            if (!nativeA && !nativeB)
            {
                report.TchAvailable = false;
                report.TchReason    = (failedA || failedB)
                                        ? "A/B 两侧源图天正对象未能成功解码"
                                        : "A/B 两侧源图均未检测到原生天正解码对象";
                return;
            }
            if (!nativeA && !emptyA)
            {
                report.TchAvailable = false;
                report.TchReason    = failedA
                                        ? "A 侧源图天正对象解码失败，无法启用天正语义轨"
                                        : "A 侧源图非原生天正对象（已炸为普通图元），跳过天正语义轨以避免误报";
                return;
            }
            if (!nativeB && !emptyB)
            {
                report.TchAvailable = false;
                report.TchReason    = failedB
                                        ? "B 侧源图天正对象解码失败，无法启用天正语义轨"
                                        : "B 侧源图非原生天正对象（已炸为普通图元），跳过天正语义轨以避免误报";
                return;
            }

            report.TchAvailable = true;
            report.TchReason.clear();

            auto addChange = [&](DiffTchChangeItem item, int& counter) {
                item.RegionId = FindMatchingRegionId(regions, item.BboxDrawing, 100.0);
                report.TchChanges.push_back(std::move(item));
                ++counter;
            };

            // 1) 墙体比对
            {
                std::vector<std::pair<size_t, size_t>> matched;
                std::vector<bool> usedA, usedB;
                MatchTwoStage(wallsA, wallsB, tolMm,
                    [](const auto& w) { return std::pair<double, double>{ 0.5 * (w.StartX + w.EndX), 0.5 * (w.StartY + w.EndY) }; },
                    [](const auto& w) { return w.Handle; },
                    [](const auto& a, const auto& b) { return a.DxfName == b.DxfName ? 30.0 : 0.0; },
                    matched, usedA, usedB);

                for (const auto& [ia, ib] : matched)
                {
                    const auto& a = wallsA[ia];
                    const auto& b = wallsB[ib];
                    std::vector<std::string> diffs;
                    if (std::abs(a.TotalThickness - b.TotalThickness) > 1.0)
                        diffs.push_back("墙厚 " + JsonDouble(a.TotalThickness, 0) + "->" + JsonDouble(b.TotalThickness, 0) + "mm");
                    if (a.HasHeight != b.HasHeight)
                    {
                        const std::string hA = a.HasHeight ? (JsonDouble(a.Height, 0) + "mm") : "缺失";
                        const std::string hB = b.HasHeight ? (JsonDouble(b.Height, 0) + "mm") : "缺失";
                        diffs.push_back("墙高 " + hA + "->" + hB);
                    }
                    else if (a.HasHeight && b.HasHeight && std::abs(a.Height - b.Height) > 1.0)
                    {
                        diffs.push_back("墙高 " + JsonDouble(a.Height, 0) + "->" + JsonDouble(b.Height, 0) + "mm");
                    }
                    if (a.MaterialName != b.MaterialName)
                        diffs.push_back("材料 " + a.MaterialName + "->" + b.MaterialName);
                    if (std::abs(a.Length - b.Length) > 5.0)
                        diffs.push_back("长度 " + JsonDouble(a.Length, 0) + "->" + JsonDouble(b.Length, 0) + "mm");
                    if (a.OpeningCount != b.OpeningCount)
                        diffs.push_back("开洞数 " + std::to_string(a.OpeningCount) + "->" + std::to_string(b.OpeningCount));

                    const double dDirect = std::max(std::hypot(a.StartX - b.StartX, a.StartY - b.StartY),
                                                    std::hypot(a.EndX - b.EndX, a.EndY - b.EndY));
                    const double dFlip   = std::max(std::hypot(a.StartX - b.EndX, a.StartY - b.EndY),
                                                    std::hypot(a.EndX - b.StartX, a.EndY - b.StartY));
                    const double endShift = std::min(dDirect, dFlip);
                    if (endShift > 10.0)
                        diffs.push_back("端点偏移 " + JsonDouble(endShift, 0) + "mm");

                    if (diffs.empty()) continue;

                    DiffTchChangeItem item;
                    item.Category    = "wall";
                    item.ChangeType  = "modified";
                    item.HandleA     = FormatHandleHex(a.Handle);
                    item.HandleB     = FormatHandleHex(b.Handle);
                    item.Label       = a.MaterialName.empty() ? a.DxfName : a.MaterialName;
                    item.X           = 0.5 * (a.StartX + a.EndX);
                    item.Y           = 0.5 * (a.StartY + a.EndY);
                    item.BboxDrawing = {
                        std::min(a.MinX, b.MinX), std::min(a.MinY, b.MinY),
                        std::max(a.MaxX, b.MaxX), std::max(a.MaxY, b.MaxY)
                    };
                    item.Summary    = "墙体变更：" + JoinParts(diffs);
                    item.BeforeJson = WallParamJson(a);
                    item.AfterJson  = WallParamJson(b);
                    addChange(std::move(item), report.TchWallCount);
                }
                for (size_t i = 0; i < wallsA.size(); ++i)
                {
                    if (usedA[i]) continue;
                    const auto& a = wallsA[i];
                    DiffTchChangeItem item;
                    item.Category    = "wall";
                    item.ChangeType  = "a_only";
                    item.HandleA     = FormatHandleHex(a.Handle);
                    item.Label       = a.MaterialName.empty() ? a.DxfName : a.MaterialName;
                    item.X           = 0.5 * (a.StartX + a.EndX);
                    item.Y           = 0.5 * (a.StartY + a.EndY);
                    item.BboxDrawing = { a.MinX, a.MinY, a.MaxX, a.MaxY };
                    item.Summary     = "仅 A 侧存在墙体（厚 " + JsonDouble(a.TotalThickness, 0)
                                     + "mm，长 " + JsonDouble(a.Length, 0) + "mm"
                                     + (a.HasHeight ? ("，高 " + JsonDouble(a.Height, 0) + "mm") : "") + "）";
                    item.BeforeJson  = WallParamJson(a);
                    addChange(std::move(item), report.TchWallCount);
                }
                for (size_t j = 0; j < wallsB.size(); ++j)
                {
                    if (usedB[j]) continue;
                    const auto& b = wallsB[j];
                    DiffTchChangeItem item;
                    item.Category    = "wall";
                    item.ChangeType  = "b_only";
                    item.HandleB     = FormatHandleHex(b.Handle);
                    item.Label       = b.MaterialName.empty() ? b.DxfName : b.MaterialName;
                    item.X           = 0.5 * (b.StartX + b.EndX);
                    item.Y           = 0.5 * (b.StartY + b.EndY);
                    item.BboxDrawing = { b.MinX, b.MinY, b.MaxX, b.MaxY };
                    item.Summary     = "仅 B 侧存在墙体（厚 " + JsonDouble(b.TotalThickness, 0)
                                     + "mm，长 " + JsonDouble(b.Length, 0) + "mm"
                                     + (b.HasHeight ? ("，高 " + JsonDouble(b.Height, 0) + "mm") : "") + "）";
                    item.AfterJson   = WallParamJson(b);
                    addChange(std::move(item), report.TchWallCount);
                }
            }

            // 2) 门窗/洞口比对
            {
                std::vector<std::pair<size_t, size_t>> matched;
                std::vector<bool> usedA, usedB;
                MatchTwoStage(openA, openB, tolMm,
                    [](const auto& o) { return std::pair<double, double>{ o.X, o.Y }; },
                    [](const auto& o) { return o.Handle; },
                    [](const auto& a, const auto& b) {
                        double bonus = 0.0;
                        if (a.KindCode == b.KindCode) bonus += 40.0;
                        if (!a.Label.empty() && a.Label == b.Label) bonus += 80.0;
                        return bonus;
                    },
                    matched, usedA, usedB);

                for (const auto& [ia, ib] : matched)
                {
                    const auto& a = openA[ia];
                    const auto& b = openB[ib];
                    std::vector<std::string> diffs;
                    if (a.KindName != b.KindName)
                        diffs.push_back("类型 " + a.KindName + "->" + b.KindName);
                    if (a.Label != b.Label)
                        diffs.push_back(a.KindName + "编号 " + a.Label + "->" + b.Label);
                    if (std::abs(a.Width - b.Width) > 1.0)
                        diffs.push_back("宽 " + JsonDouble(a.Width, 0) + "->" + JsonDouble(b.Width, 0) + "mm");
                    if (std::abs(a.Height - b.Height) > 1.0)
                        diffs.push_back("高 " + JsonDouble(a.Height, 0) + "->" + JsonDouble(b.Height, 0) + "mm");
                    if (std::abs(a.SillZ - b.SillZ) > 1.0)
                        diffs.push_back("窗台高 " + JsonDouble(a.SillZ, 0) + "->" + JsonDouble(b.SillZ, 0) + "mm");
                    if (a.Mirror != b.Mirror)
                        diffs.push_back("开启/镜像方向变化");
                    const double shift = std::hypot(a.X - b.X, a.Y - b.Y);
                    if (shift > 10.0)
                        diffs.push_back("位移 " + JsonDouble(shift, 0) + "mm");

                    if (diffs.empty()) continue;

                    DiffTchChangeItem item;
                    item.Category    = "opening";
                    item.ChangeType  = "modified";
                    item.HandleA     = FormatHandleHex(a.Handle);
                    item.HandleB     = FormatHandleHex(b.Handle);
                    item.Label       = (a.Label != b.Label) ? (a.Label + "->" + b.Label)
                                                            : (!a.Label.empty() ? a.Label : a.KindName);
                    item.X           = a.X;
                    item.Y           = a.Y;
                    item.BboxDrawing = {
                        std::min(a.MinX, b.MinX), std::min(a.MinY, b.MinY),
                        std::max(a.MaxX, b.MaxX), std::max(a.MaxY, b.MaxY)
                    };
                    item.Summary    = JoinParts(diffs);
                    item.BeforeJson = OpeningParamJson(a);
                    item.AfterJson  = OpeningParamJson(b);
                    addChange(std::move(item), report.TchOpeningCount);
                }
                for (size_t i = 0; i < openA.size(); ++i)
                {
                    if (usedA[i]) continue;
                    const auto& a = openA[i];
                    DiffTchChangeItem item;
                    item.Category    = "opening";
                    item.ChangeType  = "a_only";
                    item.HandleA     = FormatHandleHex(a.Handle);
                    item.Label       = !a.Label.empty() ? a.Label : a.KindName;
                    item.X           = a.X;
                    item.Y           = a.Y;
                    item.BboxDrawing = { a.MinX, a.MinY, a.MaxX, a.MaxY };
                    item.Summary     = "仅 A 侧存在" + a.KindName
                                     + (a.Label.empty() ? "" : (" " + a.Label))
                                     + "（" + JsonDouble(a.Width, 0) + "x" + JsonDouble(a.Height, 0) + "mm）";
                    item.BeforeJson  = OpeningParamJson(a);
                    addChange(std::move(item), report.TchOpeningCount);
                }
                for (size_t j = 0; j < openB.size(); ++j)
                {
                    if (usedB[j]) continue;
                    const auto& b = openB[j];
                    DiffTchChangeItem item;
                    item.Category    = "opening";
                    item.ChangeType  = "b_only";
                    item.HandleB     = FormatHandleHex(b.Handle);
                    item.Label       = !b.Label.empty() ? b.Label : b.KindName;
                    item.X           = b.X;
                    item.Y           = b.Y;
                    item.BboxDrawing = { b.MinX, b.MinY, b.MaxX, b.MaxY };
                    item.Summary     = "仅 B 侧存在" + b.KindName
                                     + (b.Label.empty() ? "" : (" " + b.Label))
                                     + "（" + JsonDouble(b.Width, 0) + "x" + JsonDouble(b.Height, 0) + "mm）";
                    item.AfterJson   = OpeningParamJson(b);
                    addChange(std::move(item), report.TchOpeningCount);
                }
            }

            // 3) 柱比对
            {
                std::vector<std::pair<size_t, size_t>> matched;
                std::vector<bool> usedA, usedB;
                MatchTwoStage(colA, colB, tolMm,
                    [](const auto& c) { return std::pair<double, double>{ c.X, c.Y }; },
                    [](const auto& c) { return c.Handle; },
                    [](const auto& a, const auto& b) { return a.MaterialCode == b.MaterialCode ? 20.0 : 0.0; },
                    matched, usedA, usedB);

                for (const auto& [ia, ib] : matched)
                {
                    const auto& a = colA[ia];
                    const auto& b = colB[ib];
                    std::vector<std::string> diffs;
                    if (a.MaterialName != b.MaterialName)
                        diffs.push_back("材料 " + a.MaterialName + "->" + b.MaterialName);
                    if (a.HasArea && b.HasArea && std::abs(a.AreaM2 - b.AreaM2) > 0.005)
                        diffs.push_back("截面面积 " + JsonDouble(a.AreaM2, 3) + "->" + JsonDouble(b.AreaM2, 3) + "m2");
                    if (std::abs(a.Height - b.Height) > 1.0)
                        diffs.push_back("柱高 " + JsonDouble(a.Height, 0) + "->" + JsonDouble(b.Height, 0) + "mm");
                    const double shift = std::hypot(a.X - b.X, a.Y - b.Y);
                    if (shift > 10.0)
                        diffs.push_back("位移 " + JsonDouble(shift, 0) + "mm");

                    if (diffs.empty()) continue;

                    DiffTchChangeItem item;
                    item.Category    = "column";
                    item.ChangeType  = "modified";
                    item.HandleA     = FormatHandleHex(a.Handle);
                    item.HandleB     = FormatHandleHex(b.Handle);
                    item.Label       = a.MaterialName.empty() ? "柱" : a.MaterialName;
                    item.X           = a.X;
                    item.Y           = a.Y;
                    item.BboxDrawing = {
                        std::min(a.MinX, b.MinX), std::min(a.MinY, b.MinY),
                        std::max(a.MaxX, b.MaxX), std::max(a.MaxY, b.MaxY)
                    };
                    item.Summary    = "柱变更：" + JoinParts(diffs);
                    item.BeforeJson = ColumnParamJson(a);
                    item.AfterJson  = ColumnParamJson(b);
                    addChange(std::move(item), report.TchColumnCount);
                }
                for (size_t i = 0; i < colA.size(); ++i)
                {
                    if (usedA[i]) continue;
                    const auto& a = colA[i];
                    DiffTchChangeItem item;
                    item.Category    = "column";
                    item.ChangeType  = "a_only";
                    item.HandleA     = FormatHandleHex(a.Handle);
                    item.Label       = a.MaterialName.empty() ? "柱" : a.MaterialName;
                    item.X           = a.X;
                    item.Y           = a.Y;
                    item.BboxDrawing = { a.MinX, a.MinY, a.MaxX, a.MaxY };
                    item.Summary     = "仅 A 侧存在柱（" + item.Label + "）";
                    item.BeforeJson  = ColumnParamJson(a);
                    addChange(std::move(item), report.TchColumnCount);
                }
                for (size_t j = 0; j < colB.size(); ++j)
                {
                    if (usedB[j]) continue;
                    const auto& b = colB[j];
                    DiffTchChangeItem item;
                    item.Category    = "column";
                    item.ChangeType  = "b_only";
                    item.HandleB     = FormatHandleHex(b.Handle);
                    item.Label       = b.MaterialName.empty() ? "柱" : b.MaterialName;
                    item.X           = b.X;
                    item.Y           = b.Y;
                    item.BboxDrawing = { b.MinX, b.MinY, b.MaxX, b.MaxY };
                    item.Summary     = "仅 B 侧存在柱（" + item.Label + "）";
                    item.AfterJson   = ColumnParamJson(b);
                    addChange(std::move(item), report.TchColumnCount);
                }
            }

            // 4) 房间面积比对（房间中心位移容差放宽为 max(tolMm, 1500mm)）
            {
                const double spaceTol = std::max(tolMm, 1500.0);
                std::vector<std::pair<size_t, size_t>> matched;
                std::vector<bool> usedA, usedB;
                MatchTwoStage(spA, spB, spaceTol,
                    [](const auto& s) { return std::pair<double, double>{ s.CentroidX, s.CentroidY }; },
                    [](const auto& s) { return s.Handle; },
                    [](const auto& a, const auto& b) { return (!a.Name.empty() && a.Name == b.Name) ? 400.0 : 0.0; },
                    matched, usedA, usedB);

                for (const auto& [ia, ib] : matched)
                {
                    const auto& a = spA[ia];
                    const auto& b = spB[ib];
                    std::vector<std::string> diffs;
                    if (a.Name != b.Name)
                        diffs.push_back("房间名 " + a.Name + "->" + b.Name);
                    if (std::abs(a.AreaM2 - b.AreaM2) > 0.05)
                        diffs.push_back("面积 " + JsonDouble(a.AreaM2, 2) + "->" + JsonDouble(b.AreaM2, 2) + "m2");

                    if (diffs.empty()) continue;

                    DiffTchChangeItem item;
                    item.Category    = "space";
                    item.ChangeType  = "modified";
                    item.HandleA     = FormatHandleHex(a.Handle);
                    item.HandleB     = FormatHandleHex(b.Handle);
                    item.Label       = (a.Name != b.Name) ? (a.Name + "->" + b.Name)
                                                          : (!a.Name.empty() ? a.Name : "房间");
                    item.X           = a.CentroidX;
                    item.Y           = a.CentroidY;
                    item.BboxDrawing = {
                        std::min(a.MinX, b.MinX), std::min(a.MinY, b.MinY),
                        std::max(a.MaxX, b.MaxX), std::max(a.MaxY, b.MaxY)
                    };
                    item.Summary    = "房间变更：" + JoinParts(diffs);
                    item.BeforeJson = SpaceParamJson(a);
                    item.AfterJson  = SpaceParamJson(b);
                    addChange(std::move(item), report.TchSpaceCount);
                }
                for (size_t i = 0; i < spA.size(); ++i)
                {
                    if (usedA[i]) continue;
                    const auto& a = spA[i];
                    DiffTchChangeItem item;
                    item.Category    = "space";
                    item.ChangeType  = "a_only";
                    item.HandleA     = FormatHandleHex(a.Handle);
                    item.Label       = !a.Name.empty() ? a.Name : "房间";
                    item.X           = a.CentroidX;
                    item.Y           = a.CentroidY;
                    item.BboxDrawing = { a.MinX, a.MinY, a.MaxX, a.MaxY };
                    item.Summary     = "仅 A 侧存在房间 " + item.Label + "（" + JsonDouble(a.AreaM2, 2) + "m2）";
                    item.BeforeJson  = SpaceParamJson(a);
                    addChange(std::move(item), report.TchSpaceCount);
                }
                for (size_t j = 0; j < spB.size(); ++j)
                {
                    if (usedB[j]) continue;
                    const auto& b = spB[j];
                    DiffTchChangeItem item;
                    item.Category    = "space";
                    item.ChangeType  = "b_only";
                    item.HandleB     = FormatHandleHex(b.Handle);
                    item.Label       = !b.Name.empty() ? b.Name : "房间";
                    item.X           = b.CentroidX;
                    item.Y           = b.CentroidY;
                    item.BboxDrawing = { b.MinX, b.MinY, b.MaxX, b.MaxY };
                    item.Summary     = "仅 B 侧存在房间 " + item.Label + "（" + JsonDouble(b.AreaM2, 2) + "m2）";
                    item.AfterJson   = SpaceParamJson(b);
                    addChange(std::move(item), report.TchSpaceCount);
                }
            }

            // 5) 楼梯比对
            {
                const double stairTol = std::max(tolMm, 600.0);
                std::vector<std::pair<size_t, size_t>> matched;
                std::vector<bool> usedA, usedB;
                MatchTwoStage(stA, stB, stairTol,
                    [](const auto& s) { return std::pair<double, double>{ s.X, s.Y }; },
                    [](const auto& s) { return s.Handle; },
                    [](const auto& a, const auto& b) { return a.StairKind == b.StairKind ? 100.0 : 0.0; },
                    matched, usedA, usedB);

                auto optDiffD = [](const std::optional<double>& a, const std::optional<double>& b, double eps) {
                    if (a.has_value() != b.has_value()) return true;
                    return a.has_value() && std::abs(*a - *b) > eps;
                };
                for (const auto& [ia, ib] : matched)
                {
                    const auto& a = stA[ia];
                    const auto& b = stB[ib];
                    std::vector<std::string> diffs;
                    if (a.StairKind != b.StairKind)
                        diffs.push_back("楼梯类型 " + a.StairKind + "->" + b.StairKind);
                    if (optDiffD(a.StepHeight, b.StepHeight, 0.5))
                        diffs.push_back("踏步高变化");
                    if (optDiffD(a.TreadWidth, b.TreadWidth, 0.5))
                        diffs.push_back("踏步宽变化");
                    if (optDiffD(a.FlightWidth, b.FlightWidth, 1.0))
                        diffs.push_back("梯段宽变化");
                    if (a.Steps1 != b.Steps1 || a.Steps2 != b.Steps2)
                        diffs.push_back("踏步数变化");
                    if (a.FloorTypeName != b.FloorTypeName)
                        diffs.push_back("楼层类型 " + a.FloorTypeName + "->" + b.FloorTypeName);
                    if (a.Mirror != b.Mirror)
                        diffs.push_back("镜像方向变化");
                    const double shift = std::hypot(a.X - b.X, a.Y - b.Y);
                    if (shift > 10.0)
                        diffs.push_back("位移 " + JsonDouble(shift, 0) + "mm");

                    if (diffs.empty()) continue;

                    DiffTchChangeItem item;
                    item.Category    = "stair";
                    item.ChangeType  = "modified";
                    item.HandleA     = FormatHandleHex(a.Handle);
                    item.HandleB     = FormatHandleHex(b.Handle);
                    item.Label       = a.StairKind;
                    item.X           = a.X;
                    item.Y           = a.Y;
                    item.BboxDrawing = {
                        std::min(a.MinX, b.MinX), std::min(a.MinY, b.MinY),
                        std::max(a.MaxX, b.MaxX), std::max(a.MaxY, b.MaxY)
                    };
                    item.Summary    = "楼梯变更：" + JoinParts(diffs);
                    item.BeforeJson = StairParamJson(a);
                    item.AfterJson  = StairParamJson(b);
                    addChange(std::move(item), report.TchStairCount);
                }
                for (size_t i = 0; i < stA.size(); ++i)
                {
                    if (usedA[i]) continue;
                    const auto& a = stA[i];
                    DiffTchChangeItem item;
                    item.Category    = "stair";
                    item.ChangeType  = "a_only";
                    item.HandleA     = FormatHandleHex(a.Handle);
                    item.Label       = a.StairKind;
                    item.X           = a.X;
                    item.Y           = a.Y;
                    item.BboxDrawing = { a.MinX, a.MinY, a.MaxX, a.MaxY };
                    item.Summary     = "仅 A 侧存在楼梯（" + a.StairKind + "）";
                    item.BeforeJson  = StairParamJson(a);
                    addChange(std::move(item), report.TchStairCount);
                }
                for (size_t j = 0; j < stB.size(); ++j)
                {
                    if (usedB[j]) continue;
                    const auto& b = stB[j];
                    DiffTchChangeItem item;
                    item.Category    = "stair";
                    item.ChangeType  = "b_only";
                    item.HandleB     = FormatHandleHex(b.Handle);
                    item.Label       = b.StairKind;
                    item.X           = b.X;
                    item.Y           = b.Y;
                    item.BboxDrawing = { b.MinX, b.MinY, b.MaxX, b.MaxY };
                    item.Summary     = "仅 B 侧存在楼梯（" + b.StairKind + "）";
                    item.AfterJson   = StairParamJson(b);
                    addChange(std::move(item), report.TchStairCount);
                }
            }
        }

        // ── 可选比对结果 SQLite（严格对齐设计方案 §4.4 4 表 DDL）──────────

        const char* kDiffSchemaSql = R"SQL(
CREATE TABLE diff_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE diff_regions (
    id                 INTEGER PRIMARY KEY,
    minx               REAL NOT NULL,
    miny               REAL NOT NULL,
    maxx               REAL NOT NULL,
    maxy               REAL NOT NULL,
    px_x0              INTEGER NOT NULL,
    px_y0              INTEGER NOT NULL,
    px_x1              INTEGER NOT NULL,
    px_y1              INTEGER NOT NULL,
    a_only_pixels      INTEGER NOT NULL,
    b_only_pixels      INTEGER NOT NULL,
    dominant_side      TEXT NOT NULL,
    sub_boxes_json     TEXT NOT NULL
);

CREATE VIRTUAL TABLE diff_regions_rtree USING rtree(
    id,
    minx, maxx,
    miny, maxy
);

CREATE TABLE diff_texts (
    id             INTEGER PRIMARY KEY,
    change_type    TEXT NOT NULL,
    source_kind    TEXT NOT NULL,
    tag            TEXT,
    x              REAL NOT NULL,
    y              REAL NOT NULL,
    a_text         TEXT,
    b_text         TEXT,
    a_layer        TEXT,
    b_layer        TEXT,
    a_owner_handle TEXT,
    b_owner_handle TEXT,
    dist_mm        REAL,
    region_id      INTEGER
);

CREATE TABLE diff_tch_changes (
    id          INTEGER PRIMARY KEY,
    category    TEXT NOT NULL,
    change_type TEXT NOT NULL,
    handle_a    TEXT,
    handle_b    TEXT,
    label       TEXT,
    x           REAL NOT NULL,
    y           REAL NOT NULL,
    minx        REAL NOT NULL,
    miny        REAL NOT NULL,
    maxx        REAL NOT NULL,
    maxy        REAL NOT NULL,
    summary     TEXT NOT NULL,
    before_json TEXT,
    after_json  TEXT,
    region_id   INTEGER
);

CREATE INDEX idx_diff_texts_region ON diff_texts(region_id);
CREATE INDEX idx_diff_texts_change ON diff_texts(change_type);
CREATE INDEX idx_diff_tch_region ON diff_tch_changes(region_id);
CREATE INDEX idx_diff_tch_cat ON diff_tch_changes(category, change_type);
)SQL";

        struct CoverageScopes
        {
            bool        PaperA       = false;
            bool        PaperB       = false;
            const char* LayoutSpace  = "model";
            const char* LayoutSpaceA = "model";
            const char* LayoutSpaceB = "model";
            const char* TextScope    = "model_space";
            const char* TextScopeA   = "model_space";
            const char* TextScopeB   = "model_space";
            const char* TchScope     = "model_space";
            const char* TchScopeA    = "model_space";
            const char* TchScopeB    = "model_space";
        };

        // 逐侧解析模型/纸张覆盖范围（供 diff_meta、JSON coverage、Warnings 与 CoverageNote 共享，CAD-R2-14）
        CoverageScopes ResolveCoverageScopes(const DiffReport& r)
        {
            CoverageScopes s;
            if (!r.IsPaperSpaceLayout)
                return s;
            const bool modelA = (r.SideA.Layout == "Model");
            const bool modelB = (r.SideB.Layout == "Model");
            s.PaperA       = !modelA;
            s.PaperB       = !modelB;
            s.LayoutSpaceA = s.PaperA ? "paper" : "model";
            s.LayoutSpaceB = s.PaperB ? "paper" : "model";
            s.TextScopeA   = s.PaperA ? "paper_space_only" : "model_space";
            s.TextScopeB   = s.PaperB ? "paper_space_only" : "model_space";
            s.TchScopeA    = s.TextScopeA;
            s.TchScopeB    = s.TextScopeB;
            if (s.PaperA && s.PaperB)
            {
                s.LayoutSpace = "paper";
                s.TextScope   = "paper_space_only";
                s.TchScope    = "paper_space_only";
            }
            else if (!s.PaperA && !s.PaperB)
            {
                s.LayoutSpace = "model";
                s.TextScope   = "model_space";
                s.TchScope    = "model_space";
            }
            else
            {
                s.LayoutSpace = "mixed";
                s.TextScope   = "mixed";
                s.TchScope    = "mixed";
            }
            return s;
        }

        std::string MakeUniqueDiffTempPath(const std::string& outDbPath)
        {
            static std::atomic<uint64_t> s_seq{ 0 };
#ifdef _WIN32
            const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
            const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
            const uint64_t seq = ++s_seq;
            return outDbPath + ".tmp." + std::to_string(pid) + "."
                 + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                 + "." + std::to_string(seq);
        }
    }

    void RunTrack3TchDiff(const CadSemanticSnapshot& snapA, const std::string& spaceA, const std::string& layoutA,
                          const CadSemanticSnapshot& snapB, const std::string& spaceB, const std::string& layoutB,
                          const std::array<double, 4>& cropInA,
                          double dx, double dy,
                          double tolMm,
                          const std::vector<DiffRegionItem>& regions,
                          DiffReport& report)
    {
        RunTrack3TchDiffImpl(snapA, spaceA, layoutA, snapB, spaceB, layoutB, cropInA, dx, dy, tolMm, regions, report);
    }

    bool WriteDiffSqlite(const std::string& outDbPath, const DiffReport& report, std::string* error)
    {
        const std::filesystem::path destFs = MiniCAD::Utf8ToPath(outDbPath);
        std::error_code ec;
        if (destFs.has_parent_path())
            std::filesystem::create_directories(destFs.parent_path(), ec);

        const std::string tempPath = MakeUniqueDiffTempPath(outDbPath);
        std::filesystem::remove(MiniCAD::Utf8ToPath(tempPath), ec);

        // RAII 声明顺序：tempGuard -> dbGuard -> txGuard -> stmt，逆序析构自动 finalize/rollback/close/unlink（CAD-20）
        ScopedTempFile tempGuard(tempPath);
        ScopedSqliteDb dbGuard;
        if (!dbGuard.Open(tempPath, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, error))
            return false;

        sqlite3* db = dbGuard.Get();
        if (!ExecSqliteChecked(db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=OFF; PRAGMA temp_store=MEMORY;", error))
            return false;
        if (!ExecSqliteChecked(db, kDiffSchemaSql, error))
            return false;

        {
            ScopedSqliteTransaction tx(db);
            if (!tx.Begin("BEGIN IMMEDIATE;", error))
                return false;

            // 1. diff_meta
            {
                ScopedSqliteStmt st;
                if (!st.Prepare(db, "INSERT INTO diff_meta(key,value) VALUES(?1,?2);", error))
                    return false;
                auto putMeta = [&](const char* k, const std::string& v) -> bool {
                    return st.BindTextStatic(1, k, error)
                        && st.BindText(2, v, error)
                        && st.StepDone(error);
                };
                const CoverageScopes cov = ResolveCoverageScopes(report);
                if (!putMeta("schema_version", report.SchemaVersion)
                    || !putMeta("source_a", report.SideA.Source)
                    || !putMeta("source_b", report.SideB.Source)
                    || !putMeta("layout_a", report.SideA.Layout)
                    || !putMeta("layout_b", report.SideB.Layout)
                    || !putMeta("layout_space", cov.LayoutSpace)
                    || !putMeta("layout_space_a", cov.LayoutSpaceA)
                    || !putMeta("layout_space_b", cov.LayoutSpaceB)
                    || !putMeta("offset_dx", JsonDouble(report.OffsetBToA[0], 4))
                    || !putMeta("offset_dy", JsonDouble(report.OffsetBToA[1], 4))
                    || !putMeta("alignment_mode", report.Alignment.Mode)
                    || !putMeta("alignment_source", report.Alignment.Source)
                    || !putMeta("alignment_confidence", report.Alignment.Confidence)
                    || !putMeta("mm_per_px", JsonDouble(report.MmPerPx, 4))
                    || !putMeta("tolerance_mm", JsonDouble(report.ToleranceMm, 4))
                    || !putMeta("tolerance_px", std::to_string(report.TolerancePx))
                    || !putMeta("effective_tolerance_mm", JsonDouble(report.EffectiveToleranceMm, 4))
                    || !putMeta("a_only_pixels", std::to_string(report.AOnlyPixels))
                    || !putMeta("b_only_pixels", std::to_string(report.BOnlyPixels))
                    || !putMeta("overlap_pixels", std::to_string(report.OverlapPixels))
                    || !putMeta("tch_available", report.TchAvailable ? "1" : "0")
                    || !putMeta("text_scope", cov.TextScope)
                    || !putMeta("text_scope_a", cov.TextScopeA)
                    || !putMeta("text_scope_b", cov.TextScopeB)
                    || !putMeta("tch_scope", cov.TchScope)
                    || !putMeta("tch_scope_a", cov.TchScopeA)
                    || !putMeta("tch_scope_b", cov.TchScopeB)
                    || !putMeta("viewport_model_semantics_included", report.PaperViewportSemanticsExcluded ? "0" : "1")
                    || !putMeta("coverage_note", report.CoverageNote))
                {
                    return false;
                }
            }

            // 2. diff_regions + diff_regions_rtree
            {
                ScopedSqliteStmt stR;
                ScopedSqliteStmt stRt;
                if (!stR.Prepare(db,
                        "INSERT INTO diff_regions(id,minx,miny,maxx,maxy,px_x0,px_y0,px_x1,px_y1,"
                        "a_only_pixels,b_only_pixels,dominant_side,sub_boxes_json) "
                        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);", error)
                    || !stRt.Prepare(db,
                        "INSERT INTO diff_regions_rtree(id,minx,maxx,miny,maxy) VALUES(?1,?2,?3,?4,?5);", error))
                {
                    return false;
                }

                for (const auto& r : report.Regions)
                {
                    JsonWriter subJ(false);
                    subJ.BeginArray();
                    for (const auto& sb : r.SubBoxesDrawing)
                    {
                        subJ.BeginArray();
                        for (double v : sb) subJ.Double(v, 2);
                        subJ.EndArray();
                    }
                    subJ.EndArray();
                    const std::string subJson = subJ.Take();

                    if (!stR.BindInt(1, r.Id, error)
                        || !stR.BindDouble(2, r.BboxDrawing[0], error)
                        || !stR.BindDouble(3, r.BboxDrawing[1], error)
                        || !stR.BindDouble(4, r.BboxDrawing[2], error)
                        || !stR.BindDouble(5, r.BboxDrawing[3], error)
                        || !stR.BindInt(6, r.BboxPx[0], error)
                        || !stR.BindInt(7, r.BboxPx[1], error)
                        || !stR.BindInt(8, r.BboxPx[2], error)
                        || !stR.BindInt(9, r.BboxPx[3], error)
                        || !stR.BindInt64(10, r.AOnlyPixels, error)
                        || !stR.BindInt64(11, r.BOnlyPixels, error)
                        || !stR.BindText(12, r.DominantSide, error)
                        || !stR.BindText(13, subJson, error)
                        || !stR.StepDone(error))
                    {
                        return false;
                    }

                    if (!stRt.BindInt(1, r.Id, error)
                        || !stRt.BindDouble(2, r.BboxDrawing[0], error)
                        || !stRt.BindDouble(3, r.BboxDrawing[2], error)
                        || !stRt.BindDouble(4, r.BboxDrawing[1], error)
                        || !stRt.BindDouble(5, r.BboxDrawing[3], error)
                        || !stRt.StepDone(error))
                    {
                        return false;
                    }
                }
            }

            // 3. diff_texts
            {
                ScopedSqliteStmt st;
                if (!st.Prepare(db,
                        "INSERT INTO diff_texts(id,change_type,source_kind,tag,x,y,a_text,b_text,"
                        "a_layer,b_layer,a_owner_handle,b_owner_handle,dist_mm,region_id) "
                        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14);", error))
                {
                    return false;
                }
                int id = 1;
                for (const auto& t : report.TextChanges)
                {
                    const bool distOk = t.HasDistMm ? st.BindDouble(13, t.DistMm, error) : st.BindNull(13, error);
                    const bool regOk  = (t.RegionId > 0) ? st.BindInt(14, t.RegionId, error) : st.BindNull(14, error);
                    if (!st.BindInt(1, id++, error)
                        || !st.BindText(2, t.ChangeType, error)
                        || !st.BindText(3, t.SourceKind, error)
                        || !st.BindOptText(4, t.Tag, error)
                        || !st.BindDouble(5, t.X, error)
                        || !st.BindDouble(6, t.Y, error)
                        || !st.BindOptText(7, t.AText, error)
                        || !st.BindOptText(8, t.BText, error)
                        || !st.BindOptText(9, t.ALayer, error)
                        || !st.BindOptText(10, t.BLayer, error)
                        || !st.BindOptText(11, t.AOwnerHandle, error)
                        || !st.BindOptText(12, t.BOwnerHandle, error)
                        || !distOk
                        || !regOk
                        || !st.StepDone(error))
                    {
                        return false;
                    }
                }
            }

            // 4. diff_tch_changes
            {
                ScopedSqliteStmt st;
                if (!st.Prepare(db,
                        "INSERT INTO diff_tch_changes(id,category,change_type,handle_a,handle_b,label,"
                        "x,y,minx,miny,maxx,maxy,summary,before_json,after_json,region_id) "
                        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);", error))
                {
                    return false;
                }
                int id = 1;
                for (const auto& c : report.TchChanges)
                {
                    const bool regOk = (c.RegionId > 0) ? st.BindInt(16, c.RegionId, error) : st.BindNull(16, error);
                    if (!st.BindInt(1, id++, error)
                        || !st.BindText(2, c.Category, error)
                        || !st.BindText(3, c.ChangeType, error)
                        || !st.BindOptText(4, c.HandleA, error)
                        || !st.BindOptText(5, c.HandleB, error)
                        || !st.BindOptText(6, c.Label, error)
                        || !st.BindDouble(7, c.X, error)
                        || !st.BindDouble(8, c.Y, error)
                        || !st.BindDouble(9, c.BboxDrawing[0], error)
                        || !st.BindDouble(10, c.BboxDrawing[1], error)
                        || !st.BindDouble(11, c.BboxDrawing[2], error)
                        || !st.BindDouble(12, c.BboxDrawing[3], error)
                        || !st.BindText(13, c.Summary, error)
                        || !st.BindOptText(14, c.BeforeJson, error)
                        || !st.BindOptText(15, c.AfterJson, error)
                        || !regOk
                        || !st.StepDone(error))
                    {
                        return false;
                    }
                }
            }

            if (!tx.Commit(error))
                return false;
        }

        if (!dbGuard.Close(error))
            return false;

        if (!AtomicReplaceFile(tempPath, outDbPath, error))
            return false;

        tempGuard.Release();
        return true;
    }

    bool RunDiffCommand(const DiffOptions& opts,
                        DiffReport& report,
                        std::string* errorCode,
                        std::string* errorMsg,
                        bool* isUsageError)
    {
        const auto t0 = std::chrono::steady_clock::now();
        report = {};
        if (isUsageError) *isUsageError = false;

        auto fail = [&](const char* code, const std::string& msg, bool usage) -> bool {
            if (errorCode) *errorCode = code;
            if (errorMsg)  *errorMsg  = msg;
            if (isUsageError) *isUsageError = usage;
            return false;
        };

        // 提前推导全部输出路径（含默认派生 JSON）并检查输入/输出及多输出冲突（CAD-22）
        std::string effectiveJsonPath = opts.OutJsonPath;
        if (effectiveJsonPath.empty() && !opts.OutJsonExplicit && !opts.OutPngPath.empty())
            effectiveJsonPath = DeriveDefaultJsonPathForPng(opts.OutPngPath);

        {
            std::string conflictCode, conflictMsg;
            if (!ValidateIoPathConflicts(
                    { { "A 侧图纸", opts.InputPathA }, { "B 侧图纸", opts.InputPathB } },
                    { { "输出 PNG", opts.OutPngPath }, { "输出 JSON", effectiveJsonPath }, { "输出 SQLite", opts.OutDbPath } },
                    &conflictCode, &conflictMsg))
            {
                return fail(conflictCode.c_str(), conflictMsg, true);
            }
        }

        // 提前校验全部正则与显式字体目录（CAD-34）
        std::string err;
        if (!ValidateRegexPattern(opts.IncludeLayersRegex, &err))
            return fail("INVALID_REGEX", err, true);
        if (!ValidateRegexPattern(opts.ExcludeLayersRegex, &err))
            return fail("INVALID_REGEX", err, true);
        if (!ValidateRegexPattern(opts.AlignLayersA, &err))
            return fail("INVALID_REGEX", err, true);
        if (!ValidateRegexPattern(opts.AlignLayersB, &err))
            return fail("INVALID_REGEX", err, true);

        FontSystem fontSystem;
        FontDirResolution fontRes;
        if (!InitHeadlessFontSystem(fontSystem, opts.FontsDir, opts.FontsDirExplicit, &fontRes, &err))
            return fail("INVALID_FONTS_DIR", err, true);
        if (fontRes.UsedFallback)
        {
            report.Warnings.push_back(
                "未自动探测到内置字体目录，已回退至系统字体；如需项目专用 SHX 字体请显式传入 --fonts-dir");
        }

        Scene sceneA, sceneB;
        CadSemanticSnapshot snapA, snapB;
        if (!LoadCadFileWithSemantics(opts.InputPathA, sceneA, snapA, &err))
            return fail("LOAD_FAILED", err.empty() ? ("无法加载 A 侧图纸：" + opts.InputPathA) : err, false);
        if (!LoadCadFileWithSemantics(opts.InputPathB, sceneB, snapB, &err))
            return fail("LOAD_FAILED", err.empty() ? ("无法加载 B 侧图纸：" + opts.InputPathB) : err, false);

        const int layoutIdxA = ResolveLayoutIndex(sceneA, opts.LayoutName, &err);
        if (layoutIdxA < 0)
            return fail("LAYOUT_NOT_FOUND", err, true);
        const int layoutIdxB = ResolveCompareLayoutIndex(sceneA, layoutIdxA, sceneB, opts.LayoutNameB, &err);
        if (layoutIdxB < 0)
            return fail("LAYOUT_NOT_FOUND", err, true);

        const DrawingLayout& layA = sceneA.Layouts()[static_cast<size_t>(layoutIdxA)];
        const DrawingLayout& layB = sceneB.Layouts()[static_cast<size_t>(layoutIdxB)];
        const std::string spaceA  = layA.Model ? "model" : "paper";
        const std::string spaceB  = layB.Model ? "model" : "paper";
        const std::string layNameA = layA.Model ? "Model" : layA.Name;
        const std::string layNameB = layB.Model ? "Model" : layB.Name;

        // 纸张空间（布局）未显式传尺度参数时，自动切换为纸张毫米默认值，避免模型空间 30mm 容差 / 800mm 区域阈值吞没小尺寸变化
        const bool   isPaperSpace       = !layA.Model || !layB.Model;
        const double effectiveMmPerPx     = (isPaperSpace && !opts.MmPerPxExplicit)     ? 1.0 : opts.MmPerPx;
        const double effectiveToleranceMm = (isPaperSpace && !opts.ToleranceMmExplicit) ? 1.0 : opts.ToleranceMm;
        const double effectiveMinRegionMm = (isPaperSpace && !opts.MinRegionMmExplicit) ? 2.0 : opts.MinRegionMm;
        const double effectiveRegionGapMm = (isPaperSpace && !opts.RegionGapMmExplicit) ? 8.0 : opts.RegionGapMm;

        // 先执行对齐估算（显式 --offset 优先于 --auto-align），再计算 A 与变换后 B 的边界并集（CAD-15）
        AlignmentInfo align;
        if (opts.HasOffset)
        {
            align.Mode       = "manual";
            align.Source     = "manual";
            align.Confidence = "manual";
            align.Dx         = opts.OffsetDx;
            align.Dy         = opts.OffsetDy;
        }
        else if (opts.AutoAlign)
        {
            align = ComputeAutoAlignment(sceneA, snapA, layoutIdxA,
                                         sceneB, snapB, layoutIdxB,
                                         opts.HasBbox ? &opts.Bbox : nullptr,
                                         opts, report.Warnings);
        }
        else
        {
            align.Mode       = "none";
            align.Source     = "none";
            align.Confidence = "none";
            align.Dx         = 0.0;
            align.Dy         = 0.0;
        }

        const double dx = align.Dx;
        const double dy = align.Dy;

        std::array<double, 4> cropInA{};
        if (opts.HasBbox)
        {
            cropInA = opts.Bbox;
        }
        else
        {
            if (!ComputeCompareUnionBbox(sceneA, layoutIdxA, sceneB, layoutIdxB, dx, dy, cropInA, /*expandMargin*/ true))
                return fail("EMPTY_LAYOUT", "A/B 两侧目标布局中均没有可比较的可见图元", false);
            const double maxSpan = std::max(cropInA[2] - cropInA[0], cropInA[3] - cropInA[1]);
            if (layA.Model && maxSpan > 200000.0)
            {
                report.Warnings.push_back(
                    "模型空间跨度较大（" + JsonDouble(maxSpan / 1000.0, 1)
                    + "m），可能包含多张图纸；建议指定 --bbox 聚焦单张图面以获得更高精度");
            }
        }

        CanvasLayout canvasA;
        if (!ComputeCanvasLayout(cropInA, effectiveMmPerPx, opts.MaxPx, kHardMaxTotalPx, canvasA, &err))
            return fail("INVALID_CANVAS", err, true);
        if (canvasA.Clamped)
        {
            report.Warnings.push_back(
                "请求分辨率受 --max-px 限制已自动夹紧，实际 mm_per_px=" + JsonDouble(canvasA.MmPerPx, 2));
        }

        CanvasLayout canvasB = canvasA;
        canvasB.Bbox = {
            canvasA.Bbox[0] - dx,
            canvasA.Bbox[1] - dy,
            canvasA.Bbox[2] - dx,
            canvasA.Bbox[3] - dy
        };

        // 轨 1 视觉掩膜过滤规则（默认排除注记，除非 --include-annotations）
        TessellateFilter maskFilter;
        maskFilter.IncludeLayersRegex = opts.IncludeLayersRegex;
        maskFilter.ExcludeLayersRegex = opts.ExcludeLayersRegex;
        maskFilter.ExcludeAnnotations = !opts.IncludeAnnotations;
        maskFilter.ExcludeFrames      = opts.ExcludeFrames;

        TessellatedScene tessA, tessB;
        if (!TessellateSceneLayout(sceneA, layoutIdxA, canvasA.Bbox, canvasA.MmPerPx,
                                   static_cast<double>(canvasA.HeightPx), maskFilter, fontSystem, tessA, &err))
            return fail("TESSELLATE_FAILED", err, false);
        if (!TessellateSceneLayout(sceneB, layoutIdxB, canvasB.Bbox, canvasB.MmPerPx,
                                   static_cast<double>(canvasB.HeightPx), maskFilter, fontSystem, tessB, &err))
            return fail("TESSELLATE_FAILED", err, false);

        std::vector<uint8_t> maskA, maskB;
        const uint64_t inkA = RasterizeSceneMask(tessA, canvasA, 1.0f, maskA);
        const uint64_t inkB = RasterizeSceneMask(tessB, canvasB, 1.0f, maskB);

        // CAD-17：支持 --tolerance-mm 0 对应真实 0px 膨胀半径（不再强行 std::max(1, ...)），
        // 正容差按向上取整像素并换算真实生效毫米容差（切比雪夫方形核）。
        int tolerancePx = 0;
        if (effectiveToleranceMm > 0.0)
        {
            const double rawPx = effectiveToleranceMm / canvasA.MmPerPx;
            tolerancePx = std::max(1, static_cast<int>(std::ceil(rawPx - 1e-9)));
        }
        const double appliedToleranceMm = static_cast<double>(tolerancePx) * canvasA.MmPerPx;

        std::vector<uint8_t> dilA, dilB;
        DilateMaskSeparable(maskA, canvasA.WidthPx, canvasA.HeightPx, tolerancePx, dilA);
        DilateMaskSeparable(maskB, canvasA.WidthPx, canvasA.HeightPx, tolerancePx, dilB);

        const size_t totalPx = static_cast<size_t>(canvasA.WidthPx) * static_cast<size_t>(canvasA.HeightPx);
        std::vector<uint8_t> aOnly(totalPx, 0u);
        std::vector<uint8_t> bOnly(totalPx, 0u);
        uint64_t aOnlyCnt = 0, bOnlyCnt = 0, bothCnt = 0, unionCnt = 0;

        for (size_t i = 0; i < totalPx; ++i)
        {
            const uint8_t ma = maskA[i];
            const uint8_t mb = maskB[i];
            if (ma && !dilB[i]) { aOnly[i] = 1u; ++aOnlyCnt; }
            if (mb && !dilA[i]) { bOnly[i] = 1u; ++bOnlyCnt; }
            if (ma && mb)       { ++bothCnt; }
            if (ma || mb)       { ++unionCnt; }
        }

        // 填充报告基本字段
        report.CropInASpace                   = cropInA;
        report.OffsetBToA                     = { dx, dy };
        report.Alignment                      = align;
        report.MmPerPx                        = canvasA.MmPerPx;
        report.Clamped                        = canvasA.Clamped;
        report.ToleranceMm                    = effectiveToleranceMm;
        report.EffectiveToleranceMm           = appliedToleranceMm;
        report.TolerancePx                    = tolerancePx;
        report.WidthPx                        = canvasA.WidthPx;
        report.HeightPx                       = canvasA.HeightPx;
        report.AnnotationsIncluded            = opts.IncludeAnnotations;
        report.IncludeLayers                  = opts.IncludeLayersRegex;
        report.ExtraExcludeLayers             = opts.ExcludeLayersRegex;
        report.ExcludeFrames                  = opts.ExcludeFrames;
        report.FramesExcludedA                = tessA.Stats.FramesExcluded;
        report.FramesExcludedB                = tessB.Stats.FramesExcluded;
        report.FontsDir                       = fontRes.ResolvedDir;
        report.FontsFallback                  = fontRes.UsedFallback;
        report.IsPaperSpaceLayout             = isPaperSpace;
        report.PaperViewportCountA            = CountModelViewportsInLayout(sceneA, layoutIdxA);
        report.PaperViewportCountB            = CountModelViewportsInLayout(sceneB, layoutIdxB);
        report.PaperViewportSemanticsExcluded = isPaperSpace;

        report.SideA.Source            = opts.InputPathA;
        report.SideA.Layout            = layNameA;
        report.SideA.EntitiesRendered  = tessA.Stats.EntitiesRendered;
        report.SideA.EntitiesFiltered  = tessA.Stats.EntitiesFiltered;
        report.SideA.InkPixels         = inkA;
        report.SideA.IsNativeTianzheng = snapA.TchDecodedCount > 0;
        report.SideA.UndecodedProxies  = CountDiagnosticsKind(snapA, "undecoded_proxy");
        report.SideA.UnresolvedXrefs   = CountDiagnosticsKind(snapA, "unresolved_xref");

        report.SideB.Source            = opts.InputPathB;
        report.SideB.Layout            = layNameB;
        report.SideB.EntitiesRendered  = tessB.Stats.EntitiesRendered;
        report.SideB.EntitiesFiltered  = tessB.Stats.EntitiesFiltered;
        report.SideB.InkPixels         = inkB;
        report.SideB.IsNativeTianzheng = snapB.TchDecodedCount > 0;
        report.SideB.UndecodedProxies  = CountDiagnosticsKind(snapB, "undecoded_proxy");
        report.SideB.UnresolvedXrefs   = CountDiagnosticsKind(snapB, "unresolved_xref");

        report.AOnlyPixels     = aOnlyCnt;
        report.BOnlyPixels     = bOnlyCnt;
        report.OverlapPixels   = bothCnt;
        report.InkUnionPixels  = unionCnt;
        report.AOnlyRatio      = unionCnt > 0 ? static_cast<double>(aOnlyCnt) / static_cast<double>(unionCnt) : 0.0;
        report.BOnlyRatio      = unionCnt > 0 ? static_cast<double>(bOnlyCnt) / static_cast<double>(unionCnt) : 0.0;
        report.InkBalanceRatio = static_cast<double>(inkA) / static_cast<double>(std::max<uint64_t>(inkB, 1u));

        report.MinRegionMm = effectiveMinRegionMm;
        report.RegionGapMm = effectiveRegionGapMm;
        report.MaxRegionMm = opts.MaxRegionMm;
        report.Regions     = ExtractDiffRegions(aOnly, bOnly, canvasA,
                                                effectiveMinRegionMm, effectiveRegionGapMm, opts.MaxRegionMm);

        // 轨 2：独立文字与属性比对（不受轨 1 排除注记影响）
        report.TextMatchTolMm = opts.TextMatchTolMm;
        RunTrack2TextDiff(sceneA, snapA, spaceA, layNameA,
                          sceneB, snapB, spaceB, layNameB,
                          cropInA, dx, dy, opts.TextMatchTolMm, report.Regions, report);

        // 轨 3：原生天正构件语义比对
        report.TchMatchTolMm = opts.TchMatchTolMm;
        RunTrack3TchDiff(snapA, spaceA, layNameA,
                         snapB, spaceB, layNameB,
                         cropInA, dx, dy, opts.TchMatchTolMm, report.Regions, report);

        // CAD-16 / CAD-R2-14：区分双侧纸张与模型-纸张跨空间比较的语义轨覆盖边界，逐侧如实披露
        std::string paperBoundaryNote;
        if (isPaperSpace)
        {
            const bool paperA = !layA.Model;
            const bool paperB = !layB.Model;
            if (paperA && paperB)
            {
                if (report.PaperViewportCountA > 0 || report.PaperViewportCountB > 0)
                {
                    report.Warnings.push_back(
                        "纸张布局覆盖边界：检测到浮动视口（A=" + std::to_string(report.PaperViewportCountA)
                        + "，B=" + std::to_string(report.PaperViewportCountB)
                        + "），轨 1 视觉像素已包含视口内模型几何，但轨 2（text_diff）与轨 3（tch_diff）仅覆盖纸面空间实体、未采集浮动视口内模型对象；如需比对模型文字或天正语义请使用 --layout Model");
                }
                else
                {
                    report.Warnings.push_back(
                        "纸张布局覆盖边界：轨 2（text_diff）与轨 3（tch_diff）仅覆盖纸面空间实体，未采集浮动视口内模型对象");
                }
                paperBoundaryNote = "纸张布局模式下 text_diff 与 tch_diff 语义轨仅覆盖纸面空间实体，未采集浮动视口内模型对象；";
            }
            else
            {
                const std::string scopeAZh = paperA ? "纸面空间实体（未采集浮动视口内模型对象）" : "模型空间语义对象";
                const std::string scopeBZh = paperB ? "纸面空间实体（未采集浮动视口内模型对象）" : "模型空间语义对象";
                const int paperVpCount = paperA ? report.PaperViewportCountA : report.PaperViewportCountB;
                const char* paperSide = paperA ? "A" : "B";
                if (paperVpCount > 0)
                {
                    report.Warnings.push_back(
                        std::string("跨空间比对覆盖边界：") + paperSide + " 侧纸张布局检测到浮动视口（"
                        + std::to_string(paperVpCount) + " 个），轨 1 视觉像素已包含视口内模型几何；轨 2（text_diff）与轨 3（tch_diff）中 A 侧覆盖"
                        + scopeAZh + "，B 侧覆盖" + scopeBZh + "；如需双侧比对模型文字或天正语义请双侧均使用模型空间");
                }
                else
                {
                    report.Warnings.push_back(
                        "跨空间比对覆盖边界：轨 2（text_diff）与轨 3（tch_diff）中 A 侧覆盖"
                        + scopeAZh + "，B 侧覆盖" + scopeBZh);
                }
                paperBoundaryNote = "跨空间比对模式下 text_diff 与 tch_diff 语义轨 A 侧覆盖"
                                  + scopeAZh + "、B 侧覆盖" + scopeBZh + "；";
            }
        }

        report.CoverageNote = "在已覆盖范围与实际生效容差 ±" + JsonDouble(appliedToleranceMm, 2)
                            + "mm（" + std::to_string(tolerancePx) + "px 切比雪夫膨胀，请求容差 "
                            + JsonDouble(effectiveToleranceMm, 2) + "mm）内检出差异；"
                            + paperBoundaryNote
                            + "未纳入比较项见 annotations_included、include_layers、frames_excluded 及 undecoded_proxies / unresolved_xrefs。";

        // 输出三态差异叠加 PNG
        if (!opts.OutPngPath.empty())
        {
            RgbaImage img;
            CompositeDiffOverlay(maskA, maskB, aOnly, bOnly, canvasA.WidthPx, canvasA.HeightPx, img);
            if (!opts.NoRegionBoxes)
            {
                for (const auto& r : report.Regions)
                {
                    DrawRegionBoxWithLabel(img,
                                           r.BboxPx[0], r.BboxPx[1], r.BboxPx[2], r.BboxPx[3],
                                           r.Id, 0xFFu, 0x8Cu, 0x00u, 3, 5);
                }
            }
            if (!WritePngFile(opts.OutPngPath, img, &err))
                return fail("WRITE_PNG_FAILED", err, false);
            report.PngPath = opts.OutPngPath;
        }

        if (!opts.OutDbPath.empty())
            report.DbPath = opts.OutDbPath;
        if (!effectiveJsonPath.empty())
            report.JsonPath = effectiveJsonPath;

        const auto t1 = std::chrono::steady_clock::now();
        report.ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        // 写比对 SQLite 库
        if (!opts.OutDbPath.empty())
        {
            if (!WriteDiffSqlite(opts.OutDbPath, report, &err))
                return fail("WRITE_DB_FAILED", err, false);
        }

        // 写完整 JSON 报告文件
        if (!effectiveJsonPath.empty())
        {
            const std::string fullJson = FormatDiffReportJson(report, /*compactSummary*/ false, /*pretty*/ true);
            if (!WriteUtf8TextFile(effectiveJsonPath, fullJson + "\n", &err))
                return fail("WRITE_JSON_FAILED", err, false);
        }

        return true;
    }

    std::string FormatDiffReportJson(const DiffReport& r, bool compactSummary, bool pretty)
    {
        JsonWriter j(pretty);
        j.BeginObject();
        j.KeyBool("ok", true);
        j.KeyString("tool", "SaDwgTool.diff");
        j.KeyString("schema_version", r.SchemaVersion);

        j.Key("crop_in_a_space");
        j.BeginArray();
        for (double v : r.CropInASpace) j.Double(v, 2);
        j.EndArray();

        j.Key("offset_b_to_a");
        j.BeginArray();
        j.Double(r.OffsetBToA[0], 4);
        j.Double(r.OffsetBToA[1], 4);
        j.EndArray();

        j.Key("alignment");
        j.BeginObject();
        j.KeyString("mode", r.Alignment.Mode);
        j.KeyString("source", r.Alignment.Source);
        j.KeyString("confidence", r.Alignment.Confidence);
        j.KeyDouble("dx", r.Alignment.Dx, 4);
        j.KeyDouble("dy", r.Alignment.Dy, 4);
        j.KeyInt("support_x", r.Alignment.SupportX);
        j.KeyInt("support_y", r.Alignment.SupportY);
        j.Key("candidates_x");
        j.BeginArray();
        for (const auto& c : r.Alignment.CandidatesX)
        {
            j.BeginObject();
            j.KeyDouble("delta", c.Delta, 4);
            j.KeyInt("votes", c.Votes);
            j.EndObject();
        }
        j.EndArray();
        j.Key("candidates_y");
        j.BeginArray();
        for (const auto& c : r.Alignment.CandidatesY)
        {
            j.BeginObject();
            j.KeyDouble("delta", c.Delta, 4);
            j.KeyInt("votes", c.Votes);
            j.EndObject();
        }
        j.EndArray();
        j.EndObject();

        j.KeyDouble("mm_per_px", r.MmPerPx, 4);
        j.KeyBool("clamped", r.Clamped);
        j.KeyDouble("tolerance_mm", r.ToleranceMm, 2);
        j.KeyDouble("effective_tolerance_mm", r.EffectiveToleranceMm, 2);
        j.KeyInt("tolerance_px", r.TolerancePx);
        j.KeyOptString("fonts_dir", r.FontsDir);
        j.KeyBool("fonts_fallback", r.FontsFallback);

        j.Key("canvas_px");
        j.BeginArray();
        j.Int(r.WidthPx);
        j.Int(r.HeightPx);
        j.EndArray();

        j.KeyBool("annotations_included", r.AnnotationsIncluded);
        j.KeyOptString("include_layers", r.IncludeLayers);
        j.KeyOptString("extra_exclude_layers", r.ExtraExcludeLayers);
        j.KeyBool("exclude_frames", r.ExcludeFrames);

        j.Key("frames_excluded");
        j.BeginObject();
        j.KeyBool("enabled", r.ExcludeFrames);
        j.KeyInt("a", r.FramesExcludedA);
        j.KeyInt("b", r.FramesExcludedB);
        j.EndObject();

        auto writeSide = [&](const char* key, const DiffSideSummary& s) {
            j.Key(key);
            j.BeginObject();
            j.KeyString("source", s.Source);
            j.KeyString("layout", s.Layout);
            j.KeyInt("entities_rendered", s.EntitiesRendered);
            j.KeyInt("entities_filtered", s.EntitiesFiltered);
            j.KeyUInt("ink_pixels", s.InkPixels);
            j.KeyBool("is_native_tianzheng", s.IsNativeTianzheng);
            j.KeyInt("undecoded_proxies", s.UndecodedProxies);
            j.KeyInt("unresolved_xrefs", s.UnresolvedXrefs);
            j.EndObject();
        };
        writeSide("side_a", r.SideA);
        writeSide("side_b", r.SideB);

        j.Key("diff");
        j.BeginObject();
        j.KeyUInt("a_only_pixels", r.AOnlyPixels);
        j.KeyUInt("b_only_pixels", r.BOnlyPixels);
        j.KeyUInt("overlap_pixels", r.OverlapPixels);
        j.KeyUInt("ink_union_pixels", r.InkUnionPixels);
        j.KeyDouble("a_only_ratio", r.AOnlyRatio, 6);
        j.KeyDouble("b_only_ratio", r.BOnlyRatio, 6);
        j.KeyDouble("ink_balance_ratio", r.InkBalanceRatio, 4);
        j.EndObject();

        j.Key("regions");
        j.BeginObject();
        j.KeyDouble("min_region_mm", r.MinRegionMm, 2);
        j.KeyDouble("region_gap_mm", r.RegionGapMm, 2);
        j.KeyDouble("max_region_mm", r.MaxRegionMm, 2);
        j.KeyInt("count", static_cast<int64_t>(r.Regions.size()));
        if (!compactSummary)
        {
            j.Key("items");
            j.BeginArray();
            for (const auto& reg : r.Regions)
            {
                j.BeginObject();
                j.KeyInt("id", reg.Id);
                j.Key("bbox_drawing");
                j.BeginArray();
                for (double v : reg.BboxDrawing) j.Double(v, 2);
                j.EndArray();
                j.Key("bbox_px");
                j.BeginArray();
                for (int v : reg.BboxPx) j.Int(v);
                j.EndArray();
                j.KeyInt("a_only_pixels", reg.AOnlyPixels);
                j.KeyInt("b_only_pixels", reg.BOnlyPixels);
                j.KeyString("dominant_side", reg.DominantSide);
                j.Key("sub_boxes_drawing");
                j.BeginArray();
                for (const auto& sb : reg.SubBoxesDrawing)
                {
                    j.BeginArray();
                    for (double v : sb) j.Double(v, 2);
                    j.EndArray();
                }
                j.EndArray();
                j.EndObject();
            }
            j.EndArray();
        }
        j.EndObject();

        j.Key("text_diff");
        j.BeginObject();
        j.KeyDouble("tol_mm", r.TextMatchTolMm, 2);
        j.KeyInt("unchanged_count", r.TextUnchangedCount);
        j.KeyInt("a_only_count", r.TextAOnlyCount);
        j.KeyInt("b_only_count", r.TextBOnlyCount);
        j.KeyInt("modified_count", r.TextModifiedCount);
        if (!compactSummary)
        {
            j.Key("items");
            j.BeginArray();
            for (const auto& t : r.TextChanges)
            {
                j.BeginObject();
                j.KeyString("change_type", t.ChangeType);
                j.KeyString("source_kind", t.SourceKind);
                j.KeyOptString("tag", t.Tag);
                j.KeyDouble("x", t.X, 2);
                j.KeyDouble("y", t.Y, 2);
                j.KeyOptString("a_text", t.AText);
                j.KeyOptString("b_text", t.BText);
                j.KeyOptString("a_layer", t.ALayer);
                j.KeyOptString("b_layer", t.BLayer);
                j.KeyOptString("a_owner_handle", t.AOwnerHandle);
                j.KeyOptString("b_owner_handle", t.BOwnerHandle);
                if (t.HasDistMm) j.KeyDouble("dist_mm", t.DistMm, 2);
                else             j.KeyNull("dist_mm");
                if (t.RegionId > 0) j.KeyInt("region_id", t.RegionId);
                else                j.KeyNull("region_id");
                j.EndObject();
            }
            j.EndArray();
        }
        j.EndObject();

        j.Key("tch_diff");
        j.BeginObject();
        j.KeyBool("available", r.TchAvailable);
        j.KeyOptString("reason", r.TchReason);
        j.KeyDouble("tol_mm", r.TchMatchTolMm, 2);
        j.Key("counts");
        j.BeginObject();
        j.KeyInt("wall", r.TchWallCount);
        j.KeyInt("opening", r.TchOpeningCount);
        j.KeyInt("column", r.TchColumnCount);
        j.KeyInt("space", r.TchSpaceCount);
        j.KeyInt("stair", r.TchStairCount);
        j.EndObject();
        if (!compactSummary)
        {
            j.Key("items");
            j.BeginArray();
            for (const auto& c : r.TchChanges)
            {
                j.BeginObject();
                j.KeyString("category", c.Category);
                j.KeyString("change_type", c.ChangeType);
                j.KeyOptString("handle_a", c.HandleA);
                j.KeyOptString("handle_b", c.HandleB);
                j.KeyOptString("label", c.Label);
                j.KeyDouble("x", c.X, 2);
                j.KeyDouble("y", c.Y, 2);
                j.Key("bbox_drawing");
                j.BeginArray();
                for (double v : c.BboxDrawing) j.Double(v, 2);
                j.EndArray();
                j.KeyString("summary", c.Summary);
                j.Key("before");
                if (c.BeforeJson.empty()) j.Null(); else j.Raw(c.BeforeJson);
                j.Key("after");
                if (c.AfterJson.empty())  j.Null(); else j.Raw(c.AfterJson);
                if (c.RegionId > 0) j.KeyInt("region_id", c.RegionId);
                else                j.KeyNull("region_id");
                j.EndObject();
            }
            j.EndArray();
        }
        j.EndObject();

        j.Key("outputs");
        j.BeginObject();
        j.KeyOptString("png_path", r.PngPath);
        j.KeyOptString("json_path", r.JsonPath);
        j.KeyOptString("db_path", r.DbPath);
        j.EndObject();

        j.Key("warnings");
        j.BeginArray();
        for (const auto& w : r.Warnings) j.String(w);
        j.EndArray();

        const CoverageScopes cov = ResolveCoverageScopes(r);
        j.Key("coverage");
        j.BeginObject();
        j.KeyString("layout_space", cov.LayoutSpace);
        j.KeyString("layout_space_a", cov.LayoutSpaceA);
        j.KeyString("layout_space_b", cov.LayoutSpaceB);
        j.KeyBool("visual_includes_viewports", true);
        j.KeyString("text_scope", cov.TextScope);
        j.KeyString("text_scope_a", cov.TextScopeA);
        j.KeyString("text_scope_b", cov.TextScopeB);
        j.KeyString("tch_scope", cov.TchScope);
        j.KeyString("tch_scope_a", cov.TchScopeA);
        j.KeyString("tch_scope_b", cov.TchScopeB);
        j.KeyInt("paper_viewport_count_a", r.PaperViewportCountA);
        j.KeyInt("paper_viewport_count_b", r.PaperViewportCountB);
        j.KeyBool("viewport_model_semantics_included", !r.PaperViewportSemanticsExcluded);
        j.KeyDouble("effective_tolerance_mm", r.EffectiveToleranceMm, 2);
        j.KeyInt("tolerance_px", r.TolerancePx);
        j.KeyString("note", r.CoverageNote);
        j.EndObject();

        j.KeyString("coverage_note", r.CoverageNote);
        j.KeyInt("elapsed_ms", r.ElapsedMs);
        j.EndObject();
        return j.Take();
    }
}
