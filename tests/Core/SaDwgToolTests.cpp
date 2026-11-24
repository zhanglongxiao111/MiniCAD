// SaDwgTool 核心单元测试：覆盖 CLI 解析、19 表 + 2 R-tree SQLite 导出、中文路径与缓存复用、
// 天正原生 8 表语义导出与跨表联查、纯 CPU 渲染（模型/图纸视口/双图叠加/块内递归过滤/修改框排除）、
// 以及三轨确定性比对（tch_axes 与轴线层自动对齐 + 像素差分聚类 + 文字差分 + 天正语义差分 + 4 表比对库）。
#include "CliArgs.h"
#include "CpuRasterizer.h"
#include "DiffEngine.h"
#include "PathGuard.h"
#include "PngWriter.h"
#include "SceneTessellator.h"
#include "SqliteExporter.h"
#include "TchCipherWriter.h"
#include "Core/Entity/BlockEntity.hpp"
#include "Core/Entity/DimensionEntity.hpp"
#include "Core/Entity/HatchEntity.hpp"
#include "Core/Entity/InsertEntity.hpp"
#include "Core/Entity/LineEntity.hpp"
#include "Core/Entity/WipeoutEntity.hpp"
#include "Core/Log.h"
#include "Core/PathUtils.h"
#include "Dwg/Write/DwgBitWriter.h"
#include "Dwg/Write/DwgWriter.h"
#include "Import/CadExchange.h"
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

    std::string PathToUtf8(const fs::path& p)
    {
        const auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }

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
        Check(ParseCliArgs({ "-h" }, cmd, code, msg) && cmd.Kind == CommandKind::Help,
              "CLI 解析 -h");
        Check(ParseCliArgs({ "help" }, cmd, code, msg) && cmd.Kind == CommandKind::Help,
              "CLI 解析 help");
        Check(ParseCliArgs({ "export-db", "--help" }, cmd, code, msg)
                  && cmd.Kind == CommandKind::Help && cmd.HelpTopic == CommandKind::ExportDb,
              "CLI 解析 export-db --help");
        Check(ParseCliArgs({ "render", "--help" }, cmd, code, msg)
                  && cmd.Kind == CommandKind::Help && cmd.HelpTopic == CommandKind::Render,
              "CLI 解析 render --help");
        Check(ParseCliArgs({ "render", "-h" }, cmd, code, msg)
                  && cmd.Kind == CommandKind::Help && cmd.HelpTopic == CommandKind::Render,
              "CLI 解析 render -h");
        Check(ParseCliArgs({ "diff", "--help" }, cmd, code, msg)
                  && cmd.Kind == CommandKind::Help && cmd.HelpTopic == CommandKind::Diff,
              "CLI 解析 diff --help");
        Check(ParseCliArgs({ "diff", "-h" }, cmd, code, msg)
                  && cmd.Kind == CommandKind::Help && cmd.HelpTopic == CommandKind::Diff,
              "CLI 解析 diff -h");
        Check(ParseCliArgs({ "diff", "a.dwg", "--help" }, cmd, code, msg)
                  && cmd.Kind == CommandKind::Help && cmd.HelpTopic == CommandKind::Diff,
              "CLI 解析 diff <arg> --help");
        Check(ParseCliArgs({ "help", "diff" }, cmd, code, msg)
                  && cmd.Kind == CommandKind::Help && cmd.HelpTopic == CommandKind::Diff,
              "CLI 解析 help diff");

        const std::string usage = BuildUsageText();
        Check(usage.find("export-db") != std::string::npos
                  && usage.find("render") != std::string::npos
                  && usage.find("diff") != std::string::npos
                  && usage.find("--out-png") != std::string::npos
                  && usage.find("xmin,ymin,xmax,ymax") != std::string::npos,
              "CLI 总览帮助包含子命令、关键选项与逗号 bbox 说明");

        const std::string exportHelp = BuildUsageText(CommandKind::ExportDb);
        Check(exportHelp.find("--reuse-if-fresh") != std::string::npos
                  && exportHelp.find("--force") != std::string::npos
                  && exportHelp.find("--no-block-entities") != std::string::npos,
              "export-db 子命令帮助包含完整选项");

        const std::string renderHelp = BuildUsageText(CommandKind::Render);
        Check(renderHelp.find("--compare") != std::string::npos
                  && renderHelp.find("--layout") != std::string::npos
                  && renderHelp.find("xmin,ymin,xmax,ymax") != std::string::npos
                  && renderHelp.find("--max-px") != std::string::npos,
              "render 子命令帮助包含完整选项与逗号 bbox 说明");

        const std::string diffHelp = BuildUsageText(CommandKind::Diff);
        Check(diffHelp.find("--out-png") != std::string::npos
                  && diffHelp.find("--out-json") != std::string::npos
                  && diffHelp.find("--out-db") != std::string::npos
                  && diffHelp.find("--tolerance-mm") != std::string::npos
                  && diffHelp.find("--min-region-mm") != std::string::npos
                  && diffHelp.find("--region-gap-mm") != std::string::npos
                  && diffHelp.find("xmin,ymin,xmax,ymax") != std::string::npos,
              "diff 子命令帮助包含完整输出/尺度/bbox 选项");

        Check(ParseCliArgs({ "diff", "a.dwg", "b.dwg", "--layout", "布局1" }, cmd, code, msg)
                  && !cmd.Diff.MmPerPxExplicit && !cmd.Diff.ToleranceMmExplicit
                  && !cmd.Diff.MinRegionMmExplicit && !cmd.Diff.RegionGapMmExplicit,
              "CLI diff 未显式传入尺度参数时 Explicit 标记均为 false");
        Check(ParseCliArgs({ "diff", "a.dwg", "b.dwg",
                             "--mm-per-px", "2.5", "--tolerance", "4.0",
                             "--min-region-mm", "6.0", "--region-gap-mm", "12.0" },
                           cmd, code, msg)
                  && cmd.Diff.MmPerPxExplicit && Near(cmd.Diff.MmPerPx, 2.5)
                  && cmd.Diff.ToleranceMmExplicit && Near(cmd.Diff.ToleranceMm, 4.0)
                  && cmd.Diff.MinRegionMmExplicit && Near(cmd.Diff.MinRegionMm, 6.0)
                  && cmd.Diff.RegionGapMmExplicit && Near(cmd.Diff.RegionGapMm, 12.0),
              "CLI diff 显式传入 4 个尺度参数（含 --tolerance 别名）均记录 Explicit=true");

        Check(!ParseCliArgs({}, cmd, code, msg),
              "CLI 空参数返回失败");
        Check(!ParseCliArgs({ "render", "a.dwg", "o.png", "--bbox", "10", "0", "5", "20" }, cmd, code, msg),
              "CLI 拒绝非法 bbox (xmax <= xmin)");
        Check(ParseCliArgs({ "render", "a.dwg", "o.png", "--bbox", "-10.5,-20.25,100.5,200.75" }, cmd, code, msg)
                  && cmd.Render.HasBbox
                  && Near(cmd.Render.Bbox[0], -10.5) && Near(cmd.Render.Bbox[1], -20.25)
                  && Near(cmd.Render.Bbox[2], 100.5) && Near(cmd.Render.Bbox[3], 200.75),
              "CLI render 解析逗号分隔 --bbox");
        Check(ParseCliArgs({ "render", "a.dwg", "o.png", "--bbox", "10", "20", "100", "200" }, cmd, code, msg)
                  && cmd.Render.HasBbox
                  && Near(cmd.Render.Bbox[0], 10.0) && Near(cmd.Render.Bbox[1], 20.0)
                  && Near(cmd.Render.Bbox[2], 100.0) && Near(cmd.Render.Bbox[3], 200.0),
              "CLI render 保持兼容空格分隔 --bbox");
        Check(ParseCliArgs({ "diff", "a.dwg", "b.dwg", "--bbox", "100,200,500,600" }, cmd, code, msg)
                  && cmd.Diff.HasBbox
                  && Near(cmd.Diff.Bbox[0], 100.0) && Near(cmd.Diff.Bbox[1], 200.0)
                  && Near(cmd.Diff.Bbox[2], 500.0) && Near(cmd.Diff.Bbox[3], 600.0),
              "CLI diff 解析逗号分隔 --bbox");
        Check(ParseCliArgs({ "diff", "a.dwg", "b.dwg", "--bbox", "100, 200, 500, 600" }, cmd, code, msg)
                  && cmd.Diff.HasBbox && Near(cmd.Diff.Bbox[0], 100.0) && Near(cmd.Diff.Bbox[3], 600.0),
              "CLI diff 解析带空格的逗号分隔 --bbox");
        Check(!ParseCliArgs({ "render", "a.dwg", "o.png", "--bbox", "10,20,30" }, cmd, code, msg),
              "CLI 拒绝少于 4 项的逗号 --bbox");
        Check(!ParseCliArgs({ "render", "a.dwg", "o.png", "--bbox", "10,20,30,40,50" }, cmd, code, msg),
              "CLI 拒绝多于 4 项的逗号 --bbox");
        Check(!ParseCliArgs({ "render", "a.dwg", "o.png", "--bbox", "10,20,30," }, cmd, code, msg),
              "CLI 拒绝尾逗号非法 --bbox");
        Check(!ParseCliArgs({ "diff", "a.dwg", "b.dwg", "--bbox", "100,0,50,200" }, cmd, code, msg),
              "CLI 拒绝逗号 --bbox (xmax <= xmin)");
        Check(!ParseCliArgs({ "diff", "a.dwg", "b.dwg", "--bbox", "10,abc,30,40" }, cmd, code, msg),
              "CLI 拒绝非数字逗号 --bbox");
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

    // 构造带纸张空间小尺寸几何变化（15mm x 10mm）的 A3 图纸，验证纸张布局 diff 自动尺度适配
    bool BuildPaperSpaceSampleDwg(const std::string& utf8Path, bool modifiedVersion)
    {
        CadDatabase db;
        db.SetVersion(CadVersion::AC1027);
        db.CreateDefaults();
        db.Header.InsUnits = UnitsType::Millimeters;

        auto addLine = [&](BlockRecord* blk, double x0, double y0, double x1, double y1) {
            auto l = std::make_unique<Line>();
            l->StartPoint = { x0, y0, 0 };
            l->EndPoint   = { x1, y1, 0 };
            db.AddEntity(blk, std::move(l));
        };

        // 模型空间：10000 x 8000 mm 建筑外框（用于验证 Model 空间仍保持 20/30/800/1200 默认值）
        addLine(db.ModelSpace(), 0, 0, 10000, 0);
        addLine(db.ModelSpace(), 10000, 0, 10000, 8000);
        addLine(db.ModelSpace(), 10000, 8000, 0, 8000);
        addLine(db.ModelSpace(), 0, 8000, 0, 0);
        if (modifiedVersion)
        {
            addLine(db.ModelSpace(), 2000, 2000, 4000, 2000);
            addLine(db.ModelSpace(), 4000, 2000, 4000, 3500);
        }

        // 纸张空间：A3 图框 420 x 297 mm + 内部基准线 + B 侧新增 15mm x 10mm 小尺寸变更
        if (BlockRecord* ps = db.PaperSpace())
        {
            addLine(ps, 0, 0, 420, 0);
            addLine(ps, 420, 0, 420, 297);
            addLine(ps, 420, 297, 0, 297);
            addLine(ps, 0, 297, 0, 0);
            addLine(ps, 20, 20, 400, 20);
            addLine(ps, 20, 277, 400, 277);

            auto txt = std::make_unique<MiniDWG::TextEntity>();
            txt->InsertPoint = { 300, 40, 0 };
            txt->Height      = 5.0;
            txt->Value       = modifiedVersion ? "3.200" : "4.200";
            db.AddEntity(ps, std::move(txt));

            if (modifiedVersion)
            {
                // 15mm x 10mm 纸张图面变化：模型默认 tolerance=30 / min_region=800 会全吞成 0，
                // 纸张自动默认 mm_per_px=1 / tolerance=1 / min_region=2 / region_gap=8 应准确检出
                addLine(ps, 120, 100, 135, 100);
                addLine(ps, 135, 100, 135, 110);
                addLine(ps, 135, 110, 120, 110);
                addLine(ps, 120, 110, 120, 100);
            }
        }

        DwgWriteOptions opt;
        opt.Version = CadVersion::AC1027;
        const auto bytes = WriteDwg(db, opt);
        return !bytes.empty() && WriteBytesToUtf8Path(utf8Path, bytes);
    }

    void TestPaperSpaceDiffDefaults(const std::string& workDir)
    {
        const std::string dwgA = workDir + "/paper_a.dwg";
        const std::string dwgB = workDir + "/paper_b.dwg";
        Check(BuildPaperSpaceSampleDwg(dwgA, false), "生成纸张空间样图 A");
        Check(BuildPaperSpaceSampleDwg(dwgB, true), "生成含 15x10mm 小尺寸变更的纸张空间样图 B");

        // 1. 纸张空间不传尺度参数：自动切到 1.0 / 1.0 / 2.0 / 8.0 并检出几何与文字变化
        ParsedCommand parsedPaper;
        std::string code, msg;
        Check(ParseCliArgs({ "diff", dwgA, dwgB, "--layout", "index:1" }, parsedPaper, code, msg),
              "CLI 解析纸张布局 diff 默认参数");
#ifdef MINICAD_TEST_FONTS_DIR
        parsedPaper.Diff.FontsDir = MINICAD_TEST_FONTS_DIR;
#endif
        DiffReport paperRep;
        bool usageErr = false;
        Check(RunDiffCommand(parsedPaper.Diff, paperRep, &code, &msg, &usageErr),
              "纸张布局默认尺度 diff 执行成功");
        Check(Near(paperRep.MmPerPx, 1.0, 0.05)
                  && Near(paperRep.ToleranceMm, 1.0)
                  && Near(paperRep.MinRegionMm, 2.0)
                  && Near(paperRep.RegionGapMm, 8.0),
              "纸张布局自动切换默认尺度 mm_per_px=1 / tolerance_mm=1 / min_region_mm=2 / region_gap_mm=8");
        Check(paperRep.Regions.size() >= 1 && paperRep.BOnlyPixels > 0,
              "纸张布局默认尺度自动检出 15x10mm 小尺寸几何变化 (regions.count >= 1)");
        Check(paperRep.TextModifiedCount >= 1,
              "纸张布局检出文字改字 (4.200 -> 3.200)");

        const std::string paperJson = FormatDiffReportJson(paperRep, false, false);
        Check(paperJson.find("\"mm_per_px\":1") != std::string::npos
                  && paperJson.find("\"tolerance_mm\":1") != std::string::npos
                  && paperJson.find("\"min_region_mm\":2") != std::string::npos
                  && paperJson.find("\"region_gap_mm\":8") != std::string::npos,
              "纸张布局 diff 报告 JSON 准确反映生效后的实际尺度参数");

        // 2. 纸张空间显式覆盖部分参数：仅被显式传入的参数按用户值，其余保持纸张默认
        ParsedCommand parsedOverride;
        Check(ParseCliArgs({ "diff", dwgA, dwgB, "--layout", "index:1",
                             "--tolerance-mm", "30.0", "--min-region-mm", "50.0" },
                           parsedOverride, code, msg),
              "CLI 解析纸张布局显式覆盖容差与最小区域参数");
#ifdef MINICAD_TEST_FONTS_DIR
        parsedOverride.Diff.FontsDir = MINICAD_TEST_FONTS_DIR;
#endif
        DiffReport overRep;
        Check(RunDiffCommand(parsedOverride.Diff, overRep, &code, &msg, &usageErr),
              "纸张布局显式覆盖参数 diff 执行成功");
        Check(Near(overRep.MmPerPx, 1.0, 0.05)
                  && Near(overRep.ToleranceMm, 30.0)
                  && Near(overRep.MinRegionMm, 50.0)
                  && Near(overRep.RegionGapMm, 8.0)
                  && overRep.Regions.empty(),
              "纸张布局显式参数生效且未覆盖项保持纸张默认");

        // 3. 模型空间默认参数保持 20 / 30 / 800 / 1200 不变
        ParsedCommand parsedModel;
        Check(ParseCliArgs({ "diff", dwgA, dwgB }, parsedModel, code, msg),
              "CLI 解析模型空间 diff 默认参数");
#ifdef MINICAD_TEST_FONTS_DIR
        parsedModel.Diff.FontsDir = MINICAD_TEST_FONTS_DIR;
#endif
        DiffReport modelRep;
        Check(RunDiffCommand(parsedModel.Diff, modelRep, &code, &msg, &usageErr),
              "模型空间默认参数 diff 执行成功");
        Check(Near(modelRep.MmPerPx, 20.0, 0.05)
                  && Near(modelRep.ToleranceMm, 30.0)
                  && Near(modelRep.MinRegionMm, 800.0)
                  && Near(modelRep.RegionGapMm, 1200.0),
              "模型空间保持原有默认尺度 20 / 30 / 800 / 1200 不变");
    }

    // ── CAD-14：Wipeout 保留遮罩语义 + 超长虚线按绘制序裁剪与相位保持 ─────────
    void TestWipeoutAndLongDashDrawOrder(const std::string& workDir)
    {
        // 1. 细分管线直测：超过 200 周期的长虚线（6000mm / 20mm = 300 周期）先后跨过 Wipeout
        {
            MiniCAD::Scene scene;
            MiniCAD::LineTypeRecord dashRec;
            dashRec.Name          = "TEST_DASH_20";
            dashRec.Pattern       = { 10.0, -10.0 };
            dashRec.PatternLength = 20.0;
            const MiniCAD::LineTypeID ltId = scene.GetLineTypeTable().AddOrReplace(dashRec);

            // 绘制序 1：在 Wipeout 之前的超长虚线 y = 0, x in [0, 6000]
            {
                auto ln = std::make_unique<MiniCAD::LineEntity>(
                    scene.NextObjectID(), MiniCAD::Math::Point3{ 0.0, 0.0, 0.0 },
                    MiniCAD::Math::Point3{ 6000.0, 0.0, 0.0 });
                ln->GetAttr().LineType      = ltId;
                ln->GetAttr().LinetypeScale = 1.0;
                scene.AddEntity(std::move(ln));
            }
            // 绘制序 2：无边框 Wipeout 遮罩 [2000..4000, -500..500]
            {
                auto wo = std::make_unique<MiniCAD::WipeoutEntity>(
                    scene.NextObjectID(),
                    std::vector<MiniCAD::Math::Point3>{
                        { 2000.0, -500.0, 0.0 },
                        { 4000.0, -500.0, 0.0 },
                        { 4000.0,  500.0, 0.0 },
                        { 2000.0,  500.0, 0.0 }
                    });
                wo->SetShowFrame(false);
                scene.AddEntity(std::move(wo));
            }
            // 绘制序 3：在 Wipeout 之后的超长虚线 y = 100, x in [0, 6000]
            {
                auto ln = std::make_unique<MiniCAD::LineEntity>(
                    scene.NextObjectID(), MiniCAD::Math::Point3{ 0.0, 100.0, 0.0 },
                    MiniCAD::Math::Point3{ 6000.0, 100.0, 0.0 });
                ln->GetAttr().LineType      = ltId;
                ln->GetAttr().LinetypeScale = 1.0;
                scene.AddEntity(std::move(ln));
            }

            MiniCAD::FontSystem fontSystem;
#ifdef MINICAD_TEST_FONTS_DIR
            InitHeadlessFontSystem(fontSystem, MINICAD_TEST_FONTS_DIR, false);
#else
            InitHeadlessFontSystem(fontSystem);
#endif
            TessellateFilter filter;
            filter.ExcludeAnnotations = true; // 默认注记过滤必须保留 Wipeout
            TessellatedScene tess;
            std::string err;
            const std::array<double, 4> bbox{ -100.0, -600.0, 6100.0, 600.0 };
            Check(TessellateSceneLayout(scene, 0, bbox, /*worldPerPixel*/ 2.0, /*vpH*/ 600.0,
                                        filter, fontSystem, tess, &err),
                  "CAD-14: 超长虚线与 Wipeout 细分成功");
            Check(tess.Stats.EntitiesRendered == 3 && tess.Stats.EntitiesFiltered == 0,
                  "CAD-14: 默认注记过滤不删除 Wipeout 实体");

            int beforeLeft = 0, beforeInside = 0, beforeRight = 0, afterInside = 0;
            bool phaseOk = true;
            for (size_t i = 0; i + 1 < tess.Lines.size(); i += 2)
            {
                const double x0 = tess.Lines[i].pos.x + tess.RenderOrigin.x;
                const double y0 = tess.Lines[i].pos.y + tess.RenderOrigin.y;
                const double x1 = tess.Lines[i + 1].pos.x + tess.RenderOrigin.x;
                const double y1 = tess.Lines[i + 1].pos.y + tess.RenderOrigin.y;
                const double lo = std::min(x0, x1);
                const double hi = std::max(x0, x1);
                if (std::abs(y0) < 1.0 && std::abs(y1) < 1.0)
                {
                    if (hi > 2000.5 && lo < 3999.5) ++beforeInside;
                    if (hi <= 2000.5) ++beforeLeft;
                    if (lo >= 3999.5)
                    {
                        ++beforeRight;
                        // 相位校验：周期 20mm，实线段必落在 [20k, 20k+10]
                        const double modLo = std::fmod(lo, 20.0);
                        const double modHi = std::fmod(hi, 20.0);
                        if (std::abs(modLo) > 0.25 && std::abs(modLo - 20.0) > 0.25) phaseOk = false;
                        if (std::abs(modHi - 10.0) > 0.25) phaseOk = false;
                    }
                }
                else if (std::abs(y0 - 100.0) < 1.0 && std::abs(y1 - 100.0) < 1.0)
                {
                    if (lo >= 2000.0 && hi <= 4000.0) ++afterInside;
                }
            }
            Check(beforeLeft >= 90 && beforeRight >= 90 && beforeInside == 0,
                  "CAD-14: 先于 Wipeout 的超长虚线在遮罩区内被完全裁剪且两侧正常展开");
            Check(phaseOk,
                  "CAD-14: 超长虚线跨过 Wipeout 后右侧实线段相位严格保持");
            Check(afterInside >= 90,
                  "CAD-14: 晚于 Wipeout 的超长虚线按绘制顺序不受先前 Wipeout 遮挡");
        }

        // 2. 端到端 diff 校验：默认 ExcludeAnnotations=true 下，Wipeout 遮挡后方差异线不误报
        auto buildWipeoutDwg = [](const std::string& path, bool altHiddenLine) -> bool {
            MiniCAD::Scene s;
            // 外框 [0..4000, 0..3000]
            s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 0, 0, 0 }, MiniCAD::Math::Point3{ 4000, 0, 0 }));
            s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 4000, 0, 0 }, MiniCAD::Math::Point3{ 4000, 3000, 0 }));
            s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 4000, 3000, 0 }, MiniCAD::Math::Point3{ 0, 3000, 0 }));
            s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 0, 3000, 0 }, MiniCAD::Math::Point3{ 0, 0, 0 }));
            // 被 Wipeout 完全遮挡的线（A/B 位置不同）
            if (!altHiddenLine)
                s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 1200, 1500, 0 }, MiniCAD::Math::Point3{ 2800, 1500, 0 }));
            else
                s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 1200, 1200, 0 }, MiniCAD::Math::Point3{ 2800, 1800, 0 }));
            // 覆盖 [1000..3000, 1000..2000] 的 Wipeout
            auto wo = std::make_unique<MiniCAD::WipeoutEntity>(
                s.NextObjectID(),
                std::vector<MiniCAD::Math::Point3>{
                    { 1000, 1000, 0 }, { 3000, 1000, 0 }, { 3000, 2000, 0 }, { 1000, 2000, 0 }
                });
            wo->SetShowFrame(false);
            s.AddEntity(std::move(wo));
            const auto bytes = MiniCAD::ExportCad(s, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2018);
            return !bytes.empty() && WriteBytesToUtf8Path(path, bytes);
        };

        const std::string woA = workDir + "/wipeout_a.dwg";
        const std::string woB = workDir + "/wipeout_b.dwg";
        Check(buildWipeoutDwg(woA, false) && buildWipeoutDwg(woB, true), "CAD-14: 生成含 Wipeout 遮罩的比对 DWG");
        DiffOptions dop;
        dop.InputPathA          = woA;
        dop.InputPathB          = woB;
        dop.IncludeAnnotations  = false;
        dop.MmPerPx             = 10.0;
        dop.MmPerPxExplicit     = true;
        dop.ToleranceMm         = 0.0;
        dop.ToleranceMmExplicit = true;
