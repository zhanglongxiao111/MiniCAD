// ── 嵌入宿主生命周期与渲染/拾取真实路径回归测试（CAD-R2-03/04/05/06/10 + HasMarkupChanges）──
// 直连静态库 MiniCADApp，实例化真实 MainFrame + 队列化 TestAppPlatform，不绕开任何生产状态机。
#include "GUI/MainFrame.h"
#include "GUI/AppPlatform.h"
#include "Core/UIContext.h"
#include "Render/IRenderBackend.h"
#include "Render/IRenderer.h"
#include "Render/IRenderTarget.h"
#include "Document/Document.h"
#include "Document/SaMarkupExport.h"
#include "Document/SaTextExport.h"
#include "Document/Command/AddEntityCommand.h"
#include "Document/Command/BatchDeleteCommand.h"
#include "Core/Entity/AttribEntity.hpp"
#include "Core/Entity/BlockEntity.hpp"
#include "Core/Entity/InsertEntity.hpp"
#include "Core/Entity/LineEntity.hpp"
#include "Core/Entity/PointEntity.hpp"
#include "Core/Entity/CircleEntity.hpp"
#include "Core/Entity/TextEntity.hpp"
#include "Editor/Editor.h"
#include "Editor/Tools/ITool.h"
#include "Viewport/Viewport.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace MiniCAD;

namespace
{
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    // 临时目录 RAII（存放真实图纸文件供 MainFrame::EmbedOpenMainDrawing / EmbedOpenCompareDrawing 读取）
    struct TempDir
    {
        std::filesystem::path dir;
        TempDir()
        {
            static std::atomic<uint64_t> seq{ 0 };
            dir = std::filesystem::temp_directory_path()
                / ("minicad_embed_host_test_" + std::to_string(++seq));
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
        std::string WriteDoc(const std::string& fileName, const Document& doc) const
        {
            const std::filesystem::path p = dir / fileName;
            std::ofstream out(p, std::ios::binary);
            const std::string text = doc.SaveToString(false);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.close();
            const std::u8string u8 = p.u8string();
            return std::string(u8.begin(), u8.end());
        }
    };

    class NullUiBackend final : public MiniGUI::IRenderBackend
    {
    public:
        MiniGUI::TextureId CreateTexture(int, int, MiniGUI::TextureFormat, const void*) override { return m_next++; }
        void UpdateTexture(MiniGUI::TextureId, const MiniGUI::RectI&, const void*, int) override {}
        void DestroyTexture(MiniGUI::TextureId) override {}
        void Render(const MiniGUI::DrawData&) override {}
    private:
        MiniGUI::TextureId m_next = 1;
    };

    // 模拟 WebGPU 纹理视图指针地址复用（ABA）的跟踪型 RenderTarget，同时记录单调递增代次
    class TrackingRenderTarget final : public IRenderTarget
    {
    public:
        explicit TrackingRenderTarget(void* fixedAliasPtr = nullptr)
            : m_aliasPtr(fixedAliasPtr) {}
        ~TrackingRenderTarget() override { Release(); }

        void Create(int w, int h) override { Resize(w, h); }
        void Resize(int w, int h) override
        {
            w = std::max(w, 1);
            h = std::max(h, 1);
            if (m_generation != 0 && w == m_width && h == m_height)
                return;
            m_width      = w;
            m_height     = h;
            m_generation = s_nextGen.fetch_add(1, std::memory_order_relaxed) + 1;
            m_handle     = m_aliasPtr ? m_aliasPtr : reinterpret_cast<void*>(static_cast<uintptr_t>(m_generation));
        }
        void Release() override
        {
            m_width      = 0;
            m_height     = 0;
            m_generation = 0;
            m_handle     = nullptr;
        }

        int      GetWidth()  const override { return m_width; }
        int      GetHeight() const override { return m_height; }
        void*    GetNativeHandle()         const override { return m_handle; }
        void*    GetNativeShaderResource() const override { return m_handle; }
        uint64_t GetGeneration() const { return m_generation; }

        static void ResetGlobalGenerationForTest() { s_nextGen.store(0, std::memory_order_relaxed); }

    private:
        static inline std::atomic<uint64_t> s_nextGen{ 0 };
        void*    m_aliasPtr   = nullptr;
        void*    m_handle     = nullptr;
        int      m_width      = 0;
        int      m_height     = 0;
        uint64_t m_generation = 0;
    };

    // 跟踪型 IRenderer：记录选中流提交、合成代次缓存重建（含模拟 ABA 指针复用）以及 ReleaseCompareResources 调用
    class TrackingRenderer final : public IRenderer
    {
    public:
        static constexpr uint32_t kSlotSelFill = 3;
        static constexpr uint32_t kSlotSel     = 4;
        static constexpr uint32_t kSlotGripFill = 15;
        static constexpr uint32_t kSlotGrip     = 16;

        void BeginFrame(IRenderTarget&, const ViewportDesc&) override {}
        void EndFrame() override {}
        void Submit(std::span<const Vertex_P3_C4>, const Math::Mat4&, PrimitiveType, bool, bool) override {}
        void SubmitTextured(std::span<const Vertex_P3_C4_UV>, const Math::Mat4&, void*, bool, bool) override {}
        void* GetNativeDevice() override { return nullptr; }

        void SubmitCached(uint32_t slot, uint64_t, std::span<const Vertex_P3_C4> verts,
                          const Math::Mat4&, PrimitiveType, bool, bool) override
        {
            if (slot == kSlotSel || slot == kSlotSelFill)
                submittedSelVerts += verts.size();
            if (slot == kSlotGrip || slot == kSlotGripFill)
                submittedGripVerts += verts.size();
        }

        std::unique_ptr<IRenderTarget> CreateLayerTarget() override
        {
            ++createdLayerTargets;
            // 对对比底片刻意复用固定伪指针地址，验证按非零代次缓存时仍必重建 bind group（根除指针 ABA）
            void* fixedPtr = nullptr;
            if (reuseLayerPointerSlot == 0)
                fixedPtr = reinterpret_cast<void*>(static_cast<uintptr_t>(0x10001));
            else if (reuseLayerPointerSlot == 1)
                fixedPtr = reinterpret_cast<void*>(static_cast<uintptr_t>(0x20002));
            else
                fixedPtr = reinterpret_cast<void*>(static_cast<uintptr_t>(0x30003));
            reuseLayerPointerSlot = (reuseLayerPointerSlot + 1) % 3;
            return std::make_unique<TrackingRenderTarget>(fixedPtr);
        }

        bool CompositeCompareLayers(IRenderTarget& mainLayer, IRenderTarget& compareLayer,
                                    const CompareCompositeStyle&) override
        {
            auto& m = static_cast<TrackingRenderTarget&>(mainLayer);
            auto& c = static_cast<TrackingRenderTarget&>(compareLayer);
            const uint64_t mg = m.GetGeneration();
            const uint64_t cg = c.GetGeneration();
            if (mg == 0 || cg == 0)
                return false;
            ++compositeCalls;
            lastMainGen = mg;
            lastCompGen = cg;
            if (!hasBindGroup || cachedMainGen != mg || cachedCompGen != cg)
            {
                hasBindGroup  = true;
                cachedMainGen = mg;
                cachedCompGen = cg;
                ++bindGroupRebuilds;
            }
            return true;
        }

