#include "SqliteExporter.h"
#include "JsonWriter.h"
#include "PathGuard.h"
#include "SqliteGuard.h"
#include "Core/PathUtils.h"
#include "Core/MTextFormat.h"
#include "Core/Entity/ArcEntity.hpp"
#include "Core/Entity/AttribEntity.hpp"
#include "Core/Entity/BlockEntity.hpp"
#include "Core/Entity/CircleEntity.hpp"
#include "Core/Entity/DimensionEntity.hpp"
#include "Core/Entity/EllipseEntity.hpp"
#include "Core/Entity/Face3DEntity.hpp"
#include "Core/Entity/HatchEntity.hpp"
#include "Core/Entity/ImageEntity.hpp"
#include "Core/Entity/InsertEntity.hpp"
#include "Core/Entity/LeaderEntity.hpp"
#include "Core/Entity/LineEntity.hpp"
#include "Core/Entity/MeshEntity.hpp"
#include "Core/Entity/MLeaderEntity.hpp"
#include "Core/Entity/MLineEntity.hpp"
#include "Core/Entity/MTextEntity.hpp"
#include "Core/Entity/OleFrameEntity.hpp"
#include "Core/Entity/PointEntity.hpp"
#include "Core/Entity/PolylineEntity.hpp"
#include "Core/Entity/RayEntity.hpp"
#include "Core/Entity/RectangleEntity.hpp"
#include "Core/Entity/RegionEntity.hpp"
#include "Core/Entity/ShapeEntity.hpp"
#include "Core/Entity/Solid3DEntity.hpp"
#include "Core/Entity/SolidEntity.hpp"
#include "Core/Entity/SplineEntity.hpp"
#include "Core/Entity/TableEntity.hpp"
#include "Core/Entity/TextEntity.hpp"
#include "Core/Entity/ToleranceEntity.hpp"
#include "Core/Entity/ViewportEntity.hpp"
#include "Core/Entity/WipeoutEntity.hpp"
#include "Core/Entity/XLineEntity.hpp"
#include "Import/CadExchange.h"
#include "sqlite3.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <unordered_set>

namespace MiniCAD::SaTool
{
    namespace
    {
        constexpr double kPi       = 3.14159265358979323846;
        constexpr double kRadToDeg = 180.0 / kPi;
        constexpr int    kMaxPolyJsonVerts = 256;

        bool IsFiniteBox(const AABB& b)
        {
            return std::isfinite(b.Min.x) && std::isfinite(b.Min.y) &&
                   std::isfinite(b.Max.x) && std::isfinite(b.Max.y) &&
                   b.Min.x <= b.Max.x && b.Min.y <= b.Max.y &&
                   std::abs(b.Min.x) < 1e15 && std::abs(b.Max.x) < 1e15 &&
                   std::abs(b.Min.y) < 1e15 && std::abs(b.Max.y) < 1e15;
        }

        bool IsTchInternalBlockName(const std::string& name)
        {
            return name.starts_with("*U_TCH_");
        }

        std::string NormalizePathForCompare(const std::string& s)
        {
            std::string out;
            out.reserve(s.size());
            for (char c : s)
            {
                if (c == '/') c = '\\';
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            while (out.size() > 1 && out.back() == '\\')
                out.pop_back();
            return out;
        }

        std::string CurrentUtcIso8601()
        {
            const auto now = std::chrono::system_clock::now();
            const std::time_t tt = std::chrono::system_clock::to_time_t(now);
            std::tm tmUtc{};
#ifdef _WIN32
            gmtime_s(&tmUtc, &tt);
#else
            gmtime_r(&tt, &tmUtc);
#endif
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                          tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
                          tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);
            return std::string(buf);
        }

        std::string PathToUtf8(const std::filesystem::path& p)
        {
            const std::u8string u8 = p.u8string();
            return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
        }

        bool GetFileStats(const std::string& utf8Path, uint64_t& sizeBytes, int64_t& mtimeMs,
                          std::string* error, bool* mtimeOk = nullptr)
        {
            if (mtimeOk) *mtimeOk = false;
            const std::filesystem::path p = Utf8ToPath(utf8Path);
            std::error_code ec;
            if (!std::filesystem::exists(p, ec) || ec)
            {
                if (error) *error = "源文件不存在或无法访问：" + utf8Path;
                return false;
            }
            if (!std::filesystem::is_regular_file(p, ec) || ec)
            {
                if (error) *error = "源路径不是有效文件：" + utf8Path;
                return false;
            }
            sizeBytes = std::filesystem::file_size(p, ec);
            if (ec)
            {
                if (error) *error = "无法读取源文件大小：" + utf8Path;
                return false;
            }
            const auto ftime = std::filesystem::last_write_time(p, ec);
            if (ec)
            {
                mtimeMs = 0;
                return true;
            }
            const auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
            mtimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(sysTime.time_since_epoch()).count();
            if (mtimeOk) *mtimeOk = true;
            return true;
        }

        // ── 外部参照依赖解析与缓存指纹（CAD-21，与 CadExchange::ResolveXrefPath 同口径）──

        std::vector<std::string> SplitXrefSegments(const std::string& normalizedSlash)
        {
            std::vector<std::string> segs;
            size_t i = 0;
            while (i <= normalizedSlash.size())
            {
                size_t j = normalizedSlash.find('/', i);
                if (j == std::string::npos)
                    j = normalizedSlash.size();
                std::string seg = normalizedSlash.substr(i, j - i);
                if (!seg.empty() && seg != "." && seg != ".." && !(segs.empty() && seg.back() == ':'))
                    segs.push_back(std::move(seg));
                i = j + 1;
            }
            return segs;
        }

        std::filesystem::path ResolveXrefPathOnDisk(const std::filesystem::path& hostDir,
                                                    const std::string& xrefPath)
        {
            namespace fs = std::filesystem;
            if (xrefPath.empty())
                return {};
            std::string normalized = xrefPath;
            for (char& ch : normalized)
                if (ch == '\\') ch = '/';
            const std::vector<std::string> segs = SplitXrefSegments(normalized);
            if (segs.empty())
                return {};

            std::error_code ec;
            auto probe = [&](const fs::path& c) -> fs::path {
                ec.clear();
                if (fs::is_regular_file(c, ec) && !ec)
                    return c.lexically_normal();
                if (!c.has_extension())
                {
                    fs::path d = fs::path(c).concat(".dwg");
                    ec.clear();
                    if (fs::is_regular_file(d, ec) && !ec)
                        return d.lexically_normal();
                }
                return {};
            };

            const fs::path saved = Utf8ToPath(normalized);
            if (saved.is_absolute())
            {
                if (fs::path r = probe(saved); !r.empty())
                    return r;
            }
            else if (!hostDir.empty())
            {
                if (fs::path r = probe(hostDir / saved); !r.empty())
                    return r;
            }

            if (hostDir.empty())
                return {};

            auto tail = [&](size_t k) -> fs::path {
                fs::path t;
                for (size_t i = segs.size() - k; i < segs.size(); ++i)
                    t /= Utf8ToPath(segs[i]);
                return t;
            };
            for (size_t k = segs.size(); k >= 1; --k)
            {
                const fs::path t = tail(k);
                for (fs::path dir = hostDir; !dir.empty(); )
                {
                    if (fs::path r = probe(dir / t); !r.empty())
                        return r;
                    const fs::path up = dir.parent_path();
                    if (k == 1 || up == dir)
                        break;
                    dir = up;
                }
            }
            return {};
        }

        std::string PercentEncodeField(const std::string& s)
        {
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(s.size());
            for (unsigned char c : s)
            {
                if (c == '%' || c == '\t' || c == '\n' || c == '\r')
                {
                    out.push_back('%');
                    out.push_back(kHex[(c >> 4) & 0xFu]);
                    out.push_back(kHex[c & 0xFu]);
                }
                else
                {
                    out.push_back(static_cast<char>(c));
                }
            }
            return out;
        }

