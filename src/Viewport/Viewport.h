#pragma once 
#include "Camera.h"
#include "Cursor.h"
#include "Grid.h"
#include "Axis.h"
#include "Gizmo.h"
#include "Viewport/ViewState.h"
#include "Core/Math/PackedTypes.hpp"
#include "Render/IRenderTarget.h"
#include "Render/IRenderer.h"
#include <memory>

namespace MiniCAD
{ 
    // 选择范围框
    struct SelectionGeometry
    {
        std::vector<Vertex_P3_C4> fill;
        std::vector<Vertex_P3_C4> border; 
    };

    struct GripGeometry
    {
        std::vector<Vertex_P3_C4> fill;
        std::vector<Vertex_P3_C4> border; 
    };

    struct SnapGeometry
    {
        std::vector<Vertex_P3_C4> fill;
        std::vector<Vertex_P3_C4> border;
    };
    enum class CompareDisplayMode : uint8_t
    {
        Off           = 0,
        Blend         = 1,   // 双离屏三态差异合成：相同退灰、主图翠绿、对比图珊瑚红
        Split         = 2,   // 卷帘分屏：SplitX 左侧主图原色，右侧对比图原色
        CurrentOnly   = 3,   // 只看主图原色
        CompareOnly   = 4,   // 只看对比图原色
        AlignPickBase = 5,   // 抓对比图基点：主图 22% 淡化原色 + 对比图原色置顶（或 SoloCompare 时仅显示对比图）
        AlignPickMain = 6    // 抓主图目标点：三态实时差异跟随，对齐处重合线即时变灰
    };

    struct CompareRenderPass
    {
        const ViewState*   State          = nullptr;
        CompareDisplayMode Mode           = CompareDisplayMode::Off;
        double             Dx             = 0.0;   // p_base = p_main * Scale + (Dx, Dy)
        double             Dy             = 0.0;
        double             Scale          = 1.0;
        float              OverlapOpacity = 0.40f; // 相同重合部分灰阶浓度（0 = 隐藏相同部分，只看纯差异）
        int                SplitX         = 0;     // 物理像素分割线
        bool               SoloCompare    = false;
        bool               PickCompare    = false; // true 时动态捕捉标记使用对比图的 Snap
        bool               ShowMain       = true;  // Blend 模式下是否显示仅主图差异
        bool               ShowCompare    = true;  // Blend 模式下是否显示仅对比图差异
    };

    class Viewport
    {
    public:
        static constexpr uint32_t kCompareSlotOffset = 32;

        // 库内不创建 RenderTarget，应用层负责创建并在 Render() 时传入
        Viewport(float width, float height);

        // 应用层每帧调用：传入自己的 renderer + renderTarget，Viewport 负责提交所有几何（可选单引擎同相机对比通道）
        void Render(IRenderer& renderer, IRenderTarget& target, const ViewState& viewState,
                    const CompareRenderPass* compare = nullptr);
        // 场景静态流（填充、线、文字）的当前版本是否都已由渲染后端持有：是则调用方可以释放 CPU 端的拼接流
        static bool SceneUploaded(const IRenderer& renderer, const ViewState& viewState, uint32_t slotOffset = 0);
        // 同上，按版本号查询（Editor 在渲染前核对自己的场景缓冲还在不在后端里：多个视口轮流渲染时会被别的文档顶掉）。
        // textured：场景里有纹理文字（要查文字槽位）；glyphs：有矢量字形实例（要查字形槽位，网格版本为 meshVersion）
        static bool SceneUploaded(const IRenderer& renderer, uint64_t sceneVersion, uint64_t glyphVersion,
                                  bool textured, bool glyphs, uint64_t meshVersion, uint32_t slotOffset = 0);

        void Resize(float width, float height);

        Camera&        GetCamera();
        const  Camera& GetCamera() const;

        void  Pan (float dx, float dy);
        void  Zoom(float delta, float mouseX, float mouseY);
        float GetWidth() const { return m_width; }
        float GetHeight() const { return m_height; }

