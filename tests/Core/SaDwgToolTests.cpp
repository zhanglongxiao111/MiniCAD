// SaDwgTool 核心单元测试：覆盖 CLI 解析、19 表 + 2 R-tree SQLite 导出、中文路径与缓存复用、
// 天正原生 8 表语义导出与跨表联查、纯 CPU 渲染（模型/图纸视口/双图叠加/块内递归过滤/修改框排除）、
// 以及三轨确定性比对（tch_axes 与轴线层自动对齐 + 像素差分聚类 + 文字差分 + 天正语义差分 + 4 表比对库）。
#include "CliArgs.h"
#include "CpuRasterizer.h"
#include "DiffEngine.h"
#include "PngWriter.h"
#include "SceneTessellator.h"
#include "SqliteExporter.h"
#include "TchCipherWriter.h"
#include "Core/Log.h"
#include "Core/PathUtils.h"
#include "Dwg/Write/DwgBitWriter.h"
#include "Dwg/Write/DwgWriter.h"
#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace MiniDWG;
using namespace MiniCAD::SaTool;
using TchTest::CipherWriter;

namespace
{
    int g_failed = 0;

    void Check(bool ok, const char* label)
    {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++g_failed;
    }

    bool Near(double a, double b, double tol = 1e-2)
    {
        return std::abs(a - b) <= tol;
    }

    DxfClass MakeTchClass(const char* dxf, const char* cpp, std::int16_t number)
    {
        DxfClass c;
        c.DxfName         = dxf;
        c.CppClassName    = cpp;
        c.ApplicationName = "TCH_KERNAL";
        c.ClassNumber     = number;
        c.IsAnEntity      = true;
        c.ItemClassId     = 0x1f2;
        return c;
    }

    // 合成天正墙（与 TianzhengWallTests / TchOpeningTests 布局一致，支持自定义端点、半宽、墙高、材料、开洞数）
    std::unique_ptr<UnknownEntity> MakeTchWall(XYZ start, XYZ end, double halfW, double height,
                                               int openings = 0, int material = 40, double scale = 100)
    {
        auto e = std::make_unique<UnknownEntity>();
        e->Raw.DxfName     = "TCH_WALL";
        e->Raw.Dwg.Version = CadVersion::AC1027;
        DwgBitWriter g(CadVersion::AC1027, Codec::CodePage::Gbk);
        g.Write3RawDouble(start);
        g.Write3RawDouble(end);
        g.WriteRawDouble(0);
        g.WriteRawLong(0);
        g.WriteRawDouble(halfW);
        g.WriteRawDouble(halfW);
        g.WriteRawDouble(height);
        auto protectedData = g.Take();
        for (auto& b : protectedData) b ^= 0x3b;

        DwgBitWriter r(CadVersion::AC1027, Codec::CodePage::Gbk);
        r.WriteByte(5);
        r.WriteBitDouble(scale);
        r.WriteBitShort(0);
        r.WriteByte(3);
        r.Write2Bits(0);
        r.WriteRawShort(512);
        r.WriteByte(8);
        r.WriteByte(0x3b);
        r.WriteBytes(protectedData);
        r.WriteByte(0);
        r.WriteBitShort(static_cast<std::int16_t>(openings));
        r.WriteByte(static_cast<std::uint8_t>(material));
        r.WriteByte(0);
        r.WriteBitDouble(0);
        for (int i = 0; i < 4; ++i) r.WriteBitDouble(0);
        r.WriteByte(0);
        r.Write3Bits(2);
        e->Raw.Dwg.MainBits = r.PositionInBits();
        e->Raw.Dwg.Main     = r.Take();
        return e;
    }

    // 合成天正门窗（与 TchOpeningTests 一致）
    std::unique_ptr<UnknownEntity> MakeTchOpening(XYZ pos, double width, double height,
                                                  Handle wallHandle, Handle block2d,
                                                  const std::string& label, int kind = 0, int mirror = 3)
    {
        auto e = std::make_unique<UnknownEntity>();
        e->Raw.DxfName     = "TCH_OPENING";
        auto& raw          = e->Raw.Dwg;
        raw.Version        = CadVersion::AC1027;
        DwgBitWriter r(raw.Version, Codec::CodePage::Gbk);
        r.WriteByte(5); r.WriteBitDouble(100); r.WriteBitDouble(0);
        r.WriteByte(3); r.WriteByte(0); r.WriteByte(0);
        r.WriteBitLong(2); r.WriteBitLong(200); r.WriteBitDouble(0); r.WriteBitLong(0x65);
        r.WriteByte(5);
        r.WriteBitDouble(pos.X); r.WriteBitDouble(pos.Y); r.WriteBitDouble(pos.Z);
        r.WriteBitDouble(width); r.WriteBitDouble(100); r.WriteBitDouble(height); r.WriteBitDouble(0.0);
        r.WriteByte(static_cast<std::uint8_t>(mirror));
        r.WriteByte(static_cast<std::uint8_t>(kind));
        r.WriteBitDouble(0);
        r.WriteBitDouble(pos.X + 200.0); r.WriteBitDouble(pos.Y + 150.0); r.WriteBitDouble(0);
        r.WriteBitShort(0x400);
        r.WriteBitDouble(3.5); r.WriteBitDouble(100); r.WriteBitDouble(40); r.WriteBitDouble(40);
        r.WriteBitShort(0);
        r.WriteBitLong(0); r.WriteBitShort(7); r.WriteBit(false);
        for (int i = 0; i < 4; ++i) r.WriteBitDouble(0);
        r.WriteBitDouble(100);
        raw.MainBits = r.PositionInBits();
        raw.Main     = r.Take();

        r.WriteHandle(DwgRef::HardPointer, block2d);
        r.WriteHandle(DwgRef::HardPointer, 0);
        r.WriteHandle(DwgRef::SoftPointer, wallHandle);
        for (int i = 0; i < 5; ++i) r.WriteHandle(DwgRef::HardPointer, 0);
        raw.HandleBits = r.PositionInBits();
        raw.Handles    = r.Take();

        if (!label.empty())
        {
            DwgBitWriter text(raw.Version, Codec::CodePage::Gbk);
            for (const char* t : { "a", "b", "c" }) text.WriteVariableText(t);
            text.WriteVariableText(label);
            text.WriteVariableText("d");
            raw.TextBits = text.PositionInBits();
            raw.Text     = text.Take();
        }
        return e;
    }