        void ReleaseCompareResources() override
        {
            ++releaseCompareResourcesCalls;
            hasBindGroup  = false;
            cachedMainGen = 0;
            cachedCompGen = 0;
        }

        void ResetOverlayStats()
        {
            submittedSelVerts  = 0;
            submittedGripVerts = 0;
        }

        int      createdLayerTargets          = 0;
        int      reuseLayerPointerSlot        = 0;
        int      compositeCalls               = 0;
        int      bindGroupRebuilds            = 0;
        int      releaseCompareResourcesCalls = 0;
        bool     hasBindGroup                 = false;
        uint64_t cachedMainGen                = 0;
        uint64_t cachedCompGen                = 0;
        uint64_t lastMainGen                  = 0;
        uint64_t lastCompGen                  = 0;
        size_t   submittedSelVerts            = 0;
        size_t   submittedGripVerts           = 0;
    };

    // 队列化测试平台：Post 与 RunInBackground 显式入队，由测试精确推进异步时序
    class TestAppPlatform final : public AppPlatform
    {
    public:
        TestAppPlatform()
        {
            m_viewportTarget.Create(800, 600);
        }

        MiniGUI::IRenderBackend& GetUiBackend() override { return m_uiBackend; }
        IRenderer&               GetRenderer() override { return m_renderer; }
        IRenderTarget&           GetViewportTarget() override { return m_viewportTarget; }
        MiniGUI::TextureId RegisterViewportTexture(void*) override { return m_nextTex++; }
        bool LoadUiFonts(MiniGUI::TextSystem&) override { return true; }
        void RequestRedraw() override { ++redrawRequests; }
        void BeginUiPass() override {}
        void EndUiPass() override {}
        const char* GetGraphicsName() const override { return "TestNull"; }

        void SetTitle(const std::string& utf8) override { lastTitle = utf8; }
        bool HasWindowControls() const override { return false; }
        void CloseWindow() override {}
        void Quit() override {}

        void PickFile(FileKind, std::function<void(const std::string&)>) override {}
        std::string ChooseSavePath(const std::string&, CadSaveVersion&) override { return {}; }
        std::string ChooseExportPath(FileKind, const std::string&) override { return {}; }
        std::string GetResourceDir() const override { return MINICAD_ASSET_DIR; }
        std::string GetUserDataDir() const override { return {}; }

        void RunInBackground(std::function<void()> work, std::function<void()> done) override
        {
            m_bgQueue.emplace_back(std::move(work), std::move(done));
        }

        void Post(std::function<void()> fn) override
        {
            m_posted.push_back(std::move(fn));
        }

        size_t PendingPostedCount() const { return m_posted.size(); }

        size_t DrainPosted()
        {
            size_t ran = 0;
            while (!m_posted.empty())
            {
                auto fn = std::move(m_posted.front());
                m_posted.pop_front();
                if (fn)
                    fn();
                ++ran;
            }
            return ran;
        }

        size_t DrainBackground()
        {
            size_t ran = 0;
            while (!m_bgQueue.empty())
            {
                auto [work, done] = std::move(m_bgQueue.front());
                m_bgQueue.pop_front();
                if (work) work();
                if (done) done();
                ++ran;
            }
            return ran;
        }

        // 驱动 MainFrame 完成所有排队中的后台打开与渐进细分
        void PumpOpen(MainFrame& frame, int maxSteps = 20)
        {
            for (int i = 0; i < maxSteps; ++i)
            {
                const size_t bg = DrainBackground();
                frame.RenderFrame();
                const size_t ps = DrainPosted();
                if (bg == 0 && ps == 0 && !frame.IsOpeningDrawings() && !frame.EmbedIsOpeningCompare())
                    break;
            }
        }

        TrackingRenderer& Renderer() { return m_renderer; }

        int         redrawRequests = 0;
        std::string lastTitle;

    private:
        NullUiBackend        m_uiBackend;
        TrackingRenderer     m_renderer;
        TrackingRenderTarget m_viewportTarget;
        MiniGUI::TextureId   m_nextTex = 100;
        std::deque<std::function<void()>> m_posted;
        std::deque<std::pair<std::function<void()>, std::function<void()>>> m_bgQueue;
    };

    bool InitEmbedFrame(TestAppPlatform& platform, MainFrame& frame)
    {
        frame.SetUseUserLayout(false);
        if (!frame.Initialize(platform, 800.0f, 600.0f, 1.0f))
            return false;
        frame.EmbedSetMode(0);
        frame.RenderFrame();
        platform.DrainPosted();
        return true;
    }

    // 构造一份含一条水平基线的简单图纸用于打开
    std::unique_ptr<Document> MakeSimpleDoc(double y0 = 0.0)
    {
        auto doc = std::make_unique<Document>();
        Scene& s = doc->GetScene();
        s.AddEntity(std::make_unique<LineEntity>(
            s.NextObjectID(), Math::Point3{ 0.0, y0, 0.0 }, Math::Point3{ 100.0, y0, 0.0 }));
        doc->MarkSaved();
        return doc;
    }

    // 模拟在 Editor 屏幕坐标 (sx, sy) 执行一次完整左键点击（禁用悬停时直走 HitTest）
    void ClickAt(Editor& ed, int sx, int sy)
    {
        InputEvent move{};
        move.Type = InputEventType::MouseMove;
        move.MouseX = move.LastMouseX = sx;
        move.MouseY = move.LastMouseY = sy;
        ed.OnInput(move);

        InputEvent down{};
        down.Type = InputEventType::MouseButtonDown;
        down.Button = MouseButton::Left;
        down.MouseButtons = static_cast<uint8_t>(MouseButtonState::Left);
        down.MouseX = down.LastMouseX = down.PressMouseX = sx;
        down.MouseY = down.LastMouseY = down.PressMouseY = sy;
        ed.OnInput(down);

        InputEvent up{};
        up.Type = InputEventType::MouseButtonUp;
        up.Button = MouseButton::Left;
        up.MouseButtons = 0;
        up.MouseX = up.LastMouseX = up.PressMouseX = sx;
        up.MouseY = up.LastMouseY = up.PressMouseY = sy;
        ed.OnInput(up);
    }

