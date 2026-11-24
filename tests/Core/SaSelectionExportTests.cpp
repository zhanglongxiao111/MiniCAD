// ── 嵌入选中图元摘要导出测试：DWG Handle 查询、JSON 序列化、截断、UTF-8/转义、版本号 ──
#include "Document/SaSelectionExport.h"
#include "Document/Document.h"
#include "Editor/Editor.h"
#include "Import/CadExchange.h"
#include "Scene/Scene.h"
#include "Serialization/JsonSerializer.h"
#include "Viewport/Viewport.h"

#include <cmath>
#include <cstdio>
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
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80u) { ++i; continue; }
            std::size_t len = 0;
            if ((c & 0xE0u) == 0xC0u)      len = 2;
            else if ((c & 0xF0u) == 0xE0u) len = 3;
            else if ((c & 0xF8u) == 0xF0u) len = 4;
            else return false;
            if (i + len > n) return false;
            for (std::size_t k = 1; k < len; ++k)
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0u) != 0x80u)
                    return false;
            i += len;
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
        const std::string expected = "{\"count\":0,\"truncated\":false,\"bounds\":null,\"typeCounts\":{},\"items\":[]}";
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
    return g_failures;
}