    // 合成天正柱（600x600mm 矩形，面积 0.36 m²）
    std::unique_ptr<UnknownEntity> MakeTchColumn(double cx, double cy, double halfSide = 300.0, double height = 3000.0)
    {
        CipherWriter w;
        const int key = 0x12;
        w.Base(key, 94, 100, 94);
        w.Main.WriteByte(10);
        w.Main.WriteByte(static_cast<std::uint8_t>(key));
        w.Key = key;
        w.Point3(cx, cy, 0);
        w.Main.WriteByte(1);
        w.Main.WriteBitShort(4);
        const double pts[4][2] = {
            { cx - halfSide, cy - halfSide },
            { cx + halfSide, cy - halfSide },
            { cx + halfSide, cy + halfSide },
            { cx - halfSide, cy + halfSide },
        };
        for (const auto& p : pts)
        {
            w.Main.WriteRawDouble(p[0]);
            w.Main.WriteRawDouble(p[1]);
            w.Main.WriteBitDouble(0);
        }
        w.Double(height);
        w.Byte(1);
        w.Byte(1); w.Handle(0);
        w.Byte(60);     // 钢筋混凝土
        w.Short(0);
        w.Handle(0);
        w.Byte(0);
        w.Double(80);
        w.Handle(0);
        w.Double(0); w.Text.WriteVariableText(""); w.Text.WriteVariableText("");
        w.Double(50);
        for (int i = 0; i < 7; ++i) w.Double(i);
        auto e = w.Take("TCH_COLUMN");
        e->Raw.Dwg.Version = CadVersion::AC1027;
        return e;
    }

    // 合成天正房间（TCH_SPACE）
    std::unique_ptr<UnknownEntity> MakeTchSpace(double x0, double y0, double x1, double y1,
                                                const char* roomName)
    {
        CipherWriter w;
        w.Base(0x12, 94, 100, 94);
        w.Strings().WriteVariableText("1001");
        w.Strings().WriteVariableText(roomName);
        w.Layer(2, 0x12);
        w.Double(3.5);
        w.Main.WriteBitDouble(2800);
        w.Main.WriteByte(1);
        w.Main.WriteBitShort(4);
        const std::array<std::pair<double, double>, 4> pts = {{
            { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }
        }};
        for (size_t i = 0; i < pts.size(); ++i)
        {
            w.Main.WriteRawDouble(pts[i].first);
            w.Main.WriteRawDouble(pts[i].second);
            if (i + 1 < pts.size()) w.Main.Write2Bits(0);
        }
        auto e = w.Take("TCH_SPACE");
        e->Raw.Dwg.Version = CadVersion::AC1027;
        return e;
    }

    // 合成天正双跑楼梯（TCH_RECTSTAIR）
    std::unique_ptr<UnknownEntity> MakeTchRectStair(double x, double y, int steps1 = 10, int steps2 = 10)
    {
        auto e = std::make_unique<UnknownEntity>();
        e->Raw.DxfName     = "TCH_RECTSTAIR";
        auto& raw          = e->Raw.Dwg;
        raw.Version        = CadVersion::AC1027;
        DwgBitWriter r(raw.Version, Codec::CodePage::Gbk);
        r.WriteByte(5); r.WriteBitDouble(100); r.WriteBitDouble(0); r.WriteByte(1); r.WriteByte(0); r.WriteBitLong(0);
        r.WriteByte(2); r.WriteByte(4);
        r.WriteBitDouble(x); r.WriteBitDouble(y); r.WriteBitDouble(0);
        r.WriteBitDouble(0); r.WriteBitDouble(150); r.WriteBitDouble(270);
        r.WriteBitDouble(1230); r.WriteBitDouble(1200);
        r.WriteByte(static_cast<std::uint8_t>(steps1));
        r.WriteByte(static_cast<std::uint8_t>(steps2));
        r.WriteBitDouble(2560); r.WriteBitDouble(900);
        r.WriteByte(1); r.WriteByte(0);
        r.WriteByte(2);
        r.WriteBitDouble(60); r.WriteBitDouble(0);
        r.WriteBitLong(394757);
        r.WriteBitDouble(200); r.WriteBitDouble(120); r.WriteBitDouble(120);
        r.WriteBitDouble(477.74); r.WriteBitDouble(872.26); r.WriteByte(0);
        r.WriteBitDouble(60); r.WriteBitDouble(60); r.WriteBitDouble(675); r.WriteBitDouble(0.52359877559829882);
        r.WriteBitDouble(3.5);
        for (int i = 0; i < 4; ++i) r.WriteBitDouble(1200);
        raw.MainBits = r.PositionInBits(); raw.Main = r.Take();
        raw.TextBits = 0;
        r.WriteHandle(DwgRef::SoftPointer, 0);
        for (int i = 0; i < 7; ++i) r.WriteHandle(DwgRef::HardPointer, 0);
        raw.HandleBits = r.PositionInBits(); raw.Handles = r.Take();
        return e;
    }

    // 合成天正尺寸标注（TCH_DIMENSION2）
    std::unique_ptr<UnknownEntity> MakeTchDim(XYZ pos, double span)
    {
        auto e = std::make_unique<UnknownEntity>();
        e->Raw.DxfName     = "TCH_DIMENSION2";
        auto& raw          = e->Raw.Dwg;
        raw.Version        = CadVersion::AC1027;
        DwgBitWriter r(raw.Version, Codec::CodePage::Gbk);
        r.WriteByte(5); r.WriteBitDouble(100); r.WriteBitDouble(0); r.WriteByte(0); r.WriteBitLong(0);
        r.WriteByte(1);
        r.WriteBitDouble(pos.X); r.WriteBitDouble(pos.Y); r.WriteBitDouble(pos.Z);
        r.WriteBitDouble(0.0); r.WriteBitDouble(-20.0);
        r.WriteBitShort(1);
        r.WriteBitDouble(span);
        r.WriteBitShort(0x80);
        r.WriteBitShort(0);
        raw.MainBits = r.PositionInBits(); raw.Main = r.Take();
        raw.TextBits = 0;
        r.WriteHandle(DwgRef::HardPointer, 0);
        raw.HandleBits = r.PositionInBits(); raw.Handles = r.Take();
        return e;
    }