    // 模拟屏幕框选拖拽（x0 > x1 为交叉框选）
    void BoxSelectAt(Editor& ed, int x0, int y0, int x1, int y1)
    {
        InputEvent down{};
        down.Type = InputEventType::MouseButtonDown;
        down.Button = MouseButton::Left;
        down.MouseButtons = static_cast<uint8_t>(MouseButtonState::Left);
        down.MouseX = down.LastMouseX = down.PressMouseX = x0;
        down.MouseY = down.LastMouseY = down.PressMouseY = y0;
        ed.OnInput(down);

        InputEvent drag{};
        drag.Type = InputEventType::MouseMove;
        drag.MouseButtons = static_cast<uint8_t>(MouseButtonState::Left);
        drag.MouseX = x1; drag.MouseY = y1;
        drag.LastMouseX = x0; drag.LastMouseY = y0;
        drag.PressMouseX = x0; drag.PressMouseY = y0;
        ed.OnInput(drag);

        InputEvent up{};
        up.Type = InputEventType::MouseButtonUp;
        up.Button = MouseButton::Left;
        up.MouseButtons = 0;
        up.MouseX = x1; up.MouseY = y1;
        up.LastMouseX = x1; up.LastMouseY = y1;
        up.PressMouseX = x0; up.PressMouseY = y0;
        ed.OnInput(up);
    }

    // =========================================================
    // 1. CAD-R2-03：显式取消拾点必异步回调一次；顶替/关图静默作废
    // =========================================================
    void TestPickPointLifecycle()
    {
        TempDir tmp;
        const std::string mainPath = tmp.WriteDoc("main.json", *MakeSimpleDoc(0.0));
        const std::string compPath = tmp.WriteDoc("comp.json", *MakeSimpleDoc(10.0));

        TestAppPlatform platform;
        MainFrame frame;
        Check(InitEmbedFrame(platform, frame), "CAD-R2-03: MainFrame 初始化成功");

        frame.EmbedOpenMainDrawing(mainPath);
        frame.EmbedOpenCompareDrawing(compPath);
        platform.PumpOpen(frame);
        Check(frame.GetDocumentManager().GetMainDocument() != nullptr && frame.EmbedHasCompareDocument(),
              "CAD-R2-03: 主图与对比图打开完成");

        std::vector<int> pickedSeqs;
        std::vector<int> cancelledSeqs;
        auto onPicked    = [&](int seq, double, double) { pickedSeqs.push_back(seq); };
        auto onCancelled = [&](int seq) { cancelledSeqs.push_back(seq); };

        // 1a. 主图显式取消：必须异步派发恰好一次 onCancelled(1)
        Check(frame.EmbedStartPickPoint(1, 0, onPicked, onCancelled), "CAD-R2-03: 启动主图拾点 seq=1");
        frame.EmbedCancelPickPoint();
        Check(cancelledSeqs.empty(), "CAD-R2-03: EmbedCancelPickPoint 保持异步派发（同步期间未触发）");
        platform.DrainPosted();
        Check(cancelledSeqs.size() == 1 && cancelledSeqs[0] == 1 && pickedSeqs.empty(),
              "CAD-R2-03: 排空队列后准确收到 1 次 onCancelled(1)");

        // 重复取消不产生二次回调
        frame.EmbedCancelPickPoint();
        platform.DrainPosted();
        Check(cancelledSeqs.size() == 1, "CAD-R2-03: 重复取消不重复派发 onCancelled");

        // 1b. 对比图（target=1）显式取消：异步派发恰好一次 onCancelled(11)
        Check(frame.EmbedStartPickPoint(11, 1, onPicked, onCancelled), "CAD-R2-03: 启动底图拾点 seq=11");
        frame.EmbedCancelPickPoint();
        Check(cancelledSeqs.size() == 1, "CAD-R2-03: 底图显式取消保持异步派发");
        platform.DrainPosted();
        Check(cancelledSeqs.size() == 2 && cancelledSeqs[1] == 11,
              "CAD-R2-03: 底图显式取消准确收到 1 次 onCancelled(11)");

        // 1c. 顶替旧请求：EmbedStartPickPoint(2) 后紧接 EmbedStartPickPoint(3)，2 静默作废，3 正常拾点并恢复捕捉状态
        Editor& mainEd = frame.GetDocumentManager().GetMainEditor();
        mainEd.SetSnapEnabled(false);
        Check(frame.EmbedStartPickPoint(2, 0, onPicked, onCancelled), "CAD-R2-03: 启动主图拾点 seq=2");
        Check(mainEd.IsSnapEnabled(), "CAD-R2-03: 拾点期间自动临时开启捕捉");
        Check(frame.EmbedStartPickPoint(3, 0, onPicked, onCancelled), "CAD-R2-03: seq=3 顶替 seq=2");
        platform.DrainPosted();
        Check(cancelledSeqs.size() == 2, "CAD-R2-03: 被顶替的 seq=2 静默作废，不触发 onCancelled(2)");

        // 左键拾点完成 seq=3
        ClickAt(mainEd, 200, 200);
        Check(pickedSeqs.empty(), "CAD-R2-03: 拾点完成回调也保持异步派发");
        platform.DrainPosted();
        Check(pickedSeqs.size() == 1 && pickedSeqs[0] == 3 && cancelledSeqs.size() == 2,
              "CAD-R2-03: seq=3 正常拾点回调且无多余取消通知");
        Check(!mainEd.IsSnapEnabled(), "CAD-R2-03: seq=3 拾点结束后准确恢复原捕捉关闭状态");

        // 1d. 关闭全部图纸与关闭对比图时均静默作废进行中的拾点会话
        Check(frame.EmbedStartPickPoint(4, 0, onPicked, onCancelled), "CAD-R2-03: 启动主图拾点 seq=4");
        frame.EmbedCloseAllDocuments();
        platform.DrainPosted();
        Check(cancelledSeqs.size() == 2, "CAD-R2-03: EmbedCloseAllDocuments 静默作废 seq=4");

        frame.EmbedOpenMainDrawing(mainPath);
        platform.PumpOpen(frame);
        Check(frame.EmbedStartPickPoint(5, 1, onPicked, onCancelled), "CAD-R2-03: 启动底图拾点 seq=5");
        frame.EmbedCloseCompareDrawing();
        platform.DrainPosted();
        Check(cancelledSeqs.size() == 2, "CAD-R2-03: EmbedCloseCompareDrawing 静默作废 seq=5");

        // 1e. CAD-R3-06：已 settled 入 Post 队列尚未派发时，同图启动新拾点按代次拦截旧通知
        Editor& reopenedMainEd = frame.GetDocumentManager().GetMainEditor();
        pickedSeqs.clear();
        cancelledSeqs.clear();
        platform.DrainPosted();
        Check(frame.EmbedStartPickPoint(21, 0, onPicked, onCancelled), "CAD-R3-06: 启动主图拾点 seq=21");
        ClickAt(reopenedMainEd, 200, 200); // seq=21 已 settled，回调在 Post 队列待执行
        Check(pickedSeqs.empty() && platform.PendingPostedCount() > 0 && !reopenedMainEd.IsActiveTool(),
              "CAD-R3-06: seq=21 已 settled 且回调暂存于 Post 队列尚未派发");
        Check(frame.EmbedStartPickPoint(22, 0, onPicked, onCancelled), "CAD-R3-06: 派发前立即在同图启动新拾点 seq=22");
        platform.DrainPosted();
        Check(pickedSeqs.empty() && cancelledSeqs.empty(),
              "CAD-R3-06: 队列中已 settled 的过期 seq=21 通知被代次校验拦截（零误派发）");
        ClickAt(reopenedMainEd, 220, 220);
        platform.DrainPosted();
        Check(pickedSeqs.size() == 1 && pickedSeqs[0] == 22 && cancelledSeqs.empty(),
              "CAD-R3-06: 新拾点 seq=22 正常完成并准确派发");

        // 显式取消已入队但派发前被新拾点顶替时，旧取消通知同样被代次拦截
        pickedSeqs.clear();
        cancelledSeqs.clear();
        Check(frame.EmbedStartPickPoint(23, 0, onPicked, onCancelled), "CAD-R3-06: 启动主图拾点 seq=23");
        frame.EmbedCancelPickPoint(); // onCancelled(23) 已入 Post 队列
        Check(cancelledSeqs.empty() && platform.PendingPostedCount() > 0,
              "CAD-R3-06: seq=23 显式取消已入 Post 队列尚未派发");
        Check(frame.EmbedStartPickPoint(24, 0, onPicked, onCancelled), "CAD-R3-06: 取消通知派发前启动新拾点 seq=24");
        platform.DrainPosted();
        Check(cancelledSeqs.empty() && pickedSeqs.empty(),
              "CAD-R3-06: 已入队的旧取消通知 seq=23 被新拾点代次拦截");
        ClickAt(reopenedMainEd, 240, 240);
        platform.DrainPosted();
        Check(pickedSeqs.size() == 1 && pickedSeqs[0] == 24 && cancelledSeqs.empty(),
              "CAD-R3-06: seq=24 正常完成触发");
    }

