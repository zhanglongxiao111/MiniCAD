#include "SceneTessellator.h"
#include "Core/PathUtils.h"
#include "Core/Draw/ClipDrawSink.hpp"
#include "Core/Draw/TransformDrawSink.hpp"
#include "Core/Entity/AttribEntity.hpp"
#include "Core/Entity/DimensionEntity.hpp"
#include "Core/Entity/InsertEntity.hpp"
#include "Core/Entity/LeaderEntity.hpp"
#include "Core/Entity/MLeaderEntity.hpp"
#include "Core/Entity/MTextEntity.hpp"
#include "Core/Entity/PolylineEntity.hpp"
#include "Core/Entity/RayEntity.hpp"
#include "Core/Entity/TableEntity.hpp"
#include "Core/Entity/TextEntity.hpp"
#include "Core/Entity/ViewportEntity.hpp"
#include "Core/Entity/WipeoutEntity.hpp"
#include "Core/Entity/XLineEntity.hpp"
#include "Document/DrawContext.hpp"
#include "Document/TextStyleResolver.h"
#include "Document/WipeClip.hpp"
#include "Editor/Overlay/Overlay.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <optional>
#include <regex>
#include <unordered_map>

namespace MiniCAD::SaTool
{
    namespace
    {
        // 内置注记图层正则（与 overlay_dxf.py 及设计方案 §5.3 一致）
        constexpr const char* kAnnotationLayerPattern =
            "尺寸|文字|标注|索引|面积|图框|图签|云线|DIM|TEXT|TITLE|MODI|LEAD|SYMB|ELEV|IDEN|FRAME";

#ifdef _WIN32
        std::wstring Utf8ToWide(const std::string& utf8)
        {
            if (utf8.empty())
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            if (n <= 0)
                return {};
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
            return out;
        }

        std::filesystem::path GetExecutableDir()
        {
            wchar_t buf[MAX_PATH * 4]{};
            const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
            if (len == 0 || len >= std::size(buf))
                return std::filesystem::current_path();
            return std::filesystem::path(buf).parent_path();
        }
#else
        std::filesystem::path GetExecutableDir()
        {
            return std::filesystem::current_path();
        }
#endif

        std::string PathToUtf8String(const std::filesystem::path& p)
        {
            const std::u8string u8 = p.u8string();
            return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
        }

        std::string EnsureTrailingSlash(std::string dir)
        {
            if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
                dir.push_back('/');
            return dir;
        }