    // 合成天正轴号组（TCH_AXIS_LABEL，vertical 或 horizontal）
    std::unique_ptr<UnknownEntity> MakeTchAxisGroup(double x0, double y0, double x1, double y1,
                                                    const std::vector<double>& spacings,
                                                    const std::vector<std::string>& labels)
    {
        auto e = std::make_unique<UnknownEntity>();
        e->Raw.DxfName     = "TCH_AXIS_LABEL";
        auto& raw          = e->Raw.Dwg;
        raw.Version        = CadVersion::AC1027;
        DwgBitWriter r(raw.Version, Codec::CodePage::Gbk);
        r.WriteByte(5); r.WriteBitDouble(100); r.WriteBitDouble(0); r.WriteByte(0);
        r.WriteBitLong(1); r.WriteBitLong(0); r.WriteBit(false);
        r.WriteByte(5); r.WriteBitShort(0);
        r.WriteRawDouble(x0); r.WriteRawDouble(y0);
        r.WriteRawDouble(x1); r.WriteRawDouble(y1);
        r.WriteBitDouble(0);
        r.WriteByte(static_cast<std::uint8_t>(spacings.size()));
        for (double sp : spacings) { r.WriteBitShort(0); r.WriteBitDouble(sp); }
        r.WriteBitDouble(0); r.WriteBitDouble(40); r.WriteBitDouble(40); r.WriteBitDouble(4);
        r.WriteBitShort(7); r.WriteBitShort(0x3361);
        raw.MainBits = r.PositionInBits(); raw.Main = r.Take();

        r.WriteVariableText("当前轴号列表");
        for (const auto& lbl : labels) r.WriteVariableText(lbl);
        raw.TextBits = r.PositionInBits(); raw.Text = r.Take();

        r.WriteHandle(DwgRef::HardPointer, 0);
        r.WriteHandle(DwgRef::HardPointer, 0);
        raw.HandleBits = r.PositionInBits(); raw.Handles = r.Take();
        return e;
    }

    bool WriteBytesToUtf8Path(const std::string& utf8Path, const std::vector<std::uint8_t>& bytes)
    {
        const fs::path p = MiniCAD::Utf8ToPath(utf8Path);
        std::error_code ec;
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(f);
    }

