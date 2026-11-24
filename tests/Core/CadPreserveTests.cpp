// 另存为与原图相同的格式、版本时以原图为底：未修改的TCH对象与跳过的实体原样写回，改过的实体按场景重写
#include "Dwg/Read/DwgReader.h"
#include "Dwg/Write/DwgBitWriter.h"
#include "Dwg/Write/DwgWriter.h"
#include "Database/UnknownObjects.h"
#include "Document/Document.h"
#include "Import/CadExchange.h"
#include "Scene/Scene.h"
#include "Core/Entity/InsertEntity.hpp"
#include "Core/Entity/LineEntity.hpp"
#include "Core/Entity/PolylineEntity.hpp"
#include "Core/Entity/TextEntity.hpp"
#include "Document/SaMarkupExport.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace MiniDWG;

namespace
{
    int g_failed = 0;
    void Check(bool ok, const char* label)
    {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        g_failed += ok ? 0 : 1;
    }

    std::unique_ptr<UnknownEntity> Wall()
    {
        auto e = std::make_unique<UnknownEntity>();
        e->Raw.DxfName = "TCH_WALL";
        e->Raw.Dwg.Version = CadVersion::AC1027;
        DwgBitWriter g(CadVersion::AC1027, Codec::CodePage::Gbk);
        g.Write3RawDouble({1000, 2000, 0}); g.Write3RawDouble({5000, 2000, 0});
        g.WriteRawDouble(0); g.WriteRawLong(0);
        g.WriteRawDouble(120); g.WriteRawDouble(120); g.WriteRawDouble(3000);
        auto protectedData = g.Take();
        for (auto& byte : protectedData) byte ^= 0xeb;
        DwgBitWriter r(CadVersion::AC1027, Codec::CodePage::Gbk);
        r.WriteByte(5); r.WriteBitDouble(100); r.WriteBitShort(0); r.WriteByte(3);
        r.Write2Bits(0); r.WriteRawShort(512); r.WriteByte(8); r.WriteByte(0xeb);
        r.WriteBytes(protectedData);
        r.WriteByte(0); r.WriteByte(0x8a); r.WriteByte(0); r.Write2Bits(0);
        r.WriteBitDouble(80.08);
        for (double join : {10.0, 20.0, -10.0, -20.0}) r.WriteBitDouble(join);
        r.WriteByte(0); r.Write3Bits(2);
        e->Raw.Dwg.MainBits = r.PositionInBits(); e->Raw.Dwg.Main = r.Take();
        return e;
    }

    // 无法显示的TCH对象（私有数据未知、没有代理图形）：导入时跳过
    std::unique_ptr<UnknownEntity> Column()
    {
        auto e = std::make_unique<UnknownEntity>();
        e->Raw.DxfName = "TCH_COLUMN";
        e->Raw.Dwg.Version = CadVersion::AC1027;
        e->Raw.Dwg.Main = {0x12, 0x34, 0x56, 0x78, 0x9a};
        e->Raw.Dwg.MainBits = 37;
        return e;
    }

    DxfClass Class(const char* dxf, const char* cpp, std::int16_t number)
    {
        DxfClass c;
        c.DxfName = dxf; c.CppClassName = cpp; c.ApplicationName = "TCH_KERNAL";
        c.ClassNumber = number; c.IsAnEntity = true; c.ItemClassId = 0x1f2;
        return c;
    }

    struct Fixture
    {
        std::vector<std::uint8_t> Bytes;
        Handle WallHandle = kNullHandle, ColumnHandle = kNullHandle, LineHandle = kNullHandle;
    };