        std::string ToLowerAscii(std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        struct RegexMatcher
        {
            bool Enabled = false;
#ifdef _WIN32
            std::wregex Re;
#else
            std::regex Re;
#endif
            bool Compile(const std::string& pattern, std::string* error)
            {
                if (pattern.empty())
                {
                    Enabled = false;
                    return true;
                }
                try
                {
#ifdef _WIN32
                    Re = std::wregex(Utf8ToWide(pattern),
                                     std::regex_constants::ECMAScript | std::regex_constants::icase);
#else
                    Re = std::regex(pattern,
                                    std::regex_constants::ECMAScript | std::regex_constants::icase);
#endif
                    Enabled = true;
                    return true;
                }
                catch (const std::regex_error& ex)
                {
                    Enabled = false;
                    if (error)
                        *error = std::string("无效正则表达式 '") + pattern + "'：" + ex.what();
                    return false;
                }
            }

            bool Matches(const std::string& utf8) const
            {
                if (!Enabled)
                    return false;
#ifdef _WIN32
                return std::regex_search(Utf8ToWide(utf8), Re);
#else
                return std::regex_search(utf8, Re);
#endif
            }
        };

        struct CompiledFilter
        {
            struct LayerDecision
            {
                bool AnnotationLayer = false;
                bool IncludeOk       = true;
                bool ExcludeHit      = false;
            };

            RegexMatcher IncludeLayers;
            RegexMatcher ExcludeLayers;
            RegexMatcher AnnotationLayers;
            bool         ExcludeAnnotations = false;
            bool         ExcludeFrames      = false;
            bool         NeedsLayerCheck    = false;
            mutable std::unordered_map<LayerID, LayerDecision> LayerCache;

            bool Init(const TessellateFilter& f, std::string* error)
            {
                ExcludeAnnotations = f.ExcludeAnnotations;
                ExcludeFrames      = f.ExcludeFrames;
                if (!IncludeLayers.Compile(f.IncludeLayersRegex, error))
                    return false;
                if (!ExcludeLayers.Compile(f.ExcludeLayersRegex, error))
                    return false;
                if (ExcludeAnnotations)
                    AnnotationLayers.Compile(kAnnotationLayerPattern, nullptr);
                NeedsLayerCheck = ExcludeAnnotations || IncludeLayers.Enabled || ExcludeLayers.Enabled;
                LayerCache.clear();
                return true;
            }

            LayerDecision CheckLayer(const LayerManager& lm, LayerID id) const
            {
                auto it = LayerCache.find(id);
                if (it != LayerCache.end())
                    return it->second;

                const Layer* layer = lm.GetLayer(id);
                const std::string name = layer ? layer->GetName() : std::string("0");
                LayerDecision d;
                if (ExcludeAnnotations)
                    d.AnnotationLayer = AnnotationLayers.Matches(name);
                if (IncludeLayers.Enabled)
                    d.IncludeOk = IncludeLayers.Matches(name);
                if (ExcludeLayers.Enabled)
                    d.ExcludeHit = ExcludeLayers.Matches(name);
                LayerCache.emplace(id, d);
                return d;
            }
        };

        bool IsAnnotationEntity(const Entity& e)
        {
            // TableEntity 继承自 MTextEntity，按普通表格实体放行（若在注记图层仍由图层正则拦截）
            // WipeoutEntity 属于几何遮罩语义（如门窗块/详图遮挡背景线），不得当作注记删除
            if (e.IsKindOf<TableEntity>() || e.IsKindOf<WipeoutEntity>())
                return false;
            return e.IsKindOf<TextEntity>()
                || e.IsKindOf<MTextEntity>()
                || e.IsKindOf<AttribBase>()
                || e.IsKindOf<DimensionEntity>()
                || e.IsKindOf<LeaderEntity>()
                || e.IsKindOf<MLeaderEntity>();
        }

        bool BoxesIntersect2D(const AABB& a, const AABB& b)
        {
            return !(a.Max.x < b.Min.x || a.Min.x > b.Max.x || a.Max.y < b.Min.y || a.Min.y > b.Max.y);
        }

        bool IsFiniteValidBox(const AABB& box)
        {
            if (box.Min.x > box.Max.x || box.Min.y > box.Max.y)
                return false;
            return std::isfinite(box.Min.x) && std::isfinite(box.Min.y)
                && std::isfinite(box.Max.x) && std::isfinite(box.Max.y);
        }

        bool IsUnboundedEntity(const Entity& e)
        {
            return e.IsKindOf<XLineEntity>() || e.IsKindOf<RayEntity>();
        }

        // 与 Editor::ViewportComposeSink 等价的图纸空间视口变换转发汇
        class ViewportComposeSink final : public TransformDrawSink
        {
        public:
            ViewportComposeSink(IDrawSink& target, const Math::Mat4& m, const ViewportEntity& vp)
                : TransformDrawSink(target, m)
                , m_target(target)
                , m_vp(vp)
                , m_scale(vp.ModelToPaperScale())
                , m_paperHeight(vp.GetHeight())
            {
            }

            bool IsLayerVisible(LayerID layerId) const override
            {
                return !m_vp.IsLayerFrozen(layerId) && m_target.IsLayerVisible(layerId);
            }

            double GetWorldPerPixel() const override
            {
                return m_scale > 0.0 ? m_target.GetWorldPerPixel() / m_scale : 0.0;
            }

            double GetViewportHeightPx() const override
            {
                const double wpp = m_target.GetWorldPerPixel();
                return wpp > 0.0 ? m_paperHeight / wpp : 0.0;
            }

            void MarkViewDependent() override {}
            bool BeginBlockInstance(const void*, const EntityAttr&, const Math::Mat4&, const BlockClipChain*) override
            {
                return false;
            }
            void EndBlockInstance() override {}

        private:
            IDrawSink&            m_target;
            const ViewportEntity& m_vp;
            double                m_scale;
            double                m_paperHeight;
        };

        bool LeafPassesFilter(const Entity& e,
                              LayerID effectiveLayerId,
                              const LayerManager& lm,
                              const CompiledFilter& filter,
                              TessellateStats& stats)
        {
            if (filter.ExcludeFrames && IsModificationFrame(e))
            {
                ++stats.FramesExcluded;
                ++stats.EntitiesFiltered;
                return false;
            }
            if (!filter.NeedsLayerCheck)
                return true;

            // 显式 --include-layers / --exclude-layers 对一切实体生效；
            // 默认 ExcludeAnnotations 属于内容去噪，WipeoutEntity 作为几何擦除遮罩同时豁免实体类型与注记图层名拦截（CAD-R2-07）
            const bool isWipeout = e.IsKindOf<WipeoutEntity>();
            if (filter.ExcludeAnnotations && !isWipeout && IsAnnotationEntity(e))
            {
                ++stats.EntitiesFiltered;
                return false;
            }
            const CompiledFilter::LayerDecision d = filter.CheckLayer(lm, effectiveLayerId);
            if ((filter.ExcludeAnnotations && !isWipeout && d.AnnotationLayer)
                || !d.IncludeOk
                || d.ExcludeHit)
            {
                ++stats.EntitiesFiltered;
                return false;
            }
            return true;
        }

        // 递归细分实体（顶层或块内嵌套）：在块展开时精确执行图层/注记/修改框过滤，
        // INSERT 容器本身不受 include-layers 拦截，叶子实体按解析后的图层名判定。
        void DrawEntityRecursive(const Entity& entity,
                                 IDrawSink& sink,
                                 const LayerManager& lm,
                                 const CompiledFilter& filter,
                                 TessellateStats& stats,
                                 bool topLevel,
                                 int depth = 0)
        {
            if (depth > 16)
                return;
            if (!entity.GetAttr().Visible)
                return;

            if (const auto* ins = dynamic_cast<const InsertEntity*>(&entity))
            {
                const BlockEntity* blk = ins->GetBlock();
                if (!blk)
                    return;

                // 未解析 XREF：按自身图层过滤后绘制占位标注
                if (blk->IsUnresolvedXRef())
                {
                    if (!sink.IsLayerVisible(ins->GetAttr().LayerId))
                        return;
                    if (!LeafPassesFilter(*ins, ins->GetAttr().LayerId, lm, filter, stats))
                        return;
                    if (topLevel) sink.BeginEntity(&ins->GetAttr());
                    else          sink.SetEntityContext(&ins->GetAttr());
                    ins->Draw(sink, false, false);
                    if (topLevel) sink.BeginEntity(nullptr);
                    else          sink.SetEntityContext(nullptr);
                    ++stats.EntitiesRendered;
                    return;
                }

                // 即使 INSERT 所在层关闭，块内具名图层上的子实体仍按自身开关显示（与 AutoCAD 一致）
                if (!ins->HasVisibleContentOnAnyLayer([&](LayerID id) { return sink.IsLayerVisible(id); }))
                    return;

                const auto& children = ins->GetResolvedChildren();
                if (topLevel)
                    sink.BeginEntity(&ins->GetAttr());

                const int rows = std::max(1, ins->GetRowCount());
                const int cols = std::max(1, ins->GetColumnCount());
                for (int r = 0; r < rows; ++r)
                {
                    for (int c = 0; c < cols; ++c)
                    {
                        const Math::Mat4 cell = ins->CellMatrix(c, r);
                        TransformDrawSink tsink(sink, cell);
                        ClipDrawSink csink(tsink, ins->GetClipBoundary());
                        const bool clipped = csink.Valid() && !ins->ShowWholeWhileDraggingBase();
                        IDrawSink& xsink   = clipped ? static_cast<IDrawSink&>(csink) : tsink;

                        for (const auto& child : children)
                        {
                            if (!child || !child->GetAttr().Visible)
                                continue;
                            if (const auto* def = dynamic_cast<const AttDefEntity*>(child.get()))
                            {
                                if (!def->ShowsInBlockReference())
                                    continue;
                            }
                            if (child->IsKindOf<InsertEntity>())
                            {
                                DrawEntityRecursive(*child, xsink, lm, filter, stats, /*topLevel*/ false, depth + 1);
                            }
                            else
                            {
                                if (!xsink.IsLayerVisible(child->GetAttr().LayerId))
                                    continue;
                                if (!LeafPassesFilter(*child, child->GetAttr().LayerId, lm, filter, stats))
                                    continue;
                                xsink.SetEntityContext(&child->GetAttr());
                                child->Draw(xsink, false, false);
                                xsink.SetEntityContext(nullptr);
                                ++stats.EntitiesRendered;
                            }
                        }
                    }
                }

                // 随附属性 ATTRIB 已在 WCS，不经 CellMatrix 变换
                const EntityAttr& host = ins->GetAttr();
                const LayerID hostLayer = host.LayerId;
                for (const auto& a : ins->GetAttribs())
                {
                    if (!a || !a->GetAttr().Visible)
                        continue;
                    const EntityAttr& aAttr = a->GetAttr();
                    const LayerID aLayer = (aAttr.LayerId == 0u) ? hostLayer : aAttr.LayerId;
                    if (!sink.IsLayerVisible(aLayer))
                        continue;
                    if (!LeafPassesFilter(*a, aLayer, lm, filter, stats))
                        continue;

                    auto* mutableA = const_cast<AttribEntity*>(a.get());
                    const EntityAttr saved = aAttr;
                    EntityAttr eff = saved;
                    if (eff.Color.Method == ColorMethod::ByBlock) eff.Color      = host.Color;
                    if (eff.Lineweight == Lineweight::ByBlock)    eff.Lineweight = host.Lineweight;
                    if (eff.LineType == LineTypeTable::ByBlockID) eff.LineType   = host.LineType;
                    eff.LayerId = aLayer;
                    mutableA->SetAttr(eff);
                    sink.SetEntityContext(&eff);
                    a->Draw(sink, false, false);
                    sink.SetEntityContext(nullptr);
                    mutableA->SetAttr(saved);
                    ++stats.EntitiesRendered;
                }

                if (topLevel)
                    sink.BeginEntity(nullptr);
                return;
            }

            // 普通叶子实体
            if (!sink.IsLayerVisible(entity.GetAttr().LayerId))
                return;
            if (!LeafPassesFilter(entity, entity.GetAttr().LayerId, lm, filter, stats))
                return;

            if (topLevel) sink.BeginEntity(&entity.GetAttr());
            else          sink.SetEntityContext(&entity.GetAttr());
            entity.Draw(sink, false, false);
            if (topLevel) sink.BeginEntity(nullptr);
            else          sink.SetEntityContext(nullptr);
            ++stats.EntitiesRendered;
        }

        // 将本顶层实体产生的 Wipeout 记录到批处理器（O(1) 收集，整趟结束后一次空间网格批量裁剪）
        void RecordEntityWipes(DrawContext& ctx,
                               WipeClip::Batch& wipeBatch,
                               size_t prevLines, size_t prevFills, size_t prevPoints)
        {
            const auto wipes = ctx.TakeWipes();
            for (const auto& polyPts : wipes)
                wipeBatch.Add(polyPts, { prevLines, prevFills, prevPoints });
        }

        void FlushWipeBatch(DrawContext& ctx,
                            WipeClip::Batch& wipeBatch,
                            size_t fromLines, size_t fromFills, size_t fromPoints)
        {
            if (wipeBatch.Empty())
                return;
            wipeBatch.ClipLines(ctx.Lines(), 0, fromLines);
            wipeBatch.ClipFills(ctx.Fills(), 1, 5, fromFills);
            wipeBatch.ClipPoints(ctx.Points(), 2, fromPoints);
            wipeBatch.Clear();
        }

        // 将模型空间内容合成到图纸空间的浮动视口中（含 VPLAYER 冻结层过滤、R(+Twist) 变换与视口矩形裁剪）
        void ComposeModelIntoViewportHeadless(const Scene& scene,
                                              const ViewportEntity& vp,
                                              DrawContext& ctx,
                                              const CompiledFilter& filter,
                                              TessellateStats& stats)
        {
            const AABB visible = vp.ModelVisibleBox();
            const double s = vp.ModelToPaperScale();
            if (!(s > 0.0) || !std::isfinite(s))
                return;

            const double savedLtScale = ctx.GetLineTypeScale();
            if (!scene.GetPaperSpaceLineTypeScaling())
                ctx.SetLineTypeScale(savedLtScale * s);

            const size_t clipLineFrom  = ctx.LineCount();
            const size_t clipFillFrom  = ctx.FillCount();
            const size_t clipTextFrom  = ctx.TextCount();
            const size_t clipPointFrom = ctx.PointCount();

            using namespace Math;
            Mat4 m = Mat4::Translation(Vec3{ -vp.GetViewCenter().x, -vp.GetViewCenter().y, 0.0 })
                   * Mat4::Scale(Vec3{ s, s, 1.0 });
            if (vp.GetTwist() != 0.0)
                m = m * Mat4::RotationZ(vp.GetTwist());
            m = m * Mat4::Translation(Vec3{ vp.GetCenter().x, vp.GetCenter().y, 0.0 });

            ViewportComposeSink vsink(ctx, m, vp);
            const LayerManager& lm = scene.GetLayerManager();
            WipeClip::Batch vpWipes(3);

            scene.ForEachObject([&](const Object& obj) {
                if (!obj.IsKindOf<Entity>())
                    return;
                const auto& e = static_cast<const Entity&>(obj);
                if (e.GetAttr().InPaperSpace || !e.GetAttr().Visible || e.IsKindOf<ViewportEntity>())
                    return;
                if (!e.IsKindOf<InsertEntity>())
                {
                    if (!vsink.IsLayerVisible(e.GetAttr().LayerId))
                        return;
                }
                if (!IsUnboundedEntity(e))
                {
                    const AABB box = e.GetBoundingBox();
                    if (IsFiniteValidBox(box) && !BoxesIntersect2D(box, visible))
                        return;
                }
                const size_t prevLines  = ctx.LineCount();
                const size_t prevFills  = ctx.FillCount();
                const size_t prevPoints = ctx.PointCount();
                DrawEntityRecursive(e, vsink, lm, filter, stats, /*topLevel*/ true, 0);
                RecordEntityWipes(ctx, vpWipes, prevLines, prevFills, prevPoints);
            });

            FlushWipeBatch(ctx, vpWipes, clipLineFrom, clipFillFrom, clipPointFrom);
            ctx.SetLineTypeScale(savedLtScale);

            // 视口矩形裁剪 [from, end)
            const double hw = vp.GetWidth() * 0.5;
            const double hh = vp.GetHeight() * 0.5;
            const Math::Point3 vc = vp.GetCenter();
            const Math::Point3& o = ctx.GetRenderOrigin();
            const WipeClip::Poly clipRect({
                Math::Point3{ vc.x - hw - o.x, vc.y - hh - o.y, vc.z - o.z },
                Math::Point3{ vc.x + hw - o.x, vc.y - hh - o.y, vc.z - o.z },
                Math::Point3{ vc.x + hw - o.x, vc.y + hh - o.y, vc.z - o.z },
                Math::Point3{ vc.x - hw - o.x, vc.y + hh - o.y, vc.z - o.z },
            });
            if (clipRect.Valid())
            {
                WipeClip::ClipLines(ctx.Lines(), clipRect, clipLineFrom, /*keepInside*/ true);
                WipeClip::ClipFills(ctx.Fills(), clipRect, 5, clipFillFrom, /*keepInside*/ true);
                WipeClip::ClipTexts(ctx.Texts(), clipRect, clipTextFrom);
                WipeClip::ClipPoints(ctx.Points(), clipRect, clipPointFrom, /*keepInside*/ true);
            }
        }
    }