#ifdef MINICAD_TEST_FONTS_DIR
        dop.FontsDir            = MINICAD_TEST_FONTS_DIR;
#endif
        DiffReport rep;
        std::string code, msg;
        bool usageErr = false;
        Check(RunDiffCommand(dop, rep, &code, &msg, &usageErr), "CAD-14: 默认注记过滤下 Wipeout diff 执行成功");
        Check(rep.AOnlyPixels == 0 && rep.BOnlyPixels == 0,
              "CAD-14: 默认注记过滤保留 Wipeout 遮挡语义，被遮挡差异不产生误报");

        // 3. CAD-R2-07：Wipeout 位于命中注记正则的 "DIM" 与 "PUB_TEXT" 图层时，
        // 默认 diff 仍保留遮罩零误报，而显式 --exclude-layers "^DIM$" 仍正常排除该层 Wipeout
        auto buildAnnotatedLayerWipeoutDwg = [](const std::string& path, bool altHiddenLine, const char* wipeLayerName) -> bool {
            MiniCAD::Scene s;
            const MiniCAD::LayerID annLayerId = s.GetLayerManager().AddLayer(wipeLayerName);
            s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 0, 0, 0 }, MiniCAD::Math::Point3{ 4000, 0, 0 }));
            s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 4000, 0, 0 }, MiniCAD::Math::Point3{ 4000, 3000, 0 }));
            s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 4000, 3000, 0 }, MiniCAD::Math::Point3{ 0, 3000, 0 }));
            s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 0, 3000, 0 }, MiniCAD::Math::Point3{ 0, 0, 0 }));
            if (!altHiddenLine)
                s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 1200, 1500, 0 }, MiniCAD::Math::Point3{ 2800, 1500, 0 }));
            else
                s.AddEntity(std::make_unique<MiniCAD::LineEntity>(s.NextObjectID(), MiniCAD::Math::Point3{ 1200, 1200, 0 }, MiniCAD::Math::Point3{ 2800, 1800, 0 }));

            auto wo = std::make_unique<MiniCAD::WipeoutEntity>(
                s.NextObjectID(),
                std::vector<MiniCAD::Math::Point3>{
                    { 1000, 1000, 0 }, { 3000, 1000, 0 }, { 3000, 2000, 0 }, { 1000, 2000, 0 }
                });
            wo->SetShowFrame(false);
            wo->SetLayerId(annLayerId);
            s.AddEntity(std::move(wo));
            const auto bytes = MiniCAD::ExportCad(s, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2018);
            return !bytes.empty() && WriteBytesToUtf8Path(path, bytes);
        };

        for (const char* layerName : { "DIM", "PUB_TEXT" })
        {
            const std::string aPath = workDir + "/wipeout_ann_" + layerName + "_a.dwg";
            const std::string bPath = workDir + "/wipeout_ann_" + layerName + "_b.dwg";
            Check(buildAnnotatedLayerWipeoutDwg(aPath, false, layerName)
                      && buildAnnotatedLayerWipeoutDwg(bPath, true, layerName),
                  "CAD-R2-07: 生成注记图层含 Wipeout 遮罩的测试 DWG");

            DiffOptions dDef;
            dDef.InputPathA          = aPath;
            dDef.InputPathB          = bPath;
            dDef.IncludeAnnotations  = false;
            dDef.MmPerPx             = 10.0;
            dDef.MmPerPxExplicit     = true;
            dDef.ToleranceMm         = 0.0;
            dDef.ToleranceMmExplicit = true;
#ifdef MINICAD_TEST_FONTS_DIR
            dDef.FontsDir            = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport rDef;
            Check(RunDiffCommand(dDef, rDef, &code, &msg, &usageErr)
                      && rDef.AOnlyPixels == 0 && rDef.BOnlyPixels == 0,
                  "CAD-R2-07: 注记图层（DIM / PUB_TEXT）上的 Wipeout 在默认 diff 下保留遮挡零误报");
        }

        // 显式 --exclude-layers "^DIM$" 仍必须排除 DIM 图层上的 Wipeout，使背后差异暴露
        {
            DiffOptions dExcl;
            dExcl.InputPathA          = workDir + "/wipeout_ann_DIM_a.dwg";
            dExcl.InputPathB          = workDir + "/wipeout_ann_DIM_b.dwg";
            dExcl.IncludeAnnotations  = false;
            dExcl.ExcludeLayersRegex  = "^DIM$";
            dExcl.MmPerPx             = 10.0;
            dExcl.MmPerPxExplicit     = true;
            dExcl.ToleranceMm         = 0.0;
            dExcl.ToleranceMmExplicit = true;
#ifdef MINICAD_TEST_FONTS_DIR
            dExcl.FontsDir            = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport rExcl;
            Check(RunDiffCommand(dExcl, rExcl, &code, &msg, &usageErr)
                      && (rExcl.AOnlyPixels > 0 && rExcl.BOnlyPixels > 0),
                  "CAD-R2-07: 显式 --exclude-layers \"^DIM$\" 仍正常拦截 DIM 层 Wipeout 并暴露背后差异");
        }
    }

    // ── CAD-15：默认整图窗口取 A/B 并集 + 旧图空布局可正常检出新增 ───────────────
    void TestUnionDefaultBboxAndEmptySide(const std::string& workDir)
    {
        auto writeBoxDwg = [](const std::string& path, bool hasBoxA, bool hasFarBoxB, const char* extraText) -> bool {
            CadDatabase db;
            db.SetVersion(CadVersion::AC1027);
            db.CreateDefaults();
            db.Header.InsUnits = UnitsType::Millimeters;
            auto addRect = [&](double x0, double y0, double x1, double y1) {
                auto addL = [&](double ax, double ay, double bx, double by) {
                    auto l = std::make_unique<Line>();
                    l->StartPoint = { ax, ay, 0 };
                    l->EndPoint   = { bx, by, 0 };
                    db.AddEntity(db.ModelSpace(), std::move(l));
                };
                addL(x0, y0, x1, y0); addL(x1, y0, x1, y1);
                addL(x1, y1, x0, y1); addL(x0, y1, x0, y0);
            };
            if (hasBoxA)    addRect(0.0, 0.0, 2000.0, 2000.0);
            if (hasFarBoxB) addRect(6000.0, 0.0, 8000.0, 2000.0);
            if (extraText && *extraText)
            {
                auto t = std::make_unique<MiniDWG::TextEntity>();
                t->InsertPoint = { 6500.0, 1000.0, 0.0 };
                t->Height      = 250.0;
                t->Value       = extraText;
                db.AddEntity(db.ModelSpace(), std::move(t));
            }
            DwgWriteOptions opt;
            opt.Version = CadVersion::AC1027;
            const auto bytes = WriteDwg(db, opt);
            return !bytes.empty() && WriteBytesToUtf8Path(path, bytes);
        };

        const std::string boxA   = workDir + "/union_a.dwg";
        const std::string boxB   = workDir + "/union_b.dwg";
        const std::string emptyA = workDir + "/empty_a.dwg";
        const std::string onlyB  = workDir + "/only_b.dwg";
        Check(writeBoxDwg(boxA, true, false, nullptr)
                  && writeBoxDwg(boxB, true, true, "右扩新增")
                  && writeBoxDwg(emptyA, false, false, nullptr)
                  && writeBoxDwg(onlyB, false, true, "空图新增"),
              "CAD-15: 生成并集范围与空布局样图");

        // 1. diff 未传 --bbox：窗口自动覆盖 B 外扩图元
        {
            DiffOptions dop;
            dop.InputPathA          = boxA;
            dop.InputPathB          = boxB;
            dop.MmPerPx             = 20.0;
            dop.MmPerPxExplicit     = true;
            dop.ToleranceMm         = 10.0;
            dop.ToleranceMmExplicit = true;
            dop.MinRegionMm         = 200.0;
            dop.MinRegionMmExplicit = true;
#ifdef MINICAD_TEST_FONTS_DIR
            dop.FontsDir            = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport rep;
            std::string code, msg;
            bool usageErr = false;
            Check(RunDiffCommand(dop, rep, &code, &msg, &usageErr), "CAD-15: diff 默认并集窗口执行成功");
            Check(rep.CropInASpace[2] >= 7999.0 && rep.BOnlyPixels > 0 && rep.Regions.size() >= 1 && rep.TextBOnlyCount >= 1,
                  "CAD-15: diff 默认整图并集覆盖 B 侧外扩区域并报出新增几何与文字");
        }

        // 2. diff 旧图为空、新图有图元：不报错且正确报出新增
        {
            DiffOptions dop;
            dop.InputPathA          = emptyA;
            dop.InputPathB          = onlyB;
            dop.MmPerPx             = 20.0;
            dop.MmPerPxExplicit     = true;
            dop.ToleranceMm         = 10.0;
            dop.ToleranceMmExplicit = true;
            dop.MinRegionMm         = 200.0;
            dop.MinRegionMmExplicit = true;
#ifdef MINICAD_TEST_FONTS_DIR
            dop.FontsDir            = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport rep;
            std::string code, msg;
            bool usageErr = false;
            Check(RunDiffCommand(dop, rep, &code, &msg, &usageErr),
                  "CAD-15: diff 旧图空布局 + 新图有图元不提前报 EMPTY_LAYOUT");
            Check(rep.AOnlyPixels == 0 && rep.BOnlyPixels > 0 && rep.Regions.size() >= 1 && rep.TextBOnlyCount >= 1,
                  "CAD-15: diff 旧图空布局正确检出 B 侧全部新增图元与文字");
        }

        // 3. render --compare 未传 --bbox：外扩 B 与空布局 A 均成功并取并集
        {
            RenderOptions rc;
            rc.InputPath     = emptyA;
            rc.ComparePath   = onlyB;
            rc.OutputPngPath = workDir + "/compare_empty_a.png";
            rc.MaxPx         = 800;
#ifdef MINICAD_TEST_FONTS_DIR
            rc.FontsDir      = MINICAD_TEST_FONTS_DIR;
#endif
            RenderResult rr;
            std::string code, msg;
            bool usageErr = false;
            int pw = 0, ph = 0;
            Check(RunRenderCommand(rc, rr, &code, &msg, &usageErr)
                      && rr.Bbox[0] <= 6001.0 && rr.Bbox[2] >= 7999.0
                      && HasPngMagic(rc.OutputPngPath, pw, ph),
                  "CAD-15: render --compare 在空 A / 非空 B 下自动按并集出图");
        }
    }

    // ── CAD-16：纸张布局语义轨覆盖边界透明披露 ──────────────────────────────────
    void TestPaperLayoutCoverageDisclosure(const std::string& workDir)
    {
        const std::string dwgA = workDir + "/paper_vp_a.dwg";
        const std::string dwgB = workDir + "/paper_vp_b.dwg";
        Check(BuildGeneralSampleDwg(dwgA, 0.0, 0.0, false)
                  && BuildGeneralSampleDwg(dwgB, 0.0, 0.0, true),
              "CAD-16: 生成含浮动视口的纸张布局样图");

        DiffOptions dop;
        dop.InputPathA = dwgA;
        dop.InputPathB = dwgB;
        dop.LayoutName = "index:1";
        dop.OutDbPath  = workDir + "/paper_vp_diff.sqlite";
#ifdef MINICAD_TEST_FONTS_DIR
        dop.FontsDir   = MINICAD_TEST_FONTS_DIR;
#endif
        DiffReport rep;
        std::string code, msg;
        bool usageErr = false;
        Check(RunDiffCommand(dop, rep, &code, &msg, &usageErr), "CAD-16: 含浮动视口纸张布局 diff 执行成功");
        Check(rep.IsPaperSpaceLayout
                  && rep.PaperViewportCountA >= 1 && rep.PaperViewportCountB >= 1
                  && rep.PaperViewportSemanticsExcluded
                  && !rep.CoverageNote.empty(),
              "CAD-16: 报告明确标记纸张布局含浮动视口且语义轨未采集视口模型对象");

        const std::string json = FormatDiffReportJson(rep, false, false);
        Check(json.find("\"coverage\":") != std::string::npos
                  && json.find("\"layout_space\":\"paper\"") != std::string::npos
                  && json.find("\"text_scope\":\"paper_space_only\"") != std::string::npos
                  && json.find("\"tch_scope\":\"paper_space_only\"") != std::string::npos
                  && json.find("\"viewport_model_semantics_included\":false") != std::string::npos,
              "CAD-16: JSON coverage 元数据完整披露纸张布局语义边界");

        sqlite3* db = nullptr;
        if (sqlite3_open_v2(dop.OutDbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK)
        {
            const std::string textScope = SqlText(db, "SELECT value FROM diff_meta WHERE key='text_scope';");
            const std::string vpInc     = SqlText(db, "SELECT value FROM diff_meta WHERE key='viewport_model_semantics_included';");
            Check(textScope == "paper_space_only" && vpInc == "0",
                  "CAD-16: diff_meta 表同步写入纸张布局语义边界字段");
            sqlite3_close(db);
        }
        else
        {
            Check(false, "CAD-16: 打开 paper_vp_diff.sqlite");
        }

        // CAD-R2-14：显式跨空间比较（A=Model，B=纸张布局 index:1）逐侧如实披露覆盖边界
        {
            DiffOptions dMix;
            dMix.InputPathA  = dwgA;
            dMix.InputPathB  = dwgB;
            dMix.LayoutName  = "Model";
            dMix.LayoutNameB = "index:1";
            dMix.OutDbPath   = workDir + "/mixed_space_diff.sqlite";
#ifdef MINICAD_TEST_FONTS_DIR
            dMix.FontsDir    = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport mixRep;
            Check(RunDiffCommand(dMix, mixRep, &code, &msg, &usageErr), "CAD-R2-14: 模型对纸张跨空间 diff 执行成功");
            Check(mixRep.SideA.Layout == "Model" && mixRep.SideB.Layout != "Model"
                      && mixRep.CoverageNote.find("跨空间比对") != std::string::npos
                      && mixRep.CoverageNote.find("A 侧覆盖模型空间语义对象") != std::string::npos
                      && mixRep.CoverageNote.find("B 侧覆盖纸面空间实体") != std::string::npos,
                  "CAD-R2-14: 跨空间比对 CoverageNote 逐侧如实披露模型与纸张语义边界");

            bool warnDescribesBothSides = false;
            for (const auto& w : mixRep.Warnings)
                if (w.find("跨空间比对") != std::string::npos
                    && w.find("A 侧覆盖模型空间语义对象") != std::string::npos
                    && w.find("B 侧覆盖纸面空间实体") != std::string::npos)
                    warnDescribesBothSides = true;
            Check(warnDescribesBothSides, "CAD-R2-14: 跨空间比对 Warnings 逐侧披露语义边界且不笼统宣称双侧仅纸面");

            const std::string mixJson = FormatDiffReportJson(mixRep, false, false);
            Check(mixJson.find("\"layout_space\":\"mixed\"") != std::string::npos
                      && mixJson.find("\"layout_space_a\":\"model\"") != std::string::npos
                      && mixJson.find("\"layout_space_b\":\"paper\"") != std::string::npos
                      && mixJson.find("\"text_scope\":\"mixed\"") != std::string::npos
                      && mixJson.find("\"text_scope_a\":\"model_space\"") != std::string::npos
                      && mixJson.find("\"text_scope_b\":\"paper_space_only\"") != std::string::npos
                      && mixJson.find("\"tch_scope\":\"mixed\"") != std::string::npos
                      && mixJson.find("\"tch_scope_a\":\"model_space\"") != std::string::npos
                      && mixJson.find("\"tch_scope_b\":\"paper_space_only\"") != std::string::npos,
                  "CAD-R2-14: JSON coverage 字段逐侧输出 layout_space / text_scope / tch_scope");

            sqlite3* mixDb = nullptr;
            if (sqlite3_open_v2(dMix.OutDbPath.c_str(), &mixDb, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK)
            {
                Check(SqlText(mixDb, "SELECT value FROM diff_meta WHERE key='layout_space';") == "mixed"
                          && SqlText(mixDb, "SELECT value FROM diff_meta WHERE key='layout_space_a';") == "model"
                          && SqlText(mixDb, "SELECT value FROM diff_meta WHERE key='layout_space_b';") == "paper"
                          && SqlText(mixDb, "SELECT value FROM diff_meta WHERE key='text_scope_a';") == "model_space"
                          && SqlText(mixDb, "SELECT value FROM diff_meta WHERE key='text_scope_b';") == "paper_space_only",
                      "CAD-R2-14: diff_meta 同步写入逐侧覆盖边界");
                sqlite3_close(mixDb);
            }
            else
            {
                Check(false, "CAD-R2-14: 打开 mixed_space_diff.sqlite");
            }
        }
    }

    // ── CAD-17：--tolerance-mm 0 对应真实 0px 与有效毫米容差输出 ─────────────────
    void TestZeroToleranceAndEffectiveReporting(const std::string& workDir)
    {
        auto writeShiftedLine = [](const std::string& path, double lineX) -> bool {
            CadDatabase db;
            db.SetVersion(CadVersion::AC1027);
            db.CreateDefaults();
            db.Header.InsUnits = UnitsType::Millimeters;
            auto addL = [&](double x0, double y0, double x1, double y1) {
                auto l = std::make_unique<Line>();
                l->StartPoint = { x0, y0, 0 };
                l->EndPoint   = { x1, y1, 0 };
                db.AddEntity(db.ModelSpace(), std::move(l));
            };
            // 稳定外框 [0..4000, 0..2000]
            addL(0, 0, 4000, 0); addL(4000, 0, 4000, 2000);
            addL(4000, 2000, 0, 2000); addL(0, 2000, 0, 0);
            // 中间竖线平移 10mm（在 10mm/px 下恰好偏移 1px）
            addL(lineX, 200, lineX, 1800);
            DwgWriteOptions opt;
            opt.Version = CadVersion::AC1027;
            const auto bytes = WriteDwg(db, opt);
            return !bytes.empty() && WriteBytesToUtf8Path(path, bytes);
        };

        const std::string dwgA = workDir + "/tol0_a.dwg";
        const std::string dwgB = workDir + "/tol0_b.dwg";
        Check(writeShiftedLine(dwgA, 2000.0) && writeShiftedLine(dwgB, 2010.0),
              "CAD-17: 生成 10mm 偏移测试 DWG");

        // 1. --tolerance-mm 0 @ 10mm/px -> 真实 0px，检出 1px 偏移
        {
            ParsedCommand parsed;
            std::string code, msg;
            Check(ParseCliArgs({ "diff", dwgA, dwgB, "--mm-per-px", "10", "--tolerance-mm", "0", "--min-region-mm", "50" },
                               parsed, code, msg),
                  "CAD-17: CLI 接受 --tolerance-mm 0");
#ifdef MINICAD_TEST_FONTS_DIR
            parsed.Diff.FontsDir = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport rep;
            bool usageErr = false;
            Check(RunDiffCommand(parsed.Diff, rep, &code, &msg, &usageErr), "CAD-17: 0 容差 diff 执行成功");
            Check(rep.TolerancePx == 0 && Near(rep.ToleranceMm, 0.0) && Near(rep.EffectiveToleranceMm, 0.0),
                  "CAD-17: --tolerance-mm 0 生效为 tolerance_px=0 且 effective_tolerance_mm=0");
            Check(rep.AOnlyPixels > 0 && rep.BOnlyPixels > 0,
                  "CAD-17: 0 像素容差精确检出 1 像素（10mm）偏移");
            const std::string json = FormatDiffReportJson(rep, false, false);
            Check(json.find("\"tolerance_px\":0") != std::string::npos
                      && json.find("\"effective_tolerance_mm\":0") != std::string::npos,
                  "CAD-17: JSON 报告输出 tolerance_px=0 与 effective_tolerance_mm=0");
        }

        // 2. --tolerance-mm 10 @ 10mm/px -> 1px，吸收 1px 偏移
        {
            DiffOptions dop;
            dop.InputPathA          = dwgA;
            dop.InputPathB          = dwgB;
            dop.MmPerPx             = 10.0;
            dop.MmPerPxExplicit     = true;
            dop.ToleranceMm         = 10.0;
            dop.ToleranceMmExplicit = true;
#ifdef MINICAD_TEST_FONTS_DIR
            dop.FontsDir            = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport rep;
            std::string code, msg;
            bool usageErr = false;
            Check(RunDiffCommand(dop, rep, &code, &msg, &usageErr)
                      && rep.TolerancePx == 1 && Near(rep.EffectiveToleranceMm, 10.0)
                      && rep.AOnlyPixels == 0 && rep.BOnlyPixels == 0,
                  "CAD-17: 1 像素容差吸收 10mm 偏移并汇报 effective_tolerance_mm=10");
        }

        // 3. --tolerance-mm 30 @ 20mm/px -> ceil(1.5)=2px -> effective_tolerance_mm=40
        {
            DiffOptions dop;
            dop.InputPathA          = dwgA;
            dop.InputPathB          = dwgB;
            dop.MmPerPx             = 20.0;
            dop.MmPerPxExplicit     = true;
            dop.ToleranceMm         = 30.0;
            dop.ToleranceMmExplicit = true;
#ifdef MINICAD_TEST_FONTS_DIR
            dop.FontsDir            = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport rep;
            std::string code, msg;
            bool usageErr = false;
            Check(RunDiffCommand(dop, rep, &code, &msg, &usageErr)
                      && rep.TolerancePx == 2 && Near(rep.ToleranceMm, 30.0) && Near(rep.EffectiveToleranceMm, 40.0),
                  "CAD-17: 30mm@20mm/px 真实生效 2px 并准确报告 effective_tolerance_mm=40");
        }
    }

    // ── CAD-18：天正空侧全增/全删 + 墙体高度差分与缺失值区分 ─────────────────────
    void TestTchEmptySideAndWallHeightDiff(const std::string& workDir)
    {
        auto writeTchWallDwg = [](const std::string& path, bool hasWall, double wallHeight, bool includeAxis) -> bool {
            CadDatabase db;
            db.SetVersion(CadVersion::AC1027);
            db.CreateDefaults();
            db.Header.InsUnits = UnitsType::Millimeters;
            if (hasWall || includeAxis)
            {
                db.Classes.push_back(MakeTchClass("TCH_WALL", "TDbWall", 500));
                db.Classes.push_back(MakeTchClass("TCH_AXIS_LABEL", "TDbAxisLabel", 501));
            }
            if (includeAxis)
            {
                db.AddEntity(db.ModelSpace(), MakeTchAxisGroup(0, 9000, 0, 12000, { 4000.0, 0.0 }, { "1", "2" }));
            }
            if (hasWall)
            {
                db.AddEntity(db.ModelSpace(), MakeTchWall({ 0, 0, 0 }, { 4000, 0, 0 }, 120.0, wallHeight, 0, 40));
            }
            DwgWriteOptions opt;
            opt.Version = CadVersion::AC1027;
            const auto bytes = WriteDwg(db, opt);
            return !bytes.empty() && WriteBytesToUtf8Path(path, bytes);
        };

        const std::string emptyDwg   = workDir + "/tch_empty.dwg";
        const std::string axisOnly   = workDir + "/tch_axis_only.dwg";
        const std::string wall2800   = workDir + "/tch_wall_2800.dwg";
        const std::string wall3200   = workDir + "/tch_wall_3200.dwg";
        Check(writeTchWallDwg(emptyDwg, false, 0.0, false)
                  && writeTchWallDwg(axisOnly, false, 0.0, true)
                  && writeTchWallDwg(wall2800, true, 2800.0, true)
                  && writeTchWallDwg(wall3200, true, 3200.0, true),
              "CAD-18: 生成天正墙体与空侧测试 DWG");

        // 1. 两侧均为原生天正，裁剪框内单侧全删（A 有墙、B 裁剪框内无墙）
        {
            DiffOptions dop;
            dop.InputPathA = wall2800;
            dop.InputPathB = axisOnly;
            dop.HasBbox    = true;
            dop.Bbox       = { -500.0, -500.0, 4500.0, 1500.0 };
#ifdef MINICAD_TEST_FONTS_DIR
            dop.FontsDir   = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport rep;
            std::string code, msg;
            bool usageErr = false;
            Check(RunDiffCommand(dop, rep, &code, &msg, &usageErr)
                      && rep.TchAvailable && rep.TchWallCount == 1
                      && rep.TchChanges.size() == 1 && rep.TchChanges[0].ChangeType == "a_only",
                  "CAD-18: 裁剪框内单侧全删正常启用天正轨并报出 a_only");
        }

        // 2. A 完全空白、B 有原生天正墙 -> 启用天正轨并报出 b_only
        {
            DiffOptions dop;
            dop.InputPathA = emptyDwg;
            dop.InputPathB = wall2800;
#ifdef MINICAD_TEST_FONTS_DIR
            dop.FontsDir   = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport rep;
            std::string code, msg;
            bool usageErr = false;
            Check(RunDiffCommand(dop, rep, &code, &msg, &usageErr)
                      && rep.TchAvailable && rep.TchWallCount == 1
                      && rep.TchChanges.size() == 1 && rep.TchChanges[0].ChangeType == "b_only",
                  "CAD-18: 空图对比原生天正图纸正常启用天正轨并报出 b_only");
        }

        // 3. 墙体高度变化 2800 -> 3200
        {
            DiffOptions dop;
            dop.InputPathA = wall2800;
            dop.InputPathB = wall3200;
#ifdef MINICAD_TEST_FONTS_DIR
            dop.FontsDir   = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport rep;
            std::string code, msg;
            bool usageErr = false;
            Check(RunDiffCommand(dop, rep, &code, &msg, &usageErr)
                      && rep.TchAvailable && rep.TchWallCount == 1
                      && rep.TchChanges[0].ChangeType == "modified"
                      && rep.TchChanges[0].Summary.find("墙高 2800->3200mm") != std::string::npos,
                  "CAD-18: 天正墙体高度变更 2800->3200mm 被精确检出");
        }

        // 4. 墙体高度缺失 vs 有值（区分缺失，不误按 0mm）以及双侧均缺失不误报
        {
            auto makeWallSnap = [](bool hasHeight, double height) {
                MiniCAD::CadSemanticSnapshot s;
                s.TchDecodedCount = 1;
                MiniCAD::CadSemanticSnapshot::TchWallRecord w;
                w.Handle         = 0x200;
                w.DxfName        = "TCH_WALL";
                w.Layer          = "WALL";
                w.Space          = "model";
                w.LayoutName     = "Model";
                w.StartX = 0.0; w.StartY = 0.0; w.EndX = 4000.0; w.EndY = 0.0;
                w.LeftWidth = 120.0; w.RightWidth = 120.0; w.TotalThickness = 240.0;
                w.HasHeight = hasHeight;
                w.Height    = hasHeight ? height : 0.0;
                w.Length    = 4000.0;
                w.MaterialName = "钢筋混凝土";
                w.MinX = 0.0; w.MinY = -120.0; w.MaxX = 4000.0; w.MaxY = 120.0;
                s.TchWalls.push_back(w);
                return s;
            };

            const auto snapMissing = makeWallSnap(false, 0.0);
            const auto snap3200    = makeWallSnap(true, 3200.0);
            const std::array<double, 4> crop{ -500.0, -500.0, 4500.0, 500.0 };

            DiffReport repMissToVal;
            RunTrack3TchDiff(snapMissing, "model", "Model", snap3200, "model", "Model",
                             crop, 0.0, 0.0, 300.0, {}, repMissToVal);
            Check(repMissToVal.TchAvailable && repMissToVal.TchWallCount == 1
                      && repMissToVal.TchChanges[0].ChangeType == "modified"
                      && repMissToVal.TchChanges[0].Summary.find("墙高 缺失->3200mm") != std::string::npos
                      && repMissToVal.TchChanges[0].BeforeJson.find("\"height\":null") != std::string::npos,
                  "CAD-18: 墙高缺失与真实数值变更明确区分（输出 缺失 与 height:null）");

            DiffReport repBothMiss;
            RunTrack3TchDiff(snapMissing, "model", "Model", snapMissing, "model", "Model",
                             crop, 0.0, 0.0, 300.0, {}, repBothMiss);
            Check(repBothMiss.TchAvailable && repBothMiss.TchWallCount == 0 && repBothMiss.TchChanges.empty(),
                  "CAD-18: 双侧墙高均缺失时不产生误报差异");
        }
    }

    // ── CAD-19：标注 <> 占位符前后缀替换（DimensionEntity + TCH_DIMENSION2）───────
    void TestDimensionPlaceholderOverride(const std::string& workDir)
    {
        MiniCAD::Scene scene;
        MiniCAD::DimStyle ds;
        ds.Precision = 0;

        auto addDim = [&](double x0, double y0, double x1, double y1, const std::string& ovr) {
            auto d = MiniCAD::DimensionEntity::MakeAligned(
                scene.NextObjectID(),
                MiniCAD::Math::Point3{ x0, y0, 0.0 },
                MiniCAD::Math::Point3{ x1, y1, 0.0 },
                MiniCAD::Math::Point3{ x0, y0 + 200.0, 0.0 },
                ds);
            d->SetTextOverride(ovr);
            scene.AddEntity(std::move(d));
        };

        addDim(0.0,    0.0, 3600.0,    0.0, "约<>");
        addDim(0.0, 1000.0, 1200.0, 1000.0, "<>（净宽）");
        addDim(0.0, 2000.0, 2800.0, 2000.0, "{\\H0.8x;净高<>mm}");
        addDim(0.0, 3000.0, 1500.0, 3000.0, "固定文字");
        addDim(0.0, 4000.0, 2400.0, 4000.0, "");

        // 同时构造一条带 <> 覆盖的天正尺寸标注记录
        MiniCAD::CadSemanticSnapshot snap;
        snap.Format = "DWG";
        {
            MiniCAD::CadSemanticSnapshot::TchDimensionRecord td;
            td.Handle        = 0x777;
            td.DxfName       = "TCH_DIMENSION2";
            td.Layer         = "DIM";
            td.Space         = "model";
            td.LayoutName    = "Model";
            td.X             = 500.0;
            td.Y             = 5000.0;
            td.TotalSpan     = 4500.0;
            td.Spans         = { 4500.0 };
            td.TextOverrides[0] = "约<>";
            td.MinX = 0.0; td.MinY = 4900.0; td.MaxX = 4500.0; td.MaxY = 5100.0;
            snap.TchDimensions.push_back(std::move(td));
        }

        const std::string dbPath = workDir + "/dim_placeholder.sqlite";
        ExportDbResult res;
        std::string err;
        Check(ExportSceneToSqlite(scene, snap, workDir + "/dim_src.dwg", 100, 1000, dbPath, true, res, &err),
              "CAD-19: 含 <> 占位符标注导出 SQLite 成功");

        sqlite3* db = nullptr;
        Check(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
              "CAD-19: 打开标注 SQLite");
        if (db)
        {
            Check(SqlInt(db, "SELECT COUNT(*) FROM texts WHERE content='约3600';") == 1,
                  "CAD-19: '约<>' 替换为 '约3600'");
            Check(SqlInt(db, "SELECT COUNT(*) FROM texts WHERE content='1200（净宽）';") == 1,
                  "CAD-19: '<>（净宽）' 替换为 '1200（净宽）'");
            Check(SqlInt(db, "SELECT COUNT(*) FROM texts WHERE content='净高2800mm';") == 1,
                  "CAD-19: 带 MText 格式码的 '{\\H0.8x;净高<>mm}' 替换为 '净高2800mm'");
            Check(SqlInt(db, "SELECT COUNT(*) FROM texts WHERE content='固定文字';") == 1,
                  "CAD-19: 无 <> 占位符的纯覆盖文字保留原文");
            Check(SqlInt(db, "SELECT COUNT(*) FROM texts WHERE content='2400';") == 1,
                  "CAD-19: 空覆盖回退真实测量值 '2400'");
            Check(SqlInt(db, "SELECT COUNT(*) FROM texts WHERE content='约4500';") == 1,
                  "CAD-19: TCH_DIMENSION2 分段覆盖 '约<>' 替换为 '约4500'");
            Check(SqlInt(db, "SELECT COUNT(*) FROM texts WHERE content LIKE '%<>%';") == 0,
                  "CAD-19: texts 表中不再残留未展开的 '<>' 字面量");
            sqlite3_close(db);
        }
    }

    // ── CAD-20：WriteDiffSqlite 失败严格回滚与临时文件清理 ───────────────────────
    void TestWriteDiffSqliteRollbackAndCleanup(const std::string& workDir)
    {
        const fs::path subDir = MiniCAD::Utf8ToPath(workDir) / "diff_sqlite_rollback";
        std::error_code ec;
        fs::create_directories(subDir, ec);
        const std::string targetPath = PathToUtf8(subDir / "target.sqlite");
        const std::string sentinel   = "SENTINEL_EXISTING_VALID_DB_BYTES";
        {
            std::ofstream f(MiniCAD::Utf8ToPath(targetPath), std::ios::binary | std::ios::trunc);
            f.write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
        }

        auto readAll = [](const std::string& p) -> std::string {
            std::ifstream f(MiniCAD::Utf8ToPath(p), std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        auto countExtraFiles = [&]() -> int {
            int cnt = 0;
            for (const auto& entry : fs::directory_iterator(subDir, ec))
            {
                if (entry.path().filename() != "target.sqlite") ++cnt;
            }
            return cnt;
        };

        // 1. 主键冲突（两个 Region 使用相同 Id = 1）
        {
            DiffReport badRep;
            DiffRegionItem r1; r1.Id = 1; r1.BboxDrawing = { 0, 0, 100, 100 }; r1.BboxPx = { 0, 0, 10, 10 }; r1.DominantSide = "both";
            DiffRegionItem r2; r2.Id = 1; r2.BboxDrawing = { 200, 0, 300, 100 }; r2.BboxPx = { 20, 0, 30, 10 }; r2.DominantSide = "both";
            badRep.Regions = { r1, r2 };
            std::string err;
            const bool ok = WriteDiffSqlite(targetPath, badRep, &err);
            Check(!ok && !err.empty(), "CAD-20: WriteDiffSqlite 捕获主键冲突并返回错误");
            Check(readAll(targetPath) == sentinel, "CAD-20: 主键失败不破坏已有目标库");
            Check(countExtraFiles() == 0, "CAD-20: 主键失败自动清理全部临时文件与日志");
        }

        // 2. R-tree 约束冲突（minx > maxx）
        {
            DiffReport badRtree;
            DiffRegionItem r1; r1.Id = 1; r1.BboxDrawing = { 500, 0, 100, 100 }; r1.BboxPx = { 0, 0, 10, 10 }; r1.DominantSide = "a_only";
            badRtree.Regions = { r1 };
            std::string err;
            const bool ok = WriteDiffSqlite(targetPath, badRtree, &err);
            Check(!ok && !err.empty(), "CAD-20: WriteDiffSqlite 捕获 R-tree 逆序范围约束失败");
            Check(readAll(targetPath) == sentinel, "CAD-20: R-tree 失败不破坏已有目标库");
            Check(countExtraFiles() == 0, "CAD-20: R-tree 失败自动清理全部临时文件");
        }
    }

    // ── CAD-21：--reuse-if-fresh 选项变化与外部参照依赖指纹失效 ──────────────────
    void TestExportCacheInvalidationOptionsAndXrefs(const std::string& workDir)
    {
        // 1. --no-block-entities 选项变化必须触发缓存失效
        {
            const std::string dwgPath = workDir + "/cache_opt_src.dwg";
            const std::string dbPath  = workDir + "/cache_opt.sqlite";
            Check(BuildGeneralSampleDwg(dwgPath), "CAD-21: 生成带块定义的样图");

            ExportDbOptions opts;
            opts.InputPath       = dwgPath;
            opts.OutputDbPath    = dbPath;
            opts.ReuseIfFresh    = true;
            opts.NoBlockEntities = true;

            ExportDbResult r1, r2, r3, r4;
            std::string err;
            Check(RunExportDbCommand(opts, r1, &err) && !r1.Reused && r1.Counts.BlockEntities == 0,
                  "CAD-21: 首次 --no-block-entities 导出成功且 block_entities==0");
            Check(RunExportDbCommand(opts, r2, &err) && r2.Reused,
                  "CAD-21: 同选项二次执行命中缓存");

            opts.NoBlockEntities = false;
            Check(RunExportDbCommand(opts, r3, &err) && !r3.Reused && r3.Counts.BlockEntities > 0,
                  "CAD-21: 去掉 --no-block-entities 时自动失效并重导出块内实体");
            Check(RunExportDbCommand(opts, r4, &err) && r4.Reused,
                  "CAD-21: 含块内实体导出后再次执行命中缓存");
        }

        // 2. 外部参照修改 / 删除 / 恢复均触发缓存失效
        {
            const fs::path xrefDir = MiniCAD::Utf8ToPath(workDir) / "xref_cache_test";
            std::error_code ec;
            fs::create_directories(xrefDir, ec);
            const std::string childPath = PathToUtf8(xrefDir / "sub_xref.dwg");
            const std::string hostPath  = PathToUtf8(xrefDir / "host.dwg");
            const std::string dbPath    = PathToUtf8(xrefDir / "host.sqlite");

            auto writeChild = [&](int lineCount) -> bool {
                MiniCAD::Scene s;
                for (int i = 0; i < lineCount; ++i)
                {
                    s.AddEntity(std::make_unique<MiniCAD::LineEntity>(
                        s.NextObjectID(),
                        MiniCAD::Math::Point3{ 0.0, i * 100.0, 0.0 },
                        MiniCAD::Math::Point3{ 1000.0, i * 100.0, 0.0 }));
                }
                const auto bytes = MiniCAD::ExportCad(s, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2018);
                return !bytes.empty() && WriteBytesToUtf8Path(childPath, bytes);
            };
            auto writeHost = [&]() -> bool {
                MiniCAD::Scene s;
                MiniCAD::BlockTable& tbl = s.GetBlockTable();
                const MiniCAD::BlockID bid = tbl.Create("子参照", { 0, 0, 0 });
                MiniCAD::BlockEntity* blk  = tbl.Find(bid);
                blk->SetFlags(MiniCAD::BlockEntity::XRef);
                blk->SetXRefPath("sub_xref.dwg");
                auto ins = std::make_unique<MiniCAD::InsertEntity>(
                    s.NextObjectID(), bid, "子参照", MiniCAD::Math::Point3{ 100, 200, 0 });
                ins->SetBlock(blk);
                s.AddEntity(std::move(ins));
                const auto bytes = MiniCAD::ExportCad(s, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2018);
                return !bytes.empty() && WriteBytesToUtf8Path(hostPath, bytes);
            };

            Check(writeChild(1) && writeHost(), "CAD-21: 生成带外部参照的宿主与子图 DWG");

            ExportDbOptions opts;
            opts.InputPath    = hostPath;
            opts.OutputDbPath = dbPath;
            opts.ReuseIfFresh = true;

            ExportDbResult r1, r2, r3, r4, r5, r6;
            std::string err;
            Check(RunExportDbCommand(opts, r1, &err) && !r1.Reused, "CAD-21: 宿主首次导出成功");
            Check(RunExportDbCommand(opts, r2, &err) && r2.Reused, "CAD-21: 宿主与外部参照均未变时命中缓存");

            Check(writeChild(4), "CAD-21: 修改外部参照子图内容与大小");
            Check(RunExportDbCommand(opts, r3, &err) && !r3.Reused,
                  "CAD-21: 仅外部参照变化（宿主未变）时缓存强制失效重建");

            fs::remove(MiniCAD::Utf8ToPath(childPath), ec);
            Check(RunExportDbCommand(opts, r4, &err) && !r4.Reused,
                  "CAD-21: 已解析外部参照被删除时缓存失效重建");
            Check(RunExportDbCommand(opts, r5, &err) && r5.Reused,
                  "CAD-21: 外部参照持续缺失且未变化时稳定复用");

            Check(writeChild(2), "CAD-21: 恢复缺失的外部参照文件");
            Check(RunExportDbCommand(opts, r6, &err) && !r6.Reused,
                  "CAD-21: 缺失外部参照重新出现磁盘时缓存失效重建");
        }

        // 3. CAD-R2-08 ①：外参磁盘可 stat 但内容损坏导致导入失败 -> 记 unverifiable，
        // 事后修复为同字节大小且同 mtime 的合法 DWG 后再次导出绝不误复用旧库
        {
            const fs::path corruptDir = MiniCAD::Utf8ToPath(workDir) / "xref_corrupt_test";
            std::error_code ec;
            fs::create_directories(corruptDir, ec);
            const fs::path childFs    = corruptDir / "sub_xref.dwg";
            const std::string childP  = PathToUtf8(childFs);
            const std::string hostP   = PathToUtf8(corruptDir / "host.dwg");
            const std::string dbP     = PathToUtf8(corruptDir / "host.sqlite");

            // 先在内存生成合法子外参字节，测出精确大小
            std::vector<uint8_t> goodChildBytes;
            {
                MiniCAD::Scene cs;
                cs.AddEntity(std::make_unique<MiniCAD::LineEntity>(
                    cs.NextObjectID(), MiniCAD::Math::Point3{ 0, 0, 0 }, MiniCAD::Math::Point3{ 500, 0, 0 }));
                goodChildBytes = MiniCAD::ExportCad(cs, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2018);
            }
            // 构造同大小的损坏内容写盘（磁盘可 stat 但 Importer 解析失败）
            const std::vector<uint8_t> badChildBytes(goodChildBytes.size(), static_cast<uint8_t>('X'));
            Check(!goodChildBytes.empty() && WriteBytesToUtf8Path(childP, badChildBytes),
                  "CAD-R2-08: 写出与合法外参同大小的损坏外参文件");
            const auto frozenMtime = fs::last_write_time(childFs, ec);
            Check(!ec, "CAD-R2-08: 获取损坏外参文件原始 mtime");

            // 写出引用该外参的宿主图
            {
                MiniCAD::Scene hs;
                MiniCAD::BlockTable& tbl = hs.GetBlockTable();
                const MiniCAD::BlockID bid = tbl.Create("损坏参照", { 0, 0, 0 });
                MiniCAD::BlockEntity* blk  = tbl.Find(bid);
                blk->SetFlags(MiniCAD::BlockEntity::XRef);
                blk->SetXRefPath("sub_xref.dwg");
                auto ins = std::make_unique<MiniCAD::InsertEntity>(
                    hs.NextObjectID(), bid, "损坏参照", MiniCAD::Math::Point3{ 0, 0, 0 });
                ins->SetBlock(blk);
                hs.AddEntity(std::move(ins));
                const auto hostBytes = MiniCAD::ExportCad(hs, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2018);
                Check(!hostBytes.empty() && WriteBytesToUtf8Path(hostP, hostBytes), "CAD-R2-08: 写出宿主图纸");
            }

            ExportDbOptions opts;
            opts.InputPath    = hostP;
            opts.OutputDbPath = dbP;
            opts.ReuseIfFresh = true;

            ExportDbResult rBad{}, rFixed{}, rReuse{};
            std::string err;
            Check(RunExportDbCommand(opts, rBad, &err) && !rBad.Reused && rBad.Counts.BlockEntities == 0,
                  "CAD-R2-08: 外参损坏时宿主导出完成且未解析块实体数为 0");
            {
                sqlite3* db = nullptr;
                Check(sqlite3_open_v2(dbP.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
                      "CAD-R2-08: 打开损坏外参导出的 SQLite");
                if (db)
                {
                    const std::string depsV2 = SqlText(db, "SELECT value FROM meta WHERE key='xref_deps_v2';");
                    Check(depsV2.find("\nunverifiable\t") != std::string::npos
                              && depsV2.find("\nresolved\t") == std::string::npos,
                          "CAD-R2-08: 磁盘存在但导入失败的外参在 xref_deps_v2 中严格标为 unverifiable");
                    sqlite3_close(db);
                }
            }

            // 修复外参文件并强行还原完全相同的 size 与 mtime
            Check(WriteBytesToUtf8Path(childP, goodChildBytes), "CAD-R2-08: 替换为同大小合法外参 DWG");
            fs::last_write_time(childFs, frozenMtime, ec);
            Check(!ec, "CAD-R2-08: 还原外参文件至与损坏态完全相同的 mtime");

            Check(RunExportDbCommand(opts, rFixed, &err) && !rFixed.Reused && rFixed.Counts.BlockEntities >= 1,
                  "CAD-R2-08: 修复外参后即使 size/mtime 完全不变也绝不误复用旧库，成功重导读入外参实体");
            Check(RunExportDbCommand(opts, rReuse, &err) && rReuse.Reused,
                  "CAD-R2-08: 合法外参成功入库后，后续未变动时正常命中缓存复用");
        }

        // 4. CAD-R2-08 ②：同一宿主文件经硬链接置于两个含不同相对外参的目录 -> 绝不跨目录串库
        {
            const fs::path crossRoot = MiniCAD::Utf8ToPath(workDir) / "xref_cross_dir_test";
            const fs::path dirA      = crossRoot / "proj_a";
            const fs::path dirB      = crossRoot / "proj_b";
            std::error_code ec;
            fs::create_directories(dirA, ec);
            fs::create_directories(dirB, ec);

            auto writeChildAt = [](const fs::path& dst, int lineCount) -> bool {
                MiniCAD::Scene cs;
                for (int i = 0; i < lineCount; ++i)
                    cs.AddEntity(std::make_unique<MiniCAD::LineEntity>(
                        cs.NextObjectID(),
                        MiniCAD::Math::Point3{ 0.0, i * 50.0, 0.0 },
                        MiniCAD::Math::Point3{ 300.0, i * 50.0, 0.0 }));
                const auto bytes = MiniCAD::ExportCad(cs, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2018);
                return !bytes.empty() && WriteBytesToUtf8Path(PathToUtf8(dst), bytes);
            };
            Check(writeChildAt(dirA / "sub_xref.dwg", 1) && writeChildAt(dirB / "sub_xref.dwg", 4),
                  "CAD-R2-08: 在两个项目目录分别写入不同的相对外参（1 线 vs 4 线）");

            const fs::path hostAFs = dirA / "host.dwg";
            const fs::path hostBFs = dirB / "host.dwg";
            {
                MiniCAD::Scene hs;
                MiniCAD::BlockTable& tbl = hs.GetBlockTable();
                const MiniCAD::BlockID bid = tbl.Create("共享外参", { 0, 0, 0 });
                MiniCAD::BlockEntity* blk  = tbl.Find(bid);
                blk->SetFlags(MiniCAD::BlockEntity::XRef);
                blk->SetXRefPath("sub_xref.dwg");
                auto ins = std::make_unique<MiniCAD::InsertEntity>(
                    hs.NextObjectID(), bid, "共享外参", MiniCAD::Math::Point3{ 0, 0, 0 });
                ins->SetBlock(blk);
                hs.AddEntity(std::move(ins));
                const auto hostBytes = MiniCAD::ExportCad(hs, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2018);
                Check(!hostBytes.empty() && WriteBytesToUtf8Path(PathToUtf8(hostAFs), hostBytes),
                      "CAD-R2-08: 写出 proj_a 宿主图");
            }
            fs::create_hard_link(hostAFs, hostBFs, ec);
            Check(!ec && PathsReferToSameFile(PathToUtf8(hostAFs), PathToUtf8(hostBFs)),
                  "CAD-R2-08: 创建 proj_b 硬链接指向同一宿主文件身份");

            const std::string sharedDb = PathToUtf8(crossRoot / "shared_cache.sqlite");
            ExportDbOptions optsA;
            optsA.InputPath    = PathToUtf8(hostAFs);
            optsA.OutputDbPath = sharedDb;
            optsA.ReuseIfFresh = true;

            ExportDbOptions optsB = optsA;
            optsB.InputPath = PathToUtf8(hostBFs);

            ExportDbResult rA{}, rB{}, rB2{};
            std::string err;
            Check(RunExportDbCommand(optsA, rA, &err) && !rA.Reused && rA.Counts.BlockEntities == 1,
                  "CAD-R2-08: proj_a 首次导出读入 1 条外参线");
            Check(RunExportDbCommand(optsB, rB, &err) && !rB.Reused && rB.Counts.BlockEntities == 4,
                  "CAD-R2-08: 换 proj_b 同文件身份宿主调用复用时不串库，重算相对外参链并读入 4 条线");
            Check(RunExportDbCommand(optsB, rB2, &err) && rB2.Reused && rB2.Counts.BlockEntities == 4,
                  "CAD-R2-08: proj_b 再次调用复用稳定命中缓存");
        }

        // 5. CAD-R3-04：外参读取窗口前后双 stat 指纹消费 + VersionVerified=false 落 unverifiable + 旧 xref_deps_v1 自动失效升级
        {
            const fs::path winDir = MiniCAD::Utf8ToPath(workDir) / "xref_r3_04_test";
            std::error_code ec;
            fs::create_directories(winDir, ec);
            const fs::path childFs   = winDir / "sub_xref.dwg";
            const fs::path hostFs    = winDir / "host.dwg";
            const std::string childP = PathToUtf8(childFs);
            const std::string hostP  = PathToUtf8(hostFs);
            const std::string dbP    = PathToUtf8(winDir / "host.sqlite");

            auto writeChildLines = [&](int lineCount) -> bool {
                MiniCAD::Scene cs;
                for (int i = 0; i < lineCount; ++i)
                {
                    cs.AddEntity(std::make_unique<MiniCAD::LineEntity>(
                        cs.NextObjectID(),
                        MiniCAD::Math::Point3{ 0.0, i * 100.0, 0.0 },
                        MiniCAD::Math::Point3{ 600.0, i * 100.0, 0.0 }));
                }
                const auto bytes = MiniCAD::ExportCad(cs, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2018);
                return !bytes.empty() && WriteBytesToUtf8Path(childP, bytes);
            };
            {
                MiniCAD::Scene hs;
                MiniCAD::BlockTable& tbl = hs.GetBlockTable();
                const MiniCAD::BlockID bid = tbl.Create("窗口外参", { 0, 0, 0 });
                MiniCAD::BlockEntity* blk  = tbl.Find(bid);
                blk->SetFlags(MiniCAD::BlockEntity::XRef);
                blk->SetXRefPath("sub_xref.dwg");
                auto ins = std::make_unique<MiniCAD::InsertEntity>(
                    hs.NextObjectID(), bid, "窗口外参", MiniCAD::Math::Point3{ 0, 0, 0 });
                ins->SetBlock(blk);
                hs.AddEntity(std::move(ins));
                const auto hostBytes = MiniCAD::ExportCad(hs, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2018);
                Check(writeChildLines(1) && !hostBytes.empty() && WriteBytesToUtf8Path(hostP, hostBytes),
                      "CAD-R3-04: 生成读取窗口测试宿主图与 1 线外参");
            }

            // 5a. 加载后、SQLite 写盘前篡改磁盘外参：ExportSceneToSqlite 必须消费加载瞬间的 XrefLoadRecord 指纹，
            //     严禁事后重探把旧几何签成新外参版本，从而保证后续 --reuse-if-fresh 必失效重建
            uint64_t hostSize = 0;
            int64_t  hostMtime = 0;
            Check(MiniCAD::TryStatFileVersion(hostFs, hostSize, hostMtime), "CAD-R3-04: 采集宿主版本成功");

            MiniCAD::Scene loadedScene;
            MiniCAD::CadSemanticSnapshot loadedSnap;
            std::string err;
            Check(MiniCAD::LoadCadFileWithSemantics(hostP, loadedScene, loadedSnap, &err),
                  "CAD-R3-04: LoadCadFileWithSemantics 成功加载宿主与外参");
            Check(loadedSnap.XrefLoads.size() == 1
                      && loadedSnap.XrefLoads[0].Status == MiniCAD::CadSemanticSnapshot::XrefLoadStatus::Loaded
                      && loadedSnap.XrefLoads[0].VersionVerified
                      && loadedSnap.XrefLoads[0].SizeBytes > 0,
                  "CAD-R3-04: 导入器读取窗口双 stat 校验通过并记录 VersionVerified/SizeBytes/MtimeMs");

            const uint64_t origXrefSize  = loadedSnap.XrefLoads[0].SizeBytes;
            const int64_t  origXrefMtime = loadedSnap.XrefLoads[0].MtimeMs;

            // 在写入 SQLite 前立即将磁盘外参替换为 5 线版本并推进 mtime
            Check(writeChildLines(5), "CAD-R3-04: 加载后写库前篡改磁盘外参为 5 线版本");
            const auto curChildTime = fs::last_write_time(childFs, ec);
            if (!ec)
                fs::last_write_time(childFs, curChildTime + std::chrono::seconds(5), ec);
            uint64_t tamperedSize = 0;
            int64_t  tamperedMtime = 0;
            Check(MiniCAD::TryStatFileVersion(childFs, tamperedSize, tamperedMtime)
                      && (tamperedSize != origXrefSize || tamperedMtime != origXrefMtime),
                  "CAD-R3-04: 篡改后磁盘外参指纹已与加载瞬间不同");

            ExportDbResult expStale{};
            Check(ExportSceneToSqlite(loadedScene, loadedSnap, hostP, hostSize, hostMtime,
                                      dbP, true, expStale, &err)
                      && expStale.Counts.BlockEntities == 1,
                  "CAD-R3-04: ExportSceneToSqlite 写出加载瞬间旧几何（1 条外参线）");
            {
                sqlite3* db = nullptr;
                Check(sqlite3_open_v2(dbP.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
                      "CAD-R3-04: 打开写出的 SQLite 校验 xref_deps_v2");
                if (db)
                {
                    const std::string depsV2 = SqlText(db, "SELECT value FROM meta WHERE key='xref_deps_v2';");
                    const std::string expectTail = "\t" + std::to_string(origXrefSize) + "\t" + std::to_string(origXrefMtime);
                    const std::string badTail    = "\t" + std::to_string(tamperedSize) + "\t" + std::to_string(tamperedMtime);
                    Check(depsV2.rfind("v2:", 0) == 0
                              && depsV2.find(expectTail) != std::string::npos
                              && depsV2.find(badTail) == std::string::npos,
                          "CAD-R3-04: xref_deps_v2 记录导入瞬间旧外参指纹而非写盘前篡改的新 stat");
                    sqlite3_close(db);
                }
            }

            ExportDbOptions opts;
            opts.InputPath    = hostP;
            opts.OutputDbPath = dbP;
            opts.ReuseIfFresh = true;
            ExportDbResult rebuilt{};
            Check(RunExportDbCommand(opts, rebuilt, &err) && !rebuilt.Reused && rebuilt.Counts.BlockEntities == 5,
                  "CAD-R3-04: 后续 --reuse-if-fresh 检出篡改后的外参变化，拒绝复用并重建读入 5 条线");

            // 5b. 读前后双 stat 不一致（VersionVerified == false）：落 unverifiable 且禁止复用
            {
                const std::string dbUnverifP = PathToUtf8(winDir / "host_unverif.sqlite");
                MiniCAD::CadSemanticSnapshot unverifiedSnap = loadedSnap;
                unverifiedSnap.XrefLoads[0].VersionVerified = false;
                ExportDbResult expUnverif{};
                Check(ExportSceneToSqlite(loadedScene, unverifiedSnap, hostP, hostSize, hostMtime,
                                          dbUnverifP, true, expUnverif, &err),
                      "CAD-R3-04: 写出 VersionVerified=false 的缓存库");
                {
                    sqlite3* db = nullptr;
                    Check(sqlite3_open_v2(dbUnverifP.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
                          "CAD-R3-04: 打开 unverifiable 缓存库");
                    if (db)
                    {
                        const std::string depsV2 = SqlText(db, "SELECT value FROM meta WHERE key='xref_deps_v2';");
                        Check(depsV2.find("\nunverifiable\t") != std::string::npos
                                  && depsV2.find("\nresolved\t") == std::string::npos,
                              "CAD-R3-04: VersionVerified=false 在 xref_deps_v2 中落为 unverifiable");
                        sqlite3_close(db);
                    }
                }
                ExportDbOptions uOpts = opts;
                uOpts.OutputDbPath = dbUnverifP;
                ExportDbResult uRes{};
                Check(RunExportDbCommand(uOpts, uRes, &err) && !uRes.Reused && uRes.Counts.BlockEntities == 5,
                      "CAD-R3-04: unverifiable 缓存严禁复用，自动重建成功");
            }

            // 5c. 旧协议 xref_deps_v1 缓存库：--reuse-if-fresh 自动失效并升级重建为 xref_deps_v2
            {
                sqlite3* db = nullptr;
                Check(sqlite3_open_v2(dbP.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
                      "CAD-R3-04: 以读写模式打开缓存库模拟旧版 xref_deps_v1");
                if (db)
                {
                    const int rc = sqlite3_exec(
                        db,
                        "UPDATE meta SET key='xref_deps_v1', value='v1:' || substr(value, 4) WHERE key='xref_deps_v2';",
                        nullptr, nullptr, nullptr);
                    Check(rc == SQLITE_OK, "CAD-R3-04: 将缓存库降级为仅含旧键 xref_deps_v1");
                    sqlite3_close(db);
                }

                ExportDbResult upgraded{};
                Check(RunExportDbCommand(opts, upgraded, &err) && !upgraded.Reused,
                      "CAD-R3-04: 仅含旧 xref_deps_v1 的缓存自动失效并重建");
                {
                    sqlite3* dbCheck = nullptr;
                    Check(sqlite3_open_v2(dbP.c_str(), &dbCheck, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
                          "CAD-R3-04: 打开升级后的缓存库");
                    if (dbCheck)
                    {
                        const std::string v2Val = SqlText(dbCheck, "SELECT value FROM meta WHERE key='xref_deps_v2';");
                        const int v1Cnt         = SqlInt(dbCheck, "SELECT COUNT(*) FROM meta WHERE key='xref_deps_v1';");
                        Check(v2Val.rfind("v2:", 0) == 0 && v1Cnt == 0,
                              "CAD-R3-04: 重建后仅保留 xref_deps_v2 (v2:) 且不含旧键 xref_deps_v1");
                        sqlite3_close(dbCheck);
                    }
                }
            }
        }
    }

    // ── CAD-22：输出路径与输入/多输出冲突拦截（绝不覆盖原 DWG） ─────────────────
    void TestOutputPathConflictGuard(const std::string& workDir)
    {
        const std::string dwgA = workDir + "/guard_a.dwg";
        const std::string dwgB = workDir + "/guard_b.dwg";
        Check(BuildGeneralSampleDwg(dwgA, 0.0, 0.0, false)
                  && BuildGeneralSampleDwg(dwgB, 0.0, 0.0, true),
              "CAD-22: 生成路径冲突测试原图");

        auto readBytes = [](const std::string& p) -> std::string {
            std::ifstream f(MiniCAD::Utf8ToPath(p), std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        const std::string origA = readBytes(dwgA);
        const std::string origB = readBytes(dwgB);

        // 1. export-db 输出与输入同文件（直接路径 + 大小写/相对路径别名）
        {
            ExportDbOptions eo;
            eo.InputPath    = dwgA;
            eo.OutputDbPath = dwgA;
            ExportDbResult er;
            std::string err, code;
            bool usageErr = false;
            Check(!RunExportDbCommand(eo, er, &err, &code, &usageErr)
                      && code == "OUTPUT_PATH_CONFLICT" && usageErr,
                  "CAD-22: export-db 拒绝输出路径等于输入 DWG");
            Check(readBytes(dwgA) == origA, "CAD-22: export-db 冲突拦截后原 DWG 字节完好");

#ifdef _WIN32
            eo.OutputDbPath = workDir + "/./GUARD_A.DWG";
            code.clear(); usageErr = false;
            Check(!RunExportDbCommand(eo, er, &err, &code, &usageErr)
                      && code == "OUTPUT_PATH_CONFLICT" && usageErr,
                  "CAD-22: export-db 识别 Windows 大小写/相对路径别名冲突");
            Check(readBytes(dwgA) == origA, "CAD-22: Windows 别名冲突拦截后原 DWG 完好");
#endif
        }

        // 2. render 输出指向输入或 --compare 图纸
        {
            RenderOptions ro;
            ro.InputPath     = dwgA;
            ro.ComparePath   = dwgB;
            ro.OutputPngPath = dwgB;
#ifdef MINICAD_TEST_FONTS_DIR
            ro.FontsDir      = MINICAD_TEST_FONTS_DIR;
#endif
            RenderResult rr;
            std::string code, msg;
            bool usageErr = false;
            Check(!RunRenderCommand(ro, rr, &code, &msg, &usageErr)
                      && code == "OUTPUT_PATH_CONFLICT" && usageErr,
                  "CAD-22: render --compare 拒绝输出路径覆盖 --compare 原 DWG");
            Check(readBytes(dwgB) == origB, "CAD-22: render 冲突拦截后原 DWG 完好");
        }

        // 3. diff 输出指向输入，或默认派生 JSON 与 --out-db 冲突
        {
            DiffOptions dop;
            dop.InputPathA = dwgA;
            dop.InputPathB = dwgB;
            dop.OutPngPath = dwgA;
#ifdef MINICAD_TEST_FONTS_DIR
            dop.FontsDir   = MINICAD_TEST_FONTS_DIR;
#endif
            DiffReport rep;
            std::string code, msg;
            bool usageErr = false;
            Check(!RunDiffCommand(dop, rep, &code, &msg, &usageErr)
                      && code == "OUTPUT_PATH_CONFLICT" && usageErr,
                  "CAD-22: diff 拒绝 --out-png 覆盖输入原图");
            Check(readBytes(dwgA) == origA, "CAD-22: diff 冲突拦截后原 DWG 完好");

            // 多输出冲突：--out-png auto_out.png 自动派生 auto_out.json，与 --out-db 同文件
            DiffOptions dop2;
            dop2.InputPathA = dwgA;
            dop2.InputPathB = dwgB;
            dop2.OutPngPath = workDir + "/auto_out.png";
#ifdef _WIN32
            dop2.OutDbPath  = workDir + "/AUTO_OUT.JSON";
#else
            dop2.OutDbPath  = workDir + "/auto_out.json";
#endif
#ifdef MINICAD_TEST_FONTS_DIR
            dop2.FontsDir   = MINICAD_TEST_FONTS_DIR;
#endif
            code.clear(); usageErr = false;
            Check(!RunDiffCommand(dop2, rep, &code, &msg, &usageErr)
                      && code == "OUTPUT_PATH_CONFLICT" && usageErr,
                  "CAD-22: diff 拦截默认派生 JSON 与 --out-db 的多输出同路径冲突");
        }
    }

    // ── CAD-23：带孔/带岛 Hatch 净面积（奇偶包含规则） ───────────────────────────
    void TestHatchNetAreaExport(const std::string& workDir)
    {
        auto makeRect = [](double x0, double y0, double x1, double y1) -> MiniCAD::Polyline {
            MiniCAD::Polyline pl;
            pl.Points = {
                { x0, y0, 0.0 }, { x1, y0, 0.0 }, { x1, y1, 0.0 }, { x0, y1, 0.0 }, { x0, y0, 0.0 }
            };
            return pl;
        };

        MiniCAD::Scene scene;
        // Hatch 1：外环 10x10=100，减去 5x2=10 洞口 => 净面积 90
        {
            std::vector<MiniCAD::Polyline> loops;
            loops.push_back(makeRect(0.0, 0.0, 10.0, 10.0));
            loops.push_back(makeRect(2.0, 2.0, 7.0, 4.0));
            scene.AddEntity(std::make_unique<MiniCAD::HatchEntity>(
                scene.NextObjectID(), std::move(loops), MiniCAD::HatchPattern::MakeSolid()));
        }
        // Hatch 2：外环 10x10=100，减 8x8=64 洞口，加回洞内 2x2=4 岛 => 净面积 40
        {
            std::vector<MiniCAD::Polyline> loops;
            loops.push_back(makeRect(20.0, 0.0, 30.0, 10.0));
            loops.push_back(makeRect(21.0, 1.0, 29.0, 9.0));
            loops.push_back(makeRect(24.0, 4.0, 26.0, 6.0));
            scene.AddEntity(std::make_unique<MiniCAD::HatchEntity>(
                scene.NextObjectID(), std::move(loops), MiniCAD::HatchPattern::MakeSolid()));
        }
        // Hatch 3（CAD-R2-12）：大测绘坐标 (4e8, 3.5e9) 下 50×50 带 10×10 孔洞 => 净面积 2400
        const double ox = 4.0e8, oy = 3.5e9;
        {
            std::vector<MiniCAD::Polyline> loops;
            loops.push_back(makeRect(ox, oy, ox + 50.0, oy + 50.0));
            loops.push_back(makeRect(ox + 20.0, oy + 20.0, ox + 30.0, oy + 30.0));
            scene.AddEntity(std::make_unique<MiniCAD::HatchEntity>(
                scene.NextObjectID(), std::move(loops), MiniCAD::HatchPattern::MakeSolid()));
        }
        // Hatch 4（CAD-R2-12）：大测绘坐标下部分相交的两个 10×10 方形（重叠 5×5=25）=> 奇偶净面积 150
        {
            std::vector<MiniCAD::Polyline> loops;
            loops.push_back(makeRect(ox + 100.0, oy, ox + 110.0, oy + 10.0));
            loops.push_back(makeRect(ox + 105.0, oy + 5.0, ox + 115.0, oy + 15.0));
            scene.AddEntity(std::make_unique<MiniCAD::HatchEntity>(
                scene.NextObjectID(), std::move(loops), MiniCAD::HatchPattern::MakeSolid()));
        }

        MiniCAD::CadSemanticSnapshot snap;
        snap.Format = "DWG";
        const std::string dbPath = workDir + "/hatch_net_area.sqlite";
        ExportDbResult res;
        std::string err;
        Check(ExportSceneToSqlite(scene, snap, workDir + "/hatch_src.dwg", 100, 1000, dbPath, true, res, &err),
              "CAD-23: 带孔/带岛 Hatch 导出 SQLite 成功");

        sqlite3* db = nullptr;
        Check(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
              "CAD-23: 打开 Hatch SQLite");
        if (db)
        {
            const double area1 = SqlDouble(db, "SELECT area FROM entities WHERE entity_type='HATCH' ORDER BY id LIMIT 1 OFFSET 0;");
            const double area2 = SqlDouble(db, "SELECT area FROM entities WHERE entity_type='HATCH' ORDER BY id LIMIT 1 OFFSET 1;");
            const double area3 = SqlDouble(db, "SELECT area FROM entities WHERE entity_type='HATCH' ORDER BY id LIMIT 1 OFFSET 2;");
            const double area4 = SqlDouble(db, "SELECT area FROM entities WHERE entity_type='HATCH' ORDER BY id LIMIT 1 OFFSET 3;");
            Check(Near(area1, 90.0, 1e-3), "CAD-23: 外环 100 减洞口 10 = 净面积 90");
            Check(Near(area2, 40.0, 1e-3), "CAD-23: 外环 100 减洞口 64 加洞内岛 4 = 净面积 40");
            Check(Near(area3, 2400.0, 1e-4), "CAD-R2-12: SQLite 导出大坐标 (4e8, 3.5e9) 50×50 带 10×10 孔洞精确面积 2400");
            Check(Near(area4, 150.0, 1e-4), "CAD-R2-12: SQLite 导出大坐标相交环奇偶净面积 150");
            sqlite3_close(db);
        }
    }

    // ── CAD-26：Win64 中文 / UNC 宽字符参数安全转 UTF-8 ──────────────────────────
    void TestWin64WideArgToUtf8()
    {
#ifdef _WIN32
        std::string out;
        unsigned long winErr = 0;

        Check(WideArgToUtf8(L"", out, &winErr) && out.empty() && winErr == 0,
              "CAD-26: WideArgToUtf8 空字符串正确返回");
        Check(WideArgToUtf8(L"export-db", out, &winErr) && out == "export-db",
              "CAD-26: WideArgToUtf8 ASCII 参数无尾部 NUL 误差");
        Check(WideArgToUtf8(L"\\\\daga-nas5\\\u9879\u76ee\\\u5efa\u7b51\\A-01\u5e73\u9762\u56fe.dwg", out, &winErr)
                  && out == "\\\\daga-nas5\\项目\\建筑\\A-01平面图.dwg",
              "CAD-26: WideArgToUtf8 精确转换中文与 canonical UNC 路径");

        // 孤立代理项应被 WC_ERR_INVALID_CHARS 拒绝
        const std::wstring invalidUtf16(1, static_cast<wchar_t>(0xD800));
        winErr = 0;
        Check(!WideArgToUtf8(invalidUtf16, out, &winErr) && winErr != 0,
              "CAD-26: WideArgToUtf8 拒绝非法孤立代理项并回报 Win32 错误码");
#endif
    }

    // ── CAD-34：无效 --fonts-dir 显式报错与 export-db 选项清理 ───────────────────
    void TestStrictFontsDirValidation(const std::string& workDir)
    {
        ParsedCommand cmd;
        std::string code, msg;
        Check(!ParseCliArgs({ "export-db", "a.dwg", "o.sqlite", "--fonts-dir", "fonts" }, cmd, code, msg)
                  && code == "UNKNOWN_OPTION",
              "CAD-34: export-db 不再静默吞掉无效的 --fonts-dir 参数");

        const std::string dwgA = workDir + "/fonts_test_a.dwg";
        Check(BuildGeneralSampleDwg(dwgA), "CAD-34: 生成字体校验测试图");

        const std::string badFonts = workDir + "/nonexistent_fonts_dir_xyz";

        // render 无效 --fonts-dir
        {
            RenderOptions ro;
            ro.InputPath        = dwgA;
            ro.OutputPngPath    = workDir + "/fonts_fail.png";
            ro.FontsDir         = badFonts;
            ro.FontsDirExplicit = true;
            RenderResult rr;
            bool usageErr = false;
            Check(!RunRenderCommand(ro, rr, &code, &msg, &usageErr)
                      && code == "INVALID_FONTS_DIR" && usageErr,
                  "CAD-34: render 显式传入不存在 --fonts-dir 时立即报 INVALID_FONTS_DIR");
        }

        // render 显式空 --fonts-dir ""
        {
            RenderOptions ro;
            ro.InputPath        = dwgA;
            ro.OutputPngPath    = workDir + "/fonts_empty_fail.png";
            ro.FontsDir         = "";
            ro.FontsDirExplicit = true;
            RenderResult rr;
            bool usageErr = false;
            Check(!RunRenderCommand(ro, rr, &code, &msg, &usageErr)
                      && code == "INVALID_FONTS_DIR" && usageErr,
                  "CAD-34: render 显式传入空 --fonts-dir 时立即报 INVALID_FONTS_DIR");
        }

        // diff 无效 --fonts-dir
        {
            DiffOptions dop;
            dop.InputPathA       = dwgA;
            dop.InputPathB       = dwgA;
            dop.FontsDir         = badFonts;
            dop.FontsDirExplicit = true;
            DiffReport rep;
            bool usageErr = false;
            Check(!RunDiffCommand(dop, rep, &code, &msg, &usageErr)
                      && code == "INVALID_FONTS_DIR" && usageErr,
                  "CAD-34: diff 显式传入不存在 --fonts-dir 时立即报 INVALID_FONTS_DIR");
        }
    }

    int RunAllTests(const std::vector<std::string>& args)
    {
        LOG_SET_LEVEL(MiniCAD::LogLevel::Error);

        // 可选导出真实天正合成样图到指定目录，供 Release SaDwgTool.exe CLI 端到端实测
        if (args.size() >= 2 && args[0] == "--emit-tch-fixtures")
        {
            const std::string& outDir = args[1];
            const bool okA = BuildTianzhengSampleDwg(outDir + "/tch_before.dwg", 0.0, 0.0, false);
            const bool okB = BuildTianzhengSampleDwg(outDir + "/tch_after.dwg", -1500.0, 800.0, true);
            std::printf("emit tch fixtures: %s\n", (okA && okB) ? "OK" : "FAIL");
            return (okA && okB) ? 0 : 1;
        }

        // CAD-25：使用 u8string() 避免中文用户目录下 fs::path::string() 退化为系统代码页
        const fs::path tmpRoot = fs::temp_directory_path()
            / ("sadwgtool_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::create_directories(tmpRoot, ec);
        const std::string workDir = PathToUtf8(tmpRoot);

        TestCliArgs();
        TestExportDbAndReuse(workDir);
        TestTianzhengExportAndDiff(workDir);
        TestRenderAndLayerAutoAlign(workDir);
        TestPaperSpaceDiffDefaults(workDir);
        TestWipeoutAndLongDashDrawOrder(workDir);
        TestUnionDefaultBboxAndEmptySide(workDir);
        TestPaperLayoutCoverageDisclosure(workDir);
        TestZeroToleranceAndEffectiveReporting(workDir);
        TestTchEmptySideAndWallHeightDiff(workDir);
        TestDimensionPlaceholderOverride(workDir);
        TestWriteDiffSqliteRollbackAndCleanup(workDir);
        TestExportCacheInvalidationOptionsAndXrefs(workDir);
        TestOutputPathConflictGuard(workDir);
        TestHatchNetAreaExport(workDir);
        TestWin64WideArgToUtf8();
        TestStrictFontsDirValidation(workDir);

        fs::remove_all(tmpRoot, ec);

        std::printf("\nSaDwgToolTests 完成：%d 项失败\n", g_failed);
        return g_failed == 0 ? 0 : 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i)
    {
        std::string utf8;
        if (!WideArgToUtf8(argv[i], utf8))
        {
            std::fprintf(stderr, "无效命令行宽字符参数\n");
            return 2;
        }
        args.push_back(std::move(utf8));
    }
    return RunAllTests(args);
}
#else
int main(int argc, char** argv)
{
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return RunAllTests(args);
}
#endif
