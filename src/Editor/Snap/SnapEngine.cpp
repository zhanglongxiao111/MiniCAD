#include "SnapResult.h"
#include "SnapEngine.h"
#include "Core/Entity/LineEntity.hpp"
#include "Core/Entity/PointEntity.hpp"
#include "Core/Entity/CircleEntity.hpp"
#include "Core/Entity/RectangleEntity.hpp"
#include "Core/Entity/MLineEntity.hpp"
#include "Core/Entity/RegionEntity.hpp"
#include "Core/Entity/ArcEntity.hpp"
#include "Core/Entity/EllipseEntity.hpp"
#include "Core/Entity/PolylineEntity.hpp"
#include "Core/Entity/SplineEntity.hpp"
#include "Core/Entity/XLineEntity.hpp"
#include "Core/Entity/RayEntity.hpp"
#include "Core/Entity/Solid3DEntity.hpp"
#include "Core/Entity/InsertEntity.hpp"
#include "Core/Entity/ICurveEntity.hpp"
#include "Core/GeomKernel/CurveIntersect.hpp"
#include "Document/FeaturePoints.h"
#include "Core/Object/Object.hpp"
#include "Core/Math/Point3.hpp"
#include "Core/Math/Constants.hpp"
#include "Scene/Scene.h"
#include "Viewport/Camera.h"
#include "Core/Math/MathUtils.hpp"
#include <cmath>
#include <algorithm>
#include <limits>
#include <memory>
#include <vector>
#include <unordered_set>

namespace MiniCAD
{
    // =========================================================================
    // 内部工具：屏幕距离比较辅助 + 块参照 / 外部参照孔径展开
    // =========================================================================
    namespace
    {
        // 隐藏图层(或自身不可见)的实体不参与捕捉;锁定图层仍可捕捉(同 AutoCAD)。
        inline bool IsSnapVisible(const Scene& scene, const Object& obj)
        {
            if (!obj.IsKindOf<Entity>()) return true;

            const auto& attr = static_cast<const Entity&>(obj).GetAttr();
            if (!attr.Visible) return false;

            if (const Layer* layer = scene.GetLayerManager().GetLayer(attr.LayerId))
                if (!layer->IsVisible())
                    return false;

            return true;
        }

        inline bool IsLayerVisibleById(const LayerManager& layers, LayerID id)
        {
            const Layer* l = layers.GetLayer(id);
            return !l || l->IsVisible();
        }

        // 正交投影下局部包围盒经 m 变换后的屏幕外包框是否与光标孔径相交
        inline bool BoxHitsAperture(const AABB& lb, const Math::Mat4& m, const Camera& cam, const Math::Point2& sp, double radiusPx)
        {
            if (lb.Min.x > lb.Max.x) return false;
            const Math::Point3 corners[8] = {
                { lb.Min.x, lb.Min.y, lb.Min.z }, { lb.Max.x, lb.Min.y, lb.Min.z },
                { lb.Min.x, lb.Max.y, lb.Min.z }, { lb.Max.x, lb.Max.y, lb.Min.z },
                { lb.Min.x, lb.Min.y, lb.Max.z }, { lb.Max.x, lb.Min.y, lb.Max.z },
                { lb.Min.x, lb.Max.y, lb.Max.z }, { lb.Max.x, lb.Max.y, lb.Max.z },
            };
            double x0 = std::numeric_limits<double>::max(), y0 = x0;
            double x1 = std::numeric_limits<double>::lowest(), y1 = x1;
            for (const auto& c : corners)
            {
                const Math::Point2 s = cam.WorldToScreen(m * c);
                x0 = std::min(x0, s.x); x1 = std::max(x1, s.x);
                y0 = std::min(y0, s.y); y1 = std::max(y1, s.y);
            }
            return sp.x >= x0 - radiusPx && sp.x <= x1 + radiusPx
                && sp.y >= y0 - radiusPx && sp.y <= y1 + radiusPx;
        }

        constexpr size_t kMaxBlockLeaves         = 1024;
        constexpr size_t kMaxIntersectCandidates = 64;