    bool ResolveFontsDirectoryStrict(const std::string& explicitDir,
                                     bool explicitGiven,
                                     FontDirResolution& out,
                                     std::string* error)
    {
        namespace fs = std::filesystem;
        out = {};
        auto isAccessibleDir = [](const fs::path& p, std::string* reason) -> bool {
            if (p.empty())
            {
                if (reason) *reason = "路径为空";
                return false;
            }
            std::error_code ec;
            if (!fs::exists(p, ec) || ec)
            {
                if (reason) *reason = ec ? ec.message() : "目录不存在";
                return false;
            }
            if (!fs::is_directory(p, ec) || ec)
            {
                if (reason) *reason = ec ? ec.message() : "路径不是目录";
                return false;
            }
            // 实际打开目录迭代器，确认目录具备读取权限而非拒绝访问
            fs::directory_iterator it(p, ec);
            (void)it;
            if (ec)
            {
                if (reason) *reason = ec.message();
                return false;
            }
            return true;
        };

        const bool isExplicit = explicitGiven || !explicitDir.empty();
        if (isExplicit)
        {
            out.Explicit = true;
            std::string reason;
            const fs::path p = MiniCAD::Utf8ToPath(explicitDir);
            if (!isAccessibleDir(p, &reason))
            {
                if (error)
                {
                    *error = "指定的 --fonts-dir 字体目录不存在或无法访问：" + explicitDir
                           + (reason.empty() ? "" : ("（" + reason + "）"));
                }
                return false;
            }
            std::error_code ec;
            const fs::path canon = fs::weakly_canonical(p, ec);
            out.ResolvedDir  = EnsureTrailingSlash(PathToUtf8String(ec ? p : canon));
            out.UsedFallback = false;
            return true;
        }

        const fs::path exeDir = GetExecutableDir();
        const std::vector<fs::path> candidates = {
            exeDir / "fonts",
            exeDir / ".." / "assets" / "fonts",
            exeDir / ".." / ".." / "assets" / "fonts",
            exeDir / ".." / ".." / ".." / "assets" / "fonts",
            exeDir / "assets" / "fonts",
            fs::current_path() / "assets" / "fonts",
            fs::current_path() / "fonts",
        };
        for (const auto& c : candidates)
        {
            if (isAccessibleDir(c, nullptr))
            {
                std::error_code ec;
                const fs::path canon = fs::weakly_canonical(c, ec);
                out.ResolvedDir  = EnsureTrailingSlash(PathToUtf8String(ec ? c : canon));
                out.UsedFallback = false;
                return true;
            }
        }
        out.UsedFallback = true;
        return true;
    }