    int SqlInt(sqlite3* db, const char* sql)
    {
        sqlite3_stmt* st = nullptr;
        int val = -1;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(st) == SQLITE_ROW)
                val = sqlite3_column_int(st, 0);
        }
        sqlite3_finalize(st);
        return val;
    }

    double SqlDouble(sqlite3* db, const char* sql)
    {
        sqlite3_stmt* st = nullptr;
        double val = 0.0;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(st) == SQLITE_ROW)
                val = sqlite3_column_double(st, 0);
        }
        sqlite3_finalize(st);
        return val;
    }

    std::string SqlText(sqlite3* db, const char* sql)
    {
        sqlite3_stmt* st = nullptr;
        std::string val;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(st) == SQLITE_ROW)
            {
                const unsigned char* s = sqlite3_column_text(st, 0);
                if (s) val = reinterpret_cast<const char*>(s);
            }
        }
        sqlite3_finalize(st);
        return val;
    }

    // 构造带原生天正 7 类对象的完整建筑测试 DWG（支持整体平移与变更开关，用于 export-db / diff 联测）
    bool BuildTianzhengSampleDwg(const std::string& utf8Path,
                                 double shiftX, double shiftY,
                                 bool modifiedVersion)
    {
        CadDatabase db;
        db.SetVersion(CadVersion::AC1027);
        db.CreateDefaults();
        db.Header.InsUnits = UnitsType::Millimeters;

        db.Classes.push_back(MakeTchClass("TCH_WALL",       "TDbWall",       500));
        db.Classes.push_back(MakeTchClass("TCH_OPENING",    "TDbOpening",    501));
        db.Classes.push_back(MakeTchClass("TCH_COLUMN",     "TDbColumn",     502));
        db.Classes.push_back(MakeTchClass("TCH_SPACE",      "TDbSpace",      503));
        db.Classes.push_back(MakeTchClass("TCH_RECTSTAIR",  "TDbRectStair",  504));
        db.Classes.push_back(MakeTchClass("TCH_DIMENSION2", "TDbDimension",  505));
        db.Classes.push_back(MakeTchClass("TCH_AXIS_LABEL", "TDbAxisLabel",  506));

        BlockRecord* doorBlock = db.CreateBlockRecord("$TCHSYS$DOOR2D");
        auto leaf = std::make_unique<Line>();
        leaf->StartPoint = { 0.45, 0, 0 };
        leaf->EndPoint   = { 0.45, 0.975, 0 };
        db.AddEntity(doorBlock, std::move(leaf));

        // 1. 墙体（A 侧厚 240mm，B 侧加厚到 300mm）
        const double halfW = modifiedVersion ? 150.0 : 120.0;
        const Handle wallH = db.AddEntity(db.ModelSpace(),
            MakeTchWall({ 0 + shiftX, 0 + shiftY, 0 }, { 8000 + shiftX, 0 + shiftY, 0 }, halfW, 3000.0, 1, 40))->ObjectHandle;
        db.AddEntity(db.ModelSpace(),
            MakeTchWall({ 0 + shiftX, 0 + shiftY, 0 }, { 0 + shiftX, 6000 + shiftY, 0 }, 120.0, 3000.0, 0, 40));

        // 2. 门窗（A 侧 M1024 宽 1000mm；B 侧 M1524 宽 1500mm）
        db.AddEntity(db.ModelSpace(),
            MakeTchOpening({ 3000 + shiftX, 0 + shiftY, 0 },
                           modifiedVersion ? 1500.0 : 1000.0, 2400.0,
                           wallH, doorBlock->ObjectHandle,
                           modifiedVersion ? "M1524" : "M1024", 0, 3));

        // 3. 柱（600x600mm = 0.36 m²）
        db.AddEntity(db.ModelSpace(), MakeTchColumn(0 + shiftX, 0 + shiftY, 300.0, 3000.0));

        // 4. 房间（A 侧 4m×5m = 20.0 m²；B 侧 5m×5m = 25.0 m²）
        db.AddEntity(db.ModelSpace(), MakeTchSpace(1000 + shiftX, 1000 + shiftY,
                                                   (modifiedVersion ? 6000.0 : 5000.0) + shiftX,
                                                   6000 + shiftY, "房间"));

        // 5. 双跑楼梯
        db.AddEntity(db.ModelSpace(), MakeTchRectStair(6500 + shiftX, 1000 + shiftY, 10, 10));

        // 6. 天正标注（6000mm）
        db.AddEntity(db.ModelSpace(), MakeTchDim({ 0 + shiftX, -1000 + shiftY, 0 }, 6000.0));

        // 7. 天正轴网（3 根垂直轴 "1","2","3" + 3 根水平轴 "A","B","C"，用于验证 tch_axes 自动对齐）
        db.AddEntity(db.ModelSpace(), MakeTchAxisGroup(
            0 + shiftX, 0 + shiftY, 0 + shiftX, 6000 + shiftY,
            { 4000.0, 4000.0, 0.0 }, { "1", "2", "3" }));
        db.AddEntity(db.ModelSpace(), MakeTchAxisGroup(
            0 + shiftX, 0 + shiftY, 8000 + shiftX, 0 + shiftY,
            { 3000.0, 3000.0, 0.0 }, { "A", "B", "C" }));

        // 8. 独立中文文字（用于轨 2 文字改字与新增验证）
        auto txt = std::make_unique<MiniDWG::TextEntity>();
        txt->InsertPoint = { 3000 + shiftX, 3500 + shiftY, 0 };
        txt->Height      = 350.0;
        txt->Value       = modifiedVersion ? "起居室" : "客厅";
        db.AddEntity(db.ModelSpace(), std::move(txt));

        if (modifiedVersion)
        {
            auto addedTxt = std::make_unique<MiniDWG::TextEntity>();
            addedTxt->InsertPoint = { 7000 + shiftX, 4500 + shiftY, 0 };
            addedTxt->Height      = 300.0;
            addedTxt->Value       = "新增备注";
            db.AddEntity(db.ModelSpace(), std::move(addedTxt));

            // 额外几何线框（触发轨 1 视觉差异区域聚类，长边 1500mm > min_region_mm=800mm）
            auto addLine = [&](double x0, double y0, double x1, double y1) {
                auto l = std::make_unique<Line>();
                l->StartPoint = { x0 + shiftX, y0 + shiftY, 0 };
                l->EndPoint   = { x1 + shiftX, y1 + shiftY, 0 };
                db.AddEntity(db.ModelSpace(), std::move(l));
            };
            addLine(1000, 7000, 3000, 7000);
            addLine(3000, 7000, 3000, 8500);
            addLine(3000, 8500, 1000, 8500);
            addLine(1000, 8500, 1000, 7000);
        }

        DwgWriteOptions opt;
        opt.Version = CadVersion::AC1027;
        const auto bytes = WriteDwg(db, opt);
        return !bytes.empty() && WriteBytesToUtf8Path(utf8Path, bytes);
    }

    // 构造基础综合 DWG（含图层、块、ATTRIB、MTEXT 内联格式码、无界 XLINE、修改范围框、图纸空间视口）
    bool BuildGeneralSampleDwg(const std::string& utf8Path,
                               double shiftX = 0.0, double shiftY = 0.0,
                               bool includeExtraLine = false)
    {
        CadDatabase db;
        db.SetVersion(CadVersion::AC1027);
        db.CreateDefaults();
        db.Header.InsUnits = UnitsType::Millimeters;

        auto makeLayer = [&](const char* name, std::int16_t aci) -> Handle {
            auto l = std::make_unique<MiniDWG::Layer>();
            l->Name  = name;
            l->Color = MiniDWG::Color(aci);
            return db.AddTableEntry(db.Layers(), std::move(l))->ObjectHandle;
        };
        const Handle layWall = makeLayer("WALL", 7);
        const Handle layDote = makeLayer("DOTE", 1);
        const Handle layDim  = makeLayer("DIM_TEXT", 3);

        auto addLine = [&](BlockRecord* blk, Handle lay, double x0, double y0, double x1, double y1) {
            auto l = std::make_unique<Line>();
            l->LayerHandle = lay;
            l->StartPoint  = { x0, y0, 0 };
            l->EndPoint    = { x1, y1, 0 };
            db.AddEntity(blk, std::move(l));
        };

        // 3 垂直 + 3 水平长轴线在 DOTE 层（span 10000mm > 5000mm），测试次选轨 layer_lines 自动对齐
        for (double x : { 0.0, 4000.0, 8000.0 })
            addLine(db.ModelSpace(), layDote, x + shiftX, -1000 + shiftY, x + shiftX, 9000 + shiftY);
        for (double y : { 0.0, 3000.0, 6000.0 })
            addLine(db.ModelSpace(), layDote, -1000 + shiftX, y + shiftY, 9000 + shiftX, y + shiftY);

        // 主墙线在 WALL 层
        addLine(db.ModelSpace(), layWall, 500 + shiftX, 500 + shiftY, 7500 + shiftX, 500 + shiftY);
        addLine(db.ModelSpace(), layWall, 500 + shiftX, 5500 + shiftY, 7500 + shiftX, 5500 + shiftY);
        if (includeExtraLine)
        {
            addLine(db.ModelSpace(), layWall, 2000 + shiftX, 2000 + shiftY, 4500 + shiftX, 2000 + shiftY);
            addLine(db.ModelSpace(), layWall, 4500 + shiftX, 2000 + shiftY, 4500 + shiftX, 3500 + shiftY);
        }

        // 无界 XLINE（应入 entities 表且 minx IS NULL，不入 entities_rtree）
        {
            auto xl = std::make_unique<MiniDWG::XLine>();
            xl->LayerHandle = layWall;
            xl->FirstPoint  = { 1000 + shiftX, 1000 + shiftY, 0 };
            xl->Direction   = { 1, 0, 0 };
            db.AddEntity(db.ModelSpace(), std::move(xl));
        }

        // MTEXT（含内联格式码与中文）
        {
            auto mt = std::make_unique<MiniDWG::MText>();
            mt->LayerHandle = layDim;
            mt->InsertPoint = { 2000 + shiftX, 3000 + shiftY, 0 };
            mt->Height      = 300.0;
            mt->Value       = "{\\fSimSun;\\H1.2x;会议室}";
            db.AddEntity(db.ModelSpace(), std::move(mt));
        }

        // 天正修改范围框（闭合多段线、宽 80>=50、顶点数 4<=60、长边 3000>=2000、非零凸度 2>=2）
        {
            auto pl = std::make_unique<MiniDWG::LwPolyline>();
            pl->LayerHandle   = layWall;
            pl->ConstantWidth = 80.0;
            pl->Flags         = LwPolylineFlags::Closed;
            LwPolylineVertex v0; v0.Location = { 1000 + shiftX, 1000 + shiftY }; v0.Bulge = 0.25;
            LwPolylineVertex v1; v1.Location = { 4000 + shiftX, 1000 + shiftY }; v1.Bulge = 0.0;
            LwPolylineVertex v2; v2.Location = { 4000 + shiftX, 3000 + shiftY }; v2.Bulge = 0.25;
            LwPolylineVertex v3; v3.Location = { 1000 + shiftX, 3000 + shiftY }; v3.Bulge = 0.0;
            pl->Vertices = { v0, v1, v2, v3 };
            db.AddEntity(db.ModelSpace(), std::move(pl));
        }

        // 自定义块（内部同时含 WALL 几何与 DIM_TEXT 文字，用于验证块内递归过滤）
        BlockRecord* blk = db.CreateBlockRecord("ROOM_TAG_BLK");
        addLine(blk, layWall, 0, 0, 1000, 0);
        {
            auto bt = std::make_unique<MiniDWG::TextEntity>();
            bt->LayerHandle = layDim;
            bt->InsertPoint = { 100, 100, 0 };
            bt->Height      = 200.0;
            bt->Value       = "块内注记";
            db.AddEntity(blk, std::move(bt));
        }

        auto ins = std::make_unique<MiniDWG::Insert>();
        ins->BlockHandle = blk->ObjectHandle;
        ins->InsertPoint = { 5000 + shiftX, 4000 + shiftY, 0 };
        auto* insPtr = static_cast<MiniDWG::Insert*>(db.AddEntity(db.ModelSpace(), std::move(ins)));
        auto att = std::make_unique<MiniDWG::AttributeEntity>();
        att->LayerHandle = layDim;
        att->Tag         = "ROOM_NO";
        att->Value       = "101";
        att->InsertPoint = { 5100 + shiftX, 4200 + shiftY, 0 };
        att->Height      = 250.0;
        att->OwnerHandle = insPtr->ObjectHandle;
        const Handle attH = db.AddObject(std::move(att))->ObjectHandle;
        insPtr->Attributes.push_back(attH);

        // 图纸空间浮动视口（看向模型空间中央）
        if (BlockRecord* ps = db.PaperSpace())
        {
            auto vp = std::make_unique<MiniDWG::Viewport>();
            vp->ActiveStatus = 2;
            vp->Center       = { 210, 148, 0 };
            vp->Width        = 300;
            vp->Height       = 200;
            vp->ViewCenter   = { 4000 + shiftX, 3000 + shiftY };
            vp->ViewHeight   = 8000;
            db.AddEntity(ps, std::move(vp));
        }

        DwgWriteOptions opt;
        opt.Version = CadVersion::AC1027;
        const auto bytes = WriteDwg(db, opt);
        return !bytes.empty() && WriteBytesToUtf8Path(utf8Path, bytes);
    }

    bool HasPngMagic(const std::string& utf8Path, int& outW, int& outH)
    {
        std::ifstream f(MiniCAD::Utf8ToPath(utf8Path), std::ios::binary);
        if (!f.is_open()) return false;
        unsigned char hdr[24]{};
        f.read(reinterpret_cast<char*>(hdr), 24);
        if (f.gcount() != 24) return false;
        static constexpr unsigned char kSig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        if (std::memcmp(hdr, kSig, 8) != 0) return false;
        outW = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
        outH = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
        return outW > 0 && outH > 0;
    }

    void TestCliArgs()
    {
        ParsedCommand cmd;
        std::string code, msg;

        Check(ParseCliArgs({ "--version" }, cmd, code, msg) && cmd.Kind == CommandKind::Version,
              "CLI 解析 --version");
        Check(ParseCliArgs({ "--help" }, cmd, code, msg) && cmd.Kind == CommandKind::Help,
              "CLI 解析 --help");
        Check(!ParseCliArgs({}, cmd, code, msg),
              "CLI 空参数返回失败");
        Check(!ParseCliArgs({ "render", "a.dwg", "o.png", "--bbox", "10", "0", "5", "20" }, cmd, code, msg),
              "CLI 拒绝非法 bbox (xmax <= xmin)");
        Check(!ParseCliArgs({ "render", "a.dwg", "o.png", "--max-px", "99999" }, cmd, code, msg),
              "CLI 拒绝超硬顶 --max-px");
        Check(ParseCliArgs({ "diff", "a.dwg", "b.dwg", "--auto-align", "--out-png", "d.png", "--out-db", "d.sqlite" },
                           cmd, code, msg)
                  && cmd.Kind == CommandKind::Diff && cmd.Diff.AutoAlign && cmd.Diff.OutDbPath == "d.sqlite",
              "CLI 解析 diff 完整参数");
    }

    void TestExportDbAndReuse(const std::string& workDir)
    {
        const std::string dwgPath = workDir + "/测试图纸/建筑平面图.dwg";
        const std::string dbPath  = workDir + "/缓存库/建筑平面图.sqlite";
        Check(BuildGeneralSampleDwg(dwgPath), "生成含中文路径/XLINE/MTEXT/ATTRIB/视口的 DWG");

        ExportDbOptions opts;
        opts.InputPath    = dwgPath;
        opts.OutputDbPath = dbPath;
        ExportDbResult res;
        std::string err;
        Check(RunExportDbCommand(opts, res, &err), "export-db 成功执行（中文路径）");
        Check(!res.Reused && res.Format == "DWG" && res.InsunitsName == "mm",
              "export-db 元信息正确");
        Check(res.Counts.Entities >= 10 && res.Counts.Texts >= 3 && res.Counts.Viewports >= 1,
              "export-db 核心表计数非空");

        sqlite3* db = nullptr;
        Check(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
              "export-db 产出可读 SQLite 文件");
        if (db)
        {
            // 验证 19 张核心表 + 2 张 R-tree 虚拟表全部存在
            const int tableCount = SqlInt(db,
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name IN ('meta','diagnostics','layers','layouts','viewports','blocks','block_entities',"
                "'inserts','insert_attribs','texts','entities','tch_walls','tch_openings','tch_columns',"
                "'tch_spaces','tch_stairs','tch_dimensions','tch_axes','tch_sheets','entities_rtree','texts_rtree');");
            Check(tableCount == 21, "Schema v1.0 全部 19 张表 + 2 张 R-tree 完整存在");

            // 验证无界 XLINE 进 entities 且 minx IS NULL，不进 entities_rtree
            const int xlineCnt     = SqlInt(db, "SELECT COUNT(*) FROM entities WHERE entity_type='XLINE' AND minx IS NULL;");
            const int xlineInRtree = SqlInt(db, "SELECT COUNT(*) FROM entities_rtree r JOIN entities e ON e.id=r.id WHERE e.entity_type='XLINE';");
            Check(xlineCnt == 1 && xlineInRtree == 0, "无界 XLINE 入 entities(NULL bbox) 且跳过 entities_rtree");

            // 验证 MTEXT 内联格式码剥离与中文保留
            const std::string mtPlain = SqlText(db, "SELECT content FROM texts WHERE source_kind='MTEXT' LIMIT 1;");
            const std::string mtRaw   = SqlText(db, "SELECT raw_content FROM texts WHERE source_kind='MTEXT' LIMIT 1;");
            Check(mtPlain == "会议室" && mtRaw.find("\\fSimSun") != std::string::npos,
                  "MTEXT 中文纯文本提取与 raw_content 保留正确");

            // 验证 ATTRIB tag 与内容
            const std::string attVal = SqlText(db, "SELECT content FROM texts WHERE source_kind='ATTRIB' AND tag='ROOM_NO' LIMIT 1;");
            Check(attVal == "101", "块 ATTRIB 标签与值正确入 texts 表");

            // 验证 R-tree 空间查询命中（覆盖 (5000,4000) 附近）
            const int hitEntities = SqlInt(db, "SELECT COUNT(*) FROM entities_rtree WHERE maxx>=4500 AND minx<=6500 AND maxy>=3500 AND miny<=5000;");
            const int hitTexts    = SqlInt(db, "SELECT COUNT(*) FROM texts_rtree WHERE maxx>=1500 AND minx<=3500 AND maxy>=2500 AND miny<=3800;");
            Check(hitEntities >= 1 && hitTexts >= 1, "entities_rtree 与 texts_rtree 空间范围查询命中");

            sqlite3_close(db);
        }

        // 测试 --reuse-if-fresh 缓存命中
        opts.ReuseIfFresh = true;
        ExportDbResult res2;
        Check(RunExportDbCommand(opts, res2, &err) && res2.Reused,
              "export-db --reuse-if-fresh 未变动时直接复用");

        // 测试 --force 强制重建
        opts.Force = true;
        ExportDbResult res3;
        Check(RunExportDbCommand(opts, res3, &err) && !res3.Reused,
              "export-db --force 强制重建");

        // 测试源文件变化后缓存自动失效
        opts.Force = false;
        Check(BuildGeneralSampleDwg(dwgPath, 0.0, 0.0, /*includeExtraLine*/ true), "更新源 DWG 内容");
        ExportDbResult res4;
        Check(RunExportDbCommand(opts, res4, &err) && !res4.Reused,
              "export-db --reuse-if-fresh 源文件大小/内容变化时自动重建");
    }

    void TestTianzhengExportAndDiff(const std::string& workDir)
    {
        const std::string dwgA = workDir + "/tch_before.dwg";
        const std::string dwgB = workDir + "/tch_after.dwg";
        const std::string dbA  = workDir + "/tch_before.sqlite";
        // B 侧平移 (-1500, +800)，因此 B->A 应自动恢复 dx=+1500, dy=-800
        Check(BuildTianzhengSampleDwg(dwgA, 0.0, 0.0, false), "生成原生天正测试图 A");
        Check(BuildTianzhengSampleDwg(dwgB, -1500.0, 800.0, true), "生成平移并变更的原生天正测试图 B");

        // 1. 验证天正 8 表语义导出与跨表 JOIN
        ExportDbOptions expOpts;
        expOpts.InputPath    = dwgA;
        expOpts.OutputDbPath = dbA;
        ExportDbResult expRes;
        std::string err;
        Check(RunExportDbCommand(expOpts, expRes, &err), "export-db 导出原生天正 DWG");
        Check(expRes.IsNativeTianzheng
                  && expRes.Counts.TchWalls == 2
                  && expRes.Counts.TchOpenings == 1
                  && expRes.Counts.TchColumns == 1
                  && expRes.Counts.TchSpaces == 1
                  && expRes.Counts.TchStairs == 1
                  && expRes.Counts.TchDimensions == 1
                  && expRes.Counts.TchAxes == 6,
              "天正 7 类对象导出计数完全精确");

        sqlite3* db = nullptr;
        Check(sqlite3_open_v2(dbA.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "打开天正 SQLite");
        if (db)
        {
            const double wallThick = SqlDouble(db, "SELECT total_thickness FROM tch_walls ORDER BY id LIMIT 1;");
            const double wallH     = SqlDouble(db, "SELECT height FROM tch_walls ORDER BY id LIMIT 1;");
            Check(Near(wallThick, 240.0) && Near(wallH, 3000.0), "tch_walls 墙厚 240mm 与墙高 3000mm 精确");

            const int joinCnt = SqlInt(db,
                "SELECT COUNT(*) FROM tch_openings o JOIN tch_walls w ON o.wall_handle = w.handle "
                "WHERE o.label='M1024' AND ABS(o.width-1000)<1 AND ABS(w.total_thickness-240)<1;");
            Check(joinCnt == 1, "tch_openings JOIN tch_walls 跨表查询成功");

            const double colArea   = SqlDouble(db, "SELECT area_m2 FROM tch_columns LIMIT 1;");
            const double spaceArea = SqlDouble(db, "SELECT area_m2 FROM tch_spaces WHERE name='房间' LIMIT 1;");
            Check(Near(colArea, 0.36, 0.01) && Near(spaceArea, 20.0, 0.01),
                  "tch_columns (0.36m2) 与 tch_spaces (20.0m2) 面积计算精确");

            const int stairSteps = SqlInt(db, "SELECT steps1 FROM tch_stairs WHERE stair_kind='TCH_RECTSTAIR' LIMIT 1;");
            const double dimSpan = SqlDouble(db, "SELECT total_span FROM tch_dimensions LIMIT 1;");
            Check(stairSteps == 10 && Near(dimSpan, 6000.0, 1.0), "tch_stairs 与 tch_dimensions 参数精确");

            const int vertAxes  = SqlInt(db, "SELECT COUNT(*) FROM tch_axes WHERE orientation='vertical';");
            const int horizAxes = SqlInt(db, "SELECT COUNT(*) FROM tch_axes WHERE orientation='horizontal';");
            Check(vertAxes == 3 && horizAxes == 3, "tch_axes 垂直/水平方向与展开条数精确");

            const int m1024Texts = SqlInt(db, "SELECT COUNT(*) FROM texts WHERE content='M1024';");
            Check(m1024Texts == 1, "天正门编号在 texts 中零重复出现");
            sqlite3_close(db);
        }

        // 2. 验证三轨差分 + tch_axes 自动对齐 + 4 表 diff.sqlite
        DiffOptions diffOpts;
        diffOpts.InputPathA   = dwgA;
        diffOpts.InputPathB   = dwgB;
        diffOpts.AutoAlign    = true;
        diffOpts.MmPerPx      = 15.0;
        diffOpts.ToleranceMm  = 30.0;
        diffOpts.OutPngPath   = workDir + "/tch_diff.png";
        diffOpts.OutJsonPath  = workDir + "/tch_diff.json";
        diffOpts.OutJsonExplicit = true;
        diffOpts.OutDbPath    = workDir + "/tch_diff.sqlite";
#ifdef MINICAD_TEST_FONTS_DIR
        diffOpts.FontsDir     = MINICAD_TEST_FONTS_DIR;
#endif

        DiffReport report;
        std::string errCode, errMsg;
        bool usageErr = false;
        Check(RunDiffCommand(diffOpts, report, &errCode, &errMsg, &usageErr), "天正三轨 diff 执行成功");
        Check(report.Alignment.Source == "tch_axes"
                  && report.Alignment.Confidence == "high"
                  && Near(report.Alignment.Dx, 1500.0, 2.0)
                  && Near(report.Alignment.Dy, -800.0, 2.0),
              "diff --auto-align 通过 tch_axes 精确恢复 (dx=+1500, dy=-800)");

        Check(report.Regions.size() >= 1 && (report.AOnlyPixels > 0 || report.BOnlyPixels > 0),
              "轨 1 像素掩膜容差差分检出几何变化区域");

        bool foundTextMod = false, foundTextAdd = false;
        for (const auto& t : report.TextChanges)
        {
            if (t.ChangeType == "modified" && t.AText == "客厅" && t.BText == "起居室") foundTextMod = true;
            if (t.ChangeType == "b_only" && t.BText == "新增备注") foundTextAdd = true;
        }
        Check(foundTextMod && foundTextAdd, "轨 2 独立文字比对检出改字(客厅->起居室)与新增(新增备注)");

        bool foundWallMod = false, foundDoorMod = false, foundSpaceMod = false;
        for (const auto& c : report.TchChanges)
        {
            if (c.Category == "wall" && c.ChangeType == "modified" && c.Summary.find("240->300") != std::string::npos)
                foundWallMod = true;
            if (c.Category == "opening" && c.ChangeType == "modified" && c.Label == "M1024->M1524"
                && c.Summary.find("1000->1500") != std::string::npos)
                foundDoorMod = true;
            if (c.Category == "space" && c.ChangeType == "modified" && c.Summary.find("20->25") != std::string::npos)
                foundSpaceMod = true;
        }
        Check(report.TchAvailable && foundWallMod && foundDoorMod && foundSpaceMod,
              "轨 3 天正语义比对精确检出墙厚(240->300)、门宽编号(M1024->M1524, 1000->1500)、房间面积(20->25)");

        int pw = 0, ph = 0;
        Check(HasPngMagic(diffOpts.OutPngPath, pw, ph), "diff 输出有效三态叠加 PNG");

        // 回归测试：橙色区域框外扩留白，绝不吞没紧贴 bbox 四边的 1px 红/青差异像素
        {
            const int W = 120, H = 100;
            std::vector<uint8_t> mA(static_cast<size_t>(W) * H, 0u);
            std::vector<uint8_t> mB(static_cast<size_t>(W) * H, 0u);
            std::vector<uint8_t> aO(static_cast<size_t>(W) * H, 0u);
            std::vector<uint8_t> bO(static_cast<size_t>(W) * H, 0u);
            // 矩形四边恰好落在 tight bbox [20..80, 20..70] 上
            int expectedRed = 0;
            for (int x = 20; x <= 80; ++x)
            {
                mB[20 * W + x] = bO[20 * W + x] = 1u;
                mB[70 * W + x] = bO[70 * W + x] = 1u;
            }
            for (int y = 20; y <= 70; ++y)
            {
                mB[y * W + 20] = bO[y * W + 20] = 1u;
                mB[y * W + 80] = bO[y * W + 80] = 1u;
            }
            for (uint8_t v : bO) expectedRed += v;
            // 紧贴画布边缘的青色差异线 [0..10, 0..10] 也应受保护
            int expectedCyan = 0;
            for (int x = 0; x <= 10; ++x) { mA[0 * W + x] = aO[0 * W + x] = 1u; }
            for (uint8_t v : aO) expectedCyan += v;

            RgbaImage img;
            CompositeDiffOverlay(mA, mB, aO, bO, W, H, img);
            DrawRegionBoxWithLabel(img, 20, 20, 80, 70, 1, 0xFFu, 0x8Cu, 0x00u, 3, 5);
            DrawRegionBoxWithLabel(img, 0, 0, 10, 0, 2, 0xFFu, 0x8Cu, 0x00u, 3, 5);

            int keptRed = 0, keptCyan = 0, orangePx = 0;
            for (size_t i = 0; i < img.Pixels.size(); i += 4)
            {
                const uint8_t r = img.Pixels[i], g = img.Pixels[i + 1], b = img.Pixels[i + 2];
                if (r == 0xD9u && g == 0x1Fu && b == 0x1Fu) ++keptRed;
                if (r == 0x00u && g == 0x9Eu && b == 0xB8u) ++keptCyan;
                if (r == 0xFFu && g == 0x8Cu && b == 0x00u) ++orangePx;
            }
            Check(keptRed == expectedRed && keptCyan == expectedCyan && orangePx > 0,
                  "橙色差异区域框外扩留白且不覆盖紧贴边框/画布边缘的红/青差异像素");
        }

        sqlite3* ddb = nullptr;
        Check(sqlite3_open_v2(diffOpts.OutDbPath.c_str(), &ddb, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
              "diff --out-db 生成可读 4 表比对库");
        if (ddb)
        {
            const int rCnt   = SqlInt(ddb, "SELECT COUNT(*) FROM diff_regions;");
            const int rtCnt  = SqlInt(ddb, "SELECT COUNT(*) FROM diff_regions_rtree;");
            const int tCnt   = SqlInt(ddb, "SELECT COUNT(*) FROM diff_texts;");
            const int tchCnt = SqlInt(ddb, "SELECT COUNT(*) FROM diff_tch_changes;");
            Check(rCnt >= 1 && rCnt == rtCnt && tCnt >= 2 && tchCnt >= 3,
                  "diff.sqlite 4 表及 diff_regions_rtree 行数与报告一致");
            sqlite3_close(ddb);
        }
    }

    void TestRenderAndLayerAutoAlign(const std::string& workDir)
    {
        const std::string dwgA = workDir + "/gen_a.dwg";
        const std::string dwgB = workDir + "/gen_b.dwg";
        Check(BuildGeneralSampleDwg(dwgA, 0.0, 0.0, false), "生成通用图 A");
        Check(BuildGeneralSampleDwg(dwgB, -2500.0, 1200.0, true), "生成平移 (-2500,+1200) 的通用图 B");

        // 1. 单图渲染（含块内递归 --include-layers 与 --exclude-frames）
        RenderOptions ro;
        ro.InputPath          = dwgA;
        ro.OutputPngPath      = workDir + "/render_single.png";
        ro.MaxPx              = 1200;
        ro.IncludeLayersRegex = "WALL";
        ro.ExcludeAnnotations = true;
        ro.ExcludeFrames      = true;
#ifdef MINICAD_TEST_FONTS_DIR
        ro.FontsDir           = MINICAD_TEST_FONTS_DIR;
#endif
        RenderResult rr;
        std::string errCode, errMsg;
        bool usageErr = false;
        Check(RunRenderCommand(ro, rr, &errCode, &errMsg, &usageErr), "render 单图 + 块内递归过滤 + 排除修改框");
        int pw = 0, ph = 0;
        Check(HasPngMagic(ro.OutputPngPath, pw, ph) && pw <= 1200 && ph <= 1200,
              "render 输出合法 PNG 且尺寸未超 --max-px");
        Check(rr.FramesExcluded == 1 && rr.EntitiesFiltered >= 2 && rr.EntitiesRendered >= 3,
              "render 准确排除修改范围框 (frames_excluded==1) 与注记/非白名单层");

        // 2. 图纸空间布局视口渲染
        RenderOptions rp;
        rp.InputPath     = dwgA;
        rp.OutputPngPath = workDir + "/render_paper.png";
        rp.LayoutName    = "index:1";
        rp.MaxPx         = 1000;
#ifdef MINICAD_TEST_FONTS_DIR
        rp.FontsDir      = MINICAD_TEST_FONTS_DIR;
#endif
        RenderResult rpr;
        Check(RunRenderCommand(rp, rpr, &errCode, &errMsg, &usageErr) && rpr.EntitiesRendered >= 4,
              "render 图纸空间布局视口合成模型内容成功");
        Check(HasPngMagic(rp.OutputPngPath, pw, ph), "render 布局视口输出合法 PNG");

        // 3. render --compare 双图叠加模式
        RenderOptions rc;
        rc.InputPath     = dwgA;
        rc.ComparePath   = dwgB;
        rc.OffsetDx      = 2500.0;
        rc.OffsetDy      = -1200.0;
        rc.OutputPngPath = workDir + "/render_compare.png";
        rc.MaxPx         = 1200;
#ifdef MINICAD_TEST_FONTS_DIR
        rc.FontsDir      = MINICAD_TEST_FONTS_DIR;
#endif
        RenderResult rcr;
        Check(RunRenderCommand(rc, rcr, &errCode, &errMsg, &usageErr) && rcr.Mode == "compare",
              "render --compare 双图叠加模式成功");
        Check(HasPngMagic(rc.OutputPngPath, pw, ph), "render --compare 输出合法 PNG");

        // 4. 次选轨（DOTE 轴线层 LINE）自动对齐验证
        DiffOptions ld;
        ld.InputPathA = dwgA;
        ld.InputPathB = dwgB;
        ld.AutoAlign  = true;
        ld.MmPerPx    = 20.0;
#ifdef MINICAD_TEST_FONTS_DIR
        ld.FontsDir   = MINICAD_TEST_FONTS_DIR;
#endif
        DiffReport lrep;
        Check(RunDiffCommand(ld, lrep, &errCode, &errMsg, &usageErr), "无天正轴网图纸走次选轨自动对齐");
        Check(lrep.Alignment.Source == "layer_lines"
                  && lrep.Alignment.Confidence == "high"
                  && Near(lrep.Alignment.Dx, 2500.0, 2.0)
                  && Near(lrep.Alignment.Dy, -1200.0, 2.0),
              "次选轨 layer_lines 精确恢复偏移 (dx=+2500, dy=-1200)");
    }
}

int main(int argc, char** argv)
{
    LOG_SET_LEVEL(MiniCAD::LogLevel::Error);

    // 可选导出真实天正合成样图到指定目录，供 Release SaDwgTool.exe CLI 端到端实测
    if (argc >= 3 && std::string(argv[1]) == "--emit-tch-fixtures")
    {
        const std::string outDir = argv[2];
        const bool okA = BuildTianzhengSampleDwg(outDir + "/tch_before.dwg", 0.0, 0.0, false);
        const bool okB = BuildTianzhengSampleDwg(outDir + "/tch_after.dwg", -1500.0, 800.0, true);
        std::printf("emit tch fixtures: %s\n", (okA && okB) ? "OK" : "FAIL");
        return (okA && okB) ? 0 : 1;
    }

    const fs::path tmpRoot = fs::temp_directory_path()
        / ("sadwgtool_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(tmpRoot, ec);
    const std::string workDir = tmpRoot.string();

    TestCliArgs();
    TestExportDbAndReuse(workDir);
    TestTianzhengExportAndDiff(workDir);
    TestRenderAndLayerAutoAlign(workDir);

    fs::remove_all(tmpRoot, ec);

    std::printf("\nSaDwgToolTests 完成：%d 项失败\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