        // 裁剪边界跨边时收集被截断的线段为世界 LineEntity
        class ClippedSegmentSink final : public IDrawSink
        {
        public:
            ClippedSegmentSink(const LayerManager& layers, std::vector<std::unique_ptr<Entity>>& out, size_t limit = kMaxBlockLeaves)
                : m_layers(layers), m_out(out), m_limit(limit) {}

            bool IsLayerVisible(LayerID id) const override { return IsLayerVisibleById(m_layers, id); }
            void DrawLine(const Math::Point3& a, const Math::Point3& b, const Math::Color4&, bool) override
            {
                if (m_out.size() >= m_limit)
                    return;
                if ((b - a).LengthSq() > 1e-24)
                    m_out.push_back(std::make_unique<LineEntity>(Object::InvalidID, a, b));
            }
            void DrawPoint(const Math::Point3& p, const Math::Color4&, bool) override
            {
                if (m_out.size() >= m_limit)
                    return;
                m_out.push_back(std::make_unique<PointEntity>(Object::InvalidID, p));
            }

        private:
            const LayerManager&                   m_layers;
            std::vector<std::unique_ptr<Entity>>& m_out;
            size_t                                m_limit = kMaxBlockLeaves;
        };

        void EmitTransformedPrimitive(const Entity& src, const Math::Mat4& m, std::vector<std::unique_ptr<Entity>>& out)
        {
            if (out.size() >= kMaxBlockLeaves)
                return;
            auto pushLeaf = [&](std::unique_ptr<Entity> leaf) -> bool
            {
                if (!leaf || out.size() >= kMaxBlockLeaves)
                    return false;
                out.push_back(std::move(leaf));
                return true;
            };

            const Math::Vec3 ux = m.TransformVector({ 1.0, 0.0, 0.0 });
            const Math::Vec3 uy = m.TransformVector({ 0.0, 1.0, 0.0 });
            const double lx = std::hypot(ux.x, ux.y);
            const double ly = std::hypot(uy.x, uy.y);
            const double crossZ = ux.x * uy.y - ux.y * uy.x;
            const bool uniformXY = lx > 1e-12
                && std::abs(lx - ly) <= 1e-5 * std::max(lx, ly)
                && std::abs(ux.x * uy.x + ux.y * uy.y) <= 1e-5 * lx * ly;

            if (src.IsKindOf<PointEntity>())
            {
                pushLeaf(std::make_unique<PointEntity>(Object::InvalidID, m * static_cast<const PointEntity&>(src).GetPoint().Position));
                return;
            }
            if (src.IsKindOf<LineEntity>())
            {
                const auto& l = static_cast<const LineEntity&>(src).GetLine();
                pushLeaf(std::make_unique<LineEntity>(Object::InvalidID, m * l.Start, m * l.End));
                return;
            }
            if (src.IsKindOf<RectangleEntity>())
            {
                const auto& r = static_cast<const RectangleEntity&>(src).GetRectangle();
                pushLeaf(std::make_unique<PolylineEntity>(Object::InvalidID,
                    std::vector<Math::Point3>{ m * r.P1, m * r.P2, m * r.P3, m * r.P4, m * r.P1 }));
                return;
            }
            if (src.IsKindOf<PolylineEntity>())
            {
                const auto& pl = static_cast<const PolylineEntity&>(src).GetPolyline();
                if (!pl.IsValid()) return;
                bool hasArc = false;
                for (int i = 0; i < pl.SegCount(); ++i)
                    if (pl.SegIsArc(i)) { hasArc = true; break; }
                if (!hasArc || uniformXY)
                {
                    std::vector<Math::Point3> pts;
                    pts.reserve(pl.Points.size());
                    for (const auto& p : pl.Points) pts.push_back(m * p);
                    std::vector<double> bulges = pl.Bulges;
                    if (hasArc && crossZ < 0.0)
                        for (double& b : bulges) b = -b;
                    pushLeaf(std::make_unique<PolylineEntity>(Object::InvalidID, std::move(pts), std::move(bulges)));
                }
                else
                {
                    auto tess = pl.Tessellate();
                    for (auto& p : tess) p = m * p;
                    if (tess.size() >= 2)
                        pushLeaf(std::make_unique<PolylineEntity>(Object::InvalidID, std::move(tess)));
                }
                return;
            }
            if (src.IsKindOf<CircleEntity>())
            {
                const auto& c = static_cast<const CircleEntity&>(src).GetCircle();
                if (uniformXY)
                {
                    pushLeaf(std::make_unique<CircleEntity>(Object::InvalidID, m * c.Center, c.Radius * lx));
                }
                else if (lx > 1e-12 && ly > 1e-12)
                {
                    if (!pushLeaf(std::make_unique<PointEntity>(Object::InvalidID, m * c.Center)))
                        return;
                    if (out.size() >= kMaxBlockLeaves)
                        return;
                    std::vector<Math::Point3> pts;
                    pts.reserve(65);
                    for (int k = 0; k <= 64; ++k)
                    {
                        const double a = Math::TwoPI * k / 64.0;
                        pts.push_back(m * Math::Point3{ c.Center.x + c.Radius * std::cos(a), c.Center.y + c.Radius * std::sin(a), c.Center.z });
                    }
                    pushLeaf(std::make_unique<PolylineEntity>(Object::InvalidID, std::move(pts)));
                }
                return;
            }
            if (src.IsKindOf<ArcEntity>())
            {
                const auto& arc = static_cast<const ArcEntity&>(src).GetArc();
                if (uniformXY)
                {
                    if (auto wa = Arc::FromThreePoints(m * arc.StartPoint(), m * arc.MidPoint(), m * arc.EndPoint()))
                    {
                        pushLeaf(std::make_unique<ArcEntity>(Object::InvalidID, wa->Center, wa->Radius, wa->StartAngle, wa->EndAngle));
                        return;
                    }
                }
                if (!pushLeaf(std::make_unique<PointEntity>(Object::InvalidID, m * arc.Center)))
                    return;
                if (out.size() >= kMaxBlockLeaves)
                    return;
                const double sweep = arc.SweepAngle();
                const int segs = std::max(8, static_cast<int>(std::ceil(std::abs(sweep) / (Math::TwoPI / 64.0))));
                std::vector<Math::Point3> pts;
                pts.reserve(static_cast<size_t>(segs) + 1);
                for (int k = 0; k <= segs; ++k)
                    pts.push_back(m * arc.PointAt(arc.StartAngle + sweep * k / segs));
                pushLeaf(std::make_unique<PolylineEntity>(Object::InvalidID, std::move(pts)));
                return;
            }
            if (src.IsKindOf<MLineEntity>())
            {
                for (const auto& el : static_cast<const MLineEntity&>(src).ComputeGeometry().Elements)
                {
                    if (out.size() >= kMaxBlockLeaves)
                        return;
                    if (el.size() < 2) continue;
                    std::vector<Math::Point3> pts;
                    pts.reserve(el.size());
                    for (const auto& p : el) pts.push_back(m * p);
                    if (!pushLeaf(std::make_unique<PolylineEntity>(Object::InvalidID, std::move(pts))))
                        return;
                }
                return;
            }
            if (src.IsKindOf<XLineEntity>())
            {
                const auto& xl = static_cast<const XLineEntity&>(src).GetXLine();
                const Math::Vec3 d = m.TransformVector(xl.Direction);
                if (d.LengthSq() > 1e-24)
                    pushLeaf(std::make_unique<XLineEntity>(Object::InvalidID, m * xl.Origin, d.Normalized()));
                return;
            }
            if (src.IsKindOf<RayEntity>())
            {
                const auto& r = static_cast<const RayEntity&>(src).GetRay();
                const Math::Vec3 d = m.TransformVector(r.Direction);
                if (d.LengthSq() > 1e-24)
                    pushLeaf(std::make_unique<RayEntity>(Object::InvalidID, m * r.Origin, d.Normalized()));
                return;
            }
        }

