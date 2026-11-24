#pragma once
// 嵌入模式：在当前活动布局（模型空间或图纸空间）临时叠加红色云线批注与引注文字并按原版本导出 DWG，
// 导出结束后经 RAII 作用域守卫确保临时实体与新建图层必然清理。
#include "Document/Document.h"
#include "Import/CadExchange.h"
#include "Scene/Scene.h"
#include "Core/Entity/PolylineEntity.hpp"
#include "Core/Entity/TextEntity.hpp"
#include "Core/GeomKernel/Polyline.hpp"
#include "MiniI18n/I18n.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MiniCAD::SaMarkupExport
{
    inline constexpr const char* kMarkupLayerName = "SA-问题批注"; // i18n: 数据
    inline constexpr std::size_t kMaxMarkupRects  = 4096;
    inline constexpr int         kMaxArcsPerEdge  = 256;
    inline constexpr double      kMinRectExtent   = 1e-6;
    inline constexpr double      kMaxRectExtent   = 1e15;

    inline bool IsValidMarkupRect(const std::array<double, 4>& r)
    {
        for (double v : r)
            if (!std::isfinite(v) || std::abs(v) > kMaxRectExtent)
                return false;
        const double w = r[2] - r[0];
        const double h = r[3] - r[1];
        return std::isfinite(w) && std::isfinite(h)
            && w >= kMinRectExtent && h >= kMinRectExtent
            && w <= kMaxRectExtent && h <= kMaxRectExtent;
    }

    // 云线：沿矩形四边按弧长均分，每段向外鼓的小弧（逆时针走向凸度取 -0.5 向外）。
    // 严格校验有限坐标与最小有效尺寸，并限制单边最大弧段数（防止狭长退化框 OOM 或整型转换越界）。
    inline std::optional<Polyline> BuildRevisionCloud(double minX, double minY, double maxX, double maxY)
    {
        const std::array<double, 4> rect{ minX, minY, maxX, maxY };
        if (!IsValidMarkupRect(rect))
            return std::nullopt;

        const double w = maxX - minX;
        const double h = maxY - minY;
        const double minSide = std::min(w, h);
        const double maxSide = std::max(w, h);
        const double arc = std::max(minSide / 6.0, maxSide / static_cast<double>(kMaxArcsPerEdge));

        std::vector<Math::Point3> pts;
        pts.reserve(static_cast<std::size_t>(kMaxArcsPerEdge) * 4 + 1);
        auto edge = [&](double x0, double y0, double x1, double y1)
        {
            const double len = std::hypot(x1 - x0, y1 - y0);
            const double rawN = (arc > 0.0 && std::isfinite(len)) ? std::round(len / arc) : 1.0;
            const int n = (!std::isfinite(rawN) || rawN < 1.0)
                ? 1
                : (rawN > static_cast<double>(kMaxArcsPerEdge) ? kMaxArcsPerEdge : static_cast<int>(rawN));
            for (int i = 0; i < n; ++i)
            {
                const double t = static_cast<double>(i) / static_cast<double>(n);
                pts.push_back({ x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, 0.0 });
            }
        };
        edge(minX, minY, maxX, minY);
        edge(maxX, minY, maxX, maxY);
        edge(maxX, maxY, minX, maxY);
        edge(minX, maxY, minX, minY);
        pts.push_back({ minX, minY, 0.0 });
        if (pts.size() < 2)
            return std::nullopt;

        std::vector<double> bulges(pts.size() - 1, -0.5);
        return Polyline(std::move(pts), std::move(bulges));
    }

    inline LayerID EnsureMarkupLayer(Scene& scene, bool* existedOut = nullptr)
    {
        LayerManager& layers = scene.GetLayerManager();
        for (LayerID id : layers.GetAllLayerIDs())
        {
            if (const Layer* l = layers.GetLayer(id); l && l->GetName() == kMarkupLayerName)
            {
                if (existedOut) *existedOut = true;
                return id;
            }
        }
        if (existedOut) *existedOut = false;
        const LayerID id = layers.AddLayer(kMarkupLayerName);
        if (Layer* layer = layers.GetLayer(id))
            layer->SetColor(AciToColor4(1));
        return id;
    }

    // 在 doc 的当前活动布局（模型空间或指定图纸空间布局）临时写入云线批注与文字并导出 DWG 字节。
    // 无论成功、校验失败还是导出异常，临时实体与临时图层均由 RAII 守卫自动清理。
    inline std::vector<std::uint8_t> ExportMarkupDwg(Document& doc,
                                                     const std::vector<std::array<double, 4>>& rects,
                                                     const std::vector<std::string>& labels,
                                                     std::string& error)
    {
        error.clear();
        if (rects.size() > kMaxMarkupRects)
        {
            error = Tr("批注框数量超出上限");
            return {};
        }

        // 先校验全部矩形并构造云线，避免校验失败前污染场景
        std::vector<Polyline> clouds;
        clouds.reserve(rects.size());
        for (const auto& r : rects)
        {
            auto cloud = BuildRevisionCloud(r[0], r[1], r[2], r[3]);
            if (!cloud.has_value())
            {
                error = Tr("批注框坐标无效或尺寸过小");
                return {};
            }
            clouds.push_back(std::move(*cloud));
        }

        Scene& scene = doc.GetScene();
        LayerManager& layers = scene.GetLayerManager();
        bool layerExisted = false;
        const LayerID layerId = EnsureMarkupLayer(scene, &layerExisted);

        std::vector<Object::ObjectID> added;
        added.reserve(rects.size() * 2);

        struct TempMarkupGuard
        {
            Scene&                         scene;
            LayerManager&                  layers;
            std::vector<Object::ObjectID>& added;
            LayerID                        layerId     = 0;
            bool                           removeLayer = false;

            ~TempMarkupGuard()
            {
                for (Object::ObjectID id : added)
                    scene.RemoveEntity(id);
                if (removeLayer)
                    layers.RemoveLayer(layerId);
            }
        } guard{ scene, layers, added, layerId, !layerExisted };

        // 复用正常绘图工具的布局属性初始化（EditorContext::ApplyCurrentAttr 同口径）：
        // 纸张布局下写入 InPaperSpace=true 与当前布局的 LayoutKey，避免导出的云线和文字掉进模型空间。
        const bool     inPaper   = doc.ActiveLayoutIsPaper();
        const uint64_t layoutKey = doc.ActiveLayoutOwnerKey();
        auto initAttr = [&](Entity& e)
        {
            EntityAttr& attr  = e.GetAttr();
            attr.LayerId      = layerId;
            attr.Color        = EntityColor::FromAci(1);
            attr.InPaperSpace = inPaper;
            attr.LayoutKey    = layoutKey;
        };

        for (std::size_t index = 0; index < rects.size(); ++index)
        {
            const auto& r = rects[index];
            const Object::ObjectID id = scene.NextObjectID();
            auto entity = std::make_unique<PolylineEntity>(id, std::move(clouds[index]));
            initAttr(*entity);
            scene.AddEntity(std::move(entity));
            added.push_back(id);

            if (index < labels.size() && !labels[index].empty())
            {
                // 引注文字放在云线左上角外侧，字高取框高的 1/8（至少框短边的 1/20）
                const double w = r[2] - r[0];
                const double hBox = r[3] - r[1];
                const double textH = std::max(hBox / 8.0, std::min(w, hBox) / 20.0);
                if (std::isfinite(textH) && textH > 0.0)
                {
                    const Object::ObjectID textId = scene.NextObjectID();
                    auto text = std::make_unique<TextEntity>(
                        textId,
                        Math::Point3{ r[0], r[3] + textH * 0.5, 0.0 },
                        labels[index],
                        static_cast<float>(textH));
                    initAttr(*text);
                    scene.AddEntity(std::move(text));
                    added.push_back(textId);
                }
            }
        }

        CadSaveVersion version = doc.GetCadSaveVersion();
        if (!CadSaveVersionSupported(CadFileKind::Dwg, version))
            version = CadSaveVersion::R2010;
        std::vector<std::uint8_t> bytes = ExportCad(scene, CadFileKind::Dwg, version, &error, doc.GetCadSource());
        if (bytes.empty() && error.empty())
            error = Tr("导出 DWG 失败");
        return bytes;
    }
}