    Fixture MakeDrawing()
    {
        CadDatabase db;
        db.SetVersion(CadVersion::AC1027);
        db.CreateDefaults();
        db.Classes.push_back(Class("TCH_WALL", "TDbWall", 500));
        db.Classes.push_back(Class("TCH_COLUMN", "TDbColumn", 501));

        auto layer = std::make_unique<Layer>();
        layer->Name = "WALL";
        layer->Color = Color(std::int16_t(3));
        layer->PlotFlag = false;    // MiniCAD 不保存的属性
        Layer* wallLayer = db.AddTableEntry(db.Layers(), std::move(layer));

        Fixture f;
        auto wall = Wall();
        wall->LayerHandle = wallLayer->ObjectHandle;
        f.WallHandle = db.AddEntity(db.ModelSpace(), std::move(wall))->ObjectHandle;
        f.ColumnHandle = db.AddEntity(db.ModelSpace(), Column())->ObjectHandle;
        auto line = std::make_unique<Line>();
        line->StartPoint = {0, 0, 0};
        line->EndPoint = {100, 0, 0};
        f.LineHandle = db.AddEntity(db.ModelSpace(), std::move(line))->ObjectHandle;

        DwgWriteOptions options;
        options.Version = CadVersion::AC1027;
        f.Bytes = WriteDwg(db, options);
        return f;
    }

    bool SameRaw(const CadDatabase& a, const CadDatabase& b, Handle h)
    {
        const RawObjectData* ra = a.Find(h) ? RawDataOf(*a.Find(h)) : nullptr;
        const RawObjectData* rb = b.Find(h) ? RawDataOf(*b.Find(h)) : nullptr;
        return ra && rb && ra->DxfName == rb->DxfName && ra->Dwg.Main == rb->Dwg.Main && ra->Dwg.MainBits == rb->Dwg.MainBits
            && ra->Dwg.Handles == rb->Dwg.Handles;
    }

    int CountTch(const CadDatabase& db)
    {
        int n = 0;
        for (Handle h : db.ModelSpace()->Entities)
            if (const CadObject* o = db.Find(h); o && RawDataOf(*o) && RawDataOf(*o)->DxfName.starts_with("TCH_"))
                ++n;
        return n;
    }

    template <class T>
    T* FindScene(MiniCAD::Scene& scene)
    {
        T* found = nullptr;
        scene.ForEachObject([&](const MiniCAD::Object& o)
        {
            if (!found)
                if (auto* t = dynamic_cast<const T*>(&o))
                    found = const_cast<T*>(t);
        });
        return found;
    }

    struct Opened
    {
        MiniCAD::Scene Scene;
        std::shared_ptr<const MiniCAD::CadSource> Source;
    };

    std::unique_ptr<Opened> Open(const Fixture& f)
    {
        auto o = std::make_unique<Opened>();
        MiniCAD::CadSaveVersion version{};
        if (!MiniCAD::ImportCad(f.Bytes, o->Scene, nullptr, &version, &o->Source) || version != MiniCAD::CadSaveVersion::R2013)
            return nullptr;
        return o;
    }

    std::unique_ptr<CadDatabase> Save(const Opened& o, MiniCAD::CadSaveVersion version = MiniCAD::CadSaveVersion::R2013)
    {
        const auto bytes = MiniCAD::ExportCad(o.Scene, MiniCAD::CadFileKind::Dwg, version, nullptr, o.Source.get());
        return bytes.empty() ? nullptr : ReadDwg(bytes, {});
    }
}