        void CollectInsertLeaves(const InsertEntity& ins,
                                 const Math::Mat4& parentMat,
                                 LayerID hostLayerId,
                                 const Scene& scene,
                                 const Camera& cam,
                                 const Math::Point2& sp,
                                 double radiusPx,
                                 int depth,
                                 std::vector<std::unique_ptr<Entity>>& out)
        {
            if (depth > 8 || out.size() >= kMaxBlockLeaves) return;
            const BlockEntity* block = ins.GetBlock();
            if (!block || block->IsUnresolvedXRef()) return;

            const LayerManager& layers = scene.GetLayerManager();
            const bool hasClip = ins.HasClipBoundary();

            for (int r = 0; r < ins.GetRowCount() && out.size() < kMaxBlockLeaves; ++r)
                for (int c = 0; c < ins.GetColumnCount() && out.size() < kMaxBlockLeaves; ++c)
                {
                    const Math::Mat4 worldMat = ins.CellMatrix(c, r) * parentMat;
                    if (!ins.IsBoundless() && !BoxHitsAperture(block->GetBoundingBox(), worldMat, cam, sp, radiusPx))
                        continue;

                    ClippedSegmentSink rawSink(layers, out, kMaxBlockLeaves);
                    TransformDrawSink  tsink(rawSink, worldMat);
                    ClipDrawSink       csink(tsink, ins.GetClipBoundary());
                    const bool useClip = hasClip && csink.Valid();

                    for (const auto& child : block->GetEntities())
                    {
                        if (out.size() >= kMaxBlockLeaves)
                            return;
                        if (!child || !child->GetAttr().Visible) continue;
                        const LayerID effLayer = child->GetAttr().LayerId == Layer::DefaultLayerID
                            ? hostLayerId
                            : child->GetAttr().LayerId;
                        if (!IsLayerVisibleById(layers, effLayer)) continue;
                        if (const auto* def = dynamic_cast<const AttDefEntity*>(child.get()))
                            if (!def->ShowsInBlockReference()) continue;

                        const AABB cb = child->GetBoundingBox();
                        if (!child->IsBoundless() && !BoxHitsAperture(cb, worldMat, cam, sp, radiusPx))
                            continue;

                        ClipDrawSink::Region clipRegion = ClipDrawSink::Region::Inside;
                        if (useClip && !child->IsBoundless())
                        {
                            const Math::Point3 q[4] = {
                                { cb.Min.x, cb.Min.y, 0.0 }, { cb.Max.x, cb.Min.y, 0.0 },
                                { cb.Max.x, cb.Max.y, 0.0 }, { cb.Min.x, cb.Max.y, 0.0 },
                            };
                            clipRegion = csink.Classify(q);
                            if (clipRegion == ClipDrawSink::Region::Outside)
                                continue;
                        }

                        if (child->IsKindOf<InsertEntity>())
                        {
                            if (useClip && clipRegion == ClipDrawSink::Region::Crossing)
                                child->Draw(csink, false, false);
                            else
                                CollectInsertLeaves(static_cast<const InsertEntity&>(*child), worldMat, effLayer, scene, cam, sp, radiusPx, depth + 1, out);
                            continue;
                        }

                        if (useClip && clipRegion == ClipDrawSink::Region::Crossing)
                            child->Draw(csink, false, false);
                        else
                            EmitTransformedPrimitive(*child, worldMat, out);
                    }
                }
        }