    std::string ResolveFontsDirectory(const std::string& explicitDir)
    {
        FontDirResolution res;
        if (!ResolveFontsDirectoryStrict(explicitDir, !explicitDir.empty(), res, nullptr))
            return {};
        return res.ResolvedDir;
    }

    bool InitHeadlessFontSystem(FontSystem& fontSystem,
                                const std::string& explicitFontsDir,
                                bool explicitGiven,
                                FontDirResolution* resolutionOut,
                                std::string* error)
    {
        FontDirResolution res;
        if (!ResolveFontsDirectoryStrict(explicitFontsDir, explicitGiven, res, error))
            return false;

        if (!fontSystem.IsReady())
            fontSystem.Initialize();

        if (!res.ResolvedDir.empty())
            fontSystem.GetFontEngine().SetFontDir(res.ResolvedDir);

#ifdef _WIN32
        fontSystem.GetFontEngine().AddSearchDir("C:\\Windows\\Fonts\\");
#endif
        if (resolutionOut)
            *resolutionOut = std::move(res);
        return true;
    }

    int ResolveLayoutIndex(const Scene& scene, const std::string& layoutSpec, std::string* error)
    {
        const auto& layouts = scene.Layouts();
        if (layouts.empty())
        {
            if (error) *error = "场景中不存在可用布局";
            return -1;
        }

        const std::string low = ToLowerAscii(layoutSpec);
        if (layoutSpec.empty() || low == "model" || layoutSpec == "模型")
        {
            const int idx = scene.ModelLayoutIndex();
            return idx >= 0 ? idx : 0;
        }

        // index:<N>
        if (low.size() > 6 && low.rfind("index:", 0) == 0)
        {
            try
            {
                size_t pos = 0;
                const std::string numStr = layoutSpec.substr(6);
                const int idx = std::stoi(numStr, &pos);
                if (pos == numStr.size() && idx >= 0 && idx < static_cast<int>(layouts.size()))
                    return idx;
            }
            catch (...)
            {
            }
            if (error)
                *error = "布局索引越界或非法：" + layoutSpec + "（共 " + std::to_string(layouts.size()) + " 个布局）";
            return -1;
        }

        // 精确名称匹配
        for (size_t i = 0; i < layouts.size(); ++i)
        {
            if (layouts[i].Name == layoutSpec)
                return static_cast<int>(i);
        }
        // 不区分大小写名称匹配
        for (size_t i = 0; i < layouts.size(); ++i)
        {
            if (ToLowerAscii(layouts[i].Name) == low)
                return static_cast<int>(i);
        }

        if (error)
            *error = "未找到指定布局：" + layoutSpec;
        return -1;
    }