    // =========================================================
    // 2. CAD-R2-04：MainDocumentIntentVersion 在关键意图变更时单调递增
    // =========================================================
    void TestMainDocumentIntentVersion()
    {
        TempDir tmp;
        const std::string docPath = tmp.WriteDoc("intent.json", *MakeSimpleDoc(0.0));

        TestAppPlatform platform;
        MainFrame frame;
        Check(InitEmbedFrame(platform, frame), "CAD-R2-04: MainFrame 初始化成功");

        const uint64_t v0 = frame.MainDocumentIntentVersion();
        frame.CancelPendingMainOpens();
        const uint64_t v1 = frame.MainDocumentIntentVersion();
        Check(v1 > v0, "CAD-R2-04: CancelPendingMainOpens 推进 MainDocumentIntentVersion");

        frame.OpenDrawings({ docPath });
        const uint64_t v2 = frame.MainDocumentIntentVersion();
        Check(v2 > v1, "CAD-R2-04: OpenDrawings 推进 MainDocumentIntentVersion");
        platform.PumpOpen(frame);

        // 脱离嵌入门禁后执行 file.new 验证新建文档也推进意图代次
        TestAppPlatform deskPlatform;
        MainFrame deskFrame;
        deskFrame.SetUseUserLayout(false);
        Check(deskFrame.Initialize(deskPlatform, 800.0f, 600.0f, 1.0f), "CAD-R2-04: 桌面模式 MainFrame 初始化成功");
        const uint64_t d0 = deskFrame.MainDocumentIntentVersion();
        deskFrame.GetCommands().Execute("file.new");
        const uint64_t d1 = deskFrame.MainDocumentIntentVersion();
        Check(d1 > d0, "CAD-R2-04: 新建文档（file.new）推进 MainDocumentIntentVersion");

        frame.EmbedCloseAllDocuments();
        const uint64_t v3 = frame.MainDocumentIntentVersion();
        Check(v3 > v2, "CAD-R2-04: EmbedCloseAllDocuments 推进 MainDocumentIntentVersion");
    }