        // 候选点与光标的屏幕距离，满足阈值则更新 best
        inline void TryUpdateBest(const Math::Point3& worldPt,
            const Math::Point2& sp,
            const Camera& cam,
            double snapRadiusPx,
            double& bestDist,
            SnapResult& best,
            SnapResult::Type type,
            Object::ObjectID id)
        {
            double d = Math::Distance(sp, cam.WorldToScreen(worldPt));
            if (d < snapRadiusPx && d < bestDist)
            {
                bestDist = d;
                best = { type, worldPt, id };
            }
        }
    }

    // =========================================================================
    // 主入口
    // =========================================================================
    SnapResult SnapEngine::Query(const Math::Point2& sp, const Scene& scene,
        const Camera& cam,
        const std::unordered_set<Object::ObjectID>& exclude,
        const Math::Point3* fromPoint,
        const std::vector<Object::ObjectID>* candidates) const
    {
        // 候选集仅在本次 Query 执行期间生效（各 Try* 经 ForEachSnapObject 消费）
        m_queryCandidates = candidates;
        m_blockLeaves.clear();

        // 展开候选中的块参照 / 外部参照（仅保留落在光标孔径内的可见叶图元）
        const double cullRadius = m_snapRadiusPx + 2.0;
        auto expandIfInsert = [&](const Object& obj)
        {
            if (m_blockLeaves.size() >= kMaxBlockLeaves) return;
            if (exclude.contains(obj.GetID()) || !IsSnapVisible(scene, obj)) return;
            if (!obj.IsKindOf<InsertEntity>()) return;
            const auto& ins = static_cast<const InsertEntity&>(obj);
            if (m_layoutFilter && !m_layoutFilter(ins.GetAttr())) return;
            CollectInsertLeaves(ins, Math::Mat4::Identity(), ins.GetAttr().LayerId, scene, cam, sp, cullRadius, 0, m_blockLeaves);
        };
        if (candidates)
        {
            for (auto id : *candidates)
                if (const Object* o = scene.GetEntity(id))
                    expandIfInsert(*o);
        }
        else
        {
            scene.ForEachObject(expandIfInsert);
        }

        // ── 点类捕捉（端点/中点/交点/象限/垂足）：同一孔径内按「捕捉点到
        //    光标的屏幕距离」取最近者，距离相同时按上述顺序优先 ──
        SnapResult best;
        double bestDist = std::numeric_limits<double>::max();

        auto consider = [&](SnapResult r)
        {
            if (!r.IsValid()) return;
            double d = Math::Distance(sp, cam.WorldToScreen(r.WorldPos));
            if (d < bestDist) { bestDist = d; best = r; }
        };

        if (IsModeEnabled(SnapMode::Endpoint))     consider(TryEndpoint(sp, scene, cam, exclude));
        if (IsModeEnabled(SnapMode::Midpoint))     consider(TryMidpoint(sp, scene, cam, exclude));
        if (IsModeEnabled(SnapMode::Intersection)) consider(TryIntersection(sp, scene, cam, exclude));
        if (IsModeEnabled(SnapMode::Quadrant))     consider(TryQuadrant(sp, scene, cam, exclude));
        if (IsModeEnabled(SnapMode::Perpendicular) && fromPoint)
            consider(TryPerpendicular(sp, scene, cam, exclude, *fromPoint));

        // ── 最近点：仅在点类捕捉均未命中时兜底(光标贴近曲线即生效) ──
        if (!best.IsValid() && IsModeEnabled(SnapMode::Nearest))
            best = TryNearest(sp, scene, cam, exclude);

        m_queryCandidates = nullptr;   // 候选集指针与块内叶图元仅本次查询有效
        m_blockLeaves.clear();

        if (best.IsValid()) return best;
        if (IsModeEnabled(SnapMode::Grid)) return TryGrid(sp, cam);
        return {};
    }