    int ResolveCompareLayoutIndex(const Scene& sceneA, int layoutIdxA,
                                  const Scene& sceneB, const std::string& layoutSpecB,
                                  std::string* error)
    {
        if (!layoutSpecB.empty())
            return ResolveLayoutIndex(sceneB, layoutSpecB, error);

        const auto& layoutsA = sceneA.Layouts();
        const auto& layoutsB = sceneB.Layouts();
        if (layoutIdxA < 0 || layoutIdxA >= static_cast<int>(layoutsA.size()))
        {
            if (error) *error = "A 侧布局索引无效";
            return -1;
        }
        if (layoutsA[static_cast<size_t>(layoutIdxA)].Model)
        {
            const int idx = sceneB.ModelLayoutIndex();
            return idx >= 0 ? idx : 0;
        }

        const std::string& targetName = layoutsA[static_cast<size_t>(layoutIdxA)].Name;
        for (size_t i = 0; i < layoutsB.size(); ++i)
        {
            if (!layoutsB[i].Model && layoutsB[i].Name == targetName)
                return static_cast<int>(i);
        }
        if (layoutIdxA < static_cast<int>(layoutsB.size()))
            return layoutIdxA;

        if (error)
            *error = "B 侧未找到与 A 侧对应的布局：" + targetName;
        return -1;
    }

