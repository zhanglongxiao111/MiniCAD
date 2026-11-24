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
#include "Document/Command/LayoutCommands.h"
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

        // 7. CAD-R3-03：删除 LayoutA(*Paper_Space)、保留 LayoutB(*Paper_Space0)、新建 LayoutC 后原图保留写出：
        // 验证唯一块双射、LayoutB 原句柄与 *Paper_Space0 归属不变、LayoutC 零旧布局残留（含未建模旧对象）、TCH 字节级一致
        {
            CadDatabase db3;
            db3.SetVersion(CadVersion::AC1027);
            db3.CreateDefaults();
            db3.Classes.push_back(Class("TCH_WALL", "TDbWall", 500));
            db3.Classes.push_back(Class("TCH_COLUMN", "TDbColumn", 501));

            const Handle wallH3 = db3.AddEntity(db3.ModelSpace(), Wall())->ObjectHandle;
            const Handle colH3  = db3.AddEntity(db3.ModelSpace(), Column())->ObjectHandle;

            // LayoutA 绑定 *Paper_Space：放 1 条普通线 + 1 个未解码 TCH_COLUMN 幽灵实体（检验旧布局清理彻底）
            BlockRecord* psA = db3.PaperSpace();
            if (CadDictionary* ld = db3.FindNamedDictionary("ACAD_LAYOUT"))
            {
                for (size_t i = 0; i < ld->EntryNames.size() && i < ld->EntryHandles.size(); ++i)
                {
                    if (auto* lay = db3.FindAs<Layout>(ld->EntryHandles[i]);
                        lay && lay->AssociatedBlockHandle == psA->ObjectHandle)
                    {
                        lay->Name = "LayoutA";
                        ld->EntryNames[i] = "LayoutA";
                    }
                }
            }
            auto laLine = std::make_unique<Line>();
            laLine->StartPoint = { 1, 1, 0 }; laLine->EndPoint = { 11, 1, 0 };
            const Handle laLineH  = db3.AddEntity(psA, std::move(laLine))->ObjectHandle;
            const Handle laGhostH = db3.AddEntity(psA, Column())->ObjectHandle;

            // LayoutB 绑定 *Paper_Space0：放 1 条原图线
            BlockRecord* psB = db3.CreateBlockRecord("*Paper_Space0");
            auto layB3 = std::make_unique<Layout>();
            layB3->Name                  = "LayoutB";
            layB3->TabOrder              = 2;
            layB3->AssociatedBlockHandle = psB->ObjectHandle;
            layB3->MinExtents            = { 0, 0, 0 };
            layB3->MaxExtents            = { 420, 297, 0 };
            layB3->MinLimits             = { 0, 0 };
            layB3->MaxLimits             = { 420, 297 };
            if (CadDictionary* ld = db3.FindNamedDictionary("ACAD_LAYOUT"))
            {
                if (Layout* added = db3.AddDictionaryEntry(ld, "LayoutB", std::move(layB3)))
                    psB->LayoutHandle = added->ObjectHandle;
            }
            auto lbLine = std::make_unique<Line>();
            lbLine->StartPoint = { 20, 20, 0 }; lbLine->EndPoint = { 60, 20, 0 };
            const Handle lbOrigH = db3.AddEntity(psB, std::move(lbLine))->ObjectHandle;

            DwgWriteOptions wopt3;
            wopt3.Version = CadVersion::AC1027;
            const auto srcBytes3 = WriteDwg(db3, wopt3);
            const auto origDb3   = ReadDwg(srcBytes3, {});
            Check(!srcBytes3.empty() && origDb3 != nullptr, "CAD-R3-03: LayoutA+LayoutB+TCH fixture written");

            const auto path3 = std::filesystem::temp_directory_path() / "MiniCADPreserveLayoutBijection.dwg";
            { std::ofstream out(path3, std::ios::binary); out.write(reinterpret_cast<const char*>(srcBytes3.data()), srcBytes3.size()); }
            const auto u83 = path3.u8string();
            const std::string p3(u83.begin(), u83.end());
            MiniCAD::Document doc3;
            const bool loaded3 = doc3.LoadFromFile(p3);
            std::filesystem::remove(path3);
            Check(loaded3 && doc3.GetCadSource() != nullptr, "CAD-R3-03: Document::LoadFromFile retains CadSource");

            MiniCAD::Scene& sc3 = doc3.GetScene();
            uint64_t keyA = 0, keyB = 0;
            for (const auto& l : sc3.Layouts())
            {
                if (!l.Model && l.Name == "LayoutA") keyA = l.OwnerKey;
                if (!l.Model && l.Name == "LayoutB") keyB = l.OwnerKey;
            }
            Check(keyA == static_cast<uint64_t>(psA->ObjectHandle) && keyB == static_cast<uint64_t>(psB->ObjectHandle),
                  "CAD-R3-03: imported LayoutA/LayoutB OwnerKeys match *Paper_Space / *Paper_Space0");

            // 删除 LayoutA、保留 LayoutB、新建 LayoutC
            Check(doc3.GetCommandStack().Execute(std::make_unique<MiniCAD::RemoveLayoutCommand>(keyA), sc3),
                  "CAD-R3-03: RemoveLayoutCommand deletes LayoutA");
            MiniCAD::DrawingLayout layC;
            layC.Name  = "LayoutC";
            layC.Model = false;
            layC.Order = 2;
            layC.MinX  = 0.0; layC.MinY = 0.0; layC.MaxX = 420.0; layC.MaxY = 297.0;
            auto addC = std::make_unique<MiniCAD::AddLayoutCommand>(layC, 2);
            MiniCAD::AddLayoutCommand* addCRaw = addC.get();
            Check(doc3.GetCommandStack().Execute(std::move(addC), sc3),
                  "CAD-R3-03: AddLayoutCommand creates LayoutC");
            const uint64_t keyC = addCRaw->GetKey();
            Check(keyC != 0 && keyC != keyA && keyC != keyB, "CAD-R3-03: LayoutC gets fresh unique OwnerKey");

            // 分别在 LayoutB 与 LayoutC 新增图元
            auto newBLine = std::make_unique<MiniCAD::LineEntity>(
                sc3.NextObjectID(), MiniCAD::Math::Point3{ 20, 40, 0 }, MiniCAD::Math::Point3{ 80, 40, 0 });
            newBLine->GetAttr().InPaperSpace = true;
            newBLine->GetAttr().LayoutKey    = keyB;
            sc3.AddEntity(std::move(newBLine));

            auto newCLine = std::make_unique<MiniCAD::LineEntity>(
                sc3.NextObjectID(), MiniCAD::Math::Point3{ 30, 50, 0 }, MiniCAD::Math::Point3{ 90, 50, 0 });
            newCLine->GetAttr().InPaperSpace = true;
            newCLine->GetAttr().LayoutKey    = keyC;
            sc3.AddEntity(std::move(newCLine));

            // 切到 LayoutC 调 ExportMarkupDwg 走原图保留导出
            const int idxC = sc3.FindLayout(keyC);
            MiniCAD::CameraState cam3{};
            Check(idxC > 0 && doc3.SetActiveLayout(idxC, cam3), "CAD-R3-03: activate LayoutC");

            std::string err3;
            const auto outBytes3 = MiniCAD::SaMarkupExport::ExportMarkupDwg(
                doc3, { { 10, 10, 110, 90 } }, { "LAYC-NOTE" }, err3);
            const auto outDb3 = ReadDwg(outBytes3, {});
            Check(!outBytes3.empty() && err3.empty() && outDb3 != nullptr,
                  "CAD-R3-03: ExportMarkupDwg succeeds after delete LayoutA / keep LayoutB / add LayoutC");
            Check(outDb3 && SameRaw(*origDb3, *outDb3, wallH3) && SameRaw(*origDb3, *outDb3, colH3),
                  "CAD-R3-03: Model space TCH_WALL and TCH_COLUMN preserved bit-for-bit");

            // 检查 ACAD_LAYOUT 中 LayoutB 与 LayoutC 的 AssociatedBlockHandle 互异且无 LayoutA
            Handle assocB = kNullHandle, assocC = kNullHandle;
            bool foundA = false;
            if (outDb3)
            {
                for (const auto& [h, obj] : outDb3->Objects())
                {
                    if (const auto* lay = dynamic_cast<const Layout*>(obj.get()))
                    {
                        if (lay->Name == "LayoutA") foundA = true;
                        if (lay->Name == "LayoutB") assocB = lay->AssociatedBlockHandle;
                        if (lay->Name == "LayoutC") assocC = lay->AssociatedBlockHandle;
                    }
                }
            }
            Check(!foundA && assocB != kNullHandle && assocC != kNullHandle && assocB != assocC,
                  "CAD-R3-03: LayoutB and LayoutC have distinct AssociatedBlockHandles and LayoutA is gone");
            Check(assocB == psB->ObjectHandle,
                  "CAD-R3-03: LayoutB retains its original *Paper_Space0 block handle");

            // 检查 LayoutB 原图元句柄与归属不变，且仅含 LayoutB 的 2 条线（无 LayoutC 云线/线串页）
            const BlockRecord* outRecB = outDb3 ? outDb3->FindAs<BlockRecord>(assocB) : nullptr;
            const BlockRecord* outRecC = outDb3 ? outDb3->FindAs<BlockRecord>(assocC) : nullptr;
            const Line* outLbOrig = outDb3 ? outDb3->FindAs<Line>(lbOrigH) : nullptr;
            auto recContains = [](const BlockRecord* rec, Handle h) {
                return rec && std::find(rec->Entities.begin(), rec->Entities.end(), h) != rec->Entities.end();
            };
            Check(outLbOrig && outLbOrig->OwnerHandle == psB->ObjectHandle && recContains(outRecB, lbOrigH),
                  "CAD-R3-03: LayoutB original line keeps exact handle and *Paper_Space0 ownership");

            int bLines = 0, bTotal = 0;
            if (outRecB)
            {
                bTotal = static_cast<int>(outRecB->Entities.size());
                for (Handle h : outRecB->Entities)
                    bLines += outDb3->FindAs<Line>(h) != nullptr;
            }
            Check(bTotal == 2 && bLines == 2,
                  "CAD-R3-03: LayoutB block holds strictly its 2 lines with zero LayoutC leak");

            // 检查 LayoutC 独占另一纸空间块，零 LayoutA 残留（含旧线与未解码幽灵对象），且含新线+云线+文字
            Check(outDb3 && outDb3->Find(laLineH) == nullptr && outDb3->Find(laGhostH) == nullptr
                      && !recContains(outRecC, laLineH) && !recContains(outRecC, laGhostH),
                  "CAD-R3-03: deleted LayoutA entities and undisplayable ghost object are completely purged");

            MiniCAD::Scene reloaded3;
            Check(MiniCAD::ImportCad(outBytes3, reloaded3), "CAD-R3-03: exported DWG re-imports cleanly");
            uint64_t relBKey = 0, relCKey = 0;
            for (const auto& l : reloaded3.Layouts())
            {
                if (!l.Model && l.Name == "LayoutB") relBKey = l.OwnerKey;
                if (!l.Model && l.Name == "LayoutC") relCKey = l.OwnerKey;
            }
            int relBCount = 0, relCLineCount = 0, relCMarkupCloud = 0, relCMarkupText = 0;
            reloaded3.ForEachObject([&](const MiniCAD::Object& o) {
                if (!o.IsKindOf<MiniCAD::Entity>()) return;
                const auto& e = static_cast<const MiniCAD::Entity&>(o);
                if (!e.GetAttr().InPaperSpace) return;
                if (e.GetAttr().LayoutKey == relBKey)
                {
                    ++relBCount;
                }
                else if (e.GetAttr().LayoutKey == relCKey)
                {
                    if (e.IsKindOf<MiniCAD::LineEntity>()) ++relCLineCount;
                    if (e.IsKindOf<MiniCAD::PolylineEntity>()) ++relCMarkupCloud;
                    if (e.IsKindOf<MiniCAD::TextEntity>() && static_cast<const MiniCAD::TextEntity&>(e).GetText() == "LAYC-NOTE")
                        ++relCMarkupText;
                }
            });
            Check(relBKey != 0 && relCKey != 0 && relBKey != relCKey
                      && relBCount == 2 && relCLineCount == 1 && relCMarkupCloud == 1 && relCMarkupText == 1,
                  "CAD-R3-03: re-imported LayoutB (2 lines) and LayoutC (1 line + cloud + label) are strictly isolated");
        }
    }

    return g_failed;
}