    // =========================================================================
    // TryEndpoint / TryMidpoint / TryQuadrant
    //
    // 各实体的端点 / 中点 / 象限点由 CollectFeaturePoints 枚举（Document/FeaturePoints），
    // 关联标注按同一套枚举的序号记录关联点。
    // =========================================================================
    SnapResult SnapEngine::TryFeature(const Math::Point2& sp, const Scene& scene, const Camera& cam,
        const std::unordered_set<Object::ObjectID>& exclude, int kind, SnapResult::Type type) const
    {
        SnapResult best;
        double bestDist = std::numeric_limits<double>::max();
        std::vector<Math::Point3> pts;

        ForEachSnapObject(scene, [&](const Object& obj)
            {
                if (exclude.contains(obj.GetID()) || !IsSnapVisible(scene, obj)) return;
                if (!obj.IsKindOf<Entity>()) return;

                pts.clear();
                CollectFeaturePoints(static_cast<const Entity&>(obj), static_cast<FeatureKind>(kind), pts);
                for (const auto& p : pts)
                    TryUpdateBest(p, sp, cam, m_snapRadiusPx, bestDist, best, type, obj.GetID());
            });

        return best;
    }

    SnapResult SnapEngine::TryEndpoint(const Math::Point2& sp, const Scene& scene, const Camera& cam, const std::unordered_set<Object::ObjectID>& exclude) const
    {
        return TryFeature(sp, scene, cam, exclude, static_cast<int>(FeatureKind::Endpoint), SnapResult::Type::Endpoint);
    }