    namespace
    {
        bool ComputeLayoutRawBounds(const Scene& scene, int layoutIdx, AABB& outBounds)
        {
            const auto& layouts = scene.Layouts();
            if (layoutIdx < 0 || layoutIdx >= static_cast<int>(layouts.size()))
                return false;

            const DrawingLayout& layout = layouts[static_cast<size_t>(layoutIdx)];
            const LayerManager& lm = scene.GetLayerManager();

            AABB bounds = AABB::Empty();
            bool hasAny = false;

            scene.ForEachObject([&](const Object& obj) {
                if (!obj.IsKindOf<Entity>())
                    return;
                const auto& e = static_cast<const Entity&>(obj);
                if (!e.GetAttr().Visible || IsUnboundedEntity(e))
                    return;

                if (layout.Model)
                {
                    if (e.GetAttr().InPaperSpace || e.IsKindOf<ViewportEntity>())
                        return;
                }
                else
                {
                    if (!e.GetAttr().InPaperSpace)
                        return;
                    if (layout.OwnerKey != 0 && e.GetAttr().LayoutKey != 0 && e.GetAttr().LayoutKey != layout.OwnerKey)
                        return;
                    // 图纸空间主视口（ShowsModel==false）代表整张纸张视角，不参与内容框扩展以免过大
                    if (const auto* vp = dynamic_cast<const ViewportEntity*>(&e))
                    {
                        if (!vp->ShowsModel())
                            return;
                    }
                }

                if (const auto* ins = dynamic_cast<const InsertEntity*>(&e))
                {
                    if (!ins->HasVisibleContentOnAnyLayer([&](LayerID id) {
                            const Layer* l = lm.GetLayer(id);
                            return !l || l->IsVisible();
                        }))
                        return;
                }
                else
                {
                    if (const Layer* l = lm.GetLayer(e.GetAttr().LayerId); l && !l->IsVisible())
                        return;
                }

                const AABB box = e.GetBoundingBox();
                if (!IsFiniteValidBox(box))
                    return;
                bounds.Expand(box);
                hasAny = true;
            });

            // 图纸空间若没有任何可见实体，则回退到布局纸张范围
            if (!hasAny && !layout.Model && layout.MaxX > layout.MinX && layout.MaxY > layout.MinY)
            {
                bounds.Min = { layout.MinX, layout.MinY, 0.0 };
                bounds.Max = { layout.MaxX, layout.MaxY, 0.0 };
                hasAny = true;
            }
            if (!hasAny || !IsFiniteValidBox(bounds))
                return false;

            outBounds = bounds;
            return true;
        }

        void FinalizeBboxFromBounds(const AABB& bounds, bool expandMargin, std::array<double, 4>& outBbox)
        {
            const double w = bounds.Max.x - bounds.Min.x;
            const double h = bounds.Max.y - bounds.Min.y;
            const double maxSpan = std::max(w, h);
            const double minPad  = maxSpan > 1e-6 ? maxSpan * 0.02 : 1.0;

            const double padX = expandMargin ? std::max(w * 0.02, w < 1e-6 ? minPad : 0.0) : (w < 1e-6 ? minPad : 0.0);
            const double padY = expandMargin ? std::max(h * 0.02, h < 1e-6 ? minPad : 0.0) : (h < 1e-6 ? minPad : 0.0);

            outBbox = {
                bounds.Min.x - padX,
                bounds.Min.y - padY,
                bounds.Max.x + padX,
                bounds.Max.y + padY
            };
        }
    }

    bool ComputeLayoutBbox(const Scene& scene, int layoutIdx, std::array<double, 4>& outBbox, bool expandMargin)
    {
        AABB bounds = AABB::Empty();
        if (!ComputeLayoutRawBounds(scene, layoutIdx, bounds))
            return false;
        FinalizeBboxFromBounds(bounds, expandMargin, outBbox);
        return true;
    }