        // 缓存层（网格 / 坐标轴 / 场景）重画的次数：鼠标移动等只改动态内容的帧不会增加（测试与统计用）
        uint64_t GetLayerRedraws() const { return m_layerRedraws; }
        bool     IsLayerCached()   const { return m_sceneLayer != nullptr; }

        void ShowAxis       (bool show) { m_showAxis = show; }
        void ShowGrid       (bool show) { m_showGrid = show; }
        void ShowGizmo      (bool show) { m_showGizmo = show; }
        void ShowGizmoToggle()          { m_showGizmo = !m_showGizmo; }
        void ShowGridToggle ()          { m_showGrid = !m_showGrid; }
        void ShowAxisToggle ()          { m_showAxis = !m_showAxis; }
        bool IsGizmoShown   () const    { return m_showGizmo; }
        bool IsGridShown    () const    { return m_showGrid; }
        bool IsAxisShown    () const    { return m_showAxis; }

        // 视口背景是浅色（界面浅色主题）：白色内容改画成黑色，见 IRenderer::SetLightBackground
        void SetLightBackground(bool light) { m_lightBackground = light; }
        bool IsLightBackground() const      { return m_lightBackground; }

    private:
        void              AddDashedLine(std::vector<Vertex_P3_C4>& out, Math::Float3& a, Math::Float3& b, Math::Float4& color, float dashLen = 6.0f, float gapLen = 4.0f);
        SelectionGeometry BuildSelectionGeometry(const ViewState& viewState);
        GripGeometry      BuildGripGeometry(const ViewState& vs);
        SnapGeometry      BuildSnapGeometry(const ViewState& vs);

        // 相机/尺寸/开关变化时重建网格、坐标轴、Gizmo 顶点并递增视图版本；
        // 未变化时直接复用缓存（CPU 不重建，GPU 缓冲不重新上传）
        void RefreshViewGeometry(const ViewState& vs);

    private:
        float  m_width;
        float  m_height;
        Camera m_camera;
        Cursor m_cursor;
        Grid   m_grid;
        Axis   m_axis;
        Gizmo  m_gizmo;
        ViewportDesc m_viewportDesc;

        std::vector<Vertex_P3_C4> m_vertices;

        // 视图几何缓存：仅在相机/尺寸/显示开关变化时重建
        std::vector<Vertex_P3_C4> m_gridVerts;
        std::vector<Vertex_P3_C4> m_axisVerts;
        std::vector<Vertex_P3_C4> m_gizmoVerts;
        // 供 SubmitCached 判断 GPU 缓冲是否需重传。进程内全局分配（NextViewVersion）：几个视口共用一个渲染器时
        //（文档拖出成独立窗口），两个视口的网格 / 坐标轴才不会因为版本号相同被当成同一份缓冲
        uint64_t m_viewVersion = 0;
        static uint64_t NextViewVersion();
        uint64_t m_viewKey     = 0;   // 相机状态+尺寸+开关的指纹

        // 夹点几何缓存：按夹点内容（位置、类型、悬停）的指纹缓存，没变时不重建、GPU 缓冲不重新上传
        GripGeometry m_gripGeom;
        uint64_t     m_gripKey   = 0;
        bool         m_gripValid = false;

        // 离屏缓存层：网格、坐标轴、Gizmo、场景（填充 / 线 / 文字）。只有场景版本、相机、尺寸、
        // 显示开关变化时才重画；后端不支持时为空，退回每帧全量绘制
        std::unique_ptr<IRenderTarget> m_sceneLayer;
        bool     m_layerTried   = false;
        uint64_t m_layerKey     = 0;
        bool     m_layerValid   = false;
        uint64_t m_layerRedraws = 0;

        // 双离屏三态对比底片：按需创建、退出对比即释放，主/副图各自独立缓存
        std::unique_ptr<IRenderTarget> m_compareMainLayer;
        std::unique_ptr<IRenderTarget> m_compareOtherLayer;
        uint64_t m_compareMainKey    = 0;
        bool     m_compareMainValid  = false;
        uint64_t m_compareOtherKey   = 0;
        bool     m_compareOtherValid = false;

        bool m_lightBackground = false;
        bool m_showGizmo = true;
        bool m_showGrid  = false;
        bool m_showAxis  = false;
    };
	
}