    // =========================================================
    // 3. CAD-R2-05：Blend 隐藏主图不可高亮/选中；Split 边缘按可见几何裁剪拾取
    // =========================================================
    void TestCompareVisibilityAndSplitPicking()
    {
        TempDir tmp;
        const std::string mainPath = tmp.WriteDoc("vis_main.json", *MakeSimpleDoc(0.0));
        const std::string compPath = tmp.WriteDoc("vis_comp.json", *MakeSimpleDoc(200.0));

        TestAppPlatform platform;
        MainFrame frame;
        Check(InitEmbedFrame(platform, frame), "CAD-R2-05: MainFrame 初始化成功");

        frame.EmbedOpenMainDrawing(mainPath);
        frame.EmbedOpenCompareDrawing(compPath);
        platform.PumpOpen(frame);

        Document* mainDoc = frame.GetDocumentManager().GetMainDocument();
        Check(mainDoc != nullptr && frame.EmbedHasCompareDocument(), "CAD-R2-05: 主/副图均已就绪");
        if (!mainDoc)
            return;

        // 建立一对一可预测的视口与相机：先渲染一帧让视口尺寸落定，再按真实屏幕坐标放置测试实体
        frame.RenderFrame();
        Viewport& vp = frame.GetDocumentManager().GetMainViewport();
        const int pw = static_cast<int>(vp.GetWidth());
        const int ph = static_cast<int>(vp.GetHeight());
        Check(pw > 100 && ph > 100, "CAD-R2-05: 主视口尺寸有效");
        const int sx = pw / 2;

        Camera& cam = vp.GetCamera();
        cam.ZoomToBounds(0.0, 0.0, static_cast<double>(pw), static_cast<double>(ph), 1.0);
        auto S2W = [&](double screenX, double screenY) -> Math::Point3
        {
            Math::Point3 w = cam.ScreenToWorld(screenX, screenY);
            w.z = 0.0;
            return w;
        };

        Scene& scene = mainDoc->GetScene();
        // 清理初始线，构造精确受控的 6 个几何实体
        for (Object::ObjectID id : scene.GetAllIDs())
            scene.RemoveEntity(id);

        const Object::ObjectID idHiddenLine = scene.NextObjectID();
        scene.AddEntity(std::make_unique<LineEntity>(idHiddenLine, S2W(sx + 2.0, 80.0), S2W(sx + 2.0, 140.0)));

        const Object::ObjectID idCrossingLine = scene.NextObjectID();
        scene.AddEntity(std::make_unique<LineEntity>(idCrossingLine, S2W(sx - 40.0, 190.0), S2W(sx + 40.0, 190.0)));

        const Object::ObjectID idHiddenPoint = scene.NextObjectID();
        scene.AddEntity(std::make_unique<PointEntity>(idHiddenPoint, S2W(sx + 2.0, 240.0)));

        const Object::ObjectID idHiddenCircle = scene.NextObjectID();
        const Math::Point3 cCenter = S2W(sx + 22.0, 300.0);
        const Math::Point3 cLeft   = S2W(sx + 2.0,  300.0);
        const double cRadius = std::abs(cCenter.x - cLeft.x);
        scene.AddEntity(std::make_unique<CircleEntity>(idHiddenCircle, cCenter, cRadius));

        const Object::ObjectID idHiddenText = scene.NextObjectID();
        const double worldPerPx = cam.GetWorldPerPixel();
        scene.AddEntity(std::make_unique<TextEntity>(
            idHiddenText, S2W(sx + 2.0, 360.0), "HIDDEN", static_cast<float>(16.0 * worldPerPx), 0.0f, scene.GetCurrentTextStyle()));

        const Object::ObjectID idVisibleLine = scene.NextObjectID();
        scene.AddEntity(std::make_unique<LineEntity>(idVisibleLine, S2W(sx - 80.0, 110.0), S2W(sx - 80.0, 160.0)));
        scene.MarkDirty();

        Editor& mainEd = frame.GetDocumentManager().GetMainEditor();
        mainEd.SetHoverEnabled(false);

        // 3a. Blend 下 showMain=false && overlapOpacity=0：主图完全不可见 → 覆盖层跳过、点选与框选均不命中
        frame.EmbedSetCompareState(
            static_cast<int>(CompareDisplayMode::Blend),
            1.0, 0.0, 0.0,
            /*overlapOpacity=*/0.0f, /*splitRatio=*/0.5f,
            /*soloCompare=*/false, /*showMain=*/false, /*showCompare=*/true, -1);

        const auto clipBlendHidden = Viewport::ComputeMainVisibleClip(
            CompareDisplayMode::Blend, false, false, 0.0f, sx, pw);
        Check(clipBlendHidden.IsHidden(), "CAD-R2-05: Blend(!showMain, overlap=0) 裁剪口径为 IsHidden()");

        // 强行预选可见线并渲染一帧，验证覆盖层选中流被跳过
        mainEd.SetSelection({ idVisibleLine });
        platform.Renderer().ResetOverlayStats();
        frame.RenderFrame();
        Check(platform.Renderer().submittedSelVerts == 0 && platform.Renderer().submittedGripVerts == 0,
              "CAD-R2-05: Blend 隐藏主图时不提交主图选中高亮与夹点覆盖层");

        mainEd.SetSelection({});
        ClickAt(mainEd, sx - 80, 135);
        Check(mainEd.GetSelection().empty(), "CAD-R2-05: Blend 隐藏主图时点选主图线不命中");
        BoxSelectAt(mainEd, sx - 20, 90, sx - 120, 180);
        Check(mainEd.GetSelection().empty(), "CAD-R2-05: Blend 隐藏主图时框选主图线不命中");

        // 3b. Split 模式：光标在 sx - 2（主图侧），5px 拾取孔径不能隔空命中 x >= sx + 2 的副图侧几何；跨分割线实体可命中
        frame.EmbedSetCompareState(
            static_cast<int>(CompareDisplayMode::Split),
            1.0, 0.0, 0.0,
            /*overlapOpacity=*/0.4f, /*splitRatio=*/0.5f,
            /*soloCompare=*/false, /*showMain=*/true, /*showCompare=*/true, -1);
        frame.RenderFrame();

        // 副图侧紧贴边界的线、点、圆、文字均不可命中
        mainEd.SetSelection({});
        ClickAt(mainEd, sx - 2, 110);
        Check(mainEd.GetSelection().empty(), "CAD-R2-05: Split 在 sx-2 点击不误命中位于 sx+2 的主图线段");

        ClickAt(mainEd, sx - 2, 240);
        Check(mainEd.GetSelection().empty(), "CAD-R2-05: Split 在 sx-2 点击不误命中位于 sx+2 的主图点");

        ClickAt(mainEd, sx - 2, 300);
        Check(mainEd.GetSelection().empty(), "CAD-R2-05: Split 在 sx-2 点击不误命中位于 sx+2 的主图圆");

        ClickAt(mainEd, sx - 2, 355);
        Check(mainEd.GetSelection().empty(), "CAD-R2-05: Split 在 sx-2 点击不误命中位于 sx+2 的主图文字");

        // 跨分割线线段在左侧可见段 (sx - 2, 190) 正常命中
        ClickAt(mainEd, sx - 2, 190);
        Check(mainEd.GetSelection().size() == 1 && mainEd.GetSelection().count(idCrossingLine) == 1,
              "CAD-R2-05: Split 在 sx-2 点击跨分割线主图线段可正常命中");

        // 确认完全可见侧的主图线正常命中
        mainEd.SetSelection({});
        ClickAt(mainEd, sx - 80, 135);
        Check(mainEd.GetSelection().size() == 1 && mainEd.GetSelection().count(idVisibleLine) == 1,
              "CAD-R2-05: Split 主图可见侧实体可正常命中");
    }

