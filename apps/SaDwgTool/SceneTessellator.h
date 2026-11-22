#pragma once
// 纯 CPU 无头场景细分管线：将 MiniCAD::Scene 的模型空间或图纸空间布局（含浮动视口合成与裁剪）
// 细分为相对裁剪中心的线段/填充/点顶点流，同时支持顶层与块内递归图层/注记/修改框过滤。
#include "Core/Math/Point3.hpp"
#include "Render/VertexTypes.hpp"
#include "Scene/Scene.h"
#include "Text/FontSystem.h"
#include <array>
#include <string>
#include <vector>

namespace MiniCAD::SaTool
{
    struct TessellateFilter
    {
        std::string IncludeLayersRegex;
        std::string ExcludeLayersRegex;
        bool        ExcludeAnnotations = false;
        bool        ExcludeFrames      = false;
    };

    struct TessellateStats
    {
        int EntitiesRendered = 0;
        int EntitiesFiltered = 0;
        int FramesExcluded   = 0;
    };

    struct TessellatedScene
    {
        Math::Point3              RenderOrigin{};
        std::vector<Vertex_P3_C4> Lines;    // 线段对（每 2 顶点一段，相对 RenderOrigin）
        std::vector<Vertex_P3_C4> Fills;    // 三角形（每 3 顶点一面，相对 RenderOrigin）
        std::vector<Vertex_P3_C4> Points;   // 独立点（相对 RenderOrigin）
        TessellateStats           Stats;
    };

    // 按规范顺序自动定位字体目录（--fonts-dir > <exe>/fonts > <exe>/../assets/fonts > <exe>/../../assets/fonts）。
    std::string ResolveFontsDirectory(const std::string& explicitDir = {});

    // 初始化无头 FontSystem（配置字体目录与系统字体回退目录）。
    void InitHeadlessFontSystem(FontSystem& fontSystem, const std::string& explicitFontsDir = {});

    // 解析布局名：支持 "Model" / "模型" / 精确布局名 / "index:<N>"；失败返回 -1。
    int ResolveLayoutIndex(const Scene& scene, const std::string& layoutSpec, std::string* error = nullptr);

    // 解析双图对比时 B 侧布局：显式指定则按指定解析；否则先同名匹配，再同序号匹配。
    int ResolveCompareLayoutIndex(const Scene& sceneA, int layoutIdxA,
                                  const Scene& sceneB, const std::string& layoutSpecB,
                                  std::string* error = nullptr);

    // 计算目标布局可见有界实体的包围盒；expandMargin=true 时向外扩张 2%。
    bool ComputeLayoutBbox(const Scene& scene, int layoutIdx, std::array<double, 4>& outBbox,
                           bool expandMargin = true);

    // 判定实体是否为天正修改范围框（闭合多段线、线宽>=50、顶点数<=60、长边>=2000、非零凸度>=2）。
    bool IsModificationFrame(const Entity& entity);

    // 校验 UTF-8 正则表达式（ECMAScript，不区分大小写，支持中文 UTF-16 匹配）。
    bool ValidateRegexPattern(const std::string& pattern, std::string* error = nullptr);

    // 不区分大小写的 UTF-8 正则子串匹配（支持中文）。
    bool RegexSearchUtf8Icase(const std::string& text, const std::string& pattern);

    // 纯 CPU 无头细分指定布局与裁剪窗口内的几何。
    bool TessellateSceneLayout(const Scene& scene,
                               int layoutIdx,
                               const std::array<double, 4>& bbox,
                               double worldPerPixel,
                               double viewportHeightPx,
                               const TessellateFilter& filter,
                               FontSystem& fontSystem,
                               TessellatedScene& out,
                               std::string* error = nullptr);
}