    SnapResult SnapEngine::TryMidpoint(const Math::Point2& sp, const Scene& scene, const Camera& cam, const std::unordered_set<Object::ObjectID>& exclude) const
    {
        return TryFeature(sp, scene, cam, exclude, static_cast<int>(FeatureKind::Midpoint), SnapResult::Type::Midpoint);
    }

    SnapResult SnapEngine::TryQuadrant(const Math::Point2& sp, const Scene& scene, const Camera& cam, const std::unordered_set<Object::ObjectID>& exclude) const
    {
        return TryFeature(sp, scene, cam, exclude, static_cast<int>(FeatureKind::Quadrant), SnapResult::Type::Quadrant);
    }

    // =========================================================================
    // TryNearest
    //
    //   Line         → 线段上最近点
    //   Rectangle    → 四条边上最近点
    //   Circle       → 圆周上最近点（投影到圆心方向）
    //   Arc          → 弧上最近点（Arc::ClosestPoint）
    //   Ellipse      → 椭圆周最近点（Ellipse::ClosestPoint，牛顿迭代）
    //   Polyline     → Tessellate 后逐段最近点
    //   Spline       → Tessellate 后逐段最近点
    // =========================================================================
    SnapResult SnapEngine::TryNearest(const Math::Point2& sp, const Scene& scene, const Camera& cam, const std::unordered_set<Object::ObjectID>& exclude) const
    {
        SnapResult best;
        double bestDist = std::numeric_limits<double>::max();

        Math::Point3 worldMouse = cam.ScreenToWorld(sp.x, sp.y);

        ForEachSnapObject(scene, [&](const Object& obj)
            {
                if (exclude.contains(obj.GetID()) || !IsSnapVisible(scene, obj)) return;

                const auto id = obj.GetID();
                const auto T = SnapResult::Type::Nearest;

                // ── Line ───────────────────────────────────────────────────────
                if (obj.IsKindOf<LineEntity>())
                {
                    auto* e = static_cast<const LineEntity*>(&obj);
                    auto& L = e->GetLine();
                    Math::Point3 closest = Math::ClosestPointOnSegment(worldMouse, L.Start, L.End);
                    TryUpdateBest(closest, sp, cam, m_snapRadiusPx, bestDist, best, T, id);
                }

                // ── Rectangle ──────────────────────────────────────────────────
                if (obj.IsKindOf<RectangleEntity>())
                {
                    auto* e = static_cast<const RectangleEntity*>(&obj);
                    const auto& r = e->GetRectangle();

                    for (auto [a, b] : {
                        std::pair{r.P1, r.P2}, {r.P2, r.P3}, {r.P3, r.P4}, {r.P4, r.P1} })
                    {
                        TryUpdateBest(Math::ClosestPointOnSegment(worldMouse, a, b),
                            sp, cam, m_snapRadiusPx, bestDist, best, T, id);
                    }
                }

                // ── Circle：投影到圆周 ─────────────────────────────────────────
                if (obj.IsKindOf<CircleEntity>())
                {
                    auto* e = static_cast<const CircleEntity*>(&obj);
                    const auto& c = e->GetCircle();

                    double dx = worldMouse.x - c.Center.x;
                    double dy = worldMouse.y - c.Center.y;
                    double len = std::sqrt(dx * dx + dy * dy);
                    if (len < 1e-10) return;

                    Math::Point3 onCircle =
                    {
                        c.Center.x + c.Radius * (dx / len),
                        c.Center.y + c.Radius * (dy / len),
                        c.Center.z
                    };
                    TryUpdateBest(onCircle, sp, cam, m_snapRadiusPx, bestDist, best, T, id);
                }

                // ── Arc：弧上最近点（考虑角度范围）────────────────────────────
                if (obj.IsKindOf<ArcEntity>())
                {
                    auto* e = static_cast<const ArcEntity*>(&obj);
                    Math::Point3 closest = e->GetArc().ClosestPoint(worldMouse);
                    TryUpdateBest(closest, sp, cam, m_snapRadiusPx, bestDist, best, T, id);
                }

                // ── Ellipse：椭圆周最近点（牛顿迭代）─────────────────────────
                if (obj.IsKindOf<EllipseEntity>())
                {
                    auto* e = static_cast<const EllipseEntity*>(&obj);
                    Math::Point3 closest = e->GetEllipse().ClosestPoint(worldMouse);
                    TryUpdateBest(closest, sp, cam, m_snapRadiusPx, bestDist, best, T, id);
                }

                // ── Polyline：Tessellate 后逐段最近点 ────────────────────────
                if (obj.IsKindOf<PolylineEntity>())
                {
                    auto* e = static_cast<const PolylineEntity*>(&obj);
                    const auto& pl = e->GetPolyline();

                    // 直接用 Polyline 的几何最近点查询（内部处理弧段）
                    Math::Point3 closest = pl.ClosestPoint(worldMouse);
                    TryUpdateBest(closest, sp, cam, m_snapRadiusPx, bestDist, best, T, id);
                }

                // ── Spline：Tessellate 后逐段最近点 ──────────────────────────
                if (obj.IsKindOf<SplineEntity>())
                {
                    auto* e = static_cast<const SplineEntity*>(&obj);
                    const auto& sp_ = e->GetSpline();
                    if (!sp_.IsValid()) return;

                    // 用 Spline::ClosestPoint（内部逐段采样）
                    Math::Point3 closest = sp_.ClosestPoint(worldMouse);
                    TryUpdateBest(closest, sp, cam, m_snapRadiusPx, bestDist, best, T, id);
                }

                // ── Solid3D：线框各段上最近点 ───────────────────────────────
                if (obj.IsKindOf<Solid3DEntity>())
                {
                    for (const auto& line : static_cast<const Solid3DEntity&>(obj).GetWorldWire())
                        for (size_t i = 1; i < line.size(); ++i)
                            TryUpdateBest(Math::ClosestPointOnSegment(worldMouse, line[i - 1], line[i]),
                                sp, cam, m_snapRadiusPx, bestDist, best, T, id);
                }

                // ── XLine：无限直线上最近点（参数不设限）──────────────────────
                if (obj.IsKindOf<XLineEntity>())
                {
                    auto* e = static_cast<const XLineEntity*>(&obj);
                    const XLine& xl = e->GetXLine();
                    if (!xl.IsValid()) return;
                    TryUpdateBest(xl.ClosestPoint(worldMouse), sp, cam, m_snapRadiusPx, bestDist, best, T, id);
                }

                // ── Ray：半无限直线上最近点（参数 clamp 到 t ≥ 0）─────────────
                if (obj.IsKindOf<RayEntity>())
                {
                    auto* e = static_cast<const RayEntity*>(&obj);
                    const XLine& r = e->GetRay();
                    if (!r.IsValid()) return;
                    double t = std::max(0.0, r.ProjectParam(worldMouse));
                    TryUpdateBest(r.PointAt(t), sp, cam, m_snapRadiusPx, bestDist, best, T, id);
                }
            });

        return best;
    }