    // =========================================================
    // 4. CAD-R2-06：Blend -> Off -> Blend、关副图、关最后一张主图成对释放对比资源与递增代次
    // =========================================================
    void TestCompareResourceLifecycle()
    {
        TempDir tmp;
        const std::string mainPath = tmp.WriteDoc("gpu_main.json", *MakeSimpleDoc(0.0));
        const std::string compPath = tmp.WriteDoc("gpu_comp.json", *MakeSimpleDoc(20.0));

        TestAppPlatform platform;
        MainFrame frame;
        Check(InitEmbedFrame(platform, frame), "CAD-R2-06: MainFrame 初始化成功");

        frame.EmbedOpenMainDrawing(mainPath);
        frame.EmbedOpenCompareDrawing(compPath);
        platform.PumpOpen(frame);

        Viewport& mainVp = frame.GetDocumentManager().GetMainViewport();
        TrackingRenderer& r = platform.Renderer();

        // 进入 Blend 并渲染一帧
        frame.EmbedSetCompareState(
            static_cast<int>(CompareDisplayMode::Blend),
            1.0, 0.0, 0.0, 0.4f, 0.5f, false, true, true, -1);
        frame.RenderFrame();
        Check(mainVp.HasCompareLayers() && r.compositeCalls >= 1 && r.lastMainGen > 0 && r.lastCompGen > 0,
              "CAD-R2-06: Blend 首帧创建双底片并用非零代次合成");
        const uint64_t mainGen1  = r.lastMainGen;
        const uint64_t compGen1  = r.lastCompGen;
        const int      rebuilds1 = r.bindGroupRebuilds;

        // Blend -> Off：成对释放双底片与渲染器对比绑定资源
        const int relBeforeOff = r.releaseCompareResourcesCalls;
        frame.EmbedSetCompareState(
            static_cast<int>(CompareDisplayMode::Off),
            1.0, 0.0, 0.0, 0.4f, 0.5f, false, true, true, -1);
        Check(!mainVp.HasCompareLayers() && r.releaseCompareResourcesCalls > relBeforeOff,
              "CAD-R2-06: Blend -> Off 成对调用 ReleaseCompareLayers 与 ReleaseCompareResources");

        // Off -> Blend：即使模拟指针地址复用，代次单调递增仍强制重建合成绑定组
        frame.EmbedSetCompareState(
            static_cast<int>(CompareDisplayMode::Blend),
            1.0, 0.0, 0.0, 0.4f, 0.5f, false, true, true, -1);
        frame.RenderFrame();
        Check(mainVp.HasCompareLayers() && r.lastMainGen > mainGen1 && r.lastCompGen > compGen1,
              "CAD-R2-06: Off -> Blend 重建双底片后纹理代次严格单调递增");
        Check(r.bindGroupRebuilds > rebuilds1,
              "CAD-R2-06: 重建后即使伪指针地址复用也按代次重建合成绑定组");

        // 常规关闭最后一张主图（CloseDocument / file.close）：成对释放双底片与对比绑定资源
        // 注意：嵌入浏览态屏蔽了 file.close，这里走常规 CloseDocument（在桌面模式验证）
        {
            TestAppPlatform deskPlatform;
            MainFrame deskFrame;
            deskFrame.SetUseUserLayout(false);
            Check(deskFrame.Initialize(deskPlatform, 800.0f, 600.0f, 1.0f), "CAD-R2-06: 桌面模式初始化成功");
            deskFrame.OpenDrawings({ mainPath });
            deskFrame.EmbedOpenCompareDrawing(compPath);
            deskPlatform.PumpOpen(deskFrame);
            deskFrame.EmbedSetCompareState(
                static_cast<int>(CompareDisplayMode::Blend),
                1.0, 0.0, 0.0, 0.4f, 0.5f, false, true, true, -1);
            deskFrame.RenderFrame();
            Viewport& deskVp = deskFrame.GetDocumentManager().GetMainViewport();
            Check(deskVp.HasCompareLayers(), "CAD-R2-06: 桌面模式 Blend 已持有双底片");
            const int deskRelBefore = deskPlatform.Renderer().releaseCompareResourcesCalls;
            deskFrame.GetCommands().Execute("file.close");
            Check(deskFrame.GetDocumentManager().GetMainDocument() == nullptr
                  && !deskVp.HasCompareLayers()
                  && deskPlatform.Renderer().releaseCompareResourcesCalls > deskRelBefore,
                  "CAD-R2-06: 常规关闭最后一张主图时成对回收双底片与对比绑定资源");
        }

        // EmbedCloseAllDocuments：即使对比图仍在，关闭最后一张主图也回收双底片与对比绑定资源
        const int relBeforeCloseAll = r.releaseCompareResourcesCalls;
        frame.EmbedCloseAllDocuments();
        Check(!mainVp.HasCompareLayers() && r.releaseCompareResourcesCalls > relBeforeCloseAll,
              "CAD-R2-06: EmbedCloseAllDocuments 关闭全部主图时成对释放对比底片与对比绑定资源");

        // 重开主图再次 Blend，然后 EmbedCloseCompareDrawing 成对释放
        frame.EmbedOpenMainDrawing(mainPath);
        platform.PumpOpen(frame);
        frame.EmbedSetCompareState(
            static_cast<int>(CompareDisplayMode::Blend),
            1.0, 0.0, 0.0, 0.4f, 0.5f, false, true, true, -1);
        frame.RenderFrame();
        Check(mainVp.HasCompareLayers(), "CAD-R2-06: 重开主图后 Blend 重建双底片");
        const int relBeforeCloseComp = r.releaseCompareResourcesCalls;
        frame.EmbedCloseCompareDrawing();
        Check(!mainVp.HasCompareLayers() && r.releaseCompareResourcesCalls > relBeforeCloseComp,
              "CAD-R2-06: EmbedCloseCompareDrawing 成对释放双底片与对比绑定资源");
    }

    // =========================================================
    // 5. CAD-R2-10：Editor::ActivateTool / SetViewport / Unbind 取消回调重入与销毁安全
    // =========================================================
    class HookTool final : public ITool
    {
    public:
        std::string           tag;
        std::function<void()> onCancel;
        int*                  moveCounter = nullptr;

        HookTool(std::string t, std::function<void()> c, int* mc = nullptr)
            : tag(std::move(t)), onCancel(std::move(c)), moveCounter(mc) {}

        bool OnInput(const EditorContext& ctx) override
        {
            if (ctx.event.Type == InputEventType::MouseMove && moveCounter)
                ++(*moveCounter);
            return false;
        }

        void Cancel() override
        {
            auto cb = std::move(onCancel);
            if (OnFinished)
                OnFinished();
            if (cb)
                cb();
        }

        std::string GetPrompt() const override { return tag; }
    };

    void TestEditorToolCancelReentrancy()
    {
        // 5a. Cancel() 内重入 ActivateTool：新启动的 ToolC 必须保留，不被外层 ToolB 覆盖或补发无效 MouseMove
        {
            Document doc;
            Viewport vp(800.0f, 600.0f);
            Editor ed;
            ed.Bind(doc, vp);

            int toolBMoves = 0;
            int toolCMoves = 0;
            bool toolACancelled = false;

            ed.RegisterTool("TestToolC", [&]
            {
                return std::make_unique<HookTool>("ToolC", nullptr, &toolCMoves);
            });
            ed.RegisterTool("TestToolB", [&]
            {
                return std::make_unique<HookTool>("ToolB", nullptr, &toolBMoves);
            });
            ed.RegisterTool("TestToolA", [&]
            {
                return std::make_unique<HookTool>("ToolA", [&]
                {
                    toolACancelled = true;
                    ed.ActivateRegisteredTool("TestToolC");
                });
            });

            ed.ActivateRegisteredTool("TestToolA");
            ed.ActivateRegisteredTool("TestToolB");

            Check(toolACancelled, "CAD-R2-10: ToolA::Cancel 已执行");
            Check(ed.IsActiveTool() && ed.GetLastCommand() == "TestToolC",
                  "CAD-R2-10: Cancel 内重入激活的 ToolC 未被外层 ToolB 覆盖");
            Check(toolBMoves == 0 && toolCMoves >= 1,
                  "CAD-R2-10: 外层 ToolB 不再收到过期首帧预览 MouseMove，ToolC 正常收到");
        }

        // 5b. Cancel() 内销毁 Editor：ActivateTool / SetViewport / Unbind 均零 UAF
        auto runDestroyCase = [](const char* label, const std::function<void(std::unique_ptr<Editor>&, Viewport&)>& trigger)
        {
            Document doc;
            Viewport vp1(800.0f, 600.0f);
            Viewport vp2(640.0f, 480.0f);
            auto ed = std::make_unique<Editor>();
            ed->Bind(doc, vp1);
            bool destroyed = false;
            ed->RegisterTool("DestroySelf", [&]
            {
                return std::make_unique<HookTool>("DestroySelf", [&]
                {
                    destroyed = true;
                    ed.reset();
                });
            });
            ed->ActivateRegisteredTool("DestroySelf");
            trigger(ed, vp2);
            Check(destroyed && ed == nullptr, label);
        };

        runDestroyCase("CAD-R2-10: ActivateTool 取消回调内销毁 Editor 无 UAF",
            [](std::unique_ptr<Editor>& ed, Viewport&) { ed->ActivateRegisteredTool("Line"); });
        runDestroyCase("CAD-R2-10: SetViewport 取消回调内销毁 Editor 无 UAF",
            [](std::unique_ptr<Editor>& ed, Viewport& vp2) { ed->SetViewport(vp2); });
        runDestroyCase("CAD-R2-10: Unbind 取消回调内销毁 Editor 无 UAF",
            [](std::unique_ptr<Editor>& ed, Viewport&) { ed->Unbind(); });
    }

