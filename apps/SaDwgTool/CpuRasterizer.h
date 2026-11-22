#pragma once
// 纯 CPU 光栅化器（RGBA8 彩色渲染 + 单字节墨水掩膜 + 双图叠加合成 + 橙色差异框绘制）
// 以及 SaDwgTool render 子命令完整入口。
#include "CliArgs.h"
#include "PngWriter.h"
#include "SceneTessellator.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniCAD::SaTool
{
    struct CanvasLayout
    {
        std::array<double, 4> Bbox{};          // [xmin, ymin, xmax, ymax]
        int                   WidthPx  = 0;
        int                   HeightPx = 0;
        double                MmPerPx  = 1.0;
        bool                  Clamped  = false;
    };

    // 根据世界包围盒、期望分辨率与最大像素上限计算画布尺寸与实际分辨率。
    bool ComputeCanvasLayout(const std::array<double, 4>& bbox,
                             double requestedMmPerPx,
                             int maxPx,
                             int64_t maxTotalPixels,
                             CanvasLayout& out,
                             std::string* error = nullptr);

    // 彩色 RGBA8 光栅化（单图渲染，支持 --bg white/black 与 --line-width-px）。
    void RasterizeSceneRgba(const TessellatedScene& tess,
                            const CanvasLayout& canvas,
                            bool darkBg,
                            float lineWidthPx,
                            RgbaImage& outImage);

    // 单字节布尔墨水掩膜光栅化（每像素 0/1，大小 = WidthPx * HeightPx；返回墨水像素总数）。
    uint64_t RasterizeSceneMask(const TessellatedScene& tess,
                                const CanvasLayout& canvas,
                                float lineWidthPx,
                                std::vector<uint8_t>& outMask);

    // render --compare 模式双图叠加合成（A 青 / B 红 / 重合深灰）。
    void CompositeCompareOverlay(const std::vector<uint8_t>& maskA,
                                 const std::vector<uint8_t>& maskB,
                                 int width,
                                 int height,
                                 bool darkBg,
                                 RgbaImage& outImage);

    // diff 模式三态差异叠加合成（白底 / 容差浅灰 / 精确重合深灰 / A 独有青 / B 独有红）。
    void CompositeDiffOverlay(const std::vector<uint8_t>& maskA,
                              const std::vector<uint8_t>& maskB,
                              const std::vector<uint8_t>& aOnly,
                              const std::vector<uint8_t>& bOnly,
                              int width,
                              int height,
                              RgbaImage& outImage);

    // 在 RGBA 图像上绘制实心边框矩形与编号角标（供 diff 标出橙色差异区域框）。
    void DrawRegionBoxWithLabel(RgbaImage& image,
                                int x0, int y0, int x1, int y1,
                                int labelNumber,
                                uint8_t r, uint8_t g, uint8_t b,
                                int thickness = 3);

    struct RenderResult
    {
        std::string           Mode = "single";   // "single" | "compare"
        std::string           OutputPath;
        std::string           LayoutName;
        std::string           LayoutNameB;
        std::array<double, 4> Bbox{};
        std::array<double, 2> OffsetBToA{ 0.0, 0.0 };
        int                   WidthPx          = 0;
        int                   HeightPx         = 0;
        double                MmPerPx          = 0.0;
        bool                  Clamped          = false;
        std::string           Bg               = "white";
        int                   EntitiesRendered = 0;
        int                   EntitiesFiltered = 0;
        int                   FramesExcluded   = 0;
        int64_t               ElapsedMs        = 0;
    };

    // 完整执行 render 子命令（单图或 --compare 双图叠加）。
    // 参数校验/画布非法时设置 isUsageError = true（退出码 2）。
    bool RunRenderCommand(const RenderOptions& opts,
                          RenderResult& result,
                          std::string* errorCode = nullptr,
                          std::string* errorMsg = nullptr,
                          bool* isUsageError = nullptr);

    std::string FormatRenderResultJson(const RenderResult& result, bool pretty = false);
}