    bool ComputeCompareUnionBbox(const Scene& sceneA, int layoutIdxA,
                                 const Scene& sceneB, int layoutIdxB,
                                 double dxBToA, double dyBToA,
                                 std::array<double, 4>& outBbox,
                                 bool expandMargin)
    {
        AABB boundsA = AABB::Empty();
        AABB boundsB = AABB::Empty();
        const bool hasA = ComputeLayoutRawBounds(sceneA, layoutIdxA, boundsA);
        const bool hasB = ComputeLayoutRawBounds(sceneB, layoutIdxB, boundsB);
        if (!hasA && !hasB)
            return false;

        AABB uni = AABB::Empty();
        if (hasA)
            uni.Expand(boundsA);
        if (hasB)
        {
            AABB shiftedB;
            shiftedB.Min = { boundsB.Min.x + dxBToA, boundsB.Min.y + dyBToA, boundsB.Min.z };
            shiftedB.Max = { boundsB.Max.x + dxBToA, boundsB.Max.y + dyBToA, boundsB.Max.z };
            uni.Expand(shiftedB);
        }
        if (!IsFiniteValidBox(uni))
            return false;

        FinalizeBboxFromBounds(uni, expandMargin, outBbox);
        return true;
    }

    int CountModelViewportsInLayout(const Scene& scene, int layoutIdx)
    {
        const auto& layouts = scene.Layouts();
        if (layoutIdx < 0 || layoutIdx >= static_cast<int>(layouts.size()))
            return 0;
        const DrawingLayout& layout = layouts[static_cast<size_t>(layoutIdx)];
        if (layout.Model)
            return 0;

        int count = 0;
        scene.ForEachObject([&](const Object& obj) {
            if (!obj.IsKindOf<ViewportEntity>())
                return;
            const auto& vp = static_cast<const ViewportEntity&>(obj);
            if (!vp.GetAttr().InPaperSpace || !vp.GetAttr().Visible || !vp.ShowsModel())
                return;
            if (layout.OwnerKey != 0 && vp.GetAttr().LayoutKey != 0 && vp.GetAttr().LayoutKey != layout.OwnerKey)
                return;
            ++count;
        });
        return count;
    }

    bool IsModificationFrame(const Entity& entity)
    {
        if (entity.IsKindOf<WipeoutEntity>())
            return false;
        const auto* pl = dynamic_cast<const PolylineEntity*>(&entity);
        if (!pl)
            return false;

        const Polyline& poly = pl->GetPolyline();
        if (poly.Points.size() < 4)
            return false;

        // 闭合判定：末点与起点重合
        const Math::Point3& first = poly.Points.front();
        const Math::Point3& last  = poly.Points.back();
        const double closeDist = std::hypot(last.x - first.x, last.y - first.y);
        if (closeDist > 1e-2)
            return false;

        // 恒定线宽 >= 50
        if (pl->MaxEffectiveWidth() < 50.0)
            return false;

        // 顶点数 <= 60（扣除重复闭合尾点）
        const size_t distinctVerts = poly.Points.size() - 1;
        if (distinctVerts > 60)
            return false;

        // 长边 >= 2000
        const AABB box = poly.GetBounds();
        if (!IsFiniteValidBox(box))
            return false;
        const double longEdge = std::max(box.Max.x - box.Min.x, box.Max.y - box.Min.y);
        if (longEdge < 2000.0)
            return false;

        // 非零凸度 >= 2（圆角矩形 / 云线修改框典型特征）
        int nonZeroBulges = 0;
        for (double b : poly.Bulges)
        {
            if (std::abs(b) > 1e-6)
                ++nonZeroBulges;
        }
        return nonZeroBulges >= 2;
    }

    bool ValidateRegexPattern(const std::string& pattern, std::string* error)
    {
        RegexMatcher m;
        return m.Compile(pattern, error);
    }

    bool RegexSearchUtf8Icase(const std::string& text, const std::string& pattern)
    {
        RegexMatcher m;
        if (!m.Compile(pattern, nullptr))
            return false;
        return m.Matches(text);
    }