    // =========================================================
    // 6. 跨仓批注脏态真相：EmbedHasMarkupChanges + EmbedMarkMarkupClean
    // =========================================================
    void TestEmbedMarkupDirtyState()
    {
        TempDir tmp;
        auto seed = MakeSimpleDoc(0.0);
        // 原图预先自带 SA-问题批注 层实体，也不得被误判为本次批注修改
        const LayerID preMarkupLayer = SaMarkupExport::EnsureMarkupLayer(seed->GetScene());
        auto preEnt = std::make_unique<LineEntity>(
            seed->GetScene().NextObjectID(), Math::Point3{ 0, 5, 0 }, Math::Point3{ 50, 5, 0 });
        EntityAttr preAttr;
        preAttr.LayerId = preMarkupLayer;
        preEnt->SetAttr(preAttr);
        seed->GetScene().AddEntity(std::move(preEnt));
        seed->MarkSaved();

        const std::string docPath = tmp.WriteDoc("markup_dirty.json", *seed);

        TestAppPlatform platform;
        MainFrame frame;
        Check(InitEmbedFrame(platform, frame), "MarkupDirty: MainFrame 初始化成功");
        Check(!frame.EmbedHasMarkupChanges(), "MarkupDirty: 无主图时返回 false");

        frame.EmbedOpenMainDrawing(docPath);
        platform.PumpOpen(frame);
        frame.EmbedSetMode(1);
        Check(!frame.EmbedHasMarkupChanges(), "MarkupDirty: 进入批注模式且未绘制时返回 false（原图批注层实体不误报）");

        Document* doc = frame.GetDocumentManager().GetMainDocument();
        Check(doc != nullptr, "MarkupDirty: 主文档存在");
        if (!doc)
            return;
        Scene& scene = doc->GetScene();
        const LayerID markupLayer = scene.GetLayerManager().GetActiveLayerID();

        auto addMarkupStroke = [&](double y) -> Object::ObjectID
        {
            const Object::ObjectID id = scene.NextObjectID();
            auto e = std::make_unique<LineEntity>(id, Math::Point3{ 0, y, 0 }, Math::Point3{ 40, y, 0 });
            EntityAttr attr;
            attr.LayerId = markupLayer;
            e->SetAttr(attr);
            doc->GetCommandStack().Execute(std::make_unique<AddEntityCommand>(std::move(e)), scene);
            return id;
        };

        // 画第一笔 → 脏
        const Object::ObjectID stroke1 = addMarkupStroke(30.0);
        Check(frame.EmbedHasMarkupChanges(), "MarkupDirty: 画第一笔后 EmbedHasMarkupChanges 返回 true");

        // 撤销回空基线 → 干净；重做 → 再变脏
        doc->Undo();
        Check(!frame.EmbedHasMarkupChanges(), "MarkupDirty: 撤销第一笔回到初始基线返回 false");
        doc->Redo();
        Check(frame.EmbedHasMarkupChanges(), "MarkupDirty: 重做第一笔返回 true");

        // 导出云线拿取快照令牌，模拟异步写盘期间继续画第二笔：确认 token1 后第二笔必须保持脏态，Undo 第二笔精准回到 token1 基线
        std::string exportErr;
        const std::vector<std::array<double, 4>> rects = { { 0.0, 0.0, 20.0, 20.0 } };
        const std::vector<std::string> labels = { "P1" };
        std::uint32_t token1 = 0;
        const auto dwgBytes1 = frame.EmbedExportMarkupDwg(rects, labels, exportErr, &token1);
        Check(!dwgBytes1.empty() && token1 >= 1u, "CAD-R3-01: EmbedExportMarkupDwg 成功并签发正整数快照令牌");

        // 模拟异步写盘未完成前用户继续画第二笔
        const Object::ObjectID stroke2 = addMarkupStroke(60.0);
        (void)stroke2;
        Check(frame.EmbedMarkMarkupClean(token1), "CAD-R3-01: 写盘完成后用 token1 确认导出基线返回 true");
        Check(frame.EmbedHasMarkupChanges(), "CAD-R3-01: 异步写盘期新增的第二笔在 MarkMarkupClean(token1) 后仍保持脏态");

        // 重复消费同一个 token1 必须失败且不污染基线
        Check(!frame.EmbedMarkMarkupClean(token1), "CAD-R3-01: 已消费的 token1 不可二次确认");
        Check(!frame.EmbedMarkMarkupClean(0u), "CAD-R3-01: 0 令牌被拒绝");
        Check(!frame.EmbedMarkMarkupClean(999999u), "CAD-R3-01: 无效令牌被拒绝");
        Check(frame.EmbedHasMarkupChanges(), "CAD-R3-01: 无效令牌调用不破坏脏态");

        // 撤回第二笔 → 精准回到 token1 导出瞬间基线，自动变干净
        doc->Undo();
        Check(!frame.EmbedHasMarkupChanges(), "CAD-R3-01: Undo 第二笔精准回到 token1 导出基线返回 false");

        // 在干净基线上删除已有批注第一笔 → 脏；撤销删除 → 回干净
        doc->GetCommandStack().Execute(
            std::make_unique<BatchDeleteCommand>(std::vector<Object::ObjectID>{ stroke1 }), scene);
        Check(frame.EmbedHasMarkupChanges(), "MarkupDirty: 干净基线后删除第一笔返回 true");
        doc->Undo();
        Check(!frame.EmbedHasMarkupChanges(), "MarkupDirty: 撤销删除回到干净基线返回 false");

        // 导出临时云线批注 DWG（RAII 清理临时实体）不误污染干净基线
        std::uint32_t token2 = 0;
        const auto dwgBytes2 = frame.EmbedExportMarkupDwg(rects, labels, exportErr, &token2);
        Check(!dwgBytes2.empty() && token2 > token1, "CAD-R3-01: 二次导出签发严格递增的新令牌");
        Check(!frame.EmbedHasMarkupChanges(), "MarkupDirty: 导出临时云线不污染干净基线");

        // 跨文档令牌校验：为 doc1 签发 token2 后切到 doc2 并画新批注，拿 doc1 的 token2 确认必须失败且不污染 doc2 脏态
        const std::string doc2Path = tmp.WriteDoc("markup_dirty_doc2.json", *MakeSimpleDoc(15.0));
        frame.EmbedOpenMainDrawing(doc2Path);
        platform.PumpOpen(frame);
        frame.EmbedSetMode(1);
        Document* doc2 = frame.GetDocumentManager().GetMainDocument();
        Check(doc2 != nullptr && doc2 != doc, "CAD-R3-01: 切换到新主文档 doc2 成功");
        if (doc2)
        {
            Scene& scene2 = doc2->GetScene();
            const LayerID markupLayer2 = scene2.GetLayerManager().GetActiveLayerID();
            auto e2 = std::make_unique<LineEntity>(scene2.NextObjectID(), Math::Point3{ 0, 12, 0 }, Math::Point3{ 30, 12, 0 });
            EntityAttr attr2;
            attr2.LayerId = markupLayer2;
            e2->SetAttr(attr2);
            doc2->GetCommandStack().Execute(std::make_unique<AddEntityCommand>(std::move(e2)), scene2);
            Check(frame.EmbedHasMarkupChanges(), "CAD-R3-01: doc2 画新批注后为脏态");
            Check(!frame.EmbedMarkMarkupClean(token2), "CAD-R3-01: 跨文档使用 doc1 的 token2 被拒绝");
            Check(frame.EmbedHasMarkupChanges(), "CAD-R3-01: 跨文档令牌拒绝后 doc2 保持脏态不被污染");

            // 切回浏览模式后未清理的新增批注仍准确报告为脏
            frame.EmbedSetMode(0);
            Check(frame.EmbedHasMarkupChanges(), "MarkupDirty: 切回浏览模式后未落盘批注仍准确返回 true");

            // 关文档同步按 docId 清理待确认快照令牌：关闭 doc1 后 token2 失效
            Check(frame.EmbedCloseOtherDocuments(doc2Path), "CAD-R3-01: 关闭旧文档 doc1");
            Check(!frame.EmbedMarkMarkupClean(token2), "CAD-R3-01: 关闭 doc1 后其待确认令牌已被清理");
        }
    }