    // =========================================================================
    // TryIntersection
    //
    // 通过 ICurve 抽象统一处理所有曲线两两求交（Geom::IntersectCurves），不再按
    // 类型分派。先按「曲线到光标的屏幕距离 ≤ 捕捉半径」剔除候选 —— 交点若落在
    // 捕捉半径内，则两条母曲线都必经过光标附近，故该剔除不漏解又把两两规模压到极小。
    // =========================================================================
    SnapResult SnapEngine::TryIntersection(const Math::Point2& sp, const Scene& scene, const Camera& cam, const std::unordered_set<Object::ObjectID>& exclude) const
    {
        const Math::Point3 worldMouse = cam.ScreenToWorld(sp.x, sp.y);

        struct Cand
        {
            Object::ObjectID        id;
            std::unique_ptr<ICurve> curve;
            double                  distPx = 0.0;
        };
        std::vector<Cand> cands;

        ForEachSnapObject(scene, [&](const Object& obj)
            {
                if (exclude.contains(obj.GetID()) || !IsSnapVisible(scene, obj))   return;
                if (!obj.IsKindOf<Entity>())         return;

                const ICurveEntity* ce = static_cast<const Entity*>(&obj)->AsCurveEntity();
                if (!ce) return;

                auto curve = ce->MakeCurve();
                Math::Point3 onCurve = curve->ClosestPoint(worldMouse);
                const double distPx = Math::Distance(sp, cam.WorldToScreen(onCurve));
                if (distPx > m_snapRadiusPx) return;

                cands.push_back({ obj.GetID(), std::move(curve), distPx });
            });

        // 严格施加交点求交候选预算：优先保留离光标最近的至多 kMaxIntersectCandidates 条曲线，避免密集外参下 O(N²) 阻塞
        auto byDist = [](const Cand& a, const Cand& b) { return a.distPx < b.distPx; };
        if (cands.size() > kMaxIntersectCandidates)
        {
            std::nth_element(cands.begin(), cands.begin() + kMaxIntersectCandidates, cands.end(), byDist);
            cands.resize(kMaxIntersectCandidates);
        }
        std::sort(cands.begin(), cands.end(), byDist);

        SnapResult best;
        double bestDist = std::numeric_limits<double>::max();

        for (size_t i = 0; i < cands.size(); ++i)
            for (size_t j = i + 1; j < cands.size(); ++j)
            {
                auto pts = Geom::IntersectCurves(*cands[i].curve, *cands[j].curve);
                for (const auto& p : pts)
                {
                    const double before = bestDist;
                    TryUpdateBest(p, sp, cam, m_snapRadiusPx, bestDist, best,
                                  SnapResult::Type::Intersection, cands[i].id);
                    if (bestDist < before) best.SourceID2 = cands[j].id;
                }
            }

        return best;
    }

