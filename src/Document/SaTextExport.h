#pragma once
// 嵌入模式：导出图纸里全部文字（TEXT / MTEXT / ATTRIB，含块内文字）的世界坐标、字高、图层、块路径与属性标记。
// 网页嵌入接口（MiniCADEmbed_ExportText）与无界面工具 SaDwgText 共用这一份流式实现，输出同一种 TSV：
//   kind \t x \t y \t h \t layer \t block \t tag \t text   （首行是表头；kind = TEXT / MTEXT / ATTRIB；块路径用 / 连接）
// 对横向展开施加三重硬预算（访问阵列单元数、文字行数、输出字节数），防止嵌套 MINSERT 爆炸导致卡死或 OOM。
#include "Scene/Scene.h"
#include "Core/Entity/BlockEntity.hpp"
#include "Core/Entity/InsertEntity.hpp"
#include "Core/Entity/TextEntity.hpp"
#include "Core/Entity/MTextEntity.hpp"
#include "Core/Entity/AttribEntity.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

namespace MiniCAD::SaTextExport
{
    struct Budget
    {
        std::size_t MaxArrayCells = 250'000;
        std::size_t MaxRows       = 100'000;
        std::size_t MaxBytes      = 16u * 1024u * 1024u;
    };

    enum class Status
    {
        Ok,
        ArrayCellBudgetExceeded,
        RowBudgetExceeded,
        ByteBudgetExceeded,
        SinkError,
    };

    struct Result
    {
        Status      Code       = Status::Ok;
        std::size_t ArrayCells = 0;
        std::size_t Rows       = 0;
        std::size_t Bytes      = 0;

        bool Ok() const { return Code == Status::Ok; }
    };

    inline std::string Clean(std::string s)
    {
        for (auto& c : s)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return s;
    }

    template <typename LineSink>
    struct StreamWriter
    {
        const Scene& scene;
        LineSink&    sink;
        Budget       budget;
        Result       result{};
        std::string  lineBuf;
        // 块可导出文字剪枝缓存：1 = 访中防环，2 = 无可导出文字，3 = 含可导出文字（遇环保守视作有字，不写入无字缓存）
        std::unordered_map<const BlockEntity*, std::uint8_t> blockHasTextCache;

        bool BlockHasText(const BlockEntity* blk)
        {
            if (!blk)
                return false;
            if (const auto it = blockHasTextCache.find(blk); it != blockHasTextCache.end())
                return it->second != 2u;
            blockHasTextCache[blk] = 1u;
            for (const auto& child : blk->GetEntities())
            {
                if (!child)
                    continue;
                if (child->IsKindOf<TextEntity>() || child->IsKindOf<MTextEntity>())
                {
                    blockHasTextCache[blk] = 3u;
                    return true;
                }
                if (child->IsKindOf<InsertEntity>())
                {
                    const auto& childIns = static_cast<const InsertEntity&>(*child);
                    for (const auto& a : childIns.GetAttribs())
                    {
                        if (a)
                        {
                            blockHasTextCache[blk] = 3u;
                            return true;
                        }
                    }
                    if (const auto* sub = childIns.GetBlock(); sub && BlockHasText(sub))
                    {
                        blockHasTextCache[blk] = 3u;
                        return true;
                    }
                }
            }
            blockHasTextCache[blk] = 2u;
            return false;
        }

        std::string LayerName(const Entity& e) const
        {
            const auto* l = scene.GetLayerManager().GetLayer(e.GetAttr().LayerId);
            return l ? l->GetName() : "?";
        }

        bool WriteChunk(std::string_view chunk)
        {
            if (budget.MaxBytes > 0 && (chunk.size() > budget.MaxBytes || result.Bytes > budget.MaxBytes - chunk.size()))
            {
                result.Code = Status::ByteBudgetExceeded;
                return false;
            }
            if (!sink(chunk))
            {
                result.Code = Status::SinkError;
                return false;
            }
            result.Bytes += chunk.size();
            return true;
        }