    // =========================================================
    // 7. CAD-R3-07：SaTextExport 无字复杂块 200x200 阵列零耗时剪枝 + 保留外层与嵌套 ATTRIB
    // =========================================================
    void TestSaTextExportNonTextBlockPruning()
    {
        Scene scene;

        // 含 120 个非文字几何图元的复杂无字块
        auto heavyNoText = std::make_unique<BlockEntity>(scene.NextObjectID(), "HEAVY_NO_TEXT");
        for (int i = 0; i < 100; ++i)
        {
            heavyNoText->AddEntity(std::make_unique<LineEntity>(
                scene.NextObjectID(),
                Math::Point3{ static_cast<double>(i), 0.0, 0.0 },
                Math::Point3{ static_cast<double>(i), 50.0, 0.0 }));
        }
        for (int i = 0; i < 20; ++i)
        {
            heavyNoText->AddEntity(std::make_unique<CircleEntity>(
                scene.NextObjectID(),
                Math::Point3{ static_cast<double>(i) * 5.0, 25.0, 0.0 }, 2.0));
        }
        const BlockID heavyId = scene.GetBlockTable().Add(std::move(heavyNoText));

        // 200x200（40,000 单元）无字复杂块 MINSERT + 外层挂 1 个 ATTRIB
        auto hugeIns = std::make_unique<InsertEntity>(
            scene.NextObjectID(), heavyId, "HEAVY_NO_TEXT", Math::Point3{ 0.0, 0.0, 0.0 });
        hugeIns->SetArray(200, 200, 100.0, 100.0);
        hugeIns->AddAttrib(std::make_unique<AttribEntity>(
            scene.NextObjectID(), "OUTER_TAG", "OUTER_VALUE", Math::Point3{ 10.0, 20.0, 0.0 }, 5.0f));
        scene.ResolveInsert(*hugeIns);
        scene.AddEntity(std::move(hugeIns));

        // 对照：外层块内含嵌套无字子块插入，但该嵌套插入挂有 1 个 ATTRIB（必须不能被剪枝漏掉）
        auto nestedOwner = std::make_unique<BlockEntity>(scene.NextObjectID(), "WRAP_NESTED_ATTRIB");
        for (int i = 0; i < 30; ++i)
        {
            nestedOwner->AddEntity(std::make_unique<LineEntity>(
                scene.NextObjectID(),
                Math::Point3{ 0.0, static_cast<double>(i), 0.0 },
                Math::Point3{ 10.0, static_cast<double>(i), 0.0 }));
        }
        auto innerIns = std::make_unique<InsertEntity>(
            scene.NextObjectID(), heavyId, "HEAVY_NO_TEXT", Math::Point3{ 0.0, 0.0, 0.0 });
        innerIns->SetArray(100, 100, 10.0, 10.0); // 子块无字应被跳过，但其挂载的 ATTRIB 必保留
        innerIns->SetBlock(scene.GetBlockTable().Find(heavyId));
        innerIns->AddAttrib(std::make_unique<AttribEntity>(
            scene.NextObjectID(), "NESTED_TAG", "NESTED_VALUE", Math::Point3{ 30.0, 40.0, 0.0 }, 4.0f));
        nestedOwner->AddEntity(std::move(innerIns));
        const BlockID nestedOwnerId = scene.GetBlockTable().Add(std::move(nestedOwner));

        auto wrapIns = std::make_unique<InsertEntity>(
            scene.NextObjectID(), nestedOwnerId, "WRAP_NESTED_ATTRIB", Math::Point3{ 500.0, 500.0, 0.0 });
        wrapIns->SetArray(2, 2, 100.0, 100.0); // 4 个单元，每单元产出 1 条嵌套 ATTRIB
        scene.ResolveInsert(*wrapIns);
        scene.AddEntity(std::move(wrapIns));

        const auto t0 = std::chrono::steady_clock::now();
        SaTextExport::Result res{};
        const std::string tsv = SaTextExport::ExportTsv(scene, SaTextExport::Budget{}, &res);
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        auto countSubstr = [](const std::string& haystack, const std::string& needle) -> int {
            int count = 0;
            std::size_t pos = 0;
            while ((pos = haystack.find(needle, pos)) != std::string::npos)
            {
                ++count;
                pos += needle.size();
            }
            return count;
        };

        Check(res.Ok() && elapsedMs < 500,
              "CAD-R3-07: 200×200 无字复杂块 MINSERT 经缓存剪枝快速完成且不超时");
        Check(res.ArrayCells == 4 && res.Rows == 5,
              "CAD-R3-07: 跳过 40,000+10,000 无字阵列单元（仅展开含嵌套 ATTRIB 的 2×2=4 单元，总计 5 行 ATTRIB）");
        Check(countSubstr(tsv, "\tOUTER_TAG\tOUTER_VALUE\n") == 1
                  && countSubstr(tsv, "\tNESTED_TAG\tNESTED_VALUE\n") == 4,
              "CAD-R3-07: 外层与嵌套 ATTRIB 一字不漏准确导出");
    }
}

int main()
{
    g_failures = 0;
    TestPickPointLifecycle();
    TestMainDocumentIntentVersion();
    TestCompareVisibilityAndSplitPicking();
    TestCompareResourceLifecycle();
    TestEditorToolCancelReentrancy();
    TestEmbedMarkupDirtyState();
    TestSaTextExportNonTextBlockPruning();
    std::printf("MiniCADEmbedHostTests: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