    // =========================================================================
    // TryPerpendicular
    //
    // 自基点 fromPoint（工具锚点 / 活动夹点）向所指曲线作垂线，捕捉垂足。
    // 对直线 / 射线 / 构造线 / 圆 / 弧 / 椭圆，「基点到曲线的最近点」即法向垂足，
    // 故复用 ICurve::ClosestPoint(fromPoint)。
    //
    // 判定与其他点类捕捉一致：光标须贴近垂足点本身（孔径内）才命中——
    // 不做「指到曲线任意处即给出远处垂足」的延伸垂足，避免过度灵敏、
    // 也避免与最近点捕捉互相抢占。
    // =========================================================================
    SnapResult SnapEngine::TryPerpendicular(const Math::Point2& sp, const Scene& scene, const Camera& cam, const std::unordered_set<Object::ObjectID>& exclude, const Math::Point3& fromPoint) const
    {
        SnapResult best;
        double bestDist = std::numeric_limits<double>::max();

        ForEachSnapObject(scene, [&](const Object& obj)
            {
                if (exclude.contains(obj.GetID()) || !IsSnapVisible(scene, obj))   return;
                if (!obj.IsKindOf<Entity>())         return;

                const ICurveEntity* ce = static_cast<const Entity*>(&obj)->AsCurveEntity();
                if (!ce) return;

                Math::Point3 foot = ce->MakeCurve()->ClosestPoint(fromPoint);
                TryUpdateBest(foot, sp, cam, m_snapRadiusPx, bestDist, best,
                              SnapResult::Type::Perpendicular, obj.GetID());
            });

        return best;
    }

    // =========================================================================
    // TryGrid
    // =========================================================================
    SnapResult SnapEngine::TryGrid(const Math::Point2& sp, const Camera& cam) const
    {
        Math::Point3 w = cam.ScreenToWorld(sp.x, sp.y);
        return
        {
            SnapResult::Type::Grid,
            {
                std::round(w.x / m_gridSize) * m_gridSize,
                std::round(w.y / m_gridSize) * m_gridSize,
                0.0
            },
            Object::InvalidID
        };
    }

}