    bool TessellateSceneLayout(const Scene& scene,
                               int layoutIdx,
                               const std::array<double, 4>& bbox,
                               double worldPerPixel,
                               double viewportHeightPx,
                               const TessellateFilter& filter,
                               FontSystem& fontSystem,
                               TessellatedScene& out,
                               std::string* error)
    {
        const auto& layouts = scene.Layouts();
        if (layoutIdx < 0 || layoutIdx >= static_cast<int>(layouts.size()))
        {
            if (error) *error = "无效布局索引";
            return false;
        }
        if (!(bbox[2] > bbox[0]) || !(bbox[3] > bbox[1]))
        {
            if (error) *error = "无效裁剪包围盒（xmax<=xmin 或 ymax<=ymin）";
            return false;
        }

        CompiledFilter compiled;
        if (!compiled.Init(filter, error))
            return false;

        out = {};
        const Math::Point3 renderOrigin{
            0.5 * (bbox[0] + bbox[2]),
            0.5 * (bbox[1] + bbox[3]),
            0.0
        };
        out.RenderOrigin = renderOrigin;

        const double spanW = bbox[2] - bbox[0];
        const double spanH = bbox[3] - bbox[1];
        const double pad   = std::max(500.0, 0.05 * std::max(spanW, spanH));
        const AABB cullBox{
            { bbox[0] - pad, bbox[1] - pad, -1e18 },
            { bbox[2] + pad, bbox[3] + pad,  1e18 }
        };

        std::vector<Vertex_P3_C4_UV> dummyTextVerts;
        Overlay dummyOverlay;

        DrawContext ctx(out.Lines,
                        out.Fills,
                        dummyTextVerts,
                        dummyOverlay,
                        scene.GetLayerManager(),
                        scene.GetLineTypeTable(),
                        worldPerPixel,
                        nullptr,
                        MakeTextStyleResolver(&fontSystem, &scene.GetTextStyleTable()));
        ctx.SetRenderOrigin(renderOrigin);
        ctx.SetViewportHeightPx(viewportHeightPx);
        ctx.SetPointStyle(scene.GetPointDisplayMode(), scene.GetPointDisplaySize());
        ctx.SetLineTypeScale(scene.GetLineTypeScale());
        ctx.SetTextStyleTable(&scene.GetTextStyleTable());
        ctx.SetInlineFontResolver(MakeInlineFontResolver(&fontSystem));
        ctx.SetShapeFontResolver(MakeShapeFontResolver(&fontSystem));
        // 无头单次光栅化固定在单一 worldPerPixel：长虚线按绘制序即时展开，确保块内/块间 Wipeout 遮罩
        // 与浮动视口矩形裁剪均严格遵循绘制序，并天然保留原始虚线相位（CAD-14）。
        ctx.SetDeferLongDashes(false);

        // 局部放大曲线加密，保证大弧度曲线在高清裁图下平滑无折角
        if (worldPerPixel > 0.0)
        {
            CurveRefinement refine;
            refine.box       = cullBox;
            refine.tolerance = worldPerPixel * 0.25;
            ctx.SetCurveRefinement(refine);
        }

        const DrawingLayout& layout = layouts[static_cast<size_t>(layoutIdx)];
        const LayerManager& lm      = scene.GetLayerManager();

        if (layout.Model)
        {
            WipeClip::Batch modelWipes(3);
            scene.ForEachObject([&](const Object& obj) {
                if (!obj.IsKindOf<Entity>())
                    return;
                const auto& e = static_cast<const Entity&>(obj);
                if (e.GetAttr().InPaperSpace || !e.GetAttr().Visible || e.IsKindOf<ViewportEntity>())
                    return;
                if (!IsUnboundedEntity(e))
                {
                    const AABB box = e.GetBoundingBox();
                    if (IsFiniteValidBox(box) && !BoxesIntersect2D(box, cullBox))
                        return;
                }
                const size_t prevLines  = ctx.LineCount();
                const size_t prevFills  = ctx.FillCount();
                const size_t prevPoints = ctx.PointCount();
                DrawEntityRecursive(e, ctx, lm, compiled, out.Stats, /*topLevel*/ true, 0);
                RecordEntityWipes(ctx, modelWipes, prevLines, prevFills, prevPoints);
            });
            FlushWipeBatch(ctx, modelWipes, 0, 0, 0);
        }
        else
        {
            // 图纸空间：先按视口合成模型内容，再绘纸面图元（保持纸面图签/标注压在视口之上）
            std::vector<const ViewportEntity*> viewports;
            std::vector<const Entity*>         paperEntities;
            scene.ForEachObject([&](const Object& obj) {
                if (!obj.IsKindOf<Entity>())
                    return;
                const auto& e = static_cast<const Entity&>(obj);
                if (!e.GetAttr().InPaperSpace || !e.GetAttr().Visible)
                    return;
                if (layout.OwnerKey != 0 && e.GetAttr().LayoutKey != 0 && e.GetAttr().LayoutKey != layout.OwnerKey)
                    return;
                if (const auto* vp = dynamic_cast<const ViewportEntity*>(&e))
                {
                    if (vp->ShowsModel())
                        viewports.push_back(vp);
                }
                else
                {
                    paperEntities.push_back(&e);
                }
            });

            for (const ViewportEntity* vp : viewports)
            {
                const AABB vpBox = vp->GetBoundingBox();
                if (IsFiniteValidBox(vpBox) && !BoxesIntersect2D(vpBox, cullBox))
                    continue;
                ComposeModelIntoViewportHeadless(scene, *vp, ctx, compiled, out.Stats);
                // 视口矩形边框（受图层可见性与过滤规则约束）
                if (ctx.IsLayerVisible(vp->GetAttr().LayerId)
                    && LeafPassesFilter(*vp, vp->GetAttr().LayerId, lm, compiled, out.Stats))
                {
                    ctx.BeginEntity(&vp->GetAttr());
                    vp->Draw(ctx, false, false);
                    ctx.BeginEntity(nullptr);
                    ++out.Stats.EntitiesRendered;
                }
            }

            WipeClip::Batch paperWipes(3);
            for (const Entity* e : paperEntities)
            {
                if (!IsUnboundedEntity(*e))
                {
                    const AABB box = e->GetBoundingBox();
                    if (IsFiniteValidBox(box) && !BoxesIntersect2D(box, cullBox))
                        continue;
                }
                const size_t prevLines  = ctx.LineCount();
                const size_t prevFills  = ctx.FillCount();
                const size_t prevPoints = ctx.PointCount();
                DrawEntityRecursive(*e, ctx, lm, compiled, out.Stats, /*topLevel*/ true, 0);
                RecordEntityWipes(ctx, paperWipes, prevLines, prevFills, prevPoints);
            }
            FlushWipeBatch(ctx, paperWipes, 0, 0, 0);
        }

        ctx.BeginEntity(nullptr);
        out.Points = ctx.TakePoints();
        return true;
    }
}