        bool PercentDecodeField(const std::string& s, std::string& out)
        {
            out.clear();
            out.reserve(s.size());
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '%')
                {
                    if (i + 2 >= s.size()) return false;
                    const int hi = hexVal(s[i + 1]);
                    const int lo = hexVal(s[i + 2]);
                    if (hi < 0 || lo < 0) return false;
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                }
                else
                {
                    out.push_back(s[i]);
                }
            }
            return true;
        }

        struct XrefDepRecord
        {
            std::string BlockName;
            std::string Status;        // "resolved" | "missing" | "unverifiable"
            std::string ParentDir;
            std::string RawPath;
            std::string ResolvedPath;
            uint64_t    SizeBytes = 0;
            int64_t     MtimeMs   = 0;
        };

        std::vector<XrefDepRecord> CollectXrefDependencies(const Scene& scene, const std::string& sourcePath)
        {
            namespace fs = std::filesystem;
            struct RawXrefBlk
            {
                std::string Name;
                std::string XRefPath;
                uint16_t    Flags      = 0;
                bool        Unresolved = false;
                size_t      PipeDepth  = 0;
            };
            std::vector<RawXrefBlk> blks;
            scene.GetBlockTable().ForEach([&](BlockID, const BlockEntity& blk) {
                if (!blk.IsXRef() || blk.IsXRefUnloaded())
                    return;
                const std::string& name = blk.GetName();
                size_t pipes = 0;
                for (char c : name)
                    if (c == '|') ++pipes;
                // 嵌套层中的叠加型（Overlay）外参不会被宿主加载，跳过
                if (pipes > 0 && (blk.GetFlags() & BlockEntity::XRefOverlay))
                    return;
                blks.push_back({ name, blk.GetXRefPath(), blk.GetFlags(), blk.IsUnresolvedXRef(), pipes });
            });

            std::sort(blks.begin(), blks.end(), [](const RawXrefBlk& a, const RawXrefBlk& b) {
                if (a.PipeDepth != b.PipeDepth) return a.PipeDepth < b.PipeDepth;
                return a.Name < b.Name;
            });

            const fs::path hostDir = sourcePath.empty() ? fs::path() : Utf8ToPath(sourcePath).parent_path();
            std::unordered_map<std::string, fs::path> resolvedDirByBlock;
            std::vector<XrefDepRecord> deps;
            deps.reserve(blks.size());

            for (const auto& b : blks)
            {
                fs::path parentDir = hostDir;
                if (b.PipeDepth > 0)
                {
                    const size_t lastPipe = b.Name.rfind('|');
                    const std::string parentName = b.Name.substr(0, lastPipe);
                    auto it = resolvedDirByBlock.find(parentName);
                    if (it != resolvedDirByBlock.end())
                        parentDir = it->second;
                    else
                        parentDir.clear();
                }

                XrefDepRecord rec;
                rec.BlockName = b.Name;
                rec.ParentDir = parentDir.empty() ? std::string() : PathToUtf8(parentDir);
                rec.RawPath   = b.XRefPath;

                if (b.XRefPath.empty() || parentDir.empty())
                {
                    rec.Status = "unverifiable";
                    deps.push_back(std::move(rec));
                    continue;
                }

                const fs::path found = ResolveXrefPathOnDisk(parentDir, b.XRefPath);
                if (!found.empty())
                {
                    std::error_code ec;
                    const fs::path canon = fs::weakly_canonical(found, ec);
                    const fs::path bestP = (!ec && !canon.empty()) ? canon : found;
                    const std::string utf8Found = PathToUtf8(bestP);
                    uint64_t sz = 0;
                    int64_t mt = 0;
                    bool mtOk = false;
                    if (GetFileStats(utf8Found, sz, mt, nullptr, &mtOk) && mtOk)
                    {
                        rec.Status       = "resolved";
                        rec.ResolvedPath = utf8Found;
                        rec.SizeBytes    = sz;
                        rec.MtimeMs      = mt;
                        resolvedDirByBlock[b.Name] = bestP.parent_path();
                    }
                    else
                    {
                        rec.Status = "unverifiable";
                    }
                }
                else
                {
                    // 若加载期块内已有图元却在磁盘上无法定位源文件，视为不可确认（下次强制重导）
                    rec.Status = b.Unresolved ? "missing" : "unverifiable";
                }
                deps.push_back(std::move(rec));
            }
            return deps;
        }

        std::string SerializeXrefDepsV1(const std::vector<XrefDepRecord>& deps)
        {
            std::string out = "v1:" + std::to_string(deps.size());
            for (const auto& d : deps)
            {
                out.push_back('\n');
                out += PercentEncodeField(d.Status);       out.push_back('\t');
                out += PercentEncodeField(d.BlockName);    out.push_back('\t');
                out += PercentEncodeField(d.ParentDir);    out.push_back('\t');
                out += PercentEncodeField(d.RawPath);      out.push_back('\t');
                out += PercentEncodeField(d.ResolvedPath); out.push_back('\t');
                out += std::to_string(d.SizeBytes);        out.push_back('\t');
                out += std::to_string(d.MtimeMs);
            }
            return out;
        }

        std::string FormatXrefDepsJson(const std::vector<XrefDepRecord>& deps)
        {
            JsonWriter jw(false);
            jw.BeginArray();
            for (const auto& d : deps)
            {
                jw.BeginObject();
                jw.KeyString("block", d.BlockName);
                jw.KeyString("status", d.Status);
                jw.KeyOptString("raw_path", d.RawPath);
                jw.KeyOptString("resolved_path", d.ResolvedPath);
                if (d.Status == "resolved")
                {
                    jw.KeyUInt("size_bytes", d.SizeBytes);
                    jw.KeyInt("mtime_ms", d.MtimeMs);
                }
                jw.EndObject();
            }
            jw.EndArray();
            return jw.Take();
        }

        bool ParseAndVerifyXrefDepsV1(const std::string& encoded)
        {
            if (encoded.rfind("v1:", 0) != 0)
                return false;

            std::vector<std::string> lines;
            size_t pos = 0;
            while (pos <= encoded.size())
            {
                const size_t nl = encoded.find('\n', pos);
                if (nl == std::string::npos)
                {
                    lines.push_back(encoded.substr(pos));
                    break;
                }
                lines.push_back(encoded.substr(pos, nl - pos));
                pos = nl + 1;
            }
            if (lines.empty())
                return false;

            char* endPtr = nullptr;
            const unsigned long expectedCount = std::strtoul(lines[0].c_str() + 3, &endPtr, 10);
            if (!endPtr || *endPtr != '\0' || lines.size() != static_cast<size_t>(expectedCount) + 1)
                return false;

            for (size_t i = 1; i < lines.size(); ++i)
            {
                std::vector<std::string> cols;
                size_t p = 0;
                while (p <= lines[i].size())
                {
                    const size_t tab = lines[i].find('\t', p);
                    if (tab == std::string::npos)
                    {
                        cols.push_back(lines[i].substr(p));
                        break;
                    }
                    cols.push_back(lines[i].substr(p, tab - p));
                    p = tab + 1;
                }
                if (cols.size() != 7)
                    return false;

                std::string status, blockName, parentDir, rawPath, resolvedPath;
                if (!PercentDecodeField(cols[0], status)
                    || !PercentDecodeField(cols[1], blockName)
                    || !PercentDecodeField(cols[2], parentDir)
                    || !PercentDecodeField(cols[3], rawPath)
                    || !PercentDecodeField(cols[4], resolvedPath))
                {
                    return false;
                }
                (void)blockName;

                if (status == "unverifiable")
                    return false;

                const std::filesystem::path parentFs = parentDir.empty() ? std::filesystem::path() : Utf8ToPath(parentDir);
                const std::filesystem::path nowFound = ResolveXrefPathOnDisk(parentFs, rawPath);

                if (status == "missing")
                {
                    // 当初缺失的外参如今已补齐 -> 缓存失效
                    if (!nowFound.empty())
                        return false;
                    continue;
                }

                if (status == "resolved")
                {
                    if (nowFound.empty() || resolvedPath.empty())
                        return false;
                    std::error_code ec;
                    const std::filesystem::path canon = std::filesystem::weakly_canonical(nowFound, ec);
                    const std::string nowUtf8 = PathToUtf8((!ec && !canon.empty()) ? canon : nowFound);
                    if (!PathsReferToSameFile(nowUtf8, resolvedPath))
                        return false;

                    char* szEnd = nullptr;
                    const uint64_t expectedSize = std::strtoull(cols[5].c_str(), &szEnd, 10);
                    char* mtEnd = nullptr;
                    const int64_t expectedMtime = std::strtoll(cols[6].c_str(), &mtEnd, 10);
                    if (!szEnd || *szEnd != '\0' || !mtEnd || *mtEnd != '\0')
                        return false;

                    uint64_t curSize = 0;
                    int64_t  curMtime = 0;
                    bool curMtimeOk = false;
                    if (!GetFileStats(nowUtf8, curSize, curMtime, nullptr, &curMtimeOk) || !curMtimeOk)
                        return false;
                    if (curSize != expectedSize || curMtime != expectedMtime)
                        return false;
                    continue;
                }

                return false;
            }
            return true;
        }

        std::string MakeUniqueExportTempPath(const std::string& outDbPath)
        {
            static std::atomic<uint64_t> s_seq{ 0 };
#ifdef _WIN32
            const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
            const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
            const uint64_t seq = ++s_seq;
            return outDbPath + "." + std::to_string(pid) + "."
                 + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                 + "." + std::to_string(seq) + ".tmp";
        }

        std::string TrimUtf8(std::string s)
        {
            while (!s.empty() && (s.back() == '\0' || s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
                s.pop_back();
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
                ++i;
            return i > 0 ? s.substr(i) : s;
        }

        std::string LayerNameById(const Scene& scene, LayerID id)
        {
            if (const Layer* l = scene.GetLayerManager().GetLayer(id))
                return l->GetName();
            return "0";
        }

        std::string LineTypeNameById(const Scene& scene, LineTypeID id)
        {
            if (id == LineTypeTable::ByLayerID) return "ByLayer";
            if (id == LineTypeTable::ByBlockID) return "ByBlock";
            if (const LineTypeRecord* lt = scene.GetLineTypeTable().Find(id))
                return lt->Name;
            return "Continuous";
        }

        std::string StripMTextInlineCodes(const std::string& raw)
        {
            if (raw.empty())
                return {};
            return ParseMTextFormat(raw, 1.0).PlainText();
        }

        std::string LayoutNameForEntity(const Scene& scene, const Entity& e)
        {
            const EntityAttr& attr = e.GetAttr();
            if (!attr.InPaperSpace) return "Model";
            const int idx = scene.FindLayout(attr.LayoutKey);
            if (idx >= 0 && idx < static_cast<int>(scene.Layouts().size()))
                return scene.Layouts()[static_cast<size_t>(idx)].Name;
            return "PaperSpace";
        }

        const char* SceneEntityTypeName(const Entity& e)
        {
            // 注意继承顺序：TableEntity 继承 MTextEntity，WipeoutEntity 继承 PolylineEntity
            if (e.IsKindOf<TableEntity>())     return "TABLE";
            if (e.IsKindOf<WipeoutEntity>())   return "WIPEOUT";
            if (e.IsKindOf<LineEntity>())      return "LINE";
            if (e.IsKindOf<PolylineEntity>())  return "POLYLINE";
            if (e.IsKindOf<ArcEntity>())       return "ARC";
            if (e.IsKindOf<CircleEntity>())    return "CIRCLE";
            if (e.IsKindOf<EllipseEntity>())   return "ELLIPSE";
            if (e.IsKindOf<SplineEntity>())    return "SPLINE";
            if (e.IsKindOf<PointEntity>())     return "POINT";
            if (e.IsKindOf<RayEntity>())       return "RAY";
            if (e.IsKindOf<XLineEntity>())     return "XLINE";
            if (e.IsKindOf<RectangleEntity>()) return "RECTANGLE";
            if (e.IsKindOf<SolidEntity>())     return "SOLID";
            if (e.IsKindOf<HatchEntity>())     return "HATCH";
            if (e.IsKindOf<TextEntity>())      return "TEXT";
            if (e.IsKindOf<MTextEntity>())     return "MTEXT";
            if (e.IsKindOf<DimensionEntity>()) return "DIMENSION";
            if (e.IsKindOf<MLeaderEntity>())   return "MLEADER";
            if (e.IsKindOf<LeaderEntity>())    return "LEADER";
            if (e.IsKindOf<InsertEntity>())    return "INSERT";
            if (e.IsKindOf<ViewportEntity>())  return "VIEWPORT";
            if (e.IsKindOf<ImageEntity>())     return "IMAGE";
            if (e.IsKindOf<OleFrameEntity>())  return "OLEFRAME";
            if (e.IsKindOf<MLineEntity>())     return "MLINE";
            if (e.IsKindOf<ShapeEntity>())     return "SHAPE";
            if (e.IsKindOf<ToleranceEntity>()) return "TOLERANCE";
            if (e.IsKindOf<Face3DEntity>())    return "FACE3D";
            if (e.IsKindOf<MeshEntity>())      return "MESH";
            if (e.IsKindOf<RegionEntity>())    return "REGION";
            if (e.IsKindOf<Solid3DEntity>())   return "SOLID3D";
            return "OTHER";
        }

        double PolylineLengthAndArea(const Polyline& pl, bool& closedOut, double& areaOut, int& bulgeCountOut)
        {
            closedOut = false;
            areaOut = 0.0;
            bulgeCountOut = 0;
            const auto& pts = pl.Points;
            if (pts.size() < 2) return 0.0;
            const double dClose = std::hypot(pts.back().x - pts.front().x, pts.back().y - pts.front().y);
            closedOut = (pts.size() >= 3 && dClose < 1e-4);

            double totalLen = 0.0;
            double area2 = 0.0;
            double bulgeArea = 0.0;
            const size_t segs = pts.size() - 1;
            for (size_t i = 0; i < segs; ++i)
            {
                const auto& a = pts[i];
                const auto& b = pts[i + 1];
                const double chord = std::hypot(b.x - a.x, b.y - a.y);
                const double bulge = (i < pl.Bulges.size()) ? pl.Bulges[i] : 0.0;
                if (std::abs(bulge) > 1e-9)
                {
                    ++bulgeCountOut;
                    const double theta = 4.0 * std::atan(bulge);
                    const double s = std::sin(std::abs(theta) * 0.5);
                    if (s > 1e-9)
                    {
                        const double r = chord / (2.0 * s);
                        totalLen += r * std::abs(theta);
                        const double seg = 0.5 * r * r * (std::abs(theta) - std::sin(std::abs(theta)));
                        bulgeArea += (bulge > 0.0 ? seg : -seg);
                    }
                    else
                    {
                        totalLen += chord;
                    }
                }
                else
                {
                    totalLen += chord;
                }
                if (closedOut)
                    area2 += a.x * b.y - b.x * a.y;
            }
            if (closedOut)
                areaOut = std::abs(0.5 * area2 + bulgeArea);
            return totalLen;
        }

        struct GeomSummary
        {
            std::optional<double> Length;
            std::optional<double> Area;
            std::optional<int>    IsClosed;
            std::optional<double> ConstWidth;
            std::optional<int>    VertexCount;
            std::optional<int>    BulgeCount;
            std::string           GeomJson;
        };

        GeomSummary ExtractGeomSummary(const Entity& e)
        {
            GeomSummary g;
            JsonWriter jw;

            if (e.IsKindOf<LineEntity>())
            {
                const auto& l = static_cast<const LineEntity&>(e).GetLine();
                const double len = std::hypot(l.End.x - l.Start.x, l.End.y - l.Start.y);
                g.Length = len;
                g.VertexCount = 2;
                jw.BeginObject();
                jw.Key("start"); jw.BeginArray(); jw.Double(l.Start.x, 3); jw.Double(l.Start.y, 3); jw.EndArray();
                jw.Key("end");   jw.BeginArray(); jw.Double(l.End.x, 3);   jw.Double(l.End.y, 3);   jw.EndArray();
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            else if (e.IsKindOf<WipeoutEntity>())
            {
                const auto& pl = static_cast<const WipeoutEntity&>(e).GetPolyline();
                bool closed = false; double area = 0.0; int bulges = 0;
                g.Length = PolylineLengthAndArea(pl, closed, area, bulges);
                g.IsClosed = closed ? 1 : 0;
                if (closed) g.Area = area;
                g.VertexCount = static_cast<int>(pl.Points.size());
                g.BulgeCount = bulges;
                jw.BeginObject();
                jw.KeyInt("vertices", static_cast<int64_t>(pl.Points.size()));
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            else if (e.IsKindOf<PolylineEntity>())
            {
                const auto& pe = static_cast<const PolylineEntity&>(e);
                const auto& pl = pe.GetPolyline();
                bool closed = false; double area = 0.0; int bulges = 0;
                g.Length      = PolylineLengthAndArea(pl, closed, area, bulges);
                g.IsClosed    = closed ? 1 : 0;
                if (closed) g.Area = area;
                const double cw = pe.GetWidth() > 0.0 ? pe.GetWidth() : pe.MaxEffectiveWidth();
                if (cw > 0.0) g.ConstWidth = cw;
                g.VertexCount = static_cast<int>(pl.Points.size());
                g.BulgeCount  = bulges;

                const size_t n = pl.Points.size();
                const size_t cap = std::min<size_t>(n, static_cast<size_t>(kMaxPolyJsonVerts));
                jw.BeginObject();
                jw.Key("pts");
                jw.BeginArray();
                for (size_t i = 0; i < cap; ++i)
                {
                    jw.BeginArray();
                    jw.Double(pl.Points[i].x, 3);
                    jw.Double(pl.Points[i].y, 3);
                    const double b = (i < pl.Bulges.size()) ? pl.Bulges[i] : 0.0;
                    if (std::abs(b) > 1e-9) jw.Double(b, 6);
                    jw.EndArray();
                }
                jw.EndArray();
                if (n > cap) jw.KeyBool("truncated", true);
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            else if (e.IsKindOf<CircleEntity>())
            {
                const auto& c = static_cast<const CircleEntity&>(e).GetCircle();
                g.Length = 2.0 * kPi * c.Radius;
                g.Area   = kPi * c.Radius * c.Radius;
                g.IsClosed = 1;
                jw.BeginObject();
                jw.Key("center"); jw.BeginArray(); jw.Double(c.Center.x, 3); jw.Double(c.Center.y, 3); jw.EndArray();
                jw.KeyDouble("radius", c.Radius, 4);
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            else if (e.IsKindOf<ArcEntity>())
            {
                const auto& a = static_cast<const ArcEntity&>(e).GetArc();
                g.Length   = a.Radius * std::abs(a.SweepAngle());
                g.IsClosed = 0;
                jw.BeginObject();
                jw.Key("center"); jw.BeginArray(); jw.Double(a.Center.x, 3); jw.Double(a.Center.y, 3); jw.EndArray();
                jw.KeyDouble("radius", a.Radius, 4);
                jw.KeyDouble("start_deg", a.StartAngle * kRadToDeg, 3);
                jw.KeyDouble("end_deg", a.EndAngle * kRadToDeg, 3);
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            else if (e.IsKindOf<EllipseEntity>())
            {
                const auto& el = static_cast<const EllipseEntity&>(e).GetEllipse();
                g.IsClosed = el.IsFull() ? 1 : 0;
                if (el.IsFull()) g.Area = kPi * std::abs(el.RadiusX * el.RadiusY);
                jw.BeginObject();
                jw.Key("center"); jw.BeginArray(); jw.Double(el.Center.x, 3); jw.Double(el.Center.y, 3); jw.EndArray();
                jw.KeyDouble("rx", el.RadiusX, 4);
                jw.KeyDouble("ry", el.RadiusY, 4);
                jw.KeyDouble("rot_deg", el.Rotation * kRadToDeg, 3);
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            else if (e.IsKindOf<HatchEntity>())
            {
                const auto& h = static_cast<const HatchEntity&>(e);
                g.IsClosed = 1;
                // CAD-23：复用 HatchEntity.hpp 公共奇偶包含净面积算法（外环加、洞口减、洞内岛加回）
                g.Area     = ComputeHatchNetArea(h, 0);
                jw.BeginObject();
                jw.KeyString("pattern", h.GetPattern().Name);
                jw.KeyBool("solid", h.GetPattern().Solid);
                jw.KeyInt("loops", static_cast<int64_t>(h.GetLoops().size()));
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            else if (e.IsKindOf<InsertEntity>())
            {
                const auto& ins = static_cast<const InsertEntity&>(e);
                jw.BeginObject();
                jw.KeyString("block", ins.GetBlockName());
                jw.Key("pos"); jw.BeginArray(); jw.Double(ins.GetPosition().x, 3); jw.Double(ins.GetPosition().y, 3); jw.EndArray();
                jw.KeyDouble("rot_deg", ins.GetRotation() * kRadToDeg, 3);
                jw.KeyBool("xclip", ins.HasClipBoundary());
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            else if (e.IsKindOf<DimensionEntity>())
            {
                const auto& d = static_cast<const DimensionEntity&>(e);
                jw.BeginObject();
                jw.KeyDouble("measurement", d.Measurement(), 4);
                if (!d.GetTextOverride().empty()) jw.KeyString("override", d.GetTextOverride());
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            else if (e.IsKindOf<PointEntity>())
            {
                const auto& p = static_cast<const PointEntity&>(e).GetPoint();
                jw.BeginObject();
                jw.Key("pos"); jw.BeginArray(); jw.Double(p.Position.x, 3); jw.Double(p.Position.y, 3); jw.EndArray();
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            else if (e.IsKindOf<RectangleEntity>())
            {
                const auto& r = static_cast<const RectangleEntity&>(e).GetRectangle();
                const AABB b = e.GetBoundingBox();
                const double w = b.Max.x - b.Min.x, h = b.Max.y - b.Min.y;
                g.Length = 2.0 * (w + h);
                g.Area = w * h;
                g.IsClosed = 1;
                g.VertexCount = 4;
                (void)r;
                jw.BeginObject();
                jw.KeyDouble("w", w, 3); jw.KeyDouble("h", h, 3);
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            else if (e.IsKindOf<TableEntity>())
            {
                const auto& t = static_cast<const TableEntity&>(e);
                jw.BeginObject();
                jw.KeyInt("rows", static_cast<int64_t>(t.RowCount()));
                jw.KeyInt("cols", static_cast<int64_t>(t.ColCount()));
                jw.EndObject();
                g.GeomJson = jw.Take();
            }
            return g;
        }

        std::string FormatDimensionText(const DimensionEntity& d)
        {
            // CAD-19：先识别覆盖文本中是否含 <> 占位符（如 "约<>"、"<>（净宽）" 或含 MText 格式码），
            // 含占位符时用真实测量值替换全部 <> 并保留前后缀；不含占位符的覆盖文本才直接返回。
            const std::string& ov = d.GetTextOverride();
            const std::string strippedOv = ov.empty() ? std::string() : StripMTextInlineCodes(ov);
            const bool hasPlaceholder = strippedOv.find("<>") != std::string::npos;

            if (!ov.empty() && !hasPlaceholder)
                return TrimUtf8(strippedOv);

            char buf[64];
            const double val = d.Measurement();
            if (d.GetType() == DimType::Angular)
                std::snprintf(buf, sizeof(buf), "%.1f°", val * kRadToDeg);
            else if (std::abs(val - std::round(val)) < 1e-3)
                std::snprintf(buf, sizeof(buf), "%.0f", std::round(val));
            else
                std::snprintf(buf, sizeof(buf), "%.2f", val);
            const std::string num(buf);

            if (hasPlaceholder)
            {
                std::string s = strippedOv;
                size_t pos = 0;
                while ((pos = s.find("<>", pos)) != std::string::npos)
                {
                    s.replace(pos, 2, num);
                    pos += num.size();
                }
                return TrimUtf8(s);
            }
            return num;
        }

        constexpr const char* kSchemaDdl = R"SQL(
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE diagnostics (
    id            INTEGER PRIMARY KEY,
    kind          TEXT NOT NULL,
    dxf_name      TEXT NOT NULL,
    reason        TEXT,
    count         INTEGER NOT NULL DEFAULT 1,
    sample_handle TEXT
);

CREATE TABLE layers (
    id                 INTEGER PRIMARY KEY,
    name               TEXT NOT NULL UNIQUE,
    color_aci          INTEGER,
    color_rgb          TEXT NOT NULL,
    is_on              INTEGER NOT NULL,
    is_frozen          INTEGER NOT NULL,
    is_locked          INTEGER NOT NULL,
    is_visible         INTEGER NOT NULL,
    is_plottable       INTEGER NOT NULL,
    linetype           TEXT NOT NULL,
    lineweight_mm      REAL,
    entity_count       INTEGER NOT NULL DEFAULT 0,
    model_entity_count INTEGER NOT NULL DEFAULT 0,
    paper_entity_count INTEGER NOT NULL DEFAULT 0,
    block_entity_count INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE layouts (
    id             INTEGER PRIMARY KEY,
    name           TEXT NOT NULL,
    is_model       INTEGER NOT NULL,
    tab_order      INTEGER NOT NULL,
    owner_key      INTEGER NOT NULL,
    minx           REAL,
    miny           REAL,
    maxx           REAL,
    maxy           REAL,
    view_center_x  REAL,
    view_center_y  REAL,
    view_height    REAL,
    entity_count   INTEGER NOT NULL DEFAULT 0,
    viewport_count INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE viewports (
    id                   INTEGER PRIMARY KEY,
    handle               TEXT,
    layout_name          TEXT NOT NULL,
    layout_key           INTEGER NOT NULL,
    viewport_no          INTEGER NOT NULL,
    layer                TEXT NOT NULL,
    shows_model          INTEGER NOT NULL,
    paper_cx             REAL NOT NULL,
    paper_cy             REAL NOT NULL,
    paper_w              REAL NOT NULL,
    paper_h              REAL NOT NULL,
    paper_minx           REAL NOT NULL,
    paper_miny           REAL NOT NULL,
    paper_maxx           REAL NOT NULL,
    paper_maxy           REAL NOT NULL,
    model_cx             REAL NOT NULL,
    model_cy             REAL NOT NULL,
    model_view_height    REAL NOT NULL,
    twist_deg            REAL NOT NULL,
    model_to_paper_scale REAL NOT NULL,
    model_minx           REAL,
    model_miny           REAL,
    model_maxx           REAL,
    model_maxy           REAL,
    frozen_layers_json   TEXT NOT NULL
);

CREATE TABLE blocks (
    id                INTEGER PRIMARY KEY,
    name              TEXT NOT NULL UNIQUE,
    base_x            REAL NOT NULL DEFAULT 0,
    base_y            REAL NOT NULL DEFAULT 0,
    base_z            REAL NOT NULL DEFAULT 0,
    flags             INTEGER NOT NULL DEFAULT 0,
    is_anonymous      INTEGER NOT NULL DEFAULT 0,
    is_xref           INTEGER NOT NULL DEFAULT 0,
    xref_path         TEXT,
    xref_resolved     INTEGER NOT NULL DEFAULT 0,
    description       TEXT,
    entity_count      INTEGER NOT NULL DEFAULT 0,
    insert_count      INTEGER NOT NULL DEFAULT 0,
    minx              REAL,
    miny              REAL,
    maxx              REAL,
    maxy              REAL,
    layers_json       TEXT NOT NULL,
    entity_types_json TEXT NOT NULL
);

CREATE TABLE block_entities (
    id          INTEGER PRIMARY KEY,
    block_name  TEXT NOT NULL,
    entity_type TEXT NOT NULL,
    layer       TEXT NOT NULL,
    minx        REAL,
    miny        REAL,
    maxx        REAL,
    maxy        REAL,
    length      REAL,
    geom_json   TEXT
);

CREATE TABLE inserts (
    id                  INTEGER PRIMARY KEY,
    entity_id           INTEGER,
    parent_insert_id    INTEGER,
    depth               INTEGER NOT NULL DEFAULT 0,
    handle              TEXT,
    block_name          TEXT NOT NULL,
    block_path          TEXT NOT NULL,
    layer               TEXT NOT NULL,
    space               TEXT NOT NULL,
    layout_name         TEXT NOT NULL,
    x                   REAL NOT NULL,
    y                   REAL NOT NULL,
    z                   REAL NOT NULL,
    scale_x             REAL NOT NULL,
    scale_y             REAL NOT NULL,
    scale_z             REAL NOT NULL,
    rotation_deg        REAL NOT NULL,
    col_count           INTEGER NOT NULL DEFAULT 1,
    row_count           INTEGER NOT NULL DEFAULT 1,
    col_spacing         REAL NOT NULL DEFAULT 0,
    row_spacing         REAL NOT NULL DEFAULT 0,
    has_xclip           INTEGER NOT NULL DEFAULT 0,
    xclip_inverted      INTEGER NOT NULL DEFAULT 0,
    xclip_boundary_json TEXT,
    attrib_count        INTEGER NOT NULL DEFAULT 0,
    minx                REAL,
    miny                REAL,
    maxx                REAL,
    maxy                REAL
);

CREATE TABLE insert_attribs (
    id            INTEGER PRIMARY KEY,
    insert_id     INTEGER NOT NULL,
    insert_handle TEXT,
    block_name    TEXT NOT NULL,
    tag           TEXT NOT NULL,
    value         TEXT NOT NULL,
    layer         TEXT NOT NULL,
    space         TEXT NOT NULL,
    layout_name   TEXT NOT NULL,
    x             REAL NOT NULL,
    y             REAL NOT NULL,
    height        REAL NOT NULL,
    rotation_deg  REAL NOT NULL,
    is_invisible  INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE texts (
    id           INTEGER PRIMARY KEY,
    entity_id    INTEGER,
    owner_handle TEXT,
    source_kind  TEXT NOT NULL,
    content      TEXT NOT NULL,
    raw_content  TEXT,
    tag          TEXT,
    layer        TEXT NOT NULL,
    space        TEXT NOT NULL,
    layout_name  TEXT NOT NULL,
    block_path   TEXT NOT NULL DEFAULT '',
    x            REAL NOT NULL,
    y            REAL NOT NULL,
    height       REAL NOT NULL,
    rotation_deg REAL NOT NULL,
    minx         REAL NOT NULL,
    miny         REAL NOT NULL,
    maxx         REAL NOT NULL,
    maxy         REAL NOT NULL
);

CREATE VIRTUAL TABLE texts_rtree USING rtree(
    id,
    minx, maxx,
    miny, maxy
);

CREATE TABLE entities (
    id            INTEGER PRIMARY KEY,
    handle        TEXT,
    entity_type   TEXT NOT NULL,
    dxf_name      TEXT,
    layer         TEXT NOT NULL,
    space         TEXT NOT NULL,
    layout_name   TEXT NOT NULL,
    layout_key    INTEGER NOT NULL DEFAULT 0,
    block_path    TEXT NOT NULL DEFAULT '',
    color_aci     INTEGER,
    color_rgb     TEXT,
    linetype      TEXT,
    lineweight_mm REAL,
    visible       INTEGER NOT NULL DEFAULT 1,
    minx          REAL,
    miny          REAL,
    maxx          REAL,
    maxy          REAL,
    length        REAL,
    area          REAL,
    is_closed     INTEGER,
    const_width   REAL,
    vertex_count  INTEGER,
    bulge_count   INTEGER,
    geom_json     TEXT
);

CREATE VIRTUAL TABLE entities_rtree USING rtree(
    id,
    minx, maxx,
    miny, maxy
);

CREATE TABLE tch_walls (
    id                          INTEGER PRIMARY KEY,
    handle                      TEXT NOT NULL,
    dxf_name                    TEXT NOT NULL,
    layer                       TEXT NOT NULL,
    space                       TEXT NOT NULL,
    layout_name                 TEXT NOT NULL,
    start_x                     REAL NOT NULL,
    start_y                     REAL NOT NULL,
    start_z                     REAL NOT NULL,
    end_x                       REAL NOT NULL,
    end_y                       REAL NOT NULL,
    end_z                       REAL NOT NULL,
    length                      REAL NOT NULL,
    arc_angle_rad               REAL NOT NULL,
    arc_cx                      REAL,
    arc_cy                      REAL,
    arc_radius                  REAL,
    left_width                  REAL NOT NULL,
    right_width                 REAL NOT NULL,
    total_thickness             REAL NOT NULL,
    height                      REAL,
    material_code               INTEGER NOT NULL,
    material_name               TEXT NOT NULL,
    flags                       INTEGER NOT NULL,
    scale                       REAL NOT NULL,
    opening_count               INTEGER NOT NULL,
    joins_json                  TEXT NOT NULL,
    has_connections             INTEGER NOT NULL,
    connected_wall_handles_json TEXT NOT NULL,
    minx                        REAL NOT NULL,
    miny                        REAL NOT NULL,
    maxx                        REAL NOT NULL,
    maxy                        REAL NOT NULL
);

CREATE TABLE tch_openings (
    id            INTEGER PRIMARY KEY,
    handle        TEXT NOT NULL,
    parent_handle TEXT,
    layer         TEXT NOT NULL,
    label_layer   TEXT,
    space         TEXT NOT NULL,
    layout_name   TEXT NOT NULL,
    kind_code     INTEGER NOT NULL,
    kind_name     TEXT NOT NULL,
    label         TEXT,
    x             REAL NOT NULL,
    y             REAL NOT NULL,
    sill_z        REAL NOT NULL,
    width         REAL NOT NULL,
    height        REAL NOT NULL,
    thickness     REAL NOT NULL,
    angle_rad     REAL NOT NULL,
    angle_deg     REAL NOT NULL,
    mirror        INTEGER NOT NULL,
    mirror_x      INTEGER NOT NULL,
    mirror_y      INTEGER NOT NULL,
    wall_handle   TEXT,
    label_x       REAL,
    label_y       REAL,
    label_height  REAL,
    scale         REAL NOT NULL,
    flags         INTEGER NOT NULL,
    display_flags INTEGER NOT NULL,
    minx          REAL NOT NULL,
    miny          REAL NOT NULL,
    maxx          REAL NOT NULL,
    maxy          REAL NOT NULL
);

CREATE TABLE tch_columns (
    id            INTEGER PRIMARY KEY,
    handle        TEXT NOT NULL,
    layer         TEXT NOT NULL,
    space         TEXT NOT NULL,
    layout_name   TEXT NOT NULL,
    x             REAL NOT NULL,
    y             REAL NOT NULL,
    base_z        REAL NOT NULL,
    height        REAL NOT NULL,
    material_code INTEGER NOT NULL,
    material_name TEXT NOT NULL,
    scale         REAL NOT NULL,
    area_m2       REAL,
    outline_json  TEXT NOT NULL,
    minx          REAL NOT NULL,
    miny          REAL NOT NULL,
    maxx          REAL NOT NULL,
    maxy          REAL NOT NULL
);

CREATE TABLE tch_spaces (
    id           INTEGER PRIMARY KEY,
    handle       TEXT NOT NULL,
    layer        TEXT NOT NULL,
    space        TEXT NOT NULL,
    layout_name  TEXT NOT NULL,
    name         TEXT,
    area_m2      REAL NOT NULL,
    perimeter_mm REAL NOT NULL,
    centroid_x   REAL NOT NULL,
    centroid_y   REAL NOT NULL,
    scale        REAL NOT NULL,
    outline_json TEXT NOT NULL,
    minx         REAL NOT NULL,
    miny         REAL NOT NULL,
    maxx         REAL NOT NULL,
    maxy         REAL NOT NULL
);

CREATE TABLE tch_stairs (
    id              INTEGER PRIMARY KEY,
    handle          TEXT NOT NULL,
    stair_kind      TEXT NOT NULL,
    layer           TEXT NOT NULL,
    space           TEXT NOT NULL,
    layout_name     TEXT NOT NULL,
    x               REAL NOT NULL,
    y               REAL NOT NULL,
    rotation_deg    REAL NOT NULL,
    step_height     REAL,
    tread_width     REAL,
    flight_width    REAL,
    landing_width   REAL,
    total_width     REAL,
    steps1          INTEGER,
    steps2          INTEGER,
    floor_type      INTEGER,
    floor_type_name TEXT,
    mirror          INTEGER,
    scale           REAL,
    extra_json      TEXT,
    minx            REAL NOT NULL,
    miny            REAL NOT NULL,
    maxx            REAL NOT NULL,
    maxy            REAL NOT NULL
);

CREATE TABLE tch_dimensions (
    id                  INTEGER PRIMARY KEY,
    handle              TEXT NOT NULL,
    dxf_name            TEXT NOT NULL,
    layer               TEXT NOT NULL,
    space               TEXT NOT NULL,
    layout_name         TEXT NOT NULL,
    x                   REAL NOT NULL,
    y                   REAL NOT NULL,
    z                   REAL NOT NULL,
    angle_deg           REAL NOT NULL,
    offset_val          REAL NOT NULL,
    is_arc              INTEGER NOT NULL DEFAULT 0,
    arc_radius          REAL,
    measured_radius     REAL,
    span_count          INTEGER NOT NULL,
    total_span          REAL NOT NULL,
    spans_json          TEXT NOT NULL,
    text_overrides_json TEXT,
    flags               INTEGER NOT NULL,
    scale               REAL NOT NULL,
    minx                REAL NOT NULL,
    miny                REAL NOT NULL,
    maxx                REAL NOT NULL,
    maxy                REAL NOT NULL
);

CREATE TABLE tch_axes (
    id                INTEGER PRIMARY KEY,
    group_handle      TEXT NOT NULL,
    axis_index        INTEGER NOT NULL,
    layer             TEXT NOT NULL,
    text_layer        TEXT,
    space             TEXT NOT NULL,
    layout_name       TEXT NOT NULL,
    label             TEXT,
    spacing           REAL NOT NULL,
    offset_from_first REAL NOT NULL,
    x1                REAL NOT NULL,
    y1                REAL NOT NULL,
    x2                REAL NOT NULL,
    y2                REAL NOT NULL,
    orientation       TEXT NOT NULL,
    coord             REAL,
    scale             REAL NOT NULL,
    show_text         INTEGER NOT NULL DEFAULT 1,
    flags             INTEGER NOT NULL DEFAULT 0,
    minx              REAL NOT NULL,
    miny              REAL NOT NULL,
    maxx              REAL NOT NULL,
    maxy              REAL NOT NULL
);

CREATE TABLE tch_sheets (
    id           INTEGER PRIMARY KEY,
    handle       TEXT NOT NULL,
    layer        TEXT NOT NULL,
    space        TEXT NOT NULL,
    layout_name  TEXT NOT NULL,
    title        TEXT,
    row_count    INTEGER NOT NULL,
    col_count    INTEGER NOT NULL,
    x            REAL NOT NULL,
    y            REAL NOT NULL,
    rotation_deg REAL NOT NULL,
    cells_json   TEXT NOT NULL,
    minx         REAL NOT NULL,
    miny         REAL NOT NULL,
    maxx         REAL NOT NULL,
    maxy         REAL NOT NULL
);

CREATE INDEX idx_entities_space_layout_layer ON entities(space, layout_name, layer);
CREATE INDEX idx_entities_type ON entities(entity_type);
CREATE INDEX idx_entities_handle ON entities(handle);
CREATE INDEX idx_texts_content ON texts(content);
CREATE INDEX idx_texts_space_layout_layer ON texts(space, layout_name, layer);
CREATE INDEX idx_texts_source_kind ON texts(source_kind);
CREATE INDEX idx_inserts_block_name ON inserts(block_name);
CREATE INDEX idx_inserts_space_layout ON inserts(space, layout_name);
CREATE INDEX idx_insert_attribs_insert_id ON insert_attribs(insert_id);
CREATE INDEX idx_insert_attribs_tag ON insert_attribs(tag);
CREATE INDEX idx_block_entities_block ON block_entities(block_name);
CREATE INDEX idx_block_entities_layer ON block_entities(layer);
CREATE INDEX idx_tch_walls_handle ON tch_walls(handle);
CREATE INDEX idx_tch_walls_material ON tch_walls(material_code);
CREATE INDEX idx_tch_openings_wall_handle ON tch_openings(wall_handle);
CREATE INDEX idx_tch_openings_label ON tch_openings(label);
CREATE INDEX idx_tch_spaces_name ON tch_spaces(name);
CREATE INDEX idx_tch_axes_orient_coord ON tch_axes(orientation, coord);
CREATE INDEX idx_tch_axes_label ON tch_axes(label);
)SQL";

        bool TryReuseExistingDb(const std::string& dbPath, const std::string& sourcePath,
                                uint64_t sourceSizeBytes, int64_t sourceMtimeMs, bool sourceMtimeOk,
                                bool includeBlockEntities,
                                ExportDbResult& result)
        {
            // 若无法确认宿主图修改时间，则不复用缓存（CAD-21）
            if (!sourceMtimeOk)
                return false;

            const std::filesystem::path p = Utf8ToPath(dbPath);
            std::error_code ec;
            if (!std::filesystem::exists(p, ec) || ec)
                return false;

            ScopedSqliteDb dbGuard;
            if (!dbGuard.Open(dbPath, SQLITE_OPEN_READONLY, nullptr))
                return false;
            sqlite3* db = dbGuard.Get();

            std::map<std::string, std::string> meta;
            {
                ScopedSqliteStmt st;
                if (!st.Prepare(db, "SELECT key, value FROM meta;", nullptr))
                    return false;
                int rc = SQLITE_OK;
                while ((rc = sqlite3_step(st.Get())) == SQLITE_ROW)
                {
                    const auto* k = reinterpret_cast<const char*>(sqlite3_column_text(st.Get(), 0));
                    const auto* v = reinterpret_cast<const char*>(sqlite3_column_text(st.Get(), 1));
                    if (k && v) meta[k] = v;
                }
                if (rc != SQLITE_DONE)
                    return false;
            }

            auto getStr = [&](const char* k) -> std::string {
                auto it = meta.find(k);
                return it != meta.end() ? it->second : std::string();
            };

            const std::string storedSourcePath = getStr("source_path");
            const bool sourcePathMatch =
                NormalizePathForCompare(storedSourcePath) == NormalizePathForCompare(sourcePath)
                || PathsReferToSameFile(storedSourcePath, sourcePath);

            // CAD-21：校验 schema、导出选项（include_block_entities）、宿主文件指纹以及全部外参依赖指纹
            const bool match =
                getStr("schema_version") == "1.0" &&
                getStr("include_block_entities") == (includeBlockEntities ? "1" : "0") &&
                getStr("source_size_bytes") == std::to_string(sourceSizeBytes) &&
                getStr("source_mtime_ms") == std::to_string(sourceMtimeMs) &&
                sourcePathMatch &&
                ParseAndVerifyXrefDepsV1(getStr("xref_deps_v1"));

            if (!match)
                return false;

            auto countTable = [&](const char* tbl, int& outCount) -> bool {
                ScopedSqliteStmt cs;
                const std::string q = std::string("SELECT COUNT(*) FROM ") + tbl + ";";
                if (!cs.Prepare(db, q.c_str(), nullptr))
                    return false;
                if (sqlite3_step(cs.Get()) != SQLITE_ROW)
                    return false;
                outCount = sqlite3_column_int(cs.Get(), 0);
                return true;
            };

            result = {};
            result.Reused            = true;
            result.SchemaVersion     = "1.0";
            result.SourcePath        = sourcePath;
            result.DbPath            = dbPath;
            result.Format            = getStr("format");
            result.DwgVersion        = getStr("dwg_version");
            result.InsunitsCode      = std::atoi(getStr("insunits_code").c_str());
            result.InsunitsName      = getStr("insunits_name");
            result.IsNativeTianzheng = (getStr("is_native_tianzheng") == "1");
            const std::string mnX = getStr("model_minx");
            if (!mnX.empty())
            {
                result.HasModelBbox = true;
                result.ModelBbox = {
                    std::strtod(mnX.c_str(), nullptr),
                    std::strtod(getStr("model_miny").c_str(), nullptr),
                    std::strtod(getStr("model_maxx").c_str(), nullptr),
                    std::strtod(getStr("model_maxy").c_str(), nullptr),
                };
            }
            if (!countTable("layers", result.Counts.Layers)
                || !countTable("layouts", result.Counts.Layouts)
                || !countTable("viewports", result.Counts.Viewports)
                || !countTable("blocks", result.Counts.Blocks)
                || !countTable("block_entities", result.Counts.BlockEntities)
                || !countTable("inserts", result.Counts.Inserts)
                || !countTable("insert_attribs", result.Counts.InsertAttribs)
                || !countTable("texts", result.Counts.Texts)
                || !countTable("entities", result.Counts.Entities)
                || !countTable("tch_walls", result.Counts.TchWalls)
                || !countTable("tch_openings", result.Counts.TchOpenings)
                || !countTable("tch_columns", result.Counts.TchColumns)
                || !countTable("tch_spaces", result.Counts.TchSpaces)
                || !countTable("tch_stairs", result.Counts.TchStairs)
                || !countTable("tch_dimensions", result.Counts.TchDimensions)
                || !countTable("tch_axes", result.Counts.TchAxes)
                || !countTable("tch_sheets", result.Counts.TchSheets))
            {
                return false;
            }
            result.Counts.TchFailed = std::atoi(getStr("tch_failed_count").c_str());
            {
                ScopedSqliteStmt cs;
                if (!cs.Prepare(db, "SELECT COUNT(*) FROM diagnostics WHERE kind='unresolved_xref';", nullptr))
                    return false;
                if (sqlite3_step(cs.Get()) != SQLITE_ROW)
                    return false;
                result.Counts.UnresolvedXrefs = sqlite3_column_int(cs.Get(), 0);
            }
            return true;
        }
    }

    bool AtomicReplaceFile(const std::string& tempUtf8Path, const std::string& destUtf8Path, std::string* error)
    {
        const std::filesystem::path tmpP  = Utf8ToPath(tempUtf8Path);
        const std::filesystem::path destP = Utf8ToPath(destUtf8Path);
        std::error_code ec;
        if (destP.has_parent_path())
            std::filesystem::create_directories(destP.parent_path(), ec);

#ifdef _WIN32
        if (MoveFileExW(tmpP.c_str(), destP.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        const DWORD lastErr = GetLastError();
        if (error) *error = "原子替换目标文件失败（Win32 error " + std::to_string(lastErr) + "）：" + destUtf8Path;
        return false;
#else
        std::filesystem::rename(tmpP, destP, ec);
        if (ec)
        {
            std::filesystem::remove(destP, ec);
            ec.clear();
            std::filesystem::rename(tmpP, destP, ec);
        }
        if (ec)
        {
            if (error) *error = "原子替换目标文件失败：" + ec.message();
            return false;
        }
        return true;
#endif
    }

    void CollectAllSceneTexts(const Scene& scene, const CadSemanticSnapshot& snap,
                              std::vector<ExtractedText>& out,
                              const std::unordered_map<uint64_t, int64_t>* objectToEntityId)
    {
        out.clear();
        std::unordered_set<uint64_t> emittedTchHandles;
        CollectTchReadableTexts(snap, out, emittedTchHandles);

        // 将已采集天正文字回填 entity_id（按 handle 匹配）
        std::unordered_map<uint64_t, int64_t> handleToEntityId;
        if (objectToEntityId)
        {
            for (const auto& [objId, entId] : *objectToEntityId)
                if (auto it = snap.EntityOrigins.find(objId); it != snap.EntityOrigins.end() && it->second.Handle != 0)
                    handleToEntityId[it->second.Handle] = entId;
            for (auto& t : out)
            {
                if (t.EntityId == 0 && !t.OwnerHandle.empty())
                {
                    const uint64_t h = std::strtoull(t.OwnerHandle.c_str(), nullptr, 16);
                    if (auto it = handleToEntityId.find(h); it != handleToEntityId.end())
                        t.EntityId = it->second;
                }
            }
        }

        auto transformBox = [](const AABB& local, const Math::Mat4& m, double& minx, double& miny, double& maxx, double& maxy) {
            if (!IsFiniteBox(local))
            {
                const Math::Point3 p = m.TransformPoint({ 0, 0, 0 });
                minx = maxx = p.x; miny = maxy = p.y;
                return;
            }
            const Math::Point3 cs[4] = {
                { local.Min.x, local.Min.y, local.Min.z }, { local.Max.x, local.Min.y, local.Min.z },
                { local.Min.x, local.Max.y, local.Min.z }, { local.Max.x, local.Max.y, local.Min.z },
            };
            bool first = true;
            for (const auto& c : cs)
            {
                const Math::Point3 w = m.TransformPoint(c);
                if (first) { minx = maxx = w.x; miny = maxy = w.y; first = false; }
                else
                {
                    minx = std::min(minx, w.x); maxx = std::max(maxx, w.x);
                    miny = std::min(miny, w.y); maxy = std::max(maxy, w.y);
                }
            }
        };

        auto matrixScaleAndRot = [](const Math::Mat4& m, double& scaleOut, double& rotRadOut) {
            const Math::Point3 o = m.TransformPoint({ 0, 0, 0 });
            const Math::Point3 x = m.TransformPoint({ 1, 0, 0 });
            const Math::Point3 y = m.TransformPoint({ 0, 1, 0 });
            const double sx = std::hypot(x.x - o.x, x.y - o.y);
            const double sy = std::hypot(y.x - o.x, y.y - o.y);
            scaleOut = sy > 1e-9 ? sy : (sx > 1e-9 ? sx : 1.0);
            rotRadOut = std::atan2(x.y - o.y, x.x - o.x);
        };

        struct Walker
        {
            const Scene& scene;
            std::vector<ExtractedText>& out;
            decltype(transformBox)& transformBoxFn;
            decltype(matrixScaleAndRot)& scaleRotFn;

            std::string EffectiveLayer(const Entity& e, const std::string& parentLayer) const
            {
                const std::string l = LayerNameById(scene, e.GetLayerID());
                if ((l == "0" || l.empty()) && !parentLayer.empty())
                    return parentLayer;
                return l.empty() ? "0" : l;
            }

            void Emit(int64_t entId, const std::string& ownerHandle, const char* kind,
                      const std::string& plain, const std::string& raw, const std::string& tag,
                      const std::string& layer, const std::string& space, const std::string& layout,
                      const std::string& blockPath, const Math::Point3& localPos, const Math::Mat4& m,
                      double localHeight, double localRotRad, const AABB& localBox)
            {
                const std::string cleaned = TrimUtf8(plain);
                if (cleaned.empty()) return;
                const Math::Point3 w = m.TransformPoint(localPos);
                if (!std::isfinite(w.x) || !std::isfinite(w.y)) return;
                double matScale = 1.0, matRot = 0.0;
                scaleRotFn(m, matScale, matRot);

                ExtractedText t;
                t.EntityId    = entId;
                t.OwnerHandle = ownerHandle;
                t.SourceKind  = kind;
                t.Content     = cleaned;
                if (!raw.empty() && raw != cleaned) t.RawContent = raw;
                t.Tag         = tag;
                t.Layer       = layer;
                t.Space       = space;
                t.LayoutName  = layout;
                t.BlockPath   = blockPath;
                t.X           = w.x;
                t.Y           = w.y;
                t.Height      = std::abs(localHeight * matScale);
                t.RotationDeg = (localRotRad + matRot) * kRadToDeg;
                transformBoxFn(localBox, m, t.MinX, t.MinY, t.MaxX, t.MaxY);
                if (!std::isfinite(t.MinX) || !std::isfinite(t.MaxX) || !std::isfinite(t.MinY) || !std::isfinite(t.MaxY) ||
                    t.MinX > t.MaxX || t.MinY > t.MaxY)
                {
                    t.MinX = t.MaxX = w.x;
                    t.MinY = t.MaxY = w.y;
                }
                out.push_back(std::move(t));
            }

            void Walk(const Entity& e, const Math::Mat4& m, const std::string& blockPath,
                      const std::string& space, const std::string& layout, const std::string& parentLayer,
                      int64_t rootEntityId, const std::string& ownerHandle, const char* overrideKind, int depth)
            {
                if (!e.GetAttr().Visible) return;
                const std::string layer = EffectiveLayer(e, parentLayer);

                if (e.IsKindOf<TableEntity>())
                {
                    const auto& tbl = static_cast<const TableEntity&>(e);
                    for (size_t r = 0; r < tbl.RowCount(); ++r)
                        for (size_t c = 0; c < tbl.ColCount(); ++c)
                        {
                            const std::string raw = tbl.Cell(r, c).Text;
                            if (raw.empty()) continue;
                            const Math::Point3 p = tbl.CellTopLeft(r, c);
                            const AABB box({ p.x, p.y - tbl.RowHeights()[r], p.z }, { p.x + tbl.ColWidths()[c], p.y, p.z });
                            Emit(rootEntityId, ownerHandle, overrideKind ? overrideKind : "TABLE",
                                 StripMTextInlineCodes(raw), raw, {}, layer, space, layout, blockPath,
                                 p, m, tbl.GetHeight(), tbl.GetRotation(), box);
                        }
                }
                else if (e.IsKindOf<TextEntity>())
                {
                    const auto& t = static_cast<const TextEntity&>(e);
                    Emit(rootEntityId, ownerHandle, overrideKind ? overrideKind : "TEXT",
                         t.GetText(), {}, {}, layer, space, layout, blockPath,
                         t.GetPosition(), m, t.GetHeight(), t.GetRotation(), t.GetBoundingBox());
                }
                else if (e.IsKindOf<MTextEntity>())
                {
                    const auto& t = static_cast<const MTextEntity&>(e);
                    Emit(rootEntityId, ownerHandle, overrideKind ? overrideKind : "MTEXT",
                         t.GetPlainText(), t.GetText(), {}, layer, space, layout, blockPath,
                         t.GetPosition(), m, t.GetHeight(), t.GetRotation(), t.GetBoundingBox());
                }
                else if (e.IsKindOf<DimensionEntity>())
                {
                    const auto& d = static_cast<const DimensionEntity&>(e);
                    const std::string txt = FormatDimensionText(d);
                    const Math::Point3 tp = d.TextPosition();
                    Emit(rootEntityId, ownerHandle, overrideKind ? overrideKind : "DIMENSION",
                         txt, d.GetTextOverride(), {}, layer, space, layout, blockPath,
                         tp, m, d.GetStyle().TextHeight, 0.0, d.GetBoundingBox());
                }
                else if (e.IsKindOf<MLeaderEntity>())
                {
                    const auto& ml = static_cast<const MLeaderEntity&>(e);
                    if (ml.GetContentType() == MLeaderContent::MText && !ml.GetText().empty())
                    {
                        Emit(rootEntityId, ownerHandle, overrideKind ? overrideKind : "MLEADER",
                             StripMTextInlineCodes(ml.GetText()), ml.GetText(), {}, layer, space, layout, blockPath,
                             ml.GetTextOrigin(), m, ml.GetStyle().TextHeight, 0.0, ml.GetBoundingBox());
                    }
                }
                else if (e.IsKindOf<InsertEntity>() && depth < 12)
                {
                    const auto& ins = static_cast<const InsertEntity&>(e);
                    const bool tchInternal = IsTchInternalBlockName(ins.GetBlockName());
                    const std::string nextPath = tchInternal
                        ? blockPath
                        : (blockPath.empty() ? ins.GetBlockName() : blockPath + "/" + ins.GetBlockName());
                    const int rows = std::clamp(ins.GetRowCount(), 1, 64);
                    const int cols = std::clamp(ins.GetColumnCount(), 1, 64);
                    if (const BlockEntity* blk = ins.GetBlock())
                    {
                        for (int r = 0; r < rows; ++r)
                            for (int c = 0; c < cols; ++c)
                            {
                                const Math::Mat4 cm = ins.CellMatrix(c, r) * m;
                                for (const auto& child : blk->GetEntities())
                                {
                                    if (!child) continue;
                                    // 非常量 ATTDEF 不在实例中显示
                                    if (const auto* def = dynamic_cast<const AttDefEntity*>(child.get()))
                                        if (!def->ShowsInBlockReference()) continue;
                                    Walk(*child, cm, nextPath, space, layout, layer, rootEntityId, ownerHandle, overrideKind, depth + 1);
                                }
                            }
                    }
                    // ATTRIB 已是世界坐标，直接用父矩阵 m
                    for (const auto& a : ins.GetAttribs())
                    {
                        if (!a || a->IsInvisible()) continue;
                        Emit(rootEntityId, ownerHandle, "ATTRIB",
                             a->GetText(), {}, a->GetTag(), EffectiveLayer(*a, layer),
                             space, layout, nextPath, a->GetPosition(), m, a->GetHeight(), a->GetRotation(), a->GetBoundingBox());
                    }
                }
            }
        };

        Walker walker{ scene, out, transformBox, matrixScaleAndRot };
        scene.ForEachObject([&](const Object& o) {
            if (!o.IsKindOf<Entity>()) return;
            const auto& e = static_cast<const Entity&>(o);
            const std::string space  = e.GetAttr().InPaperSpace ? "paper" : "model";
            const std::string layout = LayoutNameForEntity(scene, e);
            int64_t entId = 0;
            if (objectToEntityId)
                if (auto it = objectToEntityId->find(e.GetID()); it != objectToEntityId->end())
                    entId = it->second;

            uint64_t handle = 0;
            bool isTch = false;
            if (auto it = snap.EntityOrigins.find(e.GetID()); it != snap.EntityOrigins.end())
            {
                handle = it->second.Handle;
                isTch  = it->second.IsTchSynthesized;
            }
            // 已由 CollectTchReadableTexts 覆盖的天正对象跳过，防止重复
            if (isTch && handle != 0 && emittedTchHandles.count(handle) > 0)
                return;

            const char* overrideKind = isTch ? "TCH_SYMBOL" : nullptr;
            walker.Walk(e, Math::Mat4::Identity(), "", space, layout, "",
                        entId, FormatHandleHex(handle), overrideKind, 0);
        });
    }

    bool ExportSceneToSqlite(const Scene& scene, const CadSemanticSnapshot& snap,
                             const std::string& sourcePath, uint64_t sourceSizeBytes, int64_t sourceMtimeMs,
                             const std::string& outDbPath, bool includeBlockEntities,
                             ExportDbResult& result, std::string* error)
    {
        result = {};
        result.SchemaVersion     = "1.0";
        result.SourcePath        = sourcePath;
        result.DbPath            = outDbPath;
        result.Format            = snap.Format.empty() ? "DWG" : snap.Format;
        result.DwgVersion        = snap.DwgVersion;
        result.InsunitsCode      = snap.InsunitsCode;
        result.InsunitsName      = snap.InsunitsName;

        // CAD-22：直接调用 ExportSceneToSqlite 时同样防止覆盖源文件
        if (!sourcePath.empty())
        {
            if (!ValidateIoPathConflicts({ { "输入图纸", sourcePath } }, { { "输出 SQLite", outDbPath } }, nullptr, error))
                return false;
        }

        // 创建父目录并准备临时文件
        const std::filesystem::path destP = Utf8ToPath(outDbPath);
        std::error_code ec;
        if (destP.has_parent_path())
            std::filesystem::create_directories(destP.parent_path(), ec);

        const std::string tmpPath = MakeUniqueExportTempPath(outDbPath);
        std::filesystem::remove(Utf8ToPath(tmpPath), ec);

        // RAII 声明顺序：tempGuard -> dbGuard -> tx -> stmt，异常或失败自动回滚、关连接并删除临时库（CAD-20）
        ScopedTempFile tempGuard(tmpPath);
        ScopedSqliteDb dbGuard;
        if (!dbGuard.Open(tmpPath, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, error))
            return false;
        sqlite3* db = dbGuard.Get();

        if (!ExecSqliteChecked(db,
                               "PRAGMA journal_mode = OFF;"
                               "PRAGMA synchronous = OFF;"
                               "PRAGMA temp_store = MEMORY;"
                               "PRAGMA cache_size = -65536;",
                               error))
            return false;

        {
            ScopedSqliteTransaction tx(db);
            if (!tx.Begin("BEGIN IMMEDIATE;", error))
                return false;

            if (!ExecSqliteChecked(db, kSchemaDdl, error))
                return false;

            // 预统计图层实体数、布局实体/视口数、块被引用次数
            struct LayerStat { int total = 0, model = 0, paper = 0, block = 0; };
            std::unordered_map<std::string, LayerStat> layerStats;
            std::unordered_map<uint64_t, int> layoutEntityCounts;
            std::unordered_map<uint64_t, int> layoutViewportCounts;
            std::unordered_map<std::string, int> blockInsertCounts;

            AABB modelBounds = AABB::Empty();
            int modelEntityCount = 0;
            int paperEntityCount = 0;

            // 1. 写入 entities + entities_rtree（建立 ObjectID -> entities.id 映射）
            std::unordered_map<uint64_t, int64_t> objectToEntityId;
            {
                ScopedSqliteStmt stEnt;
                ScopedSqliteStmt stRtr;
                const char* sqlEnt =
                    "INSERT INTO entities("
                    "handle,entity_type,dxf_name,layer,space,layout_name,layout_key,block_path,"
                    "color_aci,color_rgb,linetype,lineweight_mm,visible,"
                    "minx,miny,maxx,maxy,length,area,is_closed,const_width,vertex_count,bulge_count,geom_json"
                    ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
                const char* sqlRtr = "INSERT INTO entities_rtree(id,minx,maxx,miny,maxy) VALUES(?,?,?,?,?);";
                if (!stEnt.Prepare(db, sqlEnt, error) || !stRtr.Prepare(db, sqlRtr, error))
                    return false;

                bool ok = true;
                scene.ForEachObject([&](const Object& o) {
                    if (!ok || !o.IsKindOf<Entity>()) return;
                    const auto& e = static_cast<const Entity&>(o);
                    const EntityAttr& attr = e.GetAttr();
                    const std::string layer = LayerNameById(scene, attr.LayerId);
                    const bool isPaper = attr.InPaperSpace;
                    const std::string space = isPaper ? "paper" : "model";
                    const std::string layoutName = LayoutNameForEntity(scene, e);
                    const uint64_t layoutKey = isPaper ? attr.LayoutKey : 0;

                    auto& ls = layerStats[layer];
                    ++ls.total;
                    if (isPaper) { ++ls.paper; ++paperEntityCount; }
                    else         { ++ls.model; ++modelEntityCount; }
                    ++layoutEntityCounts[layoutKey];
                    if (e.IsKindOf<ViewportEntity>())
                        ++layoutViewportCounts[layoutKey];

                    uint64_t handle = 0;
                    std::string dxfName;
                    bool isTch = false;
                    if (auto it = snap.EntityOrigins.find(e.GetID()); it != snap.EntityOrigins.end())
                    {
                        handle  = it->second.Handle;
                        dxfName = it->second.DxfName;
                        isTch   = it->second.IsTchSynthesized;
                    }

                    // 铁律：TCH 对象记真实天正类名，不记 *U_TCH_* 匿名块 INSERT
                    const std::string entityType = (isTch && !dxfName.empty()) ? dxfName : std::string(SceneEntityTypeName(e));
                    if (dxfName.empty()) dxfName = entityType;

                    const AABB box = e.IsBoundless() ? AABB::Empty() : e.GetBoundingBox();
                    const bool validBox = !e.IsBoundless() && IsFiniteBox(box);
                    if (validBox && !isPaper)
                    {
                        modelBounds.Expand(box.Min);
                        modelBounds.Expand(box.Max);
                    }

                    const GeomSummary geom = isTch ? GeomSummary{} : ExtractGeomSummary(e);

                    const bool aciOk = (attr.Color.Method == ColorMethod::ByAci)
                        ? stEnt.BindInt(9, static_cast<int>(attr.Color.Aci), error)
                        : stEnt.BindNull(9, error);
                    const bool rgbOk = (attr.Color.Method == ColorMethod::ByRgb || attr.Color.Method == ColorMethod::ByAci)
                        ? stEnt.BindText(10, FormatColorRgbHex(attr.Color.Rgba.r, attr.Color.Rgba.g, attr.Color.Rgba.b), error)
                        : stEnt.BindNull(10, error);
                    const double lwMm = LineweightToMillimeters(attr.Lineweight);
                    const bool lwOk = (lwMm > 0.0) ? stEnt.BindDouble(12, lwMm, error) : stEnt.BindNull(12, error);
                    const bool boxOk = validBox
                        ? (stEnt.BindDouble(14, box.Min.x, error) && stEnt.BindDouble(15, box.Min.y, error)
                           && stEnt.BindDouble(16, box.Max.x, error) && stEnt.BindDouble(17, box.Max.y, error))
                        : (stEnt.BindNull(14, error) && stEnt.BindNull(15, error)
                           && stEnt.BindNull(16, error) && stEnt.BindNull(17, error));

                    if (!stEnt.BindOptText(1, FormatHandleHex(handle), error)
                        || !stEnt.BindText(2, entityType, error)
                        || !stEnt.BindOptText(3, dxfName, error)
                        || !stEnt.BindText(4, layer, error)
                        || !stEnt.BindText(5, space, error)
                        || !stEnt.BindText(6, layoutName, error)
                        || !stEnt.BindInt64(7, static_cast<int64_t>(layoutKey), error)
                        || !stEnt.BindText(8, "", error)
                        || !aciOk
                        || !rgbOk
                        || !stEnt.BindText(11, LineTypeNameById(scene, attr.LineType), error)
                        || !lwOk
                        || !stEnt.BindInt(13, attr.Visible ? 1 : 0, error)
                        || !boxOk
                        || !stEnt.BindOptDouble(18, geom.Length, error)
                        || !stEnt.BindOptDouble(19, geom.Area, error)
                        || !stEnt.BindOptInt(20, geom.IsClosed, error)
                        || !stEnt.BindOptDouble(21, geom.ConstWidth, error)
                        || !stEnt.BindOptInt(22, geom.VertexCount, error)
                        || !stEnt.BindOptInt(23, geom.BulgeCount, error)
                        || !stEnt.BindOptText(24, geom.GeomJson, error)
                        || !stEnt.StepDone(error))
                    {
                        ok = false;
                        return;
                    }

                    const int64_t rowId = sqlite3_last_insert_rowid(db);
                    objectToEntityId[e.GetID()] = rowId;
                    ++result.Counts.Entities;

                    if (validBox)
                    {
                        // R-tree 列序：(id, minx, maxx, miny, maxy)
                        if (!stRtr.BindInt64(1, rowId, error)
                            || !stRtr.BindDouble(2, box.Min.x, error)
                            || !stRtr.BindDouble(3, box.Max.x, error)
                            || !stRtr.BindDouble(4, box.Min.y, error)
                            || !stRtr.BindDouble(5, box.Max.y, error)
                            || !stRtr.StepDone(error))
                        {
                            ok = false;
                            return;
                        }
                    }
                });
                if (!ok) return false;
            }

            // 2. 写入 inserts + insert_attribs（递归展开普通块引用，排除 *U_TCH_* 内部包裹块）
            {
                ScopedSqliteStmt stIns;
                ScopedSqliteStmt stAtt;
                const char* sqlIns =
                    "INSERT INTO inserts("
                    "entity_id,parent_insert_id,depth,handle,block_name,block_path,"
                    "layer,space,layout_name,x,y,z,scale_x,scale_y,scale_z,rotation_deg,"
                    "col_count,row_count,col_spacing,row_spacing,has_xclip,xclip_inverted,xclip_boundary_json,"
                    "attrib_count,minx,miny,maxx,maxy"
                    ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
                const char* sqlAtt =
                    "INSERT INTO insert_attribs("
                    "insert_id,insert_handle,block_name,tag,value,layer,space,layout_name,"
                    "x,y,height,rotation_deg,is_invisible"
                    ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);";
                if (!stIns.Prepare(db, sqlIns, error) || !stAtt.Prepare(db, sqlAtt, error))
                    return false;

                bool ok = true;
                std::function<void(const InsertEntity&, const Math::Mat4&, int64_t, int64_t, int,
                                   const std::string&, const std::string&, const std::string&, const std::string&)> walkInsert;
                walkInsert = [&](const InsertEntity& ins, const Math::Mat4& parentMat, int64_t topEntId,
                                 int64_t parentInsId, int depth, const std::string& parentPath,
                                 const std::string& space, const std::string& layout, const std::string& handleHex) {
                    if (!ok || depth > 12) return;
                    if (IsTchInternalBlockName(ins.GetBlockName())) return;

                    ++blockInsertCounts[ins.GetBlockName()];
                    const std::string path = parentPath.empty() ? ins.GetBlockName() : parentPath + "/" + ins.GetBlockName();
                    const Math::Point3 wPos = parentMat.TransformPoint(ins.GetPosition());
                    const Math::Point3 p0 = parentMat.TransformPoint({ 0, 0, 0 });
                    const Math::Point3 px = parentMat.TransformPoint({ 1, 0, 0 });
                    const Math::Point3 py = parentMat.TransformPoint({ 0, 1, 0 });
                    const double psx = std::hypot(px.x - p0.x, px.y - p0.y);
                    const double psy = std::hypot(py.x - p0.x, py.y - p0.y);
                    const double prot = std::atan2(px.y - p0.y, px.x - p0.x);

                    std::string clipJson;
                    if (ins.HasClipBoundary())
                    {
                        const Math::Mat4 cm = ins.CellMatrix(0, 0) * parentMat;
                        JsonWriter cw;
                        cw.BeginArray();
                        for (const auto& cp : ins.GetClipBoundary())
                        {
                            const Math::Point3 wp = cm.TransformPoint(cp);
                            cw.BeginArray(); cw.Double(wp.x, 3); cw.Double(wp.y, 3); cw.EndArray();
                        }
                        cw.EndArray();
                        clipJson = cw.Take();
                    }

                    const AABB localB = ins.GetBoundingBox();
                    const bool hasBox = IsFiniteBox(localB);
                    AABB wb = AABB::Empty();
                    if (hasBox)
                    {
                        const Math::Point3 cs[4] = {
                            { localB.Min.x, localB.Min.y, 0 }, { localB.Max.x, localB.Min.y, 0 },
                            { localB.Min.x, localB.Max.y, 0 }, { localB.Max.x, localB.Max.y, 0 },
                        };
                        for (const auto& c : cs) wb.Expand(parentMat.TransformPoint(c));
                    }

                    const bool topOk = topEntId > 0 ? stIns.BindInt64(1, topEntId, error) : stIns.BindNull(1, error);
                    const bool parOk = parentInsId > 0 ? stIns.BindInt64(2, parentInsId, error) : stIns.BindNull(2, error);
                    const bool clipOk = ins.HasClipBoundary() ? stIns.BindText(23, clipJson, error) : stIns.BindNull(23, error);
                    const bool wbOk = hasBox
                        ? (stIns.BindDouble(25, wb.Min.x, error) && stIns.BindDouble(26, wb.Min.y, error)
                           && stIns.BindDouble(27, wb.Max.x, error) && stIns.BindDouble(28, wb.Max.y, error))
                        : (stIns.BindNull(25, error) && stIns.BindNull(26, error)
                           && stIns.BindNull(27, error) && stIns.BindNull(28, error));

                    if (!topOk || !parOk
                        || !stIns.BindInt(3, depth, error)
                        || !stIns.BindOptText(4, handleHex, error)
                        || !stIns.BindText(5, ins.GetBlockName(), error)
                        || !stIns.BindText(6, path, error)
                        || !stIns.BindText(7, LayerNameById(scene, ins.GetLayerID()), error)
                        || !stIns.BindText(8, space, error)
                        || !stIns.BindText(9, layout, error)
                        || !stIns.BindDouble(10, wPos.x, error)
                        || !stIns.BindDouble(11, wPos.y, error)
                        || !stIns.BindDouble(12, wPos.z, error)
                        || !stIns.BindDouble(13, ins.GetScale().x * psx, error)
                        || !stIns.BindDouble(14, ins.GetScale().y * psy, error)
                        || !stIns.BindDouble(15, ins.GetScale().z, error)
                        || !stIns.BindDouble(16, (ins.GetRotation() + prot) * kRadToDeg, error)
                        || !stIns.BindInt(17, ins.GetColumnCount(), error)
                        || !stIns.BindInt(18, ins.GetRowCount(), error)
                        || !stIns.BindDouble(19, ins.GetColumnSpacing(), error)
                        || !stIns.BindDouble(20, ins.GetRowSpacing(), error)
                        || !stIns.BindInt(21, ins.HasClipBoundary() ? 1 : 0, error)
                        || !stIns.BindInt(22, 0, error)
                        || !clipOk
                        || !stIns.BindInt(24, static_cast<int>(ins.GetAttribs().size()), error)
                        || !wbOk
                        || !stIns.StepDone(error))
                    {
                        ok = false;
                        return;
                    }

                    const int64_t thisInsId = sqlite3_last_insert_rowid(db);
                    ++result.Counts.Inserts;

                    for (const auto& a : ins.GetAttribs())
                    {
                        if (!a) continue;
                        const Math::Point3 aw = parentMat.TransformPoint(a->GetPosition());
                        if (!stAtt.BindInt64(1, thisInsId, error)
                            || !stAtt.BindOptText(2, handleHex, error)
                            || !stAtt.BindText(3, ins.GetBlockName(), error)
                            || !stAtt.BindText(4, a->GetTag(), error)
                            || !stAtt.BindText(5, a->GetText(), error)
                            || !stAtt.BindText(6, LayerNameById(scene, a->GetLayerID()), error)
                            || !stAtt.BindText(7, space, error)
                            || !stAtt.BindText(8, layout, error)
                            || !stAtt.BindDouble(9, aw.x, error)
                            || !stAtt.BindDouble(10, aw.y, error)
                            || !stAtt.BindDouble(11, a->GetHeight() * psy, error)
                            || !stAtt.BindDouble(12, (a->GetRotation() + prot) * kRadToDeg, error)
                            || !stAtt.BindInt(13, a->IsInvisible() ? 1 : 0, error)
                            || !stAtt.StepDone(error))
                        {
                            ok = false;
                            return;
                        }
                        ++result.Counts.InsertAttribs;
                    }

                    // 递归嵌套 INSERT（仅沿单元格 (0,0) 追踪，防阵列嵌套爆炸）
                    if (const BlockEntity* blk = ins.GetBlock())
                    {
                        const Math::Mat4 cm = ins.CellMatrix(0, 0) * parentMat;
                        for (const auto& child : blk->GetEntities())
                            if (child && child->IsKindOf<InsertEntity>())
                                walkInsert(static_cast<const InsertEntity&>(*child), cm, 0, thisInsId, depth + 1, path, space, layout, {});
                    }
                };

                scene.ForEachObject([&](const Object& o) {
                    if (!ok || !o.IsKindOf<InsertEntity>()) return;
                    const auto& ins = static_cast<const InsertEntity&>(o);
                    if (auto it = snap.EntityOrigins.find(ins.GetID()); it != snap.EntityOrigins.end() && it->second.IsTchSynthesized)
                        return;
                    const std::string space  = ins.GetAttr().InPaperSpace ? "paper" : "model";
                    const std::string layout = LayoutNameForEntity(scene, ins);
                    int64_t entId = 0;
                    if (auto it = objectToEntityId.find(ins.GetID()); it != objectToEntityId.end())
                        entId = it->second;
                    std::string handleHex;
                    if (auto it = snap.EntityOrigins.find(ins.GetID()); it != snap.EntityOrigins.end())
                        handleHex = FormatHandleHex(it->second.Handle);
                    walkInsert(ins, Math::Mat4::Identity(), entId, 0, 0, "", space, layout, handleHex);
                });
                if (!ok) return false;
            }

            // 3. 写入 blocks + block_entities
            {
                ScopedSqliteStmt stBlk;
                ScopedSqliteStmt stBe;
                const char* sqlBlk =
                    "INSERT INTO blocks("
                    "name,base_x,base_y,base_z,flags,is_anonymous,is_xref,xref_path,xref_resolved,"
                    "description,entity_count,insert_count,minx,miny,maxx,maxy,layers_json,entity_types_json"
                    ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
                const char* sqlBe =
                    "INSERT INTO block_entities("
                    "block_name,entity_type,layer,minx,miny,maxx,maxy,length,geom_json"
                    ") VALUES(?,?,?,?,?,?,?,?,?);";
                if (!stBlk.Prepare(db, sqlBlk, error) || !stBe.Prepare(db, sqlBe, error))
                    return false;

                bool ok = true;
                scene.GetBlockTable().ForEach([&](BlockID, const BlockEntity& blk) {
                    if (!ok) return;
                    if (IsTchInternalBlockName(blk.GetName())) return;

                    std::map<std::string, int> blkLayers;
                    std::map<std::string, int> blkTypes;
                    const bool namedNonAnon = !blk.IsAnonymous();
                    for (const auto& child : blk.GetEntities())
                    {
                        if (!child) continue;
                        const std::string lyr = LayerNameById(scene, child->GetLayerID());
                        const std::string typ = SceneEntityTypeName(*child);
                        ++blkLayers[lyr];
                        ++blkTypes[typ];
                        if (namedNonAnon)
                        {
                            ++layerStats[lyr].block;
                            if (includeBlockEntities)
                            {
                                const AABB b = child->IsBoundless() ? AABB::Empty() : child->GetBoundingBox();
                                const bool validB = !child->IsBoundless() && IsFiniteBox(b);
                                const GeomSummary gs = ExtractGeomSummary(*child);
                                const bool bOk = validB
                                    ? (stBe.BindDouble(4, b.Min.x, error) && stBe.BindDouble(5, b.Min.y, error)
                                       && stBe.BindDouble(6, b.Max.x, error) && stBe.BindDouble(7, b.Max.y, error))
                                    : (stBe.BindNull(4, error) && stBe.BindNull(5, error)
                                       && stBe.BindNull(6, error) && stBe.BindNull(7, error));
                                if (!stBe.BindText(1, blk.GetName(), error)
                                    || !stBe.BindText(2, typ, error)
                                    || !stBe.BindText(3, lyr, error)
                                    || !bOk
                                    || !stBe.BindOptDouble(8, gs.Length, error)
                                    || !stBe.BindOptText(9, gs.GeomJson, error)
                                    || !stBe.StepDone(error))
                                {
                                    ok = false;
                                    return;
                                }
                                ++result.Counts.BlockEntities;
                            }
                        }
                    }

                    JsonWriter lw, tw;
                    lw.BeginObject();
                    for (const auto& [k, v] : blkLayers) lw.KeyInt(k, v);
                    lw.EndObject();
                    tw.BeginObject();
                    for (const auto& [k, v] : blkTypes) tw.KeyInt(k, v);
                    tw.EndObject();

                    const AABB bb = blk.GetBoundingBox();
                    const bool validBb = IsFiniteBox(bb);
                    const bool bbOk = validBb
                        ? (stBlk.BindDouble(13, bb.Min.x, error) && stBlk.BindDouble(14, bb.Min.y, error)
                           && stBlk.BindDouble(15, bb.Max.x, error) && stBlk.BindDouble(16, bb.Max.y, error))
                        : (stBlk.BindNull(13, error) && stBlk.BindNull(14, error)
                           && stBlk.BindNull(15, error) && stBlk.BindNull(16, error));
                    if (!stBlk.BindText(1, blk.GetName(), error)
                        || !stBlk.BindDouble(2, blk.GetBasePoint().x, error)
                        || !stBlk.BindDouble(3, blk.GetBasePoint().y, error)
                        || !stBlk.BindDouble(4, blk.GetBasePoint().z, error)
                        || !stBlk.BindInt(5, static_cast<int>(blk.GetFlags()), error)
                        || !stBlk.BindInt(6, blk.IsAnonymous() ? 1 : 0, error)
                        || !stBlk.BindInt(7, blk.IsXRef() ? 1 : 0, error)
                        || !stBlk.BindOptText(8, blk.GetXRefPath(), error)
                        || !stBlk.BindInt(9, (blk.IsXRef() && !blk.IsUnresolvedXRef()) ? 1 : 0, error)
                        || !stBlk.BindOptText(10, blk.GetDescription(), error)
                        || !stBlk.BindInt(11, static_cast<int>(blk.EntityCount()), error)
                        || !stBlk.BindInt(12, blockInsertCounts[blk.GetName()], error)
                        || !bbOk
                        || !stBlk.BindText(17, lw.Str(), error)
                        || !stBlk.BindText(18, tw.Str(), error)
                        || !stBlk.StepDone(error))
                    {
                        ok = false;
                        return;
                    }
                    ++result.Counts.Blocks;
                });
                if (!ok) return false;
            }

            // 4. 写入 layers（保留原始 is_on / is_frozen 分离语义）
            {
                ScopedSqliteStmt st;
                const char* sql =
                    "INSERT INTO layers("
                    "name,color_aci,color_rgb,is_on,is_frozen,is_locked,is_visible,is_plottable,"
                    "linetype,lineweight_mm,entity_count,model_entity_count,paper_entity_count,block_entity_count"
                    ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
                if (!st.Prepare(db, sql, error)) return false;

                const LayerManager& lm = scene.GetLayerManager();
                for (LayerID id : lm.GetAllLayerIDs())
                {
                    const Layer* l = lm.GetLayer(id);
                    if (!l) continue;
                    const auto rfIt = snap.LayerFlagsById.find(id);
                    const bool hasRaw = (rfIt != snap.LayerFlagsById.end());

                    const bool aciOk = (hasRaw && !rfIt->second.HasTrueColor && rfIt->second.ColorAci > 0)
                        ? st.BindInt(2, rfIt->second.ColorAci, error)
                        : st.BindNull(2, error);
                    const auto& col = l->GetColor();
                    const bool isOn       = hasRaw ? rfIt->second.IsOn : l->IsVisible();
                    const bool isFrozen   = hasRaw ? rfIt->second.IsFrozen : false;
                    const bool isLocked   = hasRaw ? rfIt->second.IsLocked : l->IsLocked();
                    const bool isVisible  = isOn && !isFrozen;
                    const bool isPlot     = hasRaw ? rfIt->second.IsPlottable : true;
                    const double lwMm     = LineweightToMillimeters(l->GetLineweight());
                    const bool lwOk       = (lwMm > 0.0) ? st.BindDouble(10, lwMm, error) : st.BindNull(10, error);
                    const LayerStat ls    = layerStats[l->GetName()];

                    if (!st.BindText(1, l->GetName(), error)
                        || !aciOk
                        || !st.BindText(3, FormatColorRgbHex(col.r, col.g, col.b), error)
                        || !st.BindInt(4, isOn ? 1 : 0, error)
                        || !st.BindInt(5, isFrozen ? 1 : 0, error)
                        || !st.BindInt(6, isLocked ? 1 : 0, error)
                        || !st.BindInt(7, isVisible ? 1 : 0, error)
                        || !st.BindInt(8, isPlot ? 1 : 0, error)
                        || !st.BindText(9, LineTypeNameById(scene, l->GetLineType()), error)
                        || !lwOk
                        || !st.BindInt(11, ls.total, error)
                        || !st.BindInt(12, ls.model, error)
                        || !st.BindInt(13, ls.paper, error)
                        || !st.BindInt(14, ls.block, error)
                        || !st.StepDone(error))
                    {
                        return false;
                    }
                    ++result.Counts.Layers;
                }
            }

            // 5. 写入 layouts
            {
                ScopedSqliteStmt st;
                const char* sql =
                    "INSERT INTO layouts("
                    "name,is_model,tab_order,owner_key,minx,miny,maxx,maxy,"
                    "view_center_x,view_center_y,view_height,entity_count,viewport_count"
                    ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);";
                if (!st.Prepare(db, sql, error)) return false;

                for (const DrawingLayout& lay : scene.Layouts())
                {
                    const std::string name = lay.Model ? "Model" : lay.Name;
                    const uint64_t key = lay.Model ? 0 : lay.OwnerKey;

                    bool boundsOk = true;
                    if (lay.Model)
                    {
                        boundsOk = IsFiniteBox(modelBounds)
                            ? (st.BindDouble(5, modelBounds.Min.x, error) && st.BindDouble(6, modelBounds.Min.y, error)
                               && st.BindDouble(7, modelBounds.Max.x, error) && st.BindDouble(8, modelBounds.Max.y, error))
                            : (st.BindNull(5, error) && st.BindNull(6, error)
                               && st.BindNull(7, error) && st.BindNull(8, error));
                    }
                    else
                    {
                        boundsOk = st.BindDouble(5, lay.MinX, error) && st.BindDouble(6, lay.MinY, error)
                                && st.BindDouble(7, lay.MaxX, error) && st.BindDouble(8, lay.MaxY, error);
                    }
                    const bool viewOk = (lay.ViewHeight > 0.0)
                        ? (st.BindDouble(9, lay.ViewCenterX, error) && st.BindDouble(10, lay.ViewCenterY, error)
                           && st.BindDouble(11, lay.ViewHeight, error))
                        : (st.BindNull(9, error) && st.BindNull(10, error) && st.BindNull(11, error));

                    if (!st.BindText(1, name, error)
                        || !st.BindInt(2, lay.Model ? 1 : 0, error)
                        || !st.BindInt(3, lay.Order, error)
                        || !st.BindInt64(4, static_cast<int64_t>(key), error)
                        || !boundsOk
                        || !viewOk
                        || !st.BindInt(12, layoutEntityCounts[key], error)
                        || !st.BindInt(13, layoutViewportCounts[key], error)
                        || !st.StepDone(error))
                    {
                        return false;
                    }
                    ++result.Counts.Layouts;
                }
            }

            // 6. 写入 viewports
            {
                ScopedSqliteStmt st;
                const char* sql =
                    "INSERT INTO viewports("
                    "handle,layout_name,layout_key,viewport_no,layer,shows_model,"
                    "paper_cx,paper_cy,paper_w,paper_h,paper_minx,paper_miny,paper_maxx,paper_maxy,"
                    "model_cx,model_cy,model_view_height,twist_deg,model_to_paper_scale,"
                    "model_minx,model_miny,model_maxx,model_maxy,frozen_layers_json"
                    ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
                if (!st.Prepare(db, sql, error)) return false;

                bool ok = true;
                scene.ForEachObject([&](const Object& o) {
                    if (!ok || !o.IsKindOf<ViewportEntity>()) return;
                    const auto& vp = static_cast<const ViewportEntity&>(o);
                    std::string handleHex;
                    if (auto it = snap.EntityOrigins.find(vp.GetID()); it != snap.EntityOrigins.end())
                        handleHex = FormatHandleHex(it->second.Handle);

                    const AABB pbox = vp.GetBoundingBox();
                    const AABB mbox = vp.ModelVisibleBox();
                    JsonWriter fw;
                    fw.BeginArray();
                    for (LayerID lid : vp.GetFrozenLayers())
                        fw.String(LayerNameById(scene, lid));
                    fw.EndArray();

                    const bool mboxOk = IsFiniteBox(mbox)
                        ? (st.BindDouble(20, mbox.Min.x, error) && st.BindDouble(21, mbox.Min.y, error)
                           && st.BindDouble(22, mbox.Max.x, error) && st.BindDouble(23, mbox.Max.y, error))
                        : (st.BindNull(20, error) && st.BindNull(21, error)
                           && st.BindNull(22, error) && st.BindNull(23, error));

                    if (!st.BindOptText(1, handleHex, error)
                        || !st.BindText(2, LayoutNameForEntity(scene, vp), error)
                        || !st.BindInt64(3, static_cast<int64_t>(vp.GetAttr().LayoutKey), error)
                        || !st.BindInt(4, vp.GetViewportId(), error)
                        || !st.BindText(5, LayerNameById(scene, vp.GetLayerID()), error)
                        || !st.BindInt(6, vp.ShowsModel() ? 1 : 0, error)
                        || !st.BindDouble(7, vp.GetCenter().x, error)
                        || !st.BindDouble(8, vp.GetCenter().y, error)
                        || !st.BindDouble(9, vp.GetWidth(), error)
                        || !st.BindDouble(10, vp.GetHeight(), error)
                        || !st.BindDouble(11, pbox.Min.x, error)
                        || !st.BindDouble(12, pbox.Min.y, error)
                        || !st.BindDouble(13, pbox.Max.x, error)
                        || !st.BindDouble(14, pbox.Max.y, error)
                        || !st.BindDouble(15, vp.GetViewCenter().x, error)
                        || !st.BindDouble(16, vp.GetViewCenter().y, error)
                        || !st.BindDouble(17, vp.GetViewHeight(), error)
                        || !st.BindDouble(18, vp.GetTwist() * kRadToDeg, error)
                        || !st.BindDouble(19, vp.ModelToPaperScale(), error)
                        || !mboxOk
                        || !st.BindText(24, fw.Str(), error)
                        || !st.StepDone(error))
                    {
                        ok = false;
                        return;
                    }
                    ++result.Counts.Viewports;
                });
                if (!ok) return false;
            }

            // 7. 写入 texts + texts_rtree
            {
                std::vector<ExtractedText> texts;
                CollectAllSceneTexts(scene, snap, texts, &objectToEntityId);

                ScopedSqliteStmt stTxt;
                ScopedSqliteStmt stRtr;
                const char* sqlTxt =
                    "INSERT INTO texts("
                    "entity_id,owner_handle,source_kind,content,raw_content,tag,"
                    "layer,space,layout_name,block_path,x,y,height,rotation_deg,"
                    "minx,miny,maxx,maxy"
                    ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
                const char* sqlRtr = "INSERT INTO texts_rtree(id,minx,maxx,miny,maxy) VALUES(?,?,?,?,?);";
                if (!stTxt.Prepare(db, sqlTxt, error) || !stRtr.Prepare(db, sqlRtr, error))
                    return false;

                for (const auto& t : texts)
                {
                    const bool entOk = (t.EntityId > 0) ? stTxt.BindInt64(1, t.EntityId, error) : stTxt.BindNull(1, error);
                    if (!entOk
                        || !stTxt.BindOptText(2, t.OwnerHandle, error)
                        || !stTxt.BindText(3, t.SourceKind, error)
                        || !stTxt.BindText(4, t.Content, error)
                        || !stTxt.BindOptText(5, t.RawContent, error)
                        || !stTxt.BindOptText(6, t.Tag, error)
                        || !stTxt.BindText(7, t.Layer, error)
                        || !stTxt.BindText(8, t.Space, error)
                        || !stTxt.BindText(9, t.LayoutName, error)
                        || !stTxt.BindText(10, t.BlockPath, error)
                        || !stTxt.BindDouble(11, t.X, error)
                        || !stTxt.BindDouble(12, t.Y, error)
                        || !stTxt.BindDouble(13, t.Height, error)
                        || !stTxt.BindDouble(14, t.RotationDeg, error)
                        || !stTxt.BindDouble(15, t.MinX, error)
                        || !stTxt.BindDouble(16, t.MinY, error)
                        || !stTxt.BindDouble(17, t.MaxX, error)
                        || !stTxt.BindDouble(18, t.MaxY, error)
                        || !stTxt.StepDone(error))
                    {
                        return false;
                    }
                    const int64_t rowId = sqlite3_last_insert_rowid(db);
                    ++result.Counts.Texts;

                    if (std::isfinite(t.MinX) && std::isfinite(t.MaxX) && std::isfinite(t.MinY) && std::isfinite(t.MaxY) &&
                        t.MinX <= t.MaxX && t.MinY <= t.MaxY)
                    {
                        if (!stRtr.BindInt64(1, rowId, error)
                            || !stRtr.BindDouble(2, t.MinX, error)
                            || !stRtr.BindDouble(3, t.MaxX, error)
                            || !stRtr.BindDouble(4, t.MinY, error)
                            || !stRtr.BindDouble(5, t.MaxY, error)
                            || !stRtr.StepDone(error))
                        {
                            return false;
                        }
                    }
                }
            }

            // 8. 写入天正 8 张原生语义表
            TchExportCounts tchCounts{};
            if (!WriteTchSemanticTables(db, snap, tchCounts, error))
                return false;
            result.Counts.TchWalls      = tchCounts.Walls;
            result.Counts.TchOpenings   = tchCounts.Openings;
            result.Counts.TchColumns    = tchCounts.Columns;
            result.Counts.TchSpaces     = tchCounts.Spaces;
            result.Counts.TchStairs     = tchCounts.Stairs;
            result.Counts.TchDimensions = tchCounts.Dimensions;
            result.Counts.TchAxes       = tchCounts.Axes;
            result.Counts.TchSheets     = tchCounts.Sheets;
            result.Counts.TchFailed     = snap.TchFailedCount;

            // 9. 写入 diagnostics
            JsonWriter undecodedJw, skippedJw, xrefJw;
            undecodedJw.BeginArray();
            skippedJw.BeginArray();
            xrefJw.BeginArray();
            {
                ScopedSqliteStmt st;
                const char* sql = "INSERT INTO diagnostics(kind,dxf_name,reason,count,sample_handle) VALUES(?,?,?,?,?);";
                if (!st.Prepare(db, sql, error)) return false;
                for (const auto& d : snap.Diagnostics)
                {
                    const std::string hHex = FormatHandleHex(d.SampleHandle);
                    if (!st.BindText(1, d.Kind, error)
                        || !st.BindText(2, d.DxfName, error)
                        || !st.BindOptText(3, d.Reason, error)
                        || !st.BindInt(4, d.Count, error)
                        || !st.BindOptText(5, hHex, error)
                        || !st.StepDone(error))
                    {
                        return false;
                    }

                    if (d.Kind == "unresolved_xref")
                    {
                        ++result.Counts.UnresolvedXrefs;
                        xrefJw.BeginObject();
                        xrefJw.KeyString("name", d.DxfName);
                        xrefJw.KeyString("path", d.Reason);
                        xrefJw.EndObject();
                    }
                    else if (d.Kind == "skipped_entity")
                    {
                        skippedJw.BeginObject();
                        skippedJw.KeyString("dxf_name", d.DxfName);
                        skippedJw.KeyInt("count", d.Count);
                        skippedJw.EndObject();
                    }
                    else
                    {
                        undecodedJw.BeginObject();
                        undecodedJw.KeyString("kind", d.Kind);
                        undecodedJw.KeyString("dxf_name", d.DxfName);
                        undecodedJw.KeyString("reason", d.Reason);
                        undecodedJw.KeyInt("count", d.Count);
                        undecodedJw.KeyOptString("sample_handle", hHex);
                        undecodedJw.EndObject();
                    }
                }
            }
            undecodedJw.EndArray();
            skippedJw.EndArray();
            xrefJw.EndArray();

            // 10. 写入 meta 键值表（含 CAD-21 导出选项与外参依赖指纹）
            const std::vector<XrefDepRecord> xrefDeps = CollectXrefDependencies(scene, sourcePath);
            const std::string xrefDepsV1   = SerializeXrefDepsV1(xrefDeps);
            const std::string xrefDepsJson = FormatXrefDepsJson(xrefDeps);

            const bool isNativeTch = snap.TchObjectCount > 0 ||
                                     (tchCounts.Walls + tchCounts.Openings + tchCounts.Columns +
                                      tchCounts.Spaces + tchCounts.Stairs + tchCounts.Dimensions +
                                      tchCounts.Axes + tchCounts.Sheets) > 0;
            result.IsNativeTianzheng = isNativeTch;
            if (IsFiniteBox(modelBounds))
            {
                result.HasModelBbox = true;
                result.ModelBbox    = { modelBounds.Min.x, modelBounds.Min.y, modelBounds.Max.x, modelBounds.Max.y };
            }

            {
                ScopedSqliteStmt st;
                if (!st.Prepare(db, "INSERT INTO meta(key,value) VALUES(?,?);", error)) return false;
                auto put = [&](const char* k, const std::string& v) -> bool {
                    return st.BindTextStatic(1, k, error)
                        && st.BindText(2, v, error)
                        && st.StepDone(error);
                };
                bool ok = true;
                ok = ok && put("schema_version", "1.0");
                ok = ok && put("tool_version", MINICAD_TOOL_VERSION);
                ok = ok && put("source_path", sourcePath);
                ok = ok && put("source_size_bytes", std::to_string(sourceSizeBytes));
                ok = ok && put("source_mtime_ms", std::to_string(sourceMtimeMs));
                ok = ok && put("include_block_entities", includeBlockEntities ? "1" : "0");
                ok = ok && put("xref_deps_v1", xrefDepsV1);
                ok = ok && put("xref_deps_json", xrefDepsJson);
                ok = ok && put("exported_at_utc", CurrentUtcIso8601());
                ok = ok && put("format", result.Format);
                ok = ok && put("dwg_version", snap.DwgVersion);
                ok = ok && put("insunits_code", std::to_string(snap.InsunitsCode));
                ok = ok && put("insunits_name", snap.InsunitsName);
                ok = ok && put("measurement", snap.Measurement);
                ok = ok && put("ltscale", JsonDouble(snap.LtScale, 6));
                ok = ok && put("psltscale", snap.PsLtScale ? "1" : "0");
                if (result.HasModelBbox)
                {
                    ok = ok && put("model_minx", JsonDouble(result.ModelBbox[0], 4));
                    ok = ok && put("model_miny", JsonDouble(result.ModelBbox[1], 4));
                    ok = ok && put("model_maxx", JsonDouble(result.ModelBbox[2], 4));
                    ok = ok && put("model_maxy", JsonDouble(result.ModelBbox[3], 4));
                }
                ok = ok && put("entity_count", std::to_string(result.Counts.Entities));
                ok = ok && put("model_entity_count", std::to_string(modelEntityCount));
                ok = ok && put("paper_entity_count", std::to_string(paperEntityCount));
                ok = ok && put("text_count", std::to_string(result.Counts.Texts));
                ok = ok && put("layer_count", std::to_string(result.Counts.Layers));
                ok = ok && put("layout_count", std::to_string(result.Counts.Layouts));
                ok = ok && put("viewport_count", std::to_string(result.Counts.Viewports));
                ok = ok && put("block_count", std::to_string(result.Counts.Blocks));
                ok = ok && put("insert_count", std::to_string(result.Counts.Inserts));
                ok = ok && put("is_native_tianzheng", isNativeTch ? "1" : "0");
                ok = ok && put("tch_object_count", std::to_string(snap.TchObjectCount));
                ok = ok && put("tch_decoded_count", std::to_string(snap.TchDecodedCount));
                ok = ok && put("tch_failed_count", std::to_string(snap.TchFailedCount));
                ok = ok && put("tch_config_fill", snap.TchConfigFill ? "1" : "0");
                ok = ok && put("tch_config_detail_scale", JsonDouble(snap.TchConfigDetailScale, 4));
                ok = ok && put("undecoded_proxies_json", undecodedJw.Str());
                ok = ok && put("skipped_entities_json", skippedJw.Str());
                ok = ok && put("unresolved_xrefs_json", xrefJw.Str());
                if (!ok) return false;
            }

            if (!tx.Commit(error))
                return false;
        }

        if (!ExecSqliteChecked(db, "PRAGMA journal_mode = DELETE; PRAGMA optimize;", error))
            return false;

        if (!dbGuard.Close(error))
            return false;

        if (!AtomicReplaceFile(tmpPath, outDbPath, error))
            return false;

        tempGuard.Release();
        return true;
    }

    bool RunExportDbCommand(const ExportDbOptions& opts, ExportDbResult& result,
                            std::string* error, std::string* errorCode, bool* isUsageError)
    {
        if (errorCode) errorCode->clear();
        if (isUsageError) *isUsageError = false;

        // CAD-22：首次写盘前检查输入/输出路径身份冲突，绝不覆盖源 DWG
        if (!ValidateIoPathConflicts({ { "输入图纸", opts.InputPath } },
                                     { { "输出 SQLite", opts.OutputDbPath } },
                                     errorCode, error))
        {
            if (isUsageError) *isUsageError = true;
            return false;
        }

        const auto t0 = std::chrono::steady_clock::now();
        uint64_t sizeBytes = 0;
        int64_t  mtimeMs   = 0;
        bool     mtimeOk   = false;
        if (!GetFileStats(opts.InputPath, sizeBytes, mtimeMs, error, &mtimeOk))
        {
            if (errorCode) *errorCode = "LOAD_FAILED";
            return false;
        }

        const bool includeBlockEntities = !opts.NoBlockEntities;
        if (opts.ReuseIfFresh && !opts.Force)
        {
            if (TryReuseExistingDb(opts.OutputDbPath, opts.InputPath,
                                   sizeBytes, mtimeMs, mtimeOk, includeBlockEntities, result))
            {
                result.ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                return true;
            }
        }

        Scene scene;
        CadSemanticSnapshot snap;
        if (!LoadCadFileWithSemantics(opts.InputPath, scene, snap, error))
        {
            if (errorCode) *errorCode = "LOAD_FAILED";
            return false;
        }

        if (!ExportSceneToSqlite(scene, snap, opts.InputPath, sizeBytes, mtimeMs,
                                 opts.OutputDbPath, includeBlockEntities, result, error))
        {
            if (errorCode) *errorCode = "EXPORT_DB_FAILED";
            return false;
        }

        result.ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        return true;
    }

    std::string FormatExportDbResultJson(const ExportDbResult& r, bool pretty)
    {
        JsonWriter jw(pretty);
        jw.BeginObject();
        jw.KeyBool("ok", true);
        jw.KeyString("command", "export-db");
        jw.KeyBool("reused", r.Reused);
        jw.KeyString("schema_version", r.SchemaVersion);
        jw.KeyString("source_path", r.SourcePath);
        jw.KeyString("db_path", r.DbPath);
        jw.KeyString("format", r.Format);
        jw.KeyString("dwg_version", r.DwgVersion);
        jw.Key("insunits");
        jw.BeginObject();
        jw.KeyInt("code", r.InsunitsCode);
        jw.KeyString("name", r.InsunitsName);
        jw.EndObject();
        jw.Key("model_bbox");
        if (r.HasModelBbox)
        {
            jw.BeginArray();
            for (double v : r.ModelBbox) jw.Double(v, 3);
            jw.EndArray();
        }
        else
        {
            jw.Null();
        }
        jw.KeyBool("is_native_tianzheng", r.IsNativeTianzheng);
        jw.Key("counts");
        jw.BeginObject();
        jw.KeyInt("layers", r.Counts.Layers);
        jw.KeyInt("layouts", r.Counts.Layouts);
        jw.KeyInt("viewports", r.Counts.Viewports);
        jw.KeyInt("blocks", r.Counts.Blocks);
        jw.KeyInt("block_entities", r.Counts.BlockEntities);
        jw.KeyInt("inserts", r.Counts.Inserts);
        jw.KeyInt("insert_attribs", r.Counts.InsertAttribs);
        jw.KeyInt("texts", r.Counts.Texts);
        jw.KeyInt("entities", r.Counts.Entities);
        jw.KeyInt("tch_walls", r.Counts.TchWalls);
        jw.KeyInt("tch_openings", r.Counts.TchOpenings);
        jw.KeyInt("tch_columns", r.Counts.TchColumns);
        jw.KeyInt("tch_spaces", r.Counts.TchSpaces);
        jw.KeyInt("tch_stairs", r.Counts.TchStairs);
        jw.KeyInt("tch_dimensions", r.Counts.TchDimensions);
        jw.KeyInt("tch_axes", r.Counts.TchAxes);
        jw.KeyInt("tch_sheets", r.Counts.TchSheets);
        jw.KeyInt("tch_failed", r.Counts.TchFailed);
        jw.KeyInt("unresolved_xrefs", r.Counts.UnresolvedXrefs);
        jw.EndObject();
        jw.KeyInt("elapsed_ms", r.ElapsedMs);
        jw.EndObject();
        return jw.Take();
    }
}