int main()
{
    const Fixture f = MakeDrawing();
    const auto original = ReadDwg(f.Bytes, {});
    Check(!f.Bytes.empty() && original != nullptr, "fixture drawing is written and readable");
    if (!original)
        return 1;

    // 1. 不修改直接另存
    {
        auto o = Open(f);
        Check(o != nullptr && o->Scene.EntityCount() == 2 && MiniCAD::CadSourceTchCount(o->Source.get()) == 2,
              "import shows wall and line, records both Tianzheng objects");
        const auto saved = o ? Save(*o) : nullptr;
        Check(saved && SameRaw(*original, *saved, f.WallHandle), "unmodified displayed TCH_WALL is written back bit for bit");
        Check(saved && SameRaw(*original, *saved, f.ColumnHandle), "undisplayable TCH_COLUMN is preserved");
        Check(saved && saved->FindAs<Line>(f.LineHandle) != nullptr, "unmodified line keeps its handle");
        const auto* layer = saved ? saved->FindTableEntry<Layer>(saved->Layers(), "WALL") : nullptr;
        Check(layer && !layer->Color.IsTrueColor() && layer->Color.Index() == 3 && !layer->PlotFlag,
              "unchanged layer keeps color index and properties MiniCAD does not model");
        bool tchBlock = false;
        if (saved)
            for (const auto& [h, obj] : saved->Objects())
                if (const auto* rec = dynamic_cast<const BlockRecord*>(obj.get()); rec && rec->Name.starts_with("*U_TCH_"))
                    tchBlock = true;
        Check(!tchBlock, "display block generated for the wall is not written when the wall is preserved");
    }

    // 2. 修改直线、删除墙、新增直线
    {
        auto o = Open(f);
        auto* line = o ? FindScene<MiniCAD::LineEntity>(o->Scene) : nullptr;
        auto* wall = o ? FindScene<MiniCAD::InsertEntity>(o->Scene) : nullptr;
        if (line && wall)
        {
            line->SetLine(MiniCAD::Line({0, 0, 0}, {0, 50, 0}));
            o->Scene.RemoveEntity(wall->GetID());
            o->Scene.AddEntity(std::make_unique<MiniCAD::LineEntity>(o->Scene.NextObjectID(),
                MiniCAD::Math::Point3{10, 10, 0}, MiniCAD::Math::Point3{20, 20, 0}));
        }
        const auto saved = o ? Save(*o) : nullptr;
        const auto* edited = saved ? saved->FindAs<Line>(f.LineHandle) : nullptr;
        Check(edited && edited->EndPoint.Y == 50 && edited->EndPoint.X == 0, "edited line is rewritten in place with its original handle");
        Check(saved && saved->Find(f.WallHandle) == nullptr, "deleted wall is removed");
        Check(saved && SameRaw(*original, *saved, f.ColumnHandle), "skipped column survives unrelated edits");
        int lines = 0;
        if (saved)
            for (Handle h : saved->ModelSpace()->Entities)
                lines += saved->FindAs<Line>(h) != nullptr;
        Check(lines == 2, "new line is appended to model space");
    }

    // 3. 移动墙：原对象的位流不再对应场景，写为普通几何
    {
        auto o = Open(f);
        auto* wall = o ? FindScene<MiniCAD::InsertEntity>(o->Scene) : nullptr;
        if (wall)
            wall->SetPosition({500, 0, 0});
        const auto saved = o ? Save(*o) : nullptr;
        Check(saved && saved->Find(f.WallHandle) == nullptr && CountTch(*saved) == 1,
              "moved wall no longer writes the stale TCH payload");
        MiniCAD::Scene restored;
        const auto bytes = o ? MiniCAD::ExportCad(o->Scene, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2013, nullptr, o->Source.get())
                             : std::vector<std::uint8_t>{};
        Check(MiniCAD::ImportCad(bytes, restored) && restored.EntityCount() == 2, "moved wall reopens as plain geometry");
    }

    // 4. 换版本：自定义对象不能写回，重新生成整张图纸
    {
        auto o = Open(f);
        const auto saved = o ? Save(*o, MiniCAD::CadSaveVersion::R2018) : nullptr;
        Check(saved && CountTch(*saved) == 0 && saved->ModelSpace()->Entities.size() == 2,
              "saving as another version writes only the displayed geometry");
    }

    // 5. 经 Document 打开再保存
    {
        const auto path = std::filesystem::temp_directory_path() / "MiniCADPreserve.dwg";
        { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(f.Bytes.data()), f.Bytes.size()); }
        const auto u8 = path.u8string();
        const std::string p(u8.begin(), u8.end());
        MiniCAD::Document doc;
        const bool ok = doc.LoadFromFile(p) && doc.SaveToFile(p);
        std::ifstream in(path, std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
        in.close();
        std::filesystem::remove(path);
        const auto saved = ReadDwg(bytes, {});
        Check(ok && saved && SameRaw(*original, *saved, f.WallHandle) && SameRaw(*original, *saved, f.ColumnHandle),
              "Document open and save keeps Tianzheng objects");
    }

    // 6. CAD-R2-02：多纸张布局原图保留写出（PatchDrawingSpaces）—— 纸张编辑/删除/新增/跨空间迁移 + 批注落目标布局
    {
        CadDatabase db;
        db.SetVersion(CadVersion::AC1027);
        db.CreateDefaults();
        db.Classes.push_back(Class("TCH_WALL", "TDbWall", 500));
        db.Classes.push_back(Class("TCH_COLUMN", "TDbColumn", 501));

        const Handle wallH   = db.AddEntity(db.ModelSpace(), Wall())->ObjectHandle;
        const Handle colH    = db.AddEntity(db.ModelSpace(), Column())->ObjectHandle;
        auto mLine = std::make_unique<Line>();
        mLine->StartPoint = { 0, 0, 0 }; mLine->EndPoint = { 100, 0, 0 };
        const Handle modelLineH = db.AddEntity(db.ModelSpace(), std::move(mLine))->ObjectHandle;

        BlockRecord* paperA = db.PaperSpace();
        auto paLine1 = std::make_unique<Line>();
        paLine1->StartPoint = { 10, 10, 0 }; paLine1->EndPoint = { 50, 10, 0 };
        const Handle pa1H = db.AddEntity(paperA, std::move(paLine1))->ObjectHandle;
        auto paLine2 = std::make_unique<Line>();
        paLine2->StartPoint = { 10, 20, 0 }; paLine2->EndPoint = { 50, 20, 0 };
        const Handle pa2H = db.AddEntity(paperA, std::move(paLine2))->ObjectHandle;

        BlockRecord* paperB = db.CreateBlockRecord("*Paper_Space0");
        auto layB = std::make_unique<Layout>();
        layB->Name                  = "LayoutB";
        layB->TabOrder              = 2;
        layB->AssociatedBlockHandle = paperB->ObjectHandle;
        layB->MinExtents            = { 0, 0, 0 };
        layB->MaxExtents            = { 420, 297, 0 };
        layB->MinLimits             = { 0, 0 };
        layB->MaxLimits             = { 420, 297 };
        if (CadDictionary* ld = db.FindNamedDictionary("ACAD_LAYOUT"))
        {
            if (Layout* added = db.AddDictionaryEntry(ld, "LayoutB", std::move(layB)))
                paperB->LayoutHandle = added->ObjectHandle;
        }
        auto pbLine1 = std::make_unique<Line>();
        pbLine1->StartPoint = { 5, 5, 0 }; pbLine1->EndPoint = { 30, 5, 0 };
        const Handle pb1H = db.AddEntity(paperB, std::move(pbLine1))->ObjectHandle;

        DwgWriteOptions wopt;
        wopt.Version = CadVersion::AC1027;
        const auto multiBytes = WriteDwg(db, wopt);
        const auto origMulti  = ReadDwg(multiBytes, {});
        Check(!multiBytes.empty() && origMulti != nullptr, "CAD-R2-02: multi-layout DWG with TCH objects is written");

        // 6a. 原图保留下同时执行：修改 LayoutB 已有线、删除 Layout1 线、向 LayoutB 新增线、将模型线迁移到 LayoutB
        {
            MiniCAD::Scene sc;
            MiniCAD::CadSaveVersion ver{};
            std::shared_ptr<const MiniCAD::CadSource> src;
            Check(MiniCAD::ImportCad(multiBytes, sc, nullptr, &ver, &src) && src != nullptr && ver == MiniCAD::CadSaveVersion::R2013,
                  "CAD-R2-02: multi-layout DWG imports with CadSource");

            uint64_t layoutBKey = 0;
            for (const auto& l : sc.Layouts())
                if (!l.Model && l.Name == "LayoutB")
                    layoutBKey = l.OwnerKey;
            Check(layoutBKey == static_cast<uint64_t>(paperB->ObjectHandle), "CAD-R2-02: LayoutB OwnerKey matches *Paper_Space0 handle");

            MiniCAD::Object::ObjectID deletePa2Id = MiniCAD::Object::InvalidID;
            sc.ForEachObject([&](const MiniCAD::Object& obj) {
                if (!obj.IsKindOf<MiniCAD::LineEntity>()) return;
                const auto h = static_cast<Handle>(MiniCAD::LookupCadEntityHandle(src.get(), obj.GetID()));
                auto* ln = const_cast<MiniCAD::LineEntity*>(static_cast<const MiniCAD::LineEntity*>(&obj));
                if (h == pb1H)
                {
                    ln->SetLine(MiniCAD::Line({ 5, 5, 0 }, { 88, 5, 0 }));
                }
                else if (h == pa2H)
                {
                    deletePa2Id = obj.GetID();
                }
                else if (h == modelLineH)
                {
                    MiniCAD::EntityAttr attr = ln->GetAttr();
                    attr.InPaperSpace = true;
                    attr.LayoutKey    = layoutBKey;
                    ln->SetAttr(attr);
                }
            });
            if (deletePa2Id != MiniCAD::Object::InvalidID)
                sc.RemoveEntity(deletePa2Id);

            auto newPaperLine = std::make_unique<MiniCAD::LineEntity>(
                sc.NextObjectID(), MiniCAD::Math::Point3{ 100, 100, 0 }, MiniCAD::Math::Point3{ 200, 100, 0 });
            newPaperLine->GetAttr().InPaperSpace = true;
            newPaperLine->GetAttr().LayoutKey    = layoutBKey;
            sc.AddEntity(std::move(newPaperLine));

            const auto savedBytes = MiniCAD::ExportCad(sc, MiniCAD::CadFileKind::Dwg, MiniCAD::CadSaveVersion::R2013, nullptr, src.get());
            const auto saved = ReadDwg(savedBytes, {});
            Check(saved != nullptr && SameRaw(*origMulti, *saved, wallH) && SameRaw(*origMulti, *saved, colH),
                  "CAD-R2-02: PatchDrawingSpaces preserves TCH objects bit-for-bit");

            const BlockRecord* savedModel  = saved ? saved->ModelSpace() : nullptr;
            const BlockRecord* savedPaperA = saved ? saved->FindAs<BlockRecord>(paperA->ObjectHandle) : nullptr;
            const BlockRecord* savedPaperB = saved ? saved->FindAs<BlockRecord>(paperB->ObjectHandle) : nullptr;
            auto hasHandle = [](const BlockRecord* rec, Handle h) {
                return rec && std::find(rec->Entities.begin(), rec->Entities.end(), h) != rec->Entities.end();
            };

            const Line* savedPb1 = saved ? saved->FindAs<Line>(pb1H) : nullptr;
            Check(savedPb1 && savedPb1->OwnerHandle == paperB->ObjectHandle && savedPb1->EndPoint.X == 88.0
                      && hasHandle(savedPaperB, pb1H),
                  "CAD-R2-02: edited LayoutB line rewrites in place keeping handle and *Paper_Space0 owner");
            Check(saved && saved->Find(pa1H) != nullptr && hasHandle(savedPaperA, pa1H),
                  "CAD-R2-02: untouched Layout1 line keeps handle and owner");
            Check(saved && saved->Find(pa2H) == nullptr && !hasHandle(savedPaperA, pa2H),
                  "CAD-R2-02: deleted Layout1 line is removed from db and *Paper_Space handle list");
            Check(saved && !hasHandle(savedModel, modelLineH),
                  "CAD-R2-02: line migrated from Model to LayoutB is erased from *Model_Space handle list");

            int modelLines = 0, paperBLines = 0;
            if (savedModel)
                for (Handle h : savedModel->Entities)
                    modelLines += saved->FindAs<Line>(h) != nullptr;
            if (savedPaperB)
                for (Handle h : savedPaperB->Entities)
                    paperBLines += saved->FindAs<Line>(h) != nullptr;
            Check(modelLines == 0 && paperBLines == 3,
                  "CAD-R2-02: migrated and newly appended LayoutB lines all land in *Paper_Space0 without leaking into Model");
        }

        // 6b. 真实生产路径：Document::LoadFromFile 携带非空 CadSource，切至 LayoutB 调 ExportMarkupDwg
        {
            const auto path = std::filesystem::temp_directory_path() / "MiniCADPreserveMultiLayoutMarkup.dwg";
            { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(multiBytes.data()), multiBytes.size()); }
            const auto u8 = path.u8string();
            const std::string p(u8.begin(), u8.end());
            MiniCAD::Document doc;
            const bool loadedOk = doc.LoadFromFile(p);
            std::filesystem::remove(path);

            int layoutBIdx = -1;
            for (size_t i = 0; i < doc.GetScene().Layouts().size(); ++i)
                if (!doc.GetScene().Layouts()[i].Model && doc.GetScene().Layouts()[i].Name == "LayoutB")
                    layoutBIdx = static_cast<int>(i);
            MiniCAD::CameraState cam{};
            Check(loadedOk && doc.GetCadSource() != nullptr && layoutBIdx > 0 && doc.SetActiveLayout(layoutBIdx, cam),
                  "CAD-R2-02: Document::LoadFromFile retains CadSource and activates LayoutB");

            std::string err;
            const auto outBytes = MiniCAD::SaMarkupExport::ExportMarkupDwg(doc, { { 15, 25, 115, 85 } }, { "TCH-P2" }, err);
            const auto outDb = ReadDwg(outBytes, {});
            Check(!outBytes.empty() && err.empty() && outDb != nullptr
                      && SameRaw(*origMulti, *outDb, wallH) && SameRaw(*origMulti, *outDb, colH),
                  "CAD-R2-02: ExportMarkupDwg on opened DWG preserves TCH objects");

            MiniCAD::Scene reloaded;
            Check(MiniCAD::ImportCad(outBytes, reloaded), "CAD-R2-02: exported markup DWG re-imports cleanly");
            uint64_t reloadedBKey = 0;
            for (const auto& l : reloaded.Layouts())
                if (!l.Model && l.Name == "LayoutB")
                    reloadedBKey = l.OwnerKey;

            bool cloudInB = false, textInB = false, leakToModel = false;
            reloaded.ForEachObject([&](const MiniCAD::Object& o) {
                if (!o.IsKindOf<MiniCAD::Entity>()) return;
                const auto& e = static_cast<const MiniCAD::Entity&>(o);
                const MiniCAD::Layer* lyr = reloaded.GetLayerManager().GetLayer(e.GetLayerID());
                if (!lyr || lyr->GetName() != MiniCAD::SaMarkupExport::kMarkupLayerName) return;
                if (!e.GetAttr().InPaperSpace) { leakToModel = true; return; }
                if (e.GetAttr().LayoutKey == reloadedBKey)
                {
                    if (e.IsKindOf<MiniCAD::PolylineEntity>()) cloudInB = true;
                    if (e.IsKindOf<MiniCAD::TextEntity>() && static_cast<const MiniCAD::TextEntity&>(e).GetText() == "TCH-P2")
                        textInB = true;
                }
            });
            Check(cloudInB && textInB && !leakToModel,
                  "CAD-R2-02: markup cloud & label land in LayoutB with zero leak into Model space");
        }
    }

    return g_failed;
}
