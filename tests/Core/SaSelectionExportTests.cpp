// ── 嵌入选中图元摘要导出、文字导出、批注导出、拾点重入、保护门禁、对比视口与捕捉回归测试 ──
#include "Core/Entity/MLineEntity.hpp"
#include "Document/SaSelectionExport.h"
#include "Document/SaTextExport.h"
#include "Document/SaMarkupExport.h"
#include "Document/Document.h"
#include "Document/Command/AddEntityCommand.h"
#include "Editor/Editor.h"
#include "Import/CadExchange.h"
#include "Render/IRenderer.h"
#include "Render/IRenderTarget.h"
#include "Scene/Scene.h"
#include "Serialization/JsonSerializer.h"
#include "Viewport/Viewport.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace MiniCAD;

namespace
{
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("[%s] %s\n", ok ? "通过" : "失败", what);
        if (!ok)
            ++g_failures;
    }

    bool Near(double a, double b, double eps = 1e-2)
    {
        return std::abs(a - b) <= eps;
    }

    bool IsValidUtf8(std::string_view s)
    {
        std::size_t i = 0;
        const std::size_t n = s.size();
        while (i < n)
        {
            bool valid = false;
            const std::size_t step = SaSelectionExport::Detail::DecodeUtf8Step(s, i, valid);
            if (!valid || step == 0)
                return false;
            i += step;
        }
        return true;
    }

    std::size_t CountUtf8Chars(std::string_view s)
    {
        std::size_t count = 0;
        for (unsigned char c : s)
            if ((c & 0xC0u) != 0x80u)
                ++count;
        return count;
    }

    bool IsUpperHexNoPrefix(std::string_view s)
    {
        if (s.empty())
            return false;
        for (char c : s)
        {
            const bool digit = (c >= '0' && c <= '9');
            const bool upper = (c >= 'A' && c <= 'F');
            if (!digit && !upper)
                return false;
        }
        return true;
    }

    void TestLookupCadEntityHandle()
    {
        Check(LookupCadEntityHandle(nullptr, 1) == 0, "LookupCadEntityHandle(nullptr, 1) == 0");

        // 构造含模型空间实体与图纸空间实体的场景，往返 DWG 后验证真实 handle 查询
        Scene srcScene;
        DrawingLayout modelLayout;
        modelLayout.Name  = "Model";
        modelLayout.Model = true;
        modelLayout.Order = 0;
        DrawingLayout paperLayout;
        paperLayout.Name     = "SheetA1";
        paperLayout.Model    = false;
        paperLayout.Order    = 1;
        paperLayout.OwnerKey = srcScene.NewLayoutKey();
        srcScene.SetLayouts({ modelLayout, paperLayout });

        const LayerID doorLayer = srcScene.GetLayerManager().AddLayer("A-DOOR");

        auto line = std::make_unique<LineEntity>(srcScene.NextObjectID(), Math::Point3{ 0, 0, 0 }, Math::Point3{ 1000, 0, 0 });
        line->SetLayerId(doorLayer);
        srcScene.AddEntity(std::move(line));

        auto paperText = std::make_unique<TextEntity>(srcScene.NextObjectID(), Math::Point3{ 50, 60, 0 }, "A1-NOTE", 5.0f);
        EntityAttr pAttr = paperText->GetAttr();
        pAttr.LayerId      = doorLayer;
        pAttr.InPaperSpace = true;
        pAttr.LayoutKey    = paperLayout.OwnerKey;
        paperText->SetAttr(pAttr);
        srcScene.AddEntity(std::move(paperText));

        std::string exportErr;
        const auto dwgBytes = ExportCad(srcScene, CadFileKind::Dwg, CadSaveVersion::R2018, &exportErr);
        Check(!dwgBytes.empty(), "LookupCadEntityHandle: ExportCad 生成 DWG 成功");

        Scene loadedScene;
        std::shared_ptr<const CadSource> cadSource;
        std::string importErr;
        const bool imported = ImportCad(dwgBytes, loadedScene, &importErr, nullptr, &cadSource);
        Check(imported && cadSource != nullptr, "LookupCadEntityHandle: ImportCad 读回 DWG 并保留 CadSource");

        Object::ObjectID modelLineId = Object::InvalidID;
        Object::ObjectID paperTextId = Object::InvalidID;
        loadedScene.ForEachObject([&](const Object& o) {
            if (o.IsKindOf<LineEntity>())
                modelLineId = o.GetID();
            else if (o.IsKindOf<TextEntity>())
                paperTextId = o.GetID();
        });

        const std::uint64_t lineHandle = LookupCadEntityHandle(cadSource.get(), modelLineId);
        const std::uint64_t textHandle = LookupCadEntityHandle(cadSource.get(), paperTextId);
        Check(modelLineId != Object::InvalidID && lineHandle != 0, "LookupCadEntityHandle: 模型空间实体可查到非 0 句柄");
        Check(paperTextId != Object::InvalidID && textHandle != 0, "LookupCadEntityHandle: 图纸空间实体可查到非 0 句柄");
        Check(lineHandle != textHandle, "LookupCadEntityHandle: 不同实体句柄不同");
        Check(LookupCadEntityHandle(cadSource.get(), 999999999ull) == 0, "LookupCadEntityHandle: 不存在 ID 返回 0");
        Check(LookupCadEntityHandle(cadSource.get(), Object::InvalidID) == 0, "LookupCadEntityHandle: InvalidID 返回 0");

        // 通过 ExportSelectionJson 验证 handle 十六进制格式（大写无 0x 前缀，与 SaDwgTool 一致）
        std::unordered_set<Object::ObjectID> sel{ modelLineId };
        const std::string json = SaSelectionExport::ExportSelectionJson(loadedScene, sel, cadSource.get(), 20);
        JsonSerializer js;
        Check(js.Parse(json), "ExportSelectionJson(含 DWG handle): 输出合法 JSON");
        std::size_t itemCount = 0;
        Check(js.BeginArray("items", itemCount) && itemCount == 1, "ExportSelectionJson(含 DWG handle): items 长度为 1");
        if (itemCount == 1 && js.BeginElement(0))
        {
            std::string hStr;
            js.Value("handle", hStr);
            const std::string expectedHex = SaSelectionExport::FormatHandleHex(lineHandle);
            Check(hStr == expectedHex && IsUpperHexNoPrefix(hStr), "ExportSelectionJson: handle 为大写无前缀十六进制");
            js.EndElement();
        }
        js.EndArray();
    }

    void TestEmptyAndInvalidSelection()
    {
        Scene scene;
        const std::string expected = "{\"count\":0,\"scannedCount\":0,\"truncated\":false,\"statsTruncated\":false,\"bounds\":null,\"typeCounts\":{},\"items\":[]}";
        const std::string emptyJson = SaSelectionExport::ExportSelectionJson(scene, {}, nullptr, 20);
        Check(emptyJson == expected, "空选择集返回精确空 JSON");

        std::unordered_set<Object::ObjectID> bogus{ 12345, 67890 };
        const std::string bogusJson = SaSelectionExport::ExportSelectionJson(scene, bogus, nullptr, 20);
        Check(bogusJson == expected, "仅含无效 ID 的选择集返回精确空 JSON");
    }

    void TestRichEntitySerialization()
    {
        Scene scene;
        const LayerID doorLayer = scene.GetLayerManager().AddLayer("A-DOOR");
        const LayerID wallLayer = scene.GetLayerManager().AddLayer("A-WALL");

        // 1. LINE（长度 1000）
        const Object::ObjectID lineId = scene.NextObjectID();
        auto line = std::make_unique<LineEntity>(lineId, Math::Point3{ 0, 0, 0 }, Math::Point3{ 1000, 0, 0 });
        line->SetLayerId(wallLayer);
        scene.AddEntity(std::move(line));

        // 2. CIRCLE（半径 250，面积 pi * 250^2）
        const Object::ObjectID circleId = scene.NextObjectID();
        auto circle = std::make_unique<CircleEntity>(circleId, Math::Point3{ 500, 500, 0 }, 250.0);
        circle->SetLayerId(wallLayer);
        scene.AddEntity(std::move(circle));

        // 3. ARC（半径 100，90 度弧长 50*pi）
        const Object::ObjectID arcId = scene.NextObjectID();
        auto arc = std::make_unique<ArcEntity>(arcId, Math::Point3{ 0, 0, 0 }, 100.0, 0.0, Math::HalfPI);
        arc->SetLayerId(wallLayer);
        scene.AddEntity(std::move(arc));

        // 4. 闭合 POLYLINE（100x200 矩形：周长 600，面积 20000）
        const Object::ObjectID polyId = scene.NextObjectID();
        auto poly = std::make_unique<PolylineEntity>(polyId, std::vector<Math::Point3>{
            { 0, 0, 0 }, { 100, 0, 0 }, { 100, 200, 0 }, { 0, 200, 0 }, { 0, 0, 0 }
        });
        poly->SetWidth(0.0);
        poly->SetLayerId(wallLayer);
        scene.AddEntity(std::move(poly));

        // 5. INSERT + 可见 ATTRIB 与不可见 ATTRIB
        auto blk = std::make_unique<BlockEntity>(scene.NextObjectID(), "M1021");
        blk->AddEntity(std::make_unique<LineEntity>(scene.NextObjectID(), Math::Point3{ 0, 0, 0 }, Math::Point3{ 1000, 200, 0 }));
        const BlockID blkId = scene.GetBlockTable().Add(std::move(blk));
        const Object::ObjectID insId = scene.NextObjectID();
        auto ins = std::make_unique<InsertEntity>(insId, blkId, "M1021", Math::Point3{ 100.5, 200.0, 0.0 });
        ins->SetLayerId(doorLayer);
        auto visAttr = std::make_unique<AttribEntity>(scene.NextObjectID(), "CODE", "FM甲1021", Math::Point3{ 100.5, 200.0, 0.0 }, 200.0, 0.0);
        auto hiddenAttr = std::make_unique<AttribEntity>(scene.NextObjectID(), "SECRET", "HIDDEN", Math::Point3{ 100.5, 200.0, 0.0 }, 200.0, 0.0);
        hiddenAttr->SetFlags(static_cast<uint16_t>(AttribFlags::Invisible));
        auto emptyAttr = std::make_unique<AttribEntity>(scene.NextObjectID(), "EMPTY", "   ", Math::Point3{ 100.5, 200.0, 0.0 }, 200.0, 0.0);
        ins->AddAttrib(std::move(visAttr));
        ins->AddAttrib(std::move(hiddenAttr));
        ins->AddAttrib(std::move(emptyAttr));
        scene.ResolveInsert(*ins);
        scene.AddEntity(std::move(ins));

        // 6. MTEXT（带格式码）
        const Object::ObjectID mtextId = scene.NextObjectID();
        auto mtext = std::make_unique<MTextEntity>(mtextId, 0, "{\\C1;卫生间}\\P\\H1.5x;+0.000", Math::Point3{ 200, 300, 0 }, 300.0, 0.0, 1000.0);
        mtext->SetLayerId(doorLayer);
        scene.AddEntity(std::move(mtext));

        // 7. WIPEOUT（验证不被误判为 POLYLINE）与 TABLE（验证不被误判为 MTEXT）
        const Object::ObjectID wipeId = scene.NextObjectID();
        auto wipe = std::make_unique<WipeoutEntity>(wipeId, std::vector<Math::Point3>{
            { 0, 0, 0 }, { 50, 0, 0 }, { 50, 50, 0 }, { 0, 50, 0 }
        });
        scene.AddEntity(std::move(wipe));

        const Object::ObjectID tableId = scene.NextObjectID();
        auto tbl = std::make_unique<TableEntity>(tableId, Math::Point3{ 0, 100, 0 }, std::vector<double>{ 100.0 }, std::vector<double>{ 50.0 }, 10.0);
        tbl->SetCellText(0, 0, "门窗表");
        scene.AddEntity(std::move(tbl));

        // 8. 无界实体 XLINE（bounds 为 null）
        const Object::ObjectID xlineId = scene.NextObjectID();
        scene.AddEntity(std::make_unique<XLineEntity>(xlineId, Math::Point3{ 0, 0, 0 }, Math::Vec3{ 1, 0, 0 }));

        // 9. DIMENSION（自动测量 2400 与含 <> 覆盖文本）
        const Object::ObjectID dimId = scene.NextObjectID();
        auto dim = std::make_unique<DimensionEntity>(
            dimId, Math::Point3{ 0, 0, 0 }, Math::Point3{ 2400, 0, 0 }, Math::Point3{ 1200, 300, 0 });
        dim->SetTextOverride("净宽<>mm");
        scene.AddEntity(std::move(dim));

        // 10. MLEADER（带内联格式码的文字）
        const Object::ObjectID mleaderId = scene.NextObjectID();
        auto mleader = std::make_unique<MLeaderEntity>(mleaderId);
        mleader->SetLanding({ 500, 500, 0 });
        mleader->SetText("{\\C2;构造柱}\\PGZ1");
        scene.AddEntity(std::move(mleader));

        // 11. HATCH（10x20 矩形填充，面积 200）
        const Object::ObjectID hatchId = scene.NextObjectID();
        std::vector<Polyline> loops{
            Polyline(std::vector<Math::Point3>{ { 0, 0, 0 }, { 10, 0, 0 }, { 10, 20, 0 }, { 0, 20, 0 }, { 0, 0, 0 } })
        };
        scene.AddEntity(std::make_unique<HatchEntity>(hatchId, std::move(loops), HatchPattern::MakeSolid()));

        // 测试单个 INSERT 的完整字段
        {
            const std::string json = SaSelectionExport::ExportSelectionJson(scene, { insId }, nullptr, 20);
            JsonSerializer js;
            Check(js.Parse(json), "单选 INSERT: 输出合法 JSON");
            int32_t count = -1;
            bool truncated = true;
            js.Value("count", count);
            js.Value("truncated", truncated);
            Check(count == 1 && !truncated, "单选 INSERT: count=1 且 truncated=false");
            std::size_t n = 0;
            Check(js.BeginArray("items", n) && n == 1, "单选 INSERT: items.size()==1");
            if (n == 1 && js.BeginElement(0))
            {
                uint64_t oid = 0;
                std::string type, layer, blockName, text;
                js.Value("objectId", oid);
                js.Value("type", type);
                js.Value("layer", layer);
                js.Value("blockName", blockName);
                js.Value("text", text);
                Check(oid == insId, "单选 INSERT: objectId 匹配");
                Check(type == "INSERT", "单选 INSERT: type == INSERT");
                Check(layer == "A-DOOR", "单选 INSERT: layer == A-DOOR");
                Check(blockName == "M1021", "单选 INSERT: blockName == M1021");
                Check(text == "CODE=FM甲1021", "单选 INSERT: 仅拼接可见非空 ATTRIB (TAG=VAL)");
                Check(json.find("\"handle\":null") != std::string::npos, "单选 INSERT(无 CadSource): handle 为 null");
                Check(json.find("\"length\":null,\"area\":null,\"radius\":null") != std::string::npos, "单选 INSERT: 几何量均为 null");
                js.EndElement();
            }
            js.EndArray();
        }

        // 测试混合多选：验证排序稳定性、几何量、类型计数、继承顺序、无界实体 bounds=null
        {
            std::unordered_set<Object::ObjectID> multi{
                xlineId, tableId, wipeId, mtextId, insId, polyId, arcId, circleId, lineId
            };
            const std::string json = SaSelectionExport::ExportSelectionJson(scene, multi, nullptr, 20);
            JsonSerializer js;
            Check(js.Parse(json), "多选混合图元: 输出合法 JSON");
            int32_t count = 0;
            bool truncated = true;
            js.Value("count", count);
            js.Value("truncated", truncated);
            Check(count == 9 && !truncated, "多选混合图元: count=9 且 truncated=false");

            if (js.BeginObject("typeCounts"))
            {
                int32_t cLine = 0, cPoly = 0, cWipe = 0, cMText = 0, cTable = 0, cXLine = 0;
                js.Value("LINE", cLine);
                js.Value("POLYLINE", cPoly);
                js.Value("WIPEOUT", cWipe);
                js.Value("MTEXT", cMText);
                js.Value("TABLE", cTable);
                js.Value("XLINE", cXLine);
                Check(cLine == 1 && cPoly == 1 && cWipe == 1 && cMText == 1 && cTable == 1 && cXLine == 1,
                      "多选混合图元: typeCounts 准确区分继承类 WIPEOUT/POLYLINE 与 TABLE/MTEXT");
                js.EndObject();
            }

            std::size_t n = 0;
            Check(js.BeginArray("items", n) && n == 9, "多选混合图元: items 共 9 项");
            uint64_t prevId = 0;
            bool sortedAsc = true;
            bool checkedLine = false, checkedCircle = false, checkedArc = false, checkedPoly = false, checkedMText = false;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (!js.BeginElement(i)) continue;
                uint64_t oid = 0;
                std::string type;
                js.Value("objectId", oid);
                js.Value("type", type);
                if (oid <= prevId) sortedAsc = false;
                prevId = oid;

                if (oid == lineId)
                {
                    double len = 0.0;
                    js.Value("length", len);
                    checkedLine = (type == "LINE" && Near(len, 1000.0));
                }
                else if (oid == circleId)
                {
                    double r = 0.0, area = 0.0;
                    js.Value("radius", r);
                    js.Value("area", area);
                    checkedCircle = (type == "CIRCLE" && Near(r, 250.0) && Near(area, 3.14159265358979323846 * 250.0 * 250.0, 0.1));
                }
                else if (oid == arcId)
                {
                    double r = 0.0, len = 0.0;
                    js.Value("radius", r);
                    js.Value("length", len);
                    checkedArc = (type == "ARC" && Near(r, 100.0) && Near(len, 100.0 * Math::HalfPI, 0.05));
                }
                else if (oid == polyId)
                {
                    double len = 0.0, area = 0.0;
                    js.Value("length", len);
                    js.Value("area", area);
                    checkedPoly = (type == "POLYLINE" && Near(len, 600.0) && Near(area, 20000.0));
                }
                else if (oid == mtextId)
                {
                    std::string txt;
                    js.Value("text", txt);
                    checkedMText = (type == "MTEXT" && txt == "卫生间\n+0.000");
                }
                js.EndElement();
            }
            js.EndArray();
            Check(sortedAsc, "多选混合图元: items 严格按 objectId 升序稳定排序");
            Check(checkedLine && checkedCircle && checkedArc && checkedPoly && checkedMText,
                  "多选混合图元: LINE/CIRCLE/ARC/POLYLINE 几何量与 MTEXT 去格式码准确");
        }

        // 单独选中无界实体 XLINE：合集 bounds 与 item bounds 均为 null
        {
            const std::string json = SaSelectionExport::ExportSelectionJson(scene, { xlineId }, nullptr, 20);
            JsonSerializer js;
            Check(js.Parse(json), "单选 XLINE: 输出合法 JSON");
            Check(json.find("\"bounds\":null") != std::string::npos, "单选 XLINE: 无界实体 bounds 为 null");
        }

        // 测试 DIMENSION / MLEADER / HATCH
        {
            const std::string json = SaSelectionExport::ExportSelectionJson(scene, { dimId, mleaderId, hatchId }, nullptr, 20);
            JsonSerializer js;
            Check(js.Parse(json), "DIMENSION/MLEADER/HATCH: 输出合法 JSON");
            std::size_t n = 0;
            Check(js.BeginArray("items", n) && n == 3, "DIMENSION/MLEADER/HATCH: items 数量为 3");
            bool okDim = false, okMLeader = false, okHatch = false;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (!js.BeginElement(i)) continue;
                uint64_t oid = 0;
                std::string type;
                js.Value("objectId", oid);
                js.Value("type", type);
                if (oid == dimId)
                {
                    std::string txt;
                    js.Value("text", txt);
                    okDim = (type == "DIMENSION" && txt == "净宽2400mm");
                }
                else if (oid == mleaderId)
                {
                    std::string txt;
                    js.Value("text", txt);
                    okMLeader = (type == "MLEADER" && txt == "构造柱\nGZ1");
                }
                else if (oid == hatchId)
                {
                    double area = 0.0;
                    js.Value("area", area);
                    okHatch = (type == "HATCH" && Near(area, 200.0));
                }
                js.EndElement();
            }
            js.EndArray();
            Check(okDim && okMLeader && okHatch, "DIMENSION/MLEADER/HATCH: 文字提取与填充面积准确");
        }
    }

    void TestTruncationAndClamping()
    {
        Scene scene;
        std::unordered_set<Object::ObjectID> allIds;
        for (int i = 0; i < 65; ++i)
        {
            const Object::ObjectID id = scene.NextObjectID();
            scene.AddEntity(std::make_unique<LineEntity>(
                id, Math::Point3{ static_cast<double>(i * 10), 0.0, 0.0 },
                Math::Point3{ static_cast<double>(i * 10 + 5), 10.0, 0.0 }));
            allIds.insert(id);
        }

        // maxItems = 5 → truncated=true, count=65, items=5, union bounds 覆盖 0..645
        {
            const std::string json = SaSelectionExport::ExportSelectionJson(scene, allIds, nullptr, 5);
            JsonSerializer js;
            Check(js.Parse(json), "截断测试(maxItems=5): 合法 JSON");
            int32_t count = 0;
            bool truncated = false;
            js.Value("count", count);
            js.Value("truncated", truncated);
            std::size_t n = 0;
            js.BeginArray("items", n);
            js.EndArray();
            double minX = -1, maxX = -1;
            if (js.BeginObject("bounds"))
            {
                js.Value("minX", minX);
                js.Value("maxX", maxX);
                js.EndObject();
            }
            Check(count == 65 && truncated && n == 5, "截断测试(maxItems=5): count=65, truncated=true, items=5");
            Check(Near(minX, 0.0) && Near(maxX, 645.0), "截断测试: 合集 bounds 统计全部 65 个实体而非仅前 5 个");
        }

        // maxItems 钳位下限：<= 0 钳位为 1
        {
            const std::string json = SaSelectionExport::ExportSelectionJson(scene, allIds, nullptr, 0);
            JsonSerializer js;
            Check(js.Parse(json), "钳位下限(maxItems=0): 合法 JSON");
            std::size_t n = 0;
            js.BeginArray("items", n);
            js.EndArray();
            Check(n == 1, "钳位下限(maxItems=0): items 钳位为 1");
        }

        // maxItems 钳位上限：> 50 钳位为 50
        {
            const std::string json = SaSelectionExport::ExportSelectionJson(scene, allIds, nullptr, 200);
            JsonSerializer js;
            Check(js.Parse(json), "钳位上限(maxItems=200): 合法 JSON");
            std::size_t n = 0;
            js.BeginArray("items", n);
            js.EndArray();
            Check(n == 50, "钳位上限(maxItems=200): items 钳位为 50");
        }
    }

    void TestUtf8AndJsonEscaping()
    {
        Scene scene;
        // 含引号、反斜杠、换行、制表符、低位控制字符的图层名与中文文字
        const std::string trickyLayer = "A-\"墙\"\\层\t1";
        const LayerID lyrId = scene.GetLayerManager().AddLayer(trickyLayer);

        std::string specialText = "防火门\"FM甲\"\n\\路径\t表\x01\x1F结束";
        const Object::ObjectID specialId = scene.NextObjectID();
        auto t1 = std::make_unique<TextEntity>(specialId, Math::Point3{ 0, 0, 0 }, specialText, 10.0f);
        t1->SetLayerId(lyrId);
        scene.AddEntity(std::move(t1));

        // 超过 120 个汉字的长中文字符串，验证 UTF-8 截断不切碎多字节字符
        std::string longChinese;
        for (int i = 0; i < 150; ++i)
            longChinese += "建"; // 每个汉字 3 字节 UTF-8，共 150 字符 / 450 字节
        const Object::ObjectID longId = scene.NextObjectID();
        auto t2 = std::make_unique<TextEntity>(longId, Math::Point3{ 0, 50, 0 }, longChinese, 10.0f);
        t2->SetLayerId(lyrId);
        scene.AddEntity(std::move(t2));

        const std::string json = SaSelectionExport::ExportSelectionJson(scene, { specialId, longId }, nullptr, 20);
        Check(IsValidUtf8(json), "UTF-8 与转义测试: 整体 JSON 输出为合法 UTF-8");
        Check(json.find("\\u0001") != std::string::npos && json.find("\\u001f") != std::string::npos,
              "UTF-8 与转义测试: 控制字符正确转义为 \\u00XX");

        JsonSerializer js;
        Check(js.Parse(json), "UTF-8 与转义测试: JsonSerializer 严格解析通过");
        std::size_t n = 0;
        Check(js.BeginArray("items", n) && n == 2, "UTF-8 与转义测试: items 数量为 2");
        if (n == 2)
        {
            if (js.BeginElement(0))
            {
                std::string layer, text;
                js.Value("layer", layer);
                js.Value("text", text);
                Check(layer == trickyLayer, "UTF-8 与转义测试: 含引号/反斜杠/制表符图层名往返无损");
                Check(text == specialText, "UTF-8 与转义测试: 含引号/反斜杠/换行/控制字符文字往返无损");
                js.EndElement();
            }
            if (js.BeginElement(1))
            {
                std::string text;
                js.Value("text", text);
                Check(IsValidUtf8(text), "UTF-8 截断测试: 截断后的中文字符串仍是合法 UTF-8");
                Check(CountUtf8Chars(text) == SaSelectionExport::kMaxTextChars, "UTF-8 截断测试: 截断至精确 120 字符");
                Check(text.size() == SaSelectionExport::kMaxTextChars * 3, "UTF-8 截断测试: 120 个汉字字节数为 360（未半截截断）");
                js.EndElement();
            }
        }
        js.EndArray();
    }

    void TestSelectionVersionAndLayout()
    {
        // 无文档返回 0
        Check(SaSelectionExport::ComputeEmbedSelectionVersion(nullptr, nullptr) == 0u,
              "ComputeEmbedSelectionVersion: 无文档返回 0");

        auto doc1 = std::make_unique<Document>();
        Viewport vp(800.0f, 600.0f);
        Editor ed1;
        ed1.Bind(*doc1, vp);

        DrawingLayout modelLayout;
        modelLayout.Name = "Model";
        modelLayout.Model = true;
        modelLayout.Order = 0;
        DrawingLayout sheetLayout;
        sheetLayout.Name = "Layout1";
        sheetLayout.Model = false;
        sheetLayout.Order = 1;
        sheetLayout.OwnerKey = doc1->GetScene().NewLayoutKey();
        doc1->GetScene().SetLayouts({ modelLayout, sheetLayout });

        const Object::ObjectID mId = doc1->GetScene().NextObjectID();
        doc1->GetScene().AddEntity(std::make_unique<LineEntity>(mId, Math::Point3{ 0, 0, 0 }, Math::Point3{ 10, 0, 0 }));

        const Object::ObjectID pId = doc1->GetScene().NextObjectID();
        auto pLine = std::make_unique<LineEntity>(pId, Math::Point3{ 0, 0, 0 }, Math::Point3{ 20, 0, 0 });
        EntityAttr pa = pLine->GetAttr();
        pa.InPaperSpace = true;
        pa.LayoutKey    = sheetLayout.OwnerKey;
        pLine->SetAttr(pa);
        doc1->GetScene().AddEntity(std::move(pLine));

        const std::uint32_t v0 = SaSelectionExport::ComputeEmbedSelectionVersion(doc1.get(), &ed1);
        const std::uint32_t v0Repeat = SaSelectionExport::ComputeEmbedSelectionVersion(doc1.get(), &ed1);
        Check(v0 != 0u && v0 == v0Repeat, "ComputeEmbedSelectionVersion: 有文档时非 0 且静止稳定");

        // 改变选中集合 → 版本号必变
        const uint64_t selVerBefore = ed1.SelectionVersion();
        ed1.SetSelection({ mId });
        Check(ed1.SelectionVersion() > selVerBefore, "Editor::SelectionVersion: SetSelection 后递增");
        const std::uint32_t v1 = SaSelectionExport::ComputeEmbedSelectionVersion(doc1.get(), &ed1);
        Check(v1 != 0u && v1 != v0, "ComputeEmbedSelectionVersion: 改变选中集合后版本号变化");

        // 切换到布局空间 → 版本号必变，且布局过滤能命中图纸空间实体
        CameraState cam = vp.GetCamera().GetState();
        Check(doc1->SetActiveLayout(1, cam), "Document::SetActiveLayout(1) 成功");
        const std::uint32_t v2 = SaSelectionExport::ComputeEmbedSelectionVersion(doc1.get(), &ed1);
        Check(v2 != 0u && v2 != v1, "ComputeEmbedSelectionVersion: 切换布局后版本号变化");

        ed1.SetSelection({ pId, mId });
        const std::string paperJson = SaSelectionExport::ExportSelectionJson(
            doc1->GetScene(), ed1.GetSelection(), doc1->GetCadSource(), 20, doc1.get());
        JsonSerializer js;
        Check(js.Parse(paperJson), "图纸空间选中导出: 输出合法 JSON");
        int32_t pCount = 0;
        js.Value("count", pCount);
        Check(pCount == 1, "图纸空间选中导出: 仅包含当前活动布局的图纸空间实体（排除模型空间实体）");

        // 换主图（关闭重建文档，即使地址复用 InstanceID 也不同）→ 版本号必变
        ed1.Unbind();
        doc1.reset();
        auto doc2 = std::make_unique<Document>();
        Editor ed2;
        ed2.Bind(*doc2, vp);
        const std::uint32_t vNewDoc = SaSelectionExport::ComputeEmbedSelectionVersion(doc2.get(), &ed2);
        Check(vNewDoc != 0u && vNewDoc != v0, "ComputeEmbedSelectionVersion: 换主图重建文档后版本号变化（防 ABA）");
    }

    // ── CAD-23：带孔 / 洞内岛 / 共边孔 / 分离多外环 Hatch 净面积 ────────────────────────
    void TestHatchNetArea()
    {
        auto makeRect = [](double x0, double y0, double x1, double y1) {
            return Polyline(std::vector<Math::Point3>{
                { x0, y0, 0 }, { x1, y0, 0 }, { x1, y1, 0 }, { x0, y1, 0 }, { x0, y0, 0 }
            });
        };

        // 1. 外环 100 ㎡ + 内部开洞 10 ㎡ → 净面积 90 ㎡（不能错算成 110）
        HatchEntity hatchWithHole(1, std::vector<Polyline>{
            makeRect(0, 0, 10, 10),
            makeRect(2, 2, 4, 7) // 2x5 = 10
        });
        const auto areaHole = ComputeHatchNetArea(hatchWithHole);
        Check(areaHole.has_value() && Near(*areaHole, 90.0, 1e-3), "CAD-23: 外环 100 减洞口 10 = 90");

        // 2. 外环 100 + 洞口 36 + 洞内岛 4 → 净面积 68
        HatchEntity hatchWithIsland(2, std::vector<Polyline>{
            makeRect(0, 0, 10, 10),
            makeRect(2, 2, 8, 8), // 6x6 = 36
            makeRect(4, 4, 6, 6)  // 2x2 = 4
        });
        const auto areaIsland = ComputeHatchNetArea(hatchWithIsland);
        Check(areaIsland.has_value() && Near(*areaIsland, 68.0, 1e-3), "CAD-23: 外环 100 - 洞 36 + 岛 4 = 68");

        // 3. 孔洞与外环共边（测试扫描线严格内点不因共边误判）
        HatchEntity hatchSharedEdge(3, std::vector<Polyline>{
            makeRect(0, 0, 10, 10),
            makeRect(0, 2, 4, 6) // 4x4 = 16，左侧与外环共边 x=0
        });
        const auto areaShared = ComputeHatchNetArea(hatchSharedEdge);
        Check(areaShared.has_value() && Near(*areaShared, 84.0, 1e-3), "CAD-23: 共边洞口外环 100 - 16 = 84");

        // 4. 两个互不相交的外环：50 + 30 = 80
        HatchEntity hatchDisjoint(4, std::vector<Polyline>{
            makeRect(0, 0, 10, 5),
            makeRect(20, 0, 26, 5)
        });
        const auto areaDisjoint = ComputeHatchNetArea(hatchDisjoint);
        Check(areaDisjoint.has_value() && Near(*areaDisjoint, 80.0, 1e-3), "CAD-23: 两个独立外环 50 + 30 = 80");

        // 5. 经 ExportSelectionJson 端到端校验带孔 Hatch 面积
        Scene scene;
        const Object::ObjectID hid = scene.NextObjectID();
        scene.AddEntity(std::make_unique<HatchEntity>(hid, std::vector<Polyline>{
            makeRect(0, 0, 10, 10), makeRect(2, 2, 4, 7)
        }));
        const std::string json = SaSelectionExport::ExportSelectionJson(scene, { hid }, nullptr, 20);
        JsonSerializer js;
        Check(js.Parse(json), "CAD-23: 带孔 Hatch ExportSelectionJson 合法 JSON");
        std::size_t n = 0;
        double exportedArea = 0.0;
        if (js.BeginArray("items", n) && n == 1 && js.BeginElement(0))
        {
            js.Value("area", exportedArea);
            js.EndElement();
        }
        js.EndArray();
        Check(Near(exportedArea, 90.0, 1e-2), "CAD-23: ExportSelectionJson 输出带孔 Hatch 真实净面积 90");

        // 6. CAD-R2-12：大测绘坐标 (4e8, 3.5e9) 下的 50×50、带孔与相交环奇偶净面积精度校验
        const double ox = 4.0e8;
        const double oy = 3.5e9;
        {
            HatchEntity largeSquare(10, std::vector<Polyline>{
                makeRect(ox, oy, ox + 50.0, oy + 50.0)
            });
            const auto aSq = ComputeHatchNetArea(largeSquare);
            Check(aSq.has_value() && Near(*aSq, 2500.0, 1e-6),
                  "CAD-R2-12: 大测绘坐标 (4e8, 3.5e9) 下 50×50 方形面积精确等于 2500（无 2560 有效位抵消）");

            HatchEntity largeWithHole(11, std::vector<Polyline>{
                makeRect(ox, oy, ox + 50.0, oy + 50.0),
                makeRect(ox + 20.0, oy + 20.0, ox + 30.0, oy + 30.0)
            });
            const auto aHole = ComputeHatchNetArea(largeWithHole);
            Check(aHole.has_value() && Near(*aHole, 2400.0, 1e-6),
                  "CAD-R2-12: 大测绘坐标下 50×50 减 10×10 孔洞精确等于 2400");

            // 部分相交的两个 10×10 方形（重叠 5×5=25）：奇偶净面积 = 100 + 100 - 2*25 = 150
            HatchEntity overlapping(12, std::vector<Polyline>{
                makeRect(ox, oy, ox + 10.0, oy + 10.0),
                makeRect(ox + 5.0, oy + 5.0, ox + 15.0, oy + 15.0)
            });
            const auto aOverlap = ComputeHatchNetArea(overlapping);
            Check(aOverlap.has_value() && Near(*aOverlap, 150.0, 1e-6),
                  "CAD-R2-12: 大坐标下部分相交双方形精确输出奇偶净面积 150");

            // 斜边内部相交（领结自交四边形）：(0,0)-(10,10)-(10,0)-(0,10) 交点 (5,5)，两三角形各 25，净面积 50
            HatchEntity bowtie(13, std::vector<Polyline>{
                Polyline(std::vector<Math::Point3>{
                    { ox, oy, 0 }, { ox + 10.0, oy + 10.0, 0 },
                    { ox + 10.0, oy, 0 }, { ox, oy + 10.0, 0 }, { ox, oy, 0 }
                })
            });
            const auto aBowtie = ComputeHatchNetArea(bowtie);
            Check(aBowtie.has_value() && Near(*aBowtie, 50.0, 1e-6),
                  "CAD-R2-12: 严格内部边-边相交自交环按条带切分精确算出奇偶净面积 50");

            // 超大边集（>2048 边）：无交叉凸多边形精确可算，带交叉边时诚实返回 nullopt
            std::vector<Math::Point3> bigCirclePts;
            constexpr int kN = 2400;
            bigCirclePts.reserve(kN + 1);
            for (int i = 0; i <= kN; ++i)
            {
                const double a = Math::TwoPI * i / static_cast<double>(kN);
                bigCirclePts.push_back({ ox + 100.0 * std::cos(a), oy + 100.0 * std::sin(a), 0.0 });
            }
            const double expectedPolyArea = 0.5 * kN * 100.0 * 100.0 * std::sin(Math::TwoPI / static_cast<double>(kN));
            HatchEntity bigNonCross(14, std::vector<Polyline>{ Polyline(bigCirclePts) });
            const auto aBigOk = ComputeHatchNetArea(bigNonCross);
            Check(aBigOk.has_value() && Near(*aBigOk, expectedPolyArea, 1e-3),
                  "CAD-R2-12: 超过 2048 边的非相交大环仍精确计算净面积");

            // 竖边 x = ox ± 61.3 在圆周斜段内部穿越（acos(0.613) 非 360°/2400 整数倍），触发条带内边序倒置
            HatchEntity bigCross(15, std::vector<Polyline>{
                Polyline(bigCirclePts),
                makeRect(ox - 61.3, oy - 120.0, ox + 61.3, oy + 120.0)
            });
            const auto aBigCross = ComputeHatchNetArea(bigCross);
            Check(!aBigCross.has_value(),
                  "CAD-R2-12: 超过 2048 边且存在条带内边序倒置的相交环诚实返回 std::nullopt");
        }
    }

    // ── CAD-28 & CAD-29：大选区准确 count/scannedCount/statsTruncated 与严格 UTF-8 校验 ──
    void TestLargeSelectionStatsAndStrictUtf8()
    {
        Scene scene;
        std::unordered_set<Object::ObjectID> ids;
        for (int i = 0; i < 120; ++i)
        {
            const Object::ObjectID id = scene.NextObjectID();
            scene.AddEntity(std::make_unique<LineEntity>(
                id, Math::Point3{ static_cast<double>(i), 0.0, 0.0 },
                Math::Point3{ static_cast<double>(i + 1), 1.0, 0.0 }));
            ids.insert(id);
        }

        // maxScan=50, maxItems=10 → count=120, scannedCount=50, statsTruncated=true, truncated=true
        {
            const std::string json = SaSelectionExport::ExportSelectionJson(scene, ids, nullptr, 10, nullptr, 50);
            JsonSerializer js;
            Check(js.Parse(json), "CAD-28(maxScan=50): 合法 JSON");
            int32_t count = 0, scannedCount = 0;
            bool truncated = false, statsTruncated = false;
            js.Value("count", count);
            js.Value("scannedCount", scannedCount);
            js.Value("truncated", truncated);
            js.Value("statsTruncated", statsTruncated);
            std::size_t n = 0;
            js.BeginArray("items", n);
            js.EndArray();
            int32_t lineTypeCount = 0;
            if (js.BeginObject("typeCounts"))
            {
                js.Value("LINE", lineTypeCount);
                js.EndObject();
            }
            Check(count == 120 && scannedCount == 50 && statsTruncated && truncated && n == 10 && lineTypeCount == 50,
                  "CAD-28: count=120 精确总数, scannedCount=50, statsTruncated=true, truncated=true, items=10");
        }

        // 超 50000 默认扫描上限端到端测试
        {
            Scene bigScene;
            std::unordered_set<Object::ObjectID> bigIds;
            bigIds.reserve(50020);
            for (int i = 0; i < 50020; ++i)
            {
                const Object::ObjectID id = bigScene.NextObjectID();
                bigScene.AddEntity(std::make_unique<PointEntity>(id, Math::Point3{ static_cast<double>(i), 0.0, 0.0 }));
                bigIds.insert(id);
            }
            const std::string json = SaSelectionExport::ExportSelectionJson(bigScene, bigIds, nullptr, 20);
            JsonSerializer js;
            Check(js.Parse(json), "CAD-28(>50000 默认上限): 合法 JSON");
            int32_t count = 0, scannedCount = 0;
            bool truncated = false, statsTruncated = false;
            js.Value("count", count);
            js.Value("scannedCount", scannedCount);
            js.Value("truncated", truncated);
            js.Value("statsTruncated", statsTruncated);
            Check(count == 50020 && scannedCount == 50000 && statsTruncated && truncated,
                  "CAD-28(>50000): 准确输出 count=50020, scannedCount=50000, statsTruncated=true");
        }

        // CAD-29：非法 UTF-8（过长编码、代理项、超 U+10FFFF、截断序列、孤立续字节）在图层名/块名/文字中全部替换为 U+FFFD
        {
            Scene badScene;
            const std::string overlong2("\xC0\xAF", 2);       // 过长 '/'
            const std::string overlong3("\xE0\x80\xAF", 3);   // 3 字节过长
            const std::string surrogate("\xED\xA0\x80", 3);   // UTF-16 U+D800
            const std::string tooHigh("\xF4\x90\x80\x80", 4); // U+110000
            const std::string trunc3("\xE4\xB8", 2);          // 残缺 3 字节中文
            const std::string badLayer = "L_" + overlong2 + "_" + surrogate;
            const std::string badText  = "前" + overlong3 + tooHigh + trunc3 + "后";
            const LayerID lid = badScene.GetLayerManager().AddLayer(badLayer);
            const Object::ObjectID tid = badScene.NextObjectID();
            auto te = std::make_unique<TextEntity>(tid, Math::Point3{ 0, 0, 0 }, badText, 10.0f);
            te->SetLayerId(lid);
            badScene.AddEntity(std::move(te));

            const std::string json = SaSelectionExport::ExportSelectionJson(badScene, { tid }, nullptr, 20);
            Check(IsValidUtf8(json), "CAD-29: 含过长编码/代理项/越界码点/残缺字节的 JSON 严格合法 UTF-8");
            Check(json.find("\xEF\xBF\xBD") != std::string::npos, "CAD-29: 非法字节序列被替换为 U+FFFD");
            JsonSerializer js;
            Check(js.Parse(json), "CAD-29: JsonSerializer 解析替换后的 JSON 通过");
            std::size_t n = 0;
            std::string outText;
            if (js.BeginArray("items", n) && n == 1 && js.BeginElement(0))
            {
                js.Value("text", outText);
                js.EndElement();
            }
            js.EndArray();
            Check(outText.find("前") != std::string::npos && outText.find("后") != std::string::npos,
                  "CAD-29: 非法序列前后的合法中文字符完整保留");
        }
    }

    // ── CAD-24：SaTextExport MINSERT 行列阵列展开 + 世界字高换算 + ATTRIB 不重复套矩阵 ──
    void TestSaTextExportArrayAndHeight()
    {
        Scene scene;
        auto blk = std::make_unique<BlockEntity>(scene.NextObjectID(), "TITLE_BLK");
        blk->AddEntity(std::make_unique<TextEntity>(scene.NextObjectID(), Math::Point3{ 10, 20, 0 }, "ROOM", 100.0f, 0.0f));
        blk->AddEntity(std::make_unique<MTextEntity>(
            scene.NextObjectID(), 0, "VERTICAL", Math::Point3{ 0, 0, 0 }, 50.0, Math::HalfPI, 500.0));
        const BlockID blkId = scene.GetBlockTable().Add(std::move(blk));

        auto ins = std::make_unique<InsertEntity>(scene.NextObjectID(), blkId, "TITLE_BLK", Math::Point3{ 100, 200, 0 });
        ins->SetScale({ 2.0, 3.0, 1.0 });
        ins->SetArray(2, 3, 1000.0, 500.0); // 2 列 × 3 行 = 6 单元
        ins->AddAttrib(std::make_unique<AttribEntity>(
            scene.NextObjectID(), "NO", "A-01", Math::Point3{ 500, 600, 0 }, 80.0, 0.0));
        scene.ResolveInsert(*ins);
        scene.AddEntity(std::move(ins));

        const std::string tsv = SaTextExport::ExportTsv(scene);
        int textCount = 0, mtextCount = 0, attribCount = 0;
        bool sawCell12 = false;
        bool textHeightOk = true;
        bool mtextHeightOk = true;
        bool attribOk = false;

        std::size_t pos = tsv.find('\n'); // 跳过表头
        while (pos != std::string::npos && pos + 1 < tsv.size())
        {
            const std::size_t next = tsv.find('\n', pos + 1);
            const std::string line = tsv.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
            pos = next;
            if (line.empty()) continue;

            std::vector<std::string> cols;
            std::size_t cStart = 0;
            while (true)
            {
                const std::size_t tab = line.find('\t', cStart);
                cols.push_back(line.substr(cStart, tab == std::string::npos ? std::string::npos : tab - cStart));
                if (tab == std::string::npos) break;
                cStart = tab + 1;
            }
            if (cols.size() < 8) continue;
            const double wx = std::stod(cols[1]);
            const double wy = std::stod(cols[2]);
            const double wh = std::stod(cols[3]);

            if (cols[0] == "TEXT")
            {
                ++textCount;
                // rot=0 字高沿局部 +Y，经 scale(2,3) 后世界字高为 100*3 = 300
                if (!Near(wh, 300.0, 0.05))
                    textHeightOk = false;
                // 第 (col=1, row=2) 单元：pos=(100+1000, 200+1000)=(1100,1200)，局部 (10,20)*scale(2,3)=(20,60) → (1120,1260)
                if (Near(wx, 1120.0, 0.1) && Near(wy, 1260.0, 0.1))
                    sawCell12 = true;
            }
            else if (cols[0] == "MTEXT")
            {
                ++mtextCount;
                // rot=pi/2 字高沿局部 -X，经 scale(2,3) 后世界字高为 50*2 = 100
                if (!Near(wh, 100.0, 0.05))
                    mtextHeightOk = false;
            }
            else if (cols[0] == "ATTRIB")
            {
                ++attribCount;
                attribOk = Near(wx, 500.0, 0.1) && Near(wy, 600.0, 0.1) && Near(wh, 80.0, 0.05) && cols[6] == "NO" && cols[7] == "A-01";
            }
        }

        Check(textCount == 6 && mtextCount == 6 && sawCell12, "CAD-24: MINSERT 2×3 阵列完整展开全部 6 单元");
        Check(textHeightOk && mtextHeightOk, "CAD-24: 块内文字按变换矩阵和旋转方向准确换算世界字高");
        Check(attribCount == 1 && attribOk, "CAD-24: ATTRIB 仅输出一次且保持世界坐标与字高不重复套本级矩阵");

        // CAD-R2-09：双层 50×50 嵌套 MINSERT 爆炸（理论 625 万行）快速触发硬预算并返回空串，不卡死不 OOM
        {
            Scene bombScene;
            auto leafBlk = std::make_unique<BlockEntity>(bombScene.NextObjectID(), "LEAF_TXT");
            leafBlk->AddEntity(std::make_unique<TextEntity>(
                bombScene.NextObjectID(), Math::Point3{ 0, 0, 0 }, "CELL", 10.0f));
            const BlockID leafId = bombScene.GetBlockTable().Add(std::move(leafBlk));

            auto midBlk = std::make_unique<BlockEntity>(bombScene.NextObjectID(), "MID_ARR");
            auto innerIns = std::make_unique<InsertEntity>(
                bombScene.NextObjectID(), leafId, "LEAF_TXT", Math::Point3{ 0, 0, 0 });
            innerIns->SetArray(50, 50, 10.0, 10.0);
            innerIns->SetBlock(bombScene.GetBlockTable().Find(leafId));
            midBlk->AddEntity(std::move(innerIns));
            const BlockID midId = bombScene.GetBlockTable().Add(std::move(midBlk));

            auto outerIns = std::make_unique<InsertEntity>(
                bombScene.NextObjectID(), midId, "MID_ARR", Math::Point3{ 0, 0, 0 });
            outerIns->SetArray(50, 50, 600.0, 600.0);
            bombScene.ResolveInsert(*outerIns);
            bombScene.AddEntity(std::move(outerIns));

            const auto t0 = std::chrono::steady_clock::now();
            SaTextExport::Result res{};
            const std::string bombTsv = SaTextExport::ExportTsv(bombScene, SaTextExport::Budget{}, &res);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            Check(bombTsv.empty() && !res.Ok() && res.Code == SaTextExport::Status::RowBudgetExceeded && ms < 2000,
                  "CAD-R2-09: 双层 50×50 嵌套 MINSERT 触发默认行数预算快速中止并返回空串");

            // 字节预算校验
            SaTextExport::Budget tinyBytes{};
            tinyBytes.MaxBytes = 128;
            SaTextExport::Result byteRes{};
            const std::string byteTsv = SaTextExport::ExportTsv(bombScene, tinyBytes, &byteRes);
            Check(byteTsv.empty() && byteRes.Code == SaTextExport::Status::ByteBudgetExceeded,
                  "CAD-R2-09: 输出字节超限时立即中止并返回 ByteBudgetExceeded");

            // 流式下游写入失败校验
            std::size_t chunksSeen = 0;
            const SaTextExport::Result sinkRes = SaTextExport::ExportTsvStream(
                bombScene,
                [&](std::string_view) {
                    ++chunksSeen;
                    return chunksSeen < 3;
                });
            Check(sinkRes.Code == SaTextExport::Status::SinkError && chunksSeen == 3,
                  "CAD-R2-09: ExportTsvStream 在 sink 失败时立即中止");
        }

        // CAD-R3-07：无文字的双层 50×50 嵌套 MINSERT 经无字块缓存剪枝零单元快速通过
        {
            Scene emptyBomb;
            auto emptyBlk = std::make_unique<BlockEntity>(emptyBomb.NextObjectID(), "EMPTY_LEAF");
            const BlockID leafId = emptyBomb.GetBlockTable().Add(std::move(emptyBlk));

            auto midBlk = std::make_unique<BlockEntity>(emptyBomb.NextObjectID(), "MID_EMPTY");
            auto innerIns = std::make_unique<InsertEntity>(
                emptyBomb.NextObjectID(), leafId, "EMPTY_LEAF", Math::Point3{ 0, 0, 0 });
            innerIns->SetArray(50, 50, 10.0, 10.0);
            innerIns->SetBlock(emptyBomb.GetBlockTable().Find(leafId));
            midBlk->AddEntity(std::move(innerIns));
            const BlockID midId = emptyBomb.GetBlockTable().Add(std::move(midBlk));

            auto outerIns = std::make_unique<InsertEntity>(
                emptyBomb.NextObjectID(), midId, "MID_EMPTY", Math::Point3{ 0, 0, 0 });
            outerIns->SetArray(50, 50, 600.0, 600.0);
            emptyBomb.ResolveInsert(*outerIns);
            emptyBomb.AddEntity(std::move(outerIns));

            SaTextExport::Result cellRes{};
            const std::string cellTsv = SaTextExport::ExportTsv(emptyBomb, SaTextExport::Budget{}, &cellRes);
            Check(!cellTsv.empty() && cellRes.Ok() && cellRes.ArrayCells == 0 && cellRes.Rows == 0,
                  "CAD-R3-07: 无文字嵌套 MINSERT 经无字块缓存直接剪枝跳过且不耗阵列预算");
        }

        // CAD-R2-09：含文字深层嵌套 MINSERT（每条文字伴随 3+ 单元访问）在触及行数上限前优先触发 250,000 阵列单元硬预算
        {
            Scene arrayCellBomb;
            auto leafTxt = std::make_unique<BlockEntity>(arrayCellBomb.NextObjectID(), "LEAF_CELL_TXT");
            leafTxt->AddEntity(std::make_unique<TextEntity>(
                arrayCellBomb.NextObjectID(), Math::Point3{ 0, 0, 0 }, "T", 10.0f));
            const BlockID leafId = arrayCellBomb.GetBlockTable().Add(std::move(leafTxt));

            auto wrap1 = std::make_unique<BlockEntity>(arrayCellBomb.NextObjectID(), "WRAP_1");
            auto ins1 = std::make_unique<InsertEntity>(
                arrayCellBomb.NextObjectID(), leafId, "LEAF_CELL_TXT", Math::Point3{ 0, 0, 0 });
            ins1->SetBlock(arrayCellBomb.GetBlockTable().Find(leafId));
            wrap1->AddEntity(std::move(ins1));
            const BlockID wrap1Id = arrayCellBomb.GetBlockTable().Add(std::move(wrap1));

            auto wrap2 = std::make_unique<BlockEntity>(arrayCellBomb.NextObjectID(), "WRAP_2");
            auto ins2 = std::make_unique<InsertEntity>(
                arrayCellBomb.NextObjectID(), wrap1Id, "WRAP_1", Math::Point3{ 0, 0, 0 });
            ins2->SetBlock(arrayCellBomb.GetBlockTable().Find(wrap1Id));
            wrap2->AddEntity(std::move(ins2));
            const BlockID wrap2Id = arrayCellBomb.GetBlockTable().Add(std::move(wrap2));

            auto midArr = std::make_unique<BlockEntity>(arrayCellBomb.NextObjectID(), "MID_CELL_ARR");
            auto innerIns = std::make_unique<InsertEntity>(
                arrayCellBomb.NextObjectID(), wrap2Id, "WRAP_2", Math::Point3{ 0, 0, 0 });
            innerIns->SetArray(50, 50, 10.0, 10.0);
            innerIns->SetBlock(arrayCellBomb.GetBlockTable().Find(wrap2Id));
            midArr->AddEntity(std::move(innerIns));
            const BlockID midId = arrayCellBomb.GetBlockTable().Add(std::move(midArr));

            auto outerIns = std::make_unique<InsertEntity>(
                arrayCellBomb.NextObjectID(), midId, "MID_CELL_ARR", Math::Point3{ 0, 0, 0 });
            outerIns->SetArray(50, 50, 600.0, 600.0);
            arrayCellBomb.ResolveInsert(*outerIns);
            arrayCellBomb.AddEntity(std::move(outerIns));

            SaTextExport::Result cellRes{};
            const std::string cellTsv = SaTextExport::ExportTsv(arrayCellBomb, SaTextExport::Budget{}, &cellRes);
            Check(cellTsv.empty() && cellRes.Code == SaTextExport::Status::ArrayCellBudgetExceeded,
                  "CAD-R2-09: 含文字深层嵌套 MINSERT 严格受 250,000 阵列单元预算硬约束");
        }
    }

    // ── CAD-12 & CAD-13：纸张布局导出归属、退化批注框防 OOM、RAII 临时实体必清理 ────────
    void TestMarkupExportPaperSpaceAndDegenerate()
    {
        // 1. 退化矩形与非有限坐标必须被拒绝
        Check(!SaMarkupExport::BuildRevisionCloud(0, 0, 100, 0).has_value(), "CAD-13: 零高度退化框被拒绝");
        Check(!SaMarkupExport::BuildRevisionCloud(0, 0, 0, 100).has_value(), "CAD-13: 零宽度退化框被拒绝");
        Check(!SaMarkupExport::BuildRevisionCloud(10, 10, 0, 0).has_value(), "CAD-13: 反向矩形被拒绝");
        Check(!SaMarkupExport::BuildRevisionCloud(std::numeric_limits<double>::quiet_NaN(), 0, 10, 10).has_value(), "CAD-13: NaN 坐标被拒绝");
        Check(!SaMarkupExport::BuildRevisionCloud(0, 0, std::numeric_limits<double>::infinity(), 10).has_value(), "CAD-13: Inf 坐标被拒绝");

        // 2. 极端狭长矩形云线顶点数严格受限（防 OOM / 整型溢出）
        const auto skinny = SaMarkupExport::BuildRevisionCloud(0.0, 0.0, 1e6, 1e-3);
        Check(skinny.has_value() && skinny->Points.size() <= static_cast<std::size_t>(SaMarkupExport::kMaxArcsPerEdge) * 4 + 1,
              "CAD-13: 极端狭长矩形云线顶点数严格受 kMaxArcsPerEdge 上限约束");

        // 3. 多纸张布局导出归属校验（CAD-12）与 RAII 清理校验（CAD-13）
        Document doc;
        Scene& scene = doc.GetScene();
        DrawingLayout modelLayout;
        modelLayout.Name  = "Model";
        modelLayout.Model = true;
        modelLayout.Order = 0;
        DrawingLayout layoutA;
        layoutA.Name     = "LayoutA";
        layoutA.Model    = false;
        layoutA.Order    = 1;
        layoutA.OwnerKey = scene.NewLayoutKey();
        DrawingLayout layoutB;
        layoutB.Name     = "LayoutB";
        layoutB.Model    = false;
        layoutB.Order    = 2;
        layoutB.OwnerKey = scene.NewLayoutKey();
        scene.SetLayouts({ modelLayout, layoutA, layoutB });
        scene.AddEntity(std::make_unique<LineEntity>(scene.NextObjectID(), Math::Point3{ 0, 0, 0 }, Math::Point3{ 100, 0, 0 }));

        CameraState cam{};
        Check(doc.SetActiveLayout(2, cam) && doc.ActiveLayoutIsPaper(), "CAD-12: 切换到第二个纸张布局 LayoutB");
        const int beforeEntities = scene.EntityCount();
        const std::size_t beforeLayers = scene.GetLayerManager().GetAllLayerIDs().size();

        // 退化框导出失败且不污染场景
        std::string err;
        const auto badBytes = SaMarkupExport::ExportMarkupDwg(doc, { { 0, 0, 100, 0 } }, { "bad" }, err);
        Check(badBytes.empty() && !err.empty() && scene.EntityCount() == beforeEntities,
              "CAD-13: 退化框导出失败返回错误且不污染原场景");

        // 正常框在 LayoutB 导出，验证 RAII 清理 + 读回后归属 LayoutB 而非模型空间
        err.clear();
        const auto dwgBytes = SaMarkupExport::ExportMarkupDwg(doc, { { 10, 20, 110, 80 } }, { "P1-NOTE" }, err);
        Check(!dwgBytes.empty() && err.empty(), "CAD-12: 纸张布局导出批注 DWG 成功");
        Check(scene.EntityCount() == beforeEntities && scene.GetLayerManager().GetAllLayerIDs().size() == beforeLayers,
              "CAD-13: RAII 守卫在导出结束后完整清理临时实体与临时批注层");

        Scene loaded;
        std::string importErr;
        Check(ImportCad(dwgBytes, loaded, &importErr), "CAD-12: 导出的批注 DWG 可正常读回");
        uint64_t loadedLayoutBKey = 0;
        for (const auto& l : loaded.Layouts())
            if (!l.Model && l.Name == "LayoutB")
                loadedLayoutBKey = l.OwnerKey;
        Check(loadedLayoutBKey != 0, "CAD-12: 读回图纸保留目标布局 LayoutB");

        bool foundCloudInLayoutB = false;
        bool foundTextInLayoutB  = false;
        bool anyMarkupInModel    = false;
        loaded.ForEachObject([&](const Object& o) {
            if (!o.IsKindOf<Entity>()) return;
            const auto& e = static_cast<const Entity&>(o);
            const Layer* lyr = loaded.GetLayerManager().GetLayer(e.GetLayerID());
            if (!lyr || lyr->GetName() != SaMarkupExport::kMarkupLayerName) return;
            if (!e.GetAttr().InPaperSpace)
            {
                anyMarkupInModel = true;
                return;
            }
            if (e.GetAttr().LayoutKey == loadedLayoutBKey)
            {
                if (e.IsKindOf<PolylineEntity>()) foundCloudInLayoutB = true;
                if (e.IsKindOf<TextEntity>() && static_cast<const TextEntity&>(e).GetText() == "P1-NOTE")
                    foundTextInLayoutB = true;
            }
        });
        Check(foundCloudInLayoutB && foundTextInLayoutB && !anyMarkupInModel,
              "CAD-12: 导出云线与说明文字精确落入目标纸张布局 LayoutB，未掉入模型空间");

        // 4. CAD-R2-02 真实生产路径：先写出多布局 DWG（含模型/LayoutA/LayoutB 既有实体），
        // 经 Document::LoadFromFile 携带非空 CadSource 打开，切至 LayoutB 导出批注再重读验证
        {
            Scene srcScene;
            DrawingLayout ml; ml.Name = "Model"; ml.Model = true; ml.Order = 0;
            DrawingLayout la; la.Name = "LayoutA"; la.Model = false; la.Order = 1;
            DrawingLayout lb; lb.Name = "LayoutB"; lb.Model = false; lb.Order = 2;
            srcScene.SetLayouts({ ml, la, lb });
            const uint64_t seedKeyA = srcScene.Layouts()[1].OwnerKey;
            const uint64_t seedKeyB = srcScene.Layouts()[2].OwnerKey;

            srcScene.AddEntity(std::make_unique<LineEntity>(
                srcScene.NextObjectID(), Math::Point3{ 0, 0, 0 }, Math::Point3{ 500, 0, 0 }));
            auto lineA = std::make_unique<LineEntity>(
                srcScene.NextObjectID(), Math::Point3{ 0, 10, 0 }, Math::Point3{ 100, 10, 0 });
            lineA->GetAttr().InPaperSpace = true;
            lineA->GetAttr().LayoutKey    = seedKeyA;
            srcScene.AddEntity(std::move(lineA));
            auto lineB = std::make_unique<LineEntity>(
                srcScene.NextObjectID(), Math::Point3{ 0, 20, 0 }, Math::Point3{ 120, 20, 0 });
            lineB->GetAttr().InPaperSpace = true;
            lineB->GetAttr().LayoutKey    = seedKeyB;
            srcScene.AddEntity(std::move(lineB));

            const auto seedDwg = ExportCad(srcScene, CadFileKind::Dwg, CadSaveVersion::R2013);
            Check(!seedDwg.empty(), "CAD-R2-02: 生成含多布局与既有实体的种子 DWG 成功");

            const auto tmpFile = std::filesystem::temp_directory_path() / "minicad_r2_02_markup_multilayout.dwg";
            struct TmpFileCleaner {
                std::filesystem::path p;
                ~TmpFileCleaner() { std::error_code ec; std::filesystem::remove(p, ec); }
            } cleaner{ tmpFile };
            {
                std::ofstream out(tmpFile, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(seedDwg.data()), static_cast<std::streamsize>(seedDwg.size()));
            }
            const auto u8Path = tmpFile.u8string();
            const std::string utf8Tmp(reinterpret_cast<const char*>(u8Path.data()), u8Path.size());

            Document openedDoc;
            std::string openErr;
            Check(openedDoc.LoadFromFile(utf8Tmp, &openErr) && openedDoc.GetCadSource() != nullptr,
                  "CAD-R2-02: Document 打开真实多布局 DWG 并保留非空 CadSource");

            int layoutBIndex = -1;
            uint64_t origModelHandle = 0, origLayoutAHandle = 0, origLayoutBHandle = 0;
            uint64_t origLayoutAKey = 0, origLayoutBKey = 0;
            for (std::size_t i = 0; i < openedDoc.GetScene().Layouts().size(); ++i)
            {
                const auto& l = openedDoc.GetScene().Layouts()[i];
                if (!l.Model && l.Name == "LayoutA") origLayoutAKey = l.OwnerKey;
                if (!l.Model && l.Name == "LayoutB") { origLayoutBKey = l.OwnerKey; layoutBIndex = static_cast<int>(i); }
            }
            openedDoc.GetScene().ForEachObject([&](const Object& o) {
                if (!o.IsKindOf<LineEntity>()) return;
                const auto& e = static_cast<const LineEntity&>(o);
                const uint64_t h = LookupCadEntityHandle(openedDoc.GetCadSource(), e.GetID());
                if (!e.GetAttr().InPaperSpace) origModelHandle = h;
                else if (e.GetAttr().LayoutKey == origLayoutAKey) origLayoutAHandle = h;
                else if (e.GetAttr().LayoutKey == origLayoutBKey) origLayoutBHandle = h;
            });
            Check(layoutBIndex > 0 && origModelHandle != 0 && origLayoutAHandle != 0 && origLayoutBHandle != 0,
                  "CAD-R2-02: 打开后模型与各纸张布局既有实体句柄均已关联 CadSource");

            CameraState openedCam{};
            Check(openedDoc.SetActiveLayout(layoutBIndex, openedCam) && openedDoc.ActiveLayoutIsPaper(),
                  "CAD-R2-02: 切至真实打开图纸的 LayoutB");

            std::string markupErr;
            const auto exportedBytes = SaMarkupExport::ExportMarkupDwg(
                openedDoc, { { 15, 25, 115, 85 } }, { "R2-02-NOTE" }, markupErr);
            Check(!exportedBytes.empty() && markupErr.empty(),
                  "CAD-R2-02: 携带非空 CadSource 在 LayoutB 导出批注 DWG 成功");

            Scene reloaded;
            std::shared_ptr<const CadSource> reloadedSrc;
            Check(ImportCad(exportedBytes, reloaded, &importErr, nullptr, &reloadedSrc),
                  "CAD-R2-02: 导出的保留式批注 DWG 可正常重读");

            uint64_t reloadedAKey = 0, reloadedBKey = 0;
            for (const auto& l : reloaded.Layouts())
            {
                if (!l.Model && l.Name == "LayoutA") reloadedAKey = l.OwnerKey;
                if (!l.Model && l.Name == "LayoutB") reloadedBKey = l.OwnerKey;
            }
            bool gotCloudB = false, gotTextB = false, markupLeaked = false;
            bool keptModel = false, keptA = false, keptB = false;
            reloaded.ForEachObject([&](const Object& o) {
                if (!o.IsKindOf<Entity>()) return;
                const auto& e = static_cast<const Entity&>(o);
                const uint64_t h = LookupCadEntityHandle(reloadedSrc.get(), e.GetID());
                const Layer* lyr = reloaded.GetLayerManager().GetLayer(e.GetLayerID());
                if (lyr && lyr->GetName() == SaMarkupExport::kMarkupLayerName)
                {
                    if (!e.GetAttr().InPaperSpace || e.GetAttr().LayoutKey != reloadedBKey)
                    {
                        markupLeaked = true;
                        return;
                    }
                    if (e.IsKindOf<PolylineEntity>()) gotCloudB = true;
                    if (e.IsKindOf<TextEntity>() && static_cast<const TextEntity&>(e).GetText() == "R2-02-NOTE")
                        gotTextB = true;
                    return;
                }
                if (!e.GetAttr().InPaperSpace && h == origModelHandle) keptModel = true;
                if (e.GetAttr().InPaperSpace && e.GetAttr().LayoutKey == reloadedAKey && h == origLayoutAHandle) keptA = true;
                if (e.GetAttr().InPaperSpace && e.GetAttr().LayoutKey == reloadedBKey && h == origLayoutBHandle) keptB = true;
            });
            Check(gotCloudB && gotTextB && !markupLeaked,
                  "CAD-R2-02: 携带 CadSource 导出的纸张批注云线与文字精确落入 LayoutB，无模型空间或他页泄漏");
            Check(keptModel && keptA && keptB,
                  "CAD-R2-02: 原图模型空间与各纸张布局既有实体句柄全部完好保留");
        }
    }

    // ── CAD-02：PickPointTool 回调重入（立即重启工具 / 销毁 Editor）与取消回调重入保护 ──
    void TestPickPointToolReentrancy()
    {
        // 1. onPicked 回调内立即启动第二次拾点（替换销毁第一个 PickPointTool）
        {
            Document doc;
            Viewport vp(800.0f, 600.0f);
            Editor ed;
            ed.Bind(doc, vp);

            int firstPicked = 0, secondPicked = 0;
            ed.StartPickPointTool("p1", [&](const Math::Point3&) {
                ++firstPicked;
                ed.StartPickPointTool("p2", [&](const Math::Point3&) { ++secondPicked; });
            });

            InputEvent click{};
            click.Type   = InputEventType::MouseButtonDown;
            click.Button = MouseButton::Left;
            click.MouseX = 100;
            click.MouseY = 100;
            ed.OnInput(click);
            Check(firstPicked == 1 && ed.IsToolRunning(),
                  "CAD-02: onPicked 重入启动新拾点工具时不崩溃且新工具保持活跃");

            click.MouseX = 200;
            ed.OnInput(click);
            Check(secondPicked == 1 && !ed.IsToolRunning(),
                  "CAD-02: 重入启动的第二个拾点工具可正常完成第二次拾点");
        }

        // 2. onPicked 回调内直接销毁 Editor（模拟回调里关闭对比底图）
        {
            Document doc;
            Viewport vp(800.0f, 600.0f);
            auto ed = std::make_unique<Editor>();
            ed->Bind(doc, vp);

            bool destroyed = false;
            ed->StartPickPointTool("p1", [&](const Math::Point3&) {
                ed->Unbind();
                ed.reset();
                destroyed = true;
            });

            InputEvent click{};
            click.Type   = InputEventType::MouseButtonDown;
            click.Button = MouseButton::Left;
            click.MouseX = 120;
            click.MouseY = 120;
            ed->OnInput(click);
            Check(destroyed && ed == nullptr, "CAD-02: onPicked 重入销毁 Editor 无 UAF 崩溃");
        }

        // 3. onCancelled 回调内重入启动新工具 / 销毁 Editor
        {
            Document doc;
            Viewport vp(800.0f, 600.0f);
            auto ed = std::make_unique<Editor>();
            ed->Bind(doc, vp);

            bool cancelRestarted = false;
            ed->StartPickPointTool("p1", nullptr, [&]() {
                cancelRestarted = true;
                ed->StartPickPointTool("p2", nullptr, nullptr);
            });

            InputEvent esc{};
            esc.Type = InputEventType::KeyDown;
            esc.Key  = KeyCode::Escape;
            ed->OnInput(esc);
            Check(cancelRestarted && ed->IsToolRunning(),
                  "CAD-02: onCancelled 重入启动新工具无 UAF 且新工具存活");

            // 取消回调销毁 Editor
            bool cancelDestroyed = false;
            ed->StartPickPointTool("p3", nullptr, [&]() {
                ed->Unbind();
                ed.reset();
                cancelDestroyed = true;
            });
            ed->OnInput(esc);
            Check(cancelDestroyed && ed == nullptr, "CAD-02: onCancelled 重入销毁 Editor 无 UAF 崩溃");
        }
    }

    // ── CAD-04：原图既有 SA-问题批注 层实体水位线保护 + 浏览态撤销/重做门禁 ─────────────
    void TestMarkupProtectionAndUndoGate()
    {
        Document doc;
        Viewport vp(800.0f, 600.0f);
        Editor ed;
        ed.Bind(doc, vp);
        Scene& scene = doc.GetScene();

        // 模拟原图本来就有 SA-问题批注 层及既有实体
        const LayerID markupLayer = SaMarkupExport::EnsureMarkupLayer(scene);
        const Object::ObjectID origId = scene.NextObjectID();
        auto origLine = std::make_unique<LineEntity>(origId, Math::Point3{ 0, 0, 0 }, Math::Point3{ 100, 0, 0 });
        origLine->SetLayerId(markupLayer);
        scene.AddEntity(std::move(origLine));

        // 记录既有实体水位线，启用批注保护（图层本身未锁，模拟批注态解锁 SA-问题批注 层）
        scene.SyncNextObjectID();
        scene.SetProtectedEntityWatermark(scene.PeekNextObjectID());

        // 本会话新建批注实体（ID >= 水位线）
        const Object::ObjectID newId = scene.NextObjectID();
        auto newLine = std::make_unique<LineEntity>(newId, Math::Point3{ 0, 50, 0 }, Math::Point3{ 100, 50, 0 });
        newLine->SetLayerId(markupLayer);
        doc.GetCommandStack().Execute(std::make_unique<AddEntityCommand>(std::move(newLine)), scene);

        Check(scene.IsEntityLocked(*scene.GetEntity(origId)),
              "CAD-04: 原图既有 SA-问题批注 层实体受水位线保护被锁定");
        Check(!scene.IsEntityLocked(*scene.GetEntity(newId)),
              "CAD-04: 本会话新建批注实体不受水位线阻拦可编辑");

        // 即使强行将两者放入选择集再 DeleteSelected，也只删除新建实体、原图既有实体完好无损
        ed.SetSelection({ origId, newId });
        ed.DeleteSelected();
        Check(scene.Has(origId) && !scene.Has(newId),
              "CAD-04: DeleteSelected 跳过原图既有 SA-问题批注 实体，仅删除新建批注");

        // 浏览态撤销/重做门禁：拦截带额外修饰键的 Ctrl+Shift+Z / Ctrl+Shift+Y
        struct GateReset { ~GateReset() { CommandStack::SetUndoRedoGate(nullptr); } } resetGate;
        CommandStack::SetUndoRedoGate([](const ICommand&, bool) { return false; });
        Check(!doc.CanUndo() && !doc.CanRedo(), "CAD-04: 浏览态 UndoRedoGate 拦截 CanUndo/CanRedo");

        InputEvent ctrlShiftZ{};
        ctrlShiftZ.Type      = InputEventType::KeyDown;
        ctrlShiftZ.Key       = KeyCode::Z;
        ctrlShiftZ.Modifiers = static_cast<uint8_t>(ModifierKey::Ctrl) | static_cast<uint8_t>(ModifierKey::Shift);
        ed.OnInput(ctrlShiftZ);
        Check(!scene.Has(newId), "CAD-04: 浏览态下 Ctrl+Shift+Z 无法绕过门禁撤销删除");

        // 放开门禁后撤销生效
        CommandStack::SetUndoRedoGate(nullptr);
        Check(doc.CanUndo(), "CAD-04: 解除门禁后 CanUndo 恢复");
        ed.OnInput(ctrlShiftZ);
        Check(scene.Has(newId), "CAD-04: 解除门禁后撤销可正常恢复实体");
    }

    // ── CAD-07 / CAD-08 / CAD-27 / CAD-32：对比底片满覆盖、隐藏主图裁剪与拾取、显式释放与捕捉刷新 ──
    void TestCompareViewportPickingAndSnapRefresh()
    {
        class MockTarget final : public IRenderTarget
        {
        public:
            MockTarget(int w = 800, int h = 600) : m_w(w), m_h(h) {}
            void  Create(int w, int h) override { m_w = w; m_h = h; }
            void  Resize(int w, int h) override { m_w = w; m_h = h; }
            void  Release() override { m_w = 0; m_h = 0; }
            int   GetWidth() const override { return m_w; }
            int   GetHeight() const override { return m_h; }
            void* GetNativeHandle() const override { return nullptr; }
            void* GetNativeShaderResource() const override { return nullptr; }
        private:
            int m_w = 0, m_h = 0;
        };

        class MockRenderer final : public IRenderer
        {
        public:
            int  fullCoveragePasses   = 0;
            int  selDraws             = 0;
            bool selDrawHadSplitClip  = false;
            int  splitClipMaxWidth    = 0;

            void BeginFrame(IRenderTarget& target, const ViewportDesc&) override
            {
                m_curTarget  = &target;
                m_paintStyle = {};
                m_scissor    = {};
            }
            void EndFrame() override { m_curTarget = nullptr; }
            void Submit(std::span<const Vertex_P3_C4> verts, const Math::Mat4&, PrimitiveType, bool, bool) override
            {
                if (!verts.empty() && m_paintStyle.FullCoverage)
                    ++fullCoveragePasses;
            }
            void SubmitTextured(std::span<const Vertex_P3_C4_UV>, const Math::Mat4&, void*, bool, bool) override {}
            void SubmitCached(uint32_t slot, uint64_t, std::span<const Vertex_P3_C4> verts, const Math::Mat4&, PrimitiveType, bool, bool) override
            {
                if (m_paintStyle.FullCoverage && !verts.empty())
                    ++fullCoveragePasses;
                // kSlotSelFill=3, kSlotSel=4, kSlotGripFill=15, kSlotGrip=16
                if ((slot == 3 || slot == 4 || slot == 15 || slot == 16) && !verts.empty())
                {
                    ++selDraws;
                    if (m_scissor.Enabled)
                    {
                        selDrawHadSplitClip = true;
                        splitClipMaxWidth   = m_scissor.Width;
                    }
                }
            }
            std::unique_ptr<IRenderTarget> CreateLayerTarget() override { return std::make_unique<MockTarget>(800, 600); }
            bool CompositeCompareLayers(IRenderTarget&, IRenderTarget&, const CompareCompositeStyle&) override { return true; }
            void SetPaintStyle(const PaintStyle& s) override { m_paintStyle = s; }
            void SetScissorRect(const ScissorRect& r) override { m_scissor = r; }
            void* GetNativeDevice() override { return nullptr; }

        private:
            IRenderTarget* m_curTarget = nullptr;
            PaintStyle     m_paintStyle{};
            ScissorRect    m_scissor{};
        };

        MockRenderer renderer;
        MockTarget   screenTarget(800, 600);
        Viewport     vp(800.0f, 600.0f);

        std::vector<Vertex_P3_C4> mainSceneVerts{
            { { 0, 0, 0 }, { 1, 1, 1, 0.5f } },
            { { 10, 0, 0 }, { 1, 1, 1, 0.5f } },
        };
        std::vector<Vertex_P3_C4> mainSelVerts{
            { { 0, 0, 0 }, { 0, 0.5f, 1, 1 } },
            { { 10, 0, 0 }, { 0, 0.5f, 1, 1 } },
        };
        std::vector<GripDraw> mainGrips{
            { { 0, 0 }, GripDraw::Type::Start, false },
        };
        ViewState mainVs{};
        mainVs.SceneVersion = 1;
        mainVs.Scene        = mainSceneVerts;
        mainVs.SelVersion   = 1;
        mainVs.SelScene     = mainSelVerts;
        mainVs.Grips        = mainGrips;

        std::vector<Vertex_P3_C4> compSceneVerts{
            { { 0, 0, 0 }, { 1, 1, 1, 1.0f } },
            { { 10, 0, 0 }, { 1, 1, 1, 1.0f } },
        };
        ViewState compVs{};
        compVs.SceneVersion = 2;
        compVs.Scene        = compSceneVerts;

        CompareRenderPass cp{};
        cp.State = &compVs;
        cp.Mode  = CompareDisplayMode::Blend;

        // CAD-07 & CAD-27：Blend 绘制双底片启用 FullCoverage，且 ReleaseCompareLayers 立即释放双底片
        vp.Render(renderer, screenTarget, mainVs, &cp);
        Check(renderer.fullCoveragePasses >= 2, "CAD-07: 三态对比双底片渲染开启 PaintStyle::FullCoverage 抵消锁层淡显");
        Check(vp.HasCompareLayers(), "CAD-27: 三态对比后双底片已按需创建");
        vp.ReleaseCompareLayers();
        Check(!vp.HasCompareLayers(), "CAD-27: ReleaseCompareLayers 显式释放双底片渲染目标");

        // CAD-08：CompareOnly 隐藏主图时不绘制任何主图选中高亮/夹点
        renderer.selDraws = 0;
        cp.Mode = CompareDisplayMode::CompareOnly;
        vp.Render(renderer, screenTarget, mainVs, &cp);
        Check(renderer.selDraws == 0, "CAD-08: CompareOnly 模式隐藏主图选中高亮与夹点覆盖层");

        // CAD-08：Split 模式下主图选中高亮/夹点被 ScissorRect 限制在 [0, SplitX)
        renderer.selDraws            = 0;
        renderer.selDrawHadSplitClip = false;
        renderer.splitClipMaxWidth   = 0;
        cp.Mode   = CompareDisplayMode::Split;
        cp.SplitX = 320;
        vp.Render(renderer, screenTarget, mainVs, &cp);
        Check(renderer.selDraws > 0 && renderer.selDrawHadSplitClip && renderer.splitClipMaxWidth == 320,
              "CAD-08: Split 模式将主图选中高亮与夹点裁剪到卷帘主图侧 [0, 320)");

        // CAD-08 拾取与全视口平移缩放 + CAD-32 无溢出悬停刷新捕捉
        Document doc;
        Viewport pickVp(800.0f, 600.0f);
        pickVp.GetCamera().SetState(CameraState{ { 0.0, 0.0, 0.0 }, 600.0, -90.0, 90.0 });
        Editor ed;
        ed.Bind(doc, pickVp);

        // 左侧点（世界 x=-200 → 屏幕 x=200）和右侧点（世界 x=+200 → 屏幕 x=600）
        const Object::ObjectID leftId  = doc.GetScene().NextObjectID();
        const Object::ObjectID rightId = doc.GetScene().NextObjectID();
        doc.GetScene().AddEntity(std::make_unique<PointEntity>(leftId,  Math::Point3{ -200.0, 0.0, 0.0 }));
        doc.GetScene().AddEntity(std::make_unique<PointEntity>(rightId, Math::Point3{  200.0, 0.0, 0.0 }));

        const Math::Point2 leftScr  = pickVp.GetCamera().WorldToScreen({ -200.0, 0.0, 0.0 });
        const Math::Point2 rightScr = pickVp.GetCamera().WorldToScreen({  200.0, 0.0, 0.0 });
        const int splitPx = static_cast<int>(std::round(0.5 * (leftScr.x + rightScr.x)));

        ed.SetPickClipX(true, 0.0, static_cast<double>(splitPx));

        auto clickAt = [&](int sx, int sy) {
            InputEvent dn{};
            dn.Type = InputEventType::MouseButtonDown; dn.Button = MouseButton::Left;
            dn.MouseX = dn.PressMouseX = sx; dn.MouseY = dn.PressMouseY = sy;
            ed.OnInput(dn);
            InputEvent up = dn; up.Type = InputEventType::MouseButtonUp;
            ed.OnInput(up);
        };
        clickAt(static_cast<int>(std::round(rightScr.x)), static_cast<int>(std::round(rightScr.y)));
        Check(ed.GetSelection().empty(), "CAD-08: 卷帘副图侧点击不能选中隐藏的主图实体");

        clickAt(static_cast<int>(std::round(leftScr.x)), static_cast<int>(std::round(leftScr.y)));
        Check(ed.GetSelection().size() == 1 && ed.GetSelection().count(leftId) == 1,
              "CAD-08: 卷帘主图侧点击可正常选中主图实体");

        // 从主图侧拖框跨越卷帘到副图侧：只选中主图侧实体，不选中副图侧隐藏实体
        ed.SetSelection({});
        {
            InputEvent dn{};
            dn.Type = InputEventType::MouseButtonDown; dn.Button = MouseButton::Left;
            dn.MouseX = dn.PressMouseX = static_cast<int>(leftScr.x) - 30;
            dn.MouseY = dn.PressMouseY = static_cast<int>(leftScr.y) - 30;
            ed.OnInput(dn);
            InputEvent mv = dn;
            mv.Type = InputEventType::MouseMove;
            mv.MouseX = static_cast<int>(rightScr.x) + 30;
            mv.MouseY = static_cast<int>(rightScr.y) + 30;
            ed.OnInput(mv);
            InputEvent up = mv;
            up.Type = InputEventType::MouseButtonUp;
            ed.OnInput(up);
        }
        Check(ed.GetSelection().size() == 1 && ed.GetSelection().count(leftId) == 1 && ed.GetSelection().count(rightId) == 0,
              "CAD-08: 跨卷帘框选仅命中主图可见侧实体，不命中隐藏副图侧实体");

        // CompareOnly（全隐藏）下仍允许全视口滚轮缩放与中键平移
        ed.SetPickClipX(true, 0.0, 0.0);
        const double zoomBefore = pickVp.GetCamera().GetState().Zoom;
        InputEvent wheel{};
        wheel.Type = InputEventType::MouseWheel;
        wheel.MouseX = 600; wheel.MouseY = 300;
        wheel.WheelDelta = 1.0f;
        ed.OnInput(wheel);
        Check(pickVp.GetCamera().GetState().Zoom != zoomBefore, "CAD-08: 隐藏主图时全视口滚轮缩放仍正常生效");

        // CAD-32：RefreshHoverSnap 刷新捕捉且不改变相机状态
        const CameraState camSnapBefore = pickVp.GetCamera().GetState();
        ed.RefreshHoverSnap(static_cast<int>(std::round(leftScr.x)), static_cast<int>(std::round(leftScr.y)),
                            static_cast<uint8_t>(ModifierKey::Shift));
        const CameraState camSnapAfter = pickVp.GetCamera().GetState();
        Check(Near(camSnapBefore.Target.x, camSnapAfter.Target.x, 1e-9)
              && Near(camSnapBefore.Target.y, camSnapAfter.Target.y, 1e-9)
              && Near(camSnapBefore.Zoom, camSnapAfter.Zoom, 1e-9),
              "CAD-32: RefreshHoverSnap 刷新捕捉不污染相机且无 INT_MIN 溢出");
    }

    // ── CAD-09 & CAD-10：SnapEngine 密集块交点预算 + 斜切仿射正交性校验 ──────────────
    void TestSnapBudgetAndShearOrthogonality()
    {
        // CAD-10：斜切等长基向量（lx == ly == 1 但不垂直）的嵌套块内圆，不能按等比圆误判捕捉
        {
            Scene scene;
            auto innerBlk = std::make_unique<BlockEntity>(scene.NextObjectID(), "INNER_CIRCLE");
            innerBlk->AddEntity(std::make_unique<CircleEntity>(scene.NextObjectID(), Math::Point3{ 0, 0, 0 }, 100.0));
            const BlockID innerId = scene.GetBlockTable().Add(std::move(innerBlk));

            auto outerBlk = std::make_unique<BlockEntity>(scene.NextObjectID(), "OUTER_ROT");
            auto innerIns = std::make_unique<InsertEntity>(scene.NextObjectID(), innerId, "INNER_CIRCLE", Math::Point3{ 0, 0, 0 });
            innerIns->SetRotation(Math::PI / 4.0); // 45° 旋转
            innerIns->SetBlock(scene.GetBlockTable().Find(innerId));
            outerBlk->AddEntity(std::move(innerIns));
            const BlockID outerId = scene.GetBlockTable().Add(std::move(outerBlk));

            // 外层非等比缩放 (2, 1) 套内层 45° 旋转：两个基向量等长 sqrt(2.5) 但夹角非 90°（斜切！）
            auto outerIns = std::make_unique<InsertEntity>(scene.NextObjectID(), outerId, "OUTER_ROT", Math::Point3{ 0, 0, 0 });
            outerIns->SetScale({ 2.0, 1.0, 1.0 });
            scene.ResolveInsert(*outerIns);
            scene.AddEntity(std::move(outerIns));

            Camera cam(800.0f, 600.0f);
            cam.SetState(CameraState{ { 0.0, 0.0, 0.0 }, 600.0, -90.0, 90.0 });
            SnapEngine snap;
            snap.SetEnableSnap(true);
            snap.SetSnapModes(static_cast<uint32_t>(SnapMode::Quadrant));
            snap.SetSnapRadiusPx(5.0);

            // 若误判为等比圆，其半径会被算成 100*sqrt(2.5)≈158.11，右象限点 (158.11, 0) 落在变换后包围盒
            // [-282.84, 282.84]×[-141.42, 141.42] 内部（不会被 BoxHitsAperture 提前短路），旧实现会误吸附；
            // 实际椭圆在 x 轴顶点位于 (200, 0)，因此 (158.11, 0) 绝不应命中象限捕捉（CAD-R2-11）。
            const Math::Point2 falseQuadSp = cam.WorldToScreen({ 100.0 * std::sqrt(2.5), 0.0, 0.0 });
            const SnapResult r = snap.Query(falseQuadSp, scene, cam);
            Check(!r.IsValid(), "CAD-10/R2-11: 包围盒内假象限点 (158.11, 0) 不会被斜切圆误判吸附");
        }

        // CAD-R2-11：单个 MLineEntity 含 1200 条元素线时，EmitTransformedPrimitive 输出处严格截断在 1024 叶节点上限
        {
            Scene scene;
            MLineStyleRecord style;
            style.Name = "DENSE_1200";
            for (int i = 0; i < 1200; ++i)
                style.Elements.push_back({ static_cast<double>(i) * 10.0, false, {} });
            style.Normalize(); // 偏移按降序排列：元素 0 在 y=11990，元素 1023 在 y=1760，元素 1199 在 y=0

            auto blk = std::make_unique<BlockEntity>(scene.NextObjectID(), "MLINE_1200_BLK");
            blk->AddEntity(std::make_unique<MLineEntity>(
                scene.NextObjectID(),
                std::vector<Math::Point3>{ { 0.0, 0.0, 0.0 }, { 100.0, 0.0, 0.0 } },
                style,
                MLineJustify::Zero,
                1.0,
                false));
            const BlockID blkId = scene.GetBlockTable().Add(std::move(blk));
            auto ins = std::make_unique<InsertEntity>(scene.NextObjectID(), blkId, "MLINE_1200_BLK", Math::Point3{ 0, 0, 0 });
            scene.ResolveInsert(*ins);
            scene.AddEntity(std::move(ins));

            Camera cam(800.0f, 600.0f);
            // 1 世界单位 ≈ 1 像素，两条元素线间距 10px > 4px 捕捉半径
            cam.SetState(CameraState{ { 0.0, 11990.0, 0.0 }, 600.0, -90.0, 90.0 });
            SnapEngine snap;
            snap.SetEnableSnap(true);
            snap.SetSnapModes(static_cast<uint32_t>(SnapMode::Endpoint));
            snap.SetSnapRadiusPx(4.0);

            const SnapResult rFirst = snap.Query(cam.WorldToScreen({ 0.0, 11990.0, 0.0 }), scene, cam);
            Check(rFirst.IsValid() && rFirst.SnapType == SnapResult::Type::Endpoint,
                  "CAD-R2-11: 单 MLine 前 1024 条元素线正常参与端点捕捉");

            cam.SetState(CameraState{ { 0.0, 0.0, 0.0 }, 600.0, -90.0, 90.0 });
            const SnapResult rOverflow = snap.Query(cam.WorldToScreen({ 0.0, 0.0, 0.0 }), scene, cam);
            Check(!rOverflow.IsValid(),
                  "CAD-R2-11: 单 MLine 第 1200 条元素线被 kMaxBlockLeaves=1024 严格截断，不再超额溢出");
        }

        // CAD-09：大型块含 1500 条交错线，交点捕捉受叶节点与 64 条最近曲线预算约束，不阻塞界面
        {
            Scene scene;
            auto denseBlk = std::make_unique<BlockEntity>(scene.NextObjectID(), "DENSE_XREF");
            for (int i = 0; i < 750; ++i)
            {
                const double d = static_cast<double>(i) * 0.01;
                denseBlk->AddEntity(std::make_unique<LineEntity>(
                    scene.NextObjectID(), Math::Point3{ -100.0, d, 0.0 }, Math::Point3{ 100.0, d, 0.0 }));
                denseBlk->AddEntity(std::make_unique<LineEntity>(
                    scene.NextObjectID(), Math::Point3{ d, -100.0, 0.0 }, Math::Point3{ d, 100.0, 0.0 }));
            }
            const BlockID blkId = scene.GetBlockTable().Add(std::move(denseBlk));
            auto ins = std::make_unique<InsertEntity>(scene.NextObjectID(), blkId, "DENSE_XREF", Math::Point3{ 0, 0, 0 });
            scene.ResolveInsert(*ins);
            scene.AddEntity(std::move(ins));

            Camera cam(800.0f, 600.0f);
            cam.SetState(CameraState{ { 0.0, 0.0, 0.0 }, 600.0, -90.0, 90.0 });
            SnapEngine snap;
            snap.SetEnableSnap(true);
            snap.SetSnapModes(static_cast<uint32_t>(SnapMode::Intersection));
            snap.SetSnapRadiusPx(15.0);

            const auto t0 = std::chrono::steady_clock::now();
            const SnapResult r = snap.Query(cam.WorldToScreen({ 0.0, 0.0, 0.0 }), scene, cam);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            Check(r.IsValid() && r.SnapType == SnapResult::Type::Intersection && ms < 2000,
                  "CAD-09: 1500 线密集块交点捕捉在预算内快速收敛并命中有效交点");
        }
    }
}

int RunSaSelectionExportTests()
{
    g_failures = 0;
    std::printf("\n── 嵌入选中图元摘要导出测试 (SaSelectionExport) ──\n");
    TestLookupCadEntityHandle();
    TestEmptyAndInvalidSelection();
    TestRichEntitySerialization();
    TestTruncationAndClamping();
    TestUtf8AndJsonEscaping();
    TestSelectionVersionAndLayout();
    TestHatchNetArea();
    TestLargeSelectionStatsAndStrictUtf8();
    TestSaTextExportArrayAndHeight();
    TestMarkupExportPaperSpaceAndDegenerate();
    TestPickPointToolReentrancy();
    TestMarkupProtectionAndUndoGate();
    TestCompareViewportPickingAndSnapRefresh();
    TestSnapBudgetAndShearOrthogonality();
    return g_failures;
}
