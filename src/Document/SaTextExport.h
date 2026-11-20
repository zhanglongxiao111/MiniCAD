#pragma once
// 嵌入模式：导出图纸里全部文字（TEXT / MTEXT / ATTRIB，含块内文字）的世界坐标、字高、图层、块路径与属性标记。
// 网页嵌入接口（MiniCADEmbed_ExportText）与无界面工具 SaDwgText 共用这一份实现，输出同一种 TSV：
//   kind \t x \t y \t h \t layer \t block \t tag \t text   （首行是表头；kind = TEXT / MTEXT / ATTRIB；块路径用 / 连接）
#include "Scene/Scene.h"
#include "Core/Entity/InsertEntity.hpp"
#include "Core/Entity/TextEntity.hpp"
#include "Core/Entity/MTextEntity.hpp"
#include "Core/Entity/AttribEntity.hpp"
#include <cstdio>
#include <string>

namespace MiniCAD::SaTextExport
{
    inline std::string Clean(std::string s)
    {
        for (auto& c : s)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return s;
    }

    struct Writer
    {
        const Scene& scene;
        std::string  out;

        std::string LayerName(const Entity& e) const
        {
            const auto* l = scene.GetLayerManager().GetLayer(e.GetAttr().LayerId);
            return l ? l->GetName() : "?";
        }
        void Emit(const char* kind, const Entity& e, Math::Point3 p, const Math::Mat4& m, double h,
                  const std::string& text, const std::string& path, const std::string& tag = {})
        {
            const auto w = m.TransformPoint(p);
            char num[96];
            std::snprintf(num, sizeof num, "\t%.2f\t%.2f\t%.2f\t", w.x, w.y, h);
            out += kind; out += num;
            out += Clean(LayerName(e)); out += '\t';
            out += Clean(path); out += '\t';
            out += Clean(tag); out += '\t';
            out += Clean(text); out += '\n';
        }
        void Walk(const Entity& e, const Math::Mat4& m, const std::string& path, int depth)
        {
            if (e.IsKindOf<TextEntity>())
            {
                const auto& t = static_cast<const TextEntity&>(e);
                Emit("TEXT", e, t.GetPosition(), m, t.GetHeight(), t.GetText(), path);
            }
            else if (e.IsKindOf<MTextEntity>())
            {
                const auto& t = static_cast<const MTextEntity&>(e);
                Emit("MTEXT", e, t.GetPosition(), m, t.GetHeight(), t.GetPlainText(), path);
            }
            else if (e.IsKindOf<InsertEntity>() && depth < 12)
            {
                const auto& ins = static_cast<const InsertEntity&>(e);
                const Math::Mat4 cm = ins.CellMatrix(0, 0) * m;
                const std::string p = path.empty() ? ins.GetBlockName() : path + "/" + ins.GetBlockName();
                if (const auto* b = ins.GetBlock())
                    for (const auto& c : b->GetEntities())
                        Walk(*c, cm, p, depth + 1);
                for (const auto& a : ins.GetAttribs())
                    Emit("ATTRIB", *a, a->GetPosition(), m, a->GetHeight(), a->GetText(), p, a->GetTag());
            }
        }
    };

    inline std::string ExportTsv(const Scene& scene)
    {
        Writer w{ scene, "kind\tx\ty\th\tlayer\tblock\ttag\ttext\n" };
        scene.ForEachObject([&](const Object& o) {
            if (const auto* e = dynamic_cast<const Entity*>(&o))
                w.Walk(*e, Math::Mat4::Identity(), "", 0);
        });
        return w.out;
    }
}