        bool Emit(const char* kind, const Entity& e, Math::Point3 p, const Math::Mat4& m, double h, double rot,
                  const std::string& text, const std::string& path, const std::string& tag = {})
        {
            if (!result.Ok())
                return false;
            if (budget.MaxRows > 0 && result.Rows >= budget.MaxRows)
            {
                result.Code = Status::RowBudgetExceeded;
                return false;
            }
            const auto w = m.TransformPoint(p);
            // 沿局部文字高度方向（旋转角 rot 逆时针 90°）变换到世界空间以换算真实世界字高
            const Math::Vec3 upLocal{ -std::sin(rot) * h, std::cos(rot) * h, 0.0 };
            const double worldH = m.TransformVector(upLocal).Length();
            const double outH   = std::isfinite(worldH) ? worldH : 0.0;
            char num[96];
            std::snprintf(num, sizeof num, "\t%.2f\t%.2f\t%.2f\t", w.x, w.y, outH);
            lineBuf.clear();
            lineBuf += kind;
            lineBuf += num;
            lineBuf += Clean(LayerName(e));
            lineBuf += '\t';
            lineBuf += Clean(path);
            lineBuf += '\t';
            lineBuf += Clean(tag);
            lineBuf += '\t';
            lineBuf += Clean(text);
            lineBuf += '\n';
            if (!WriteChunk(lineBuf))
                return false;
            ++result.Rows;
            return true;
        }

        void Walk(const Entity& e, const Math::Mat4& m, const std::string& path, int depth)
        {
            if (!result.Ok())
                return;
            if (e.IsKindOf<TextEntity>())
            {
                const auto& t = static_cast<const TextEntity&>(e);
                Emit("TEXT", e, t.GetPosition(), m, t.GetHeight(), t.GetRotation(), t.GetText(), path);
            }
            else if (e.IsKindOf<MTextEntity>())
            {
                const auto& t = static_cast<const MTextEntity&>(e);
                Emit("MTEXT", e, t.GetPosition(), m, t.GetHeight(), t.GetRotation(), t.GetPlainText(), path);
            }
            else if (e.IsKindOf<InsertEntity>() && depth < 12)
            {
                const auto& ins = static_cast<const InsertEntity&>(e);
                const std::string p = path.empty() ? ins.GetBlockName() : path + "/" + ins.GetBlockName();
                if (const auto* b = ins.GetBlock(); b && BlockHasText(b))
                {
                    const int rows = std::max(1, ins.GetRowCount());
                    const int cols = std::max(1, ins.GetColumnCount());
                    for (int r = 0; r < rows && result.Ok(); ++r)
                    {
                        for (int c = 0; c < cols && result.Ok(); ++c)
                        {
                            if (budget.MaxArrayCells > 0 && result.ArrayCells >= budget.MaxArrayCells)
                            {
                                result.Code = Status::ArrayCellBudgetExceeded;
                                return;
                            }
                            ++result.ArrayCells;
                            const Math::Mat4 cm = ins.CellMatrix(c, r) * m;
                            for (const auto& child : b->GetEntities())
                            {
                                if (!result.Ok())
                                    return;
                                if (child)
                                    Walk(*child, cm, p, depth + 1);
                            }
                        }
                    }
                }
                // ATTRIB 已位于所属插入的世界坐标系，不重复套本级 CellMatrix（仅套外层嵌套块矩阵 m）
                for (const auto& a : ins.GetAttribs())
                {
                    if (!result.Ok())
                        return;
                    if (a)
                        Emit("ATTRIB", *a, a->GetPosition(), m, a->GetHeight(), a->GetRotation(), a->GetText(), p, a->GetTag());
                }
            }
        }
    };

    // 流式导出 TSV：逐行调用 lineSink(std::string_view)，触发任一硬预算或下游写错时立即中止并返回错误码。
    template <typename LineSink>
    inline Result ExportTsvStream(const Scene& scene, LineSink&& lineSink, const Budget& budget = {})
    {
        StreamWriter<LineSink> w{ scene, lineSink, budget, {}, {}, {} };
        w.lineBuf.reserve(256);
        constexpr std::string_view kHeader = "kind\tx\ty\th\tlayer\tblock\ttag\ttext\n";
        if (!w.WriteChunk(kHeader))
            return w.result;

        scene.ForEachObject([&](const Object& o) {
            if (!w.result.Ok())
                return;
            if (const auto* e = dynamic_cast<const Entity*>(&o))
                w.Walk(*e, Math::Mat4::Identity(), "", 0);
        });
        return w.result;
    }

    // 内存便捷接口：超预算立即中止并返回空串 ""（供嵌入接口自动判失败且不落盘残缺文件）。
    inline std::string ExportTsv(const Scene& scene, const Budget& budget = {}, Result* outResult = nullptr)
    {
        std::string out;
        const Result res = ExportTsvStream(
            scene,
            [&](std::string_view chunk) {
                out.append(chunk.data(), chunk.size());
                return true;
            },
            budget);
        if (outResult)
            *outResult = res;
        if (!res.Ok())
            return {};
        return out;
    }
}
