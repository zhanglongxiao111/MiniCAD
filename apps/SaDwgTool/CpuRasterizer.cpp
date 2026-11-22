#include "CpuRasterizer.h"
#include "JsonWriter.h"
#include "Import/CadExchange.h"
#include "Import/CadSemanticSnapshot.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace MiniCAD::SaTool
{
    namespace
    {
        constexpr int     kHardMaxPx        = 8192;
        constexpr int64_t kHardMaxTotalPx   = 40'000'000LL;

        struct ColorRgba8
        {
            uint8_t r = 0, g = 0, b = 0, a = 255;
        };

        uint8_t ClampByte(double v)
        {
            if (v <= 0.0) return 0u;
            if (v >= 255.0) return 255u;
            return static_cast<uint8_t>(v + 0.5);
        }

        ColorRgba8 MapVertexColor(const VertexColor& vc, bool darkBg)
        {
            if (!darkBg)
            {
                // 白底模式：纯白/ACI-7 近白线条自动转为深灰 #1A1A1A
                if (vc.r > 230u && vc.g > 230u && vc.b > 230u)
                    return { 0x1Au, 0x1Au, 0x1Au, vc.a };
            }
            else
            {
                // 黑底模式：近黑线条自动转为纯白 #FFFFFF
                if (vc.r < 20u && vc.g < 20u && vc.b < 20u)
                    return { 0xFFu, 0xFFu, 0xFFu, vc.a };
            }
            return { vc.r, vc.g, vc.b, vc.a };
        }

        inline void BlendPixel(RgbaImage& img, int x, int y, ColorRgba8 c, float coverage = 1.0f)
        {
            if (x < 0 || x >= img.Width || y < 0 || y >= img.Height)
                return;
            const float alpha = (static_cast<float>(c.a) / 255.0f) * coverage;
            if (alpha <= 0.002f)
                return;
            uint8_t* dst = img.Pixels.data()
                         + (static_cast<size_t>(y) * static_cast<size_t>(img.Width) + static_cast<size_t>(x)) * 4u;
            if (alpha >= 0.996f)
            {
                dst[0] = c.r;
                dst[1] = c.g;
                dst[2] = c.b;
                dst[3] = 255u;
                return;
            }
            const float inv = 1.0f - alpha;
            dst[0] = ClampByte(c.r * alpha + dst[0] * inv);
            dst[1] = ClampByte(c.g * alpha + dst[1] * inv);
            dst[2] = ClampByte(c.b * alpha + dst[2] * inv);
            dst[3] = 255u;
        }

        inline void PlotMask(std::vector<uint8_t>& mask, int w, int h, int x, int y)
        {
            if (x < 0 || x >= w || y < 0 || y >= h)
                return;
            mask[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = 1u;
        }

        // Liang-Barsky 2D 线段裁剪到 [xmin, xmax] x [ymin, ymax]
        bool ClipLine2D(double& x0, double& y0, double& x1, double& y1,
                        double xmin, double ymin, double xmax, double ymax)
        {
            const double dx = x1 - x0;
            const double dy = y1 - y0;
            double t0 = 0.0;
            double t1 = 1.0;

            auto test = [&](double p, double q) -> bool {
                if (p == 0.0)
                    return q >= 0.0;
                const double r = q / p;
                if (p < 0.0)
                {
                    if (r > t1) return false;
                    if (r > t0) t0 = r;
                }
                else
                {
                    if (r < t0) return false;
                    if (r < t1) t1 = r;
                }
                return true;
            };

            if (!test(-dx, x0 - xmin) || !test(dx, xmax - x0)
                || !test(-dy, y0 - ymin) || !test(dy, ymax - y0))
                return false;

            const double nx0 = x0 + t0 * dx;
            const double ny0 = y0 + t0 * dy;
            const double nx1 = x0 + t1 * dx;
            const double ny1 = y0 + t1 * dy;
            x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
            return true;
        }

        // Xiaolin Wu 抗锯齿 1px 线段（用于彩色 RGBA8 渲染，细线条与中文 SHX 文字更清晰）
        void DrawLineRgbaAA(RgbaImage& img, double x0, double y0, double x1, double y1, ColorRgba8 col)
        {
            if (!ClipLine2D(x0, y0, x1, y1, -1.0, -1.0,
                            static_cast<double>(img.Width), static_cast<double>(img.Height)))
                return;

            const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
            if (steep)
            {
                std::swap(x0, y0);
                std::swap(x1, y1);
            }
            if (x0 > x1)
            {
                std::swap(x0, x1);
                std::swap(y0, y1);
            }

            auto plot = [&](int x, int y, double cov) {
                if (steep) BlendPixel(img, y, x, col, static_cast<float>(cov));
                else       BlendPixel(img, x, y, col, static_cast<float>(cov));
            };
            auto fpart  = [](double v) { return v - std::floor(v); };
            auto rfpart = [](double v) { return 1.0 - (v - std::floor(v)); };

            const double dx = x1 - x0;
            const double dy = y1 - y0;
            const double gradient = (dx < 1e-6) ? 1.0 : (dy / dx);

            // 起点
            const double xend0 = std::round(x0);
            const double yend0 = y0 + gradient * (xend0 - x0);
            const double xgap0 = rfpart(x0 + 0.5);
            const int xpxl0 = static_cast<int>(xend0);
            const int ypxl0 = static_cast<int>(std::floor(yend0));
            plot(xpxl0, ypxl0,     rfpart(yend0) * xgap0);
            plot(xpxl0, ypxl0 + 1, fpart(yend0)  * xgap0);
            double intery = yend0 + gradient;

            // 终点
            const double xend1 = std::round(x1);
            const double yend1 = y1 + gradient * (xend1 - x1);
            const double xgap1 = fpart(x1 + 0.5);
            const int xpxl1 = static_cast<int>(xend1);
            const int ypxl1 = static_cast<int>(std::floor(yend1));
            plot(xpxl1, ypxl1,     rfpart(yend1) * xgap1);
            plot(xpxl1, ypxl1 + 1, fpart(yend1)  * xgap1);

            for (int x = xpxl0 + 1; x < xpxl1; ++x)
            {
                const int iy = static_cast<int>(std::floor(intery));
                plot(x, iy,     rfpart(intery));
                plot(x, iy + 1, fpart(intery));
                intery += gradient;
            }
        }

        // Bresenham 精确 1px 整数线段（用于布尔墨水掩膜，零灰边、确定性高）
        void DrawLineMask1Px(std::vector<uint8_t>& mask, int w, int h,
                             double x0, double y0, double x1, double y1)
        {
            if (!ClipLine2D(x0, y0, x1, y1, -1.0, -1.0, static_cast<double>(w), static_cast<double>(h)))
                return;

            int ix0 = static_cast<int>(std::lround(x0));
            int iy0 = static_cast<int>(std::lround(y0));
            const int ix1 = static_cast<int>(std::lround(x1));
            const int iy1 = static_cast<int>(std::lround(y1));

            const int dx = std::abs(ix1 - ix0);
            const int sx = ix0 < ix1 ? 1 : -1;
            const int dy = -std::abs(iy1 - iy0);
            const int sy = iy0 < iy1 ? 1 : -1;
            int err = dx + dy;

            for (;;)
            {
                PlotMask(mask, w, h, ix0, iy0);
                if (ix0 == ix1 && iy0 == iy1)
                    break;
                const int e2 = err * 2;
                if (e2 >= dy) { err += dy; ix0 += sx; }
                if (e2 <= dx) { err += dx; iy0 += sy; }
            }
        }

        // 左上填充规则 2D 三角形光栅化（支持彩色 RGBA8 与布尔掩膜两种回调）
        template <class PixelFn>
        int RasterizeTriangleImpl(double x0, double y0,
                                  double x1, double y1,
                                  double x2, double y2,
                                  int w, int h,
                                  PixelFn&& emitPixel)
        {
            // 快速视口裁剪
            const double minXf = std::min({ x0, x1, x2 });
            const double maxXf = std::max({ x0, x1, x2 });
            const double minYf = std::min({ y0, y1, y2 });
            const double maxYf = std::max({ y0, y1, y2 });
            if (maxXf < 0.0 || minXf >= static_cast<double>(w)
                || maxYf < 0.0 || minYf >= static_cast<double>(h))
                return 0;

            // 1/256 亚像素吸附
            auto snap = [](double v) -> double { return std::round(v * 256.0) / 256.0; };
            x0 = snap(x0); y0 = snap(y0);
            x1 = snap(x1); y1 = snap(y1);
            x2 = snap(x2); y2 = snap(y2);

            double area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
            if (std::abs(area) < 1e-9)
                return 0;
            if (area < 0.0)
            {
                std::swap(x1, x2);
                std::swap(y1, y2);
                area = -area;
            }

            const int minX = std::max(0, static_cast<int>(std::floor(minXf)));
            const int maxX = std::min(w - 1, static_cast<int>(std::ceil(maxXf)));
            const int minY = std::max(0, static_cast<int>(std::floor(minYf)));
            const int maxY = std::min(h - 1, static_cast<int>(std::ceil(maxYf)));

            auto isTopLeft = [](double ax, double ay, double bx, double by) -> bool {
                const double dy = by - ay;
                const double dx = bx - ax;
                return (dy > 0.0) || (dy == 0.0 && dx < 0.0);
            };
            const double bias0 = isTopLeft(x1, y1, x2, y2) ? 0.0 : -1e-9;
            const double bias1 = isTopLeft(x2, y2, x0, y0) ? 0.0 : -1e-9;
            const double bias2 = isTopLeft(x0, y0, x1, y1) ? 0.0 : -1e-9;

            int plotted = 0;
            for (int y = minY; y <= maxY; ++y)
            {
                const double py = static_cast<double>(y) + 0.5;
                for (int x = minX; x <= maxX; ++x)
                {
                    const double px = static_cast<double>(x) + 0.5;
                    const double w0 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
                    const double w1 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);
                    const double w2 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
                    if (w0 >= bias0 && w1 >= bias1 && w2 >= bias2)
                    {
                        emitPixel(x, y);
                        ++plotted;
                    }
                }
            }
            return plotted;
        }

        // 粗线（widthPx > 1.25）：挤出 2D 四边形（2 个三角形）光栅化
        void DrawThickLineRgba(RgbaImage& img, double x0, double y0, double x1, double y1,
                               float widthPx, ColorRgba8 col)
        {
            const double pad = static_cast<double>(widthPx) + 2.0;
            if (!ClipLine2D(x0, y0, x1, y1, -pad, -pad,
                            static_cast<double>(img.Width) + pad, static_cast<double>(img.Height) + pad))
                return;
            const double dx = x1 - x0;
            const double dy = y1 - y0;
            const double len = std::hypot(dx, dy);
            const double hw = static_cast<double>(widthPx) * 0.5;
            if (len < 1e-6)
            {
                const int cx = static_cast<int>(std::lround(x0));
                const int cy = static_cast<int>(std::lround(y0));
                const int r  = std::max(1, static_cast<int>(std::ceil(hw)));
                for (int oy = -r; oy <= r; ++oy)
                    for (int ox = -r; ox <= r; ++ox)
                        if (ox * ox + oy * oy <= r * r)
                            BlendPixel(img, cx + ox, cy + oy, col, 1.0f);
                return;
            }
            const double nx = -dy / len * hw;
            const double ny =  dx / len * hw;
            auto emit = [&](int x, int y) { BlendPixel(img, x, y, col, 1.0f); };
            RasterizeTriangleImpl(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, img.Width, img.Height, emit);
            RasterizeTriangleImpl(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, img.Width, img.Height, emit);
        }

        void DrawThickLineMask(std::vector<uint8_t>& mask, int w, int h,
                               double x0, double y0, double x1, double y1, float widthPx)
        {
            const double pad = static_cast<double>(widthPx) + 2.0;
            if (!ClipLine2D(x0, y0, x1, y1, -pad, -pad,
                            static_cast<double>(w) + pad, static_cast<double>(h) + pad))
                return;
            const double dx = x1 - x0;
            const double dy = y1 - y0;
            const double len = std::hypot(dx, dy);
            const double hw = static_cast<double>(widthPx) * 0.5;
            if (len < 1e-6)
            {
                PlotMask(mask, w, h, static_cast<int>(std::lround(x0)), static_cast<int>(std::lround(y0)));
                return;
            }
            const double nx = -dy / len * hw;
            const double ny =  dx / len * hw;
            auto emit = [&](int x, int y) { PlotMask(mask, w, h, x, y); };
            RasterizeTriangleImpl(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, w, h, emit);
            RasterizeTriangleImpl(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, w, h, emit);
        }

        // 5x7 像素位图数字（0-9 及 '#'），用于在 diff 输出 PNG 上标出区域编号 #1, #2...
        const uint8_t* Glyph5x7(char ch)
        {
            static constexpr uint8_t kDigits[11][7] = {
                { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, // '0'
                { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, // '1'
                { 0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F }, // '2'
                { 0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E }, // '3'
                { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, // '4'
                { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, // '5'
                { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, // '6'
                { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, // '7'
                { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, // '8'
                { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, // '9'
                { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A }, // '#'
            };
            if (ch >= '0' && ch <= '9') return kDigits[ch - '0'];
            if (ch == '#') return kDigits[10];
            return nullptr;
        }
    }

    bool ComputeCanvasLayout(const std::array<double, 4>& bbox,
                             double requestedMmPerPx,
                             int maxPx,
                             int64_t maxTotalPixels,
                             CanvasLayout& out,
                             std::string* error)
    {
        const double spanW = bbox[2] - bbox[0];
        const double spanH = bbox[3] - bbox[1];
        if (!std::isfinite(spanW) || !std::isfinite(spanH) || !(spanW > 0.0) || !(spanH > 0.0))
        {
            if (error) *error = "无效渲染包围盒（宽或高必须大于 0）";
            return false;
        }

        const int effectiveMaxPx = std::clamp(maxPx > 0 ? maxPx : 2400, 16, kHardMaxPx);
        const int64_t effectiveMaxTotal = (maxTotalPixels > 0 && maxTotalPixels <= kHardMaxTotalPx)
                                            ? maxTotalPixels : kHardMaxTotalPx;

        const double maxSpan = std::max(spanW, spanH);
        double mmPerPx = requestedMmPerPx;
        bool clamped = false;

        if (!(mmPerPx > 0.0))
        {
            mmPerPx = maxSpan / static_cast<double>(effectiveMaxPx);
        }
        else
        {
            const double minMmPerPx = maxSpan / static_cast<double>(effectiveMaxPx);
            if (mmPerPx < minMmPerPx)
            {
                mmPerPx = minMmPerPx;
                clamped = true;
            }
        }

        auto calcDims = [&](double res, int& w, int& h) {
            w = std::max(1, static_cast<int>(std::ceil(spanW / res)));
            h = std::max(1, static_cast<int>(std::ceil(spanH / res)));
            w = std::min(w, effectiveMaxPx);
            h = std::min(h, effectiveMaxPx);
        };

        int w = 0, h = 0;
        calcDims(mmPerPx, w, h);

        // 总像素上限保护（硬顶 40,000,000）
        if (static_cast<int64_t>(w) * static_cast<int64_t>(h) > effectiveMaxTotal)
        {
            const double scaleUp = std::sqrt(
                static_cast<double>(static_cast<int64_t>(w) * static_cast<int64_t>(h))
                / static_cast<double>(effectiveMaxTotal));
            mmPerPx *= scaleUp;
            clamped = true;
            calcDims(mmPerPx, w, h);
        }

        out.Bbox     = bbox;
        out.WidthPx  = w;
        out.HeightPx = h;
        out.MmPerPx  = mmPerPx;
        out.Clamped  = clamped;
        return true;
    }

    void RasterizeSceneRgba(const TessellatedScene& tess,
                            const CanvasLayout& canvas,
                            bool darkBg,
                            float lineWidthPx,
                            RgbaImage& outImage)
    {
        outImage.Resize(canvas.WidthPx, canvas.HeightPx);
        if (darkBg)
            outImage.Clear(0u, 0u, 0u, 255u);
        else
            outImage.Clear(255u, 255u, 255u, 255u);

        if (canvas.WidthPx <= 0 || canvas.HeightPx <= 0 || !(canvas.MmPerPx > 0.0))
            return;

        const double invRes = 1.0 / canvas.MmPerPx;
        const double offX   = tess.RenderOrigin.x - canvas.Bbox[0];
        const double offY   = canvas.Bbox[3] - tess.RenderOrigin.y;
        const float  lw     = std::clamp(lineWidthPx, 1.0f, 4.0f);

        auto toPx = [&](const Math::Float3& p, double& sx, double& sy) {
            sx = (offX + static_cast<double>(p.x)) * invRes;
            sy = (offY - static_cast<double>(p.y)) * invRes;
        };

        // 1. 先光栅化填充三角形（HATCH 实体填充、宽多段线、TTF 字体轮廓）
        for (size_t i = 0; i + 2 < tess.Fills.size(); i += 3)
        {
            double x0, y0, x1, y1, x2, y2;
            toPx(tess.Fills[i].pos,     x0, y0);
            toPx(tess.Fills[i + 1].pos, x1, y1);
            toPx(tess.Fills[i + 2].pos, x2, y2);
            const ColorRgba8 col = MapVertexColor(tess.Fills[i].color, darkBg);

            const int plotted = RasterizeTriangleImpl(
                x0, y0, x1, y1, x2, y2, canvas.WidthPx, canvas.HeightPx,
                [&](int x, int y) { BlendPixel(outImage, x, y, col, 1.0f); });

            // 亚像素极细三角（如缩小后的宽多段线或细笔画 TTF）：保底描最长边防止断笔
            if (plotted == 0)
            {
                const double d01 = (x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0);
                const double d12 = (x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1);
                const double d20 = (x0 - x2) * (x0 - x2) + (y0 - y2) * (y0 - y2);
                if (d01 >= d12 && d01 >= d20)      DrawLineRgbaAA(outImage, x0, y0, x1, y1, col);
                else if (d12 >= d20)               DrawLineRgbaAA(outImage, x1, y1, x2, y2, col);
                else                               DrawLineRgbaAA(outImage, x2, y2, x0, y0, col);
            }
        }

        // 2. 光栅化线段对
        for (size_t i = 0; i + 1 < tess.Lines.size(); i += 2)
        {
            double x0, y0, x1, y1;
            toPx(tess.Lines[i].pos,     x0, y0);
            toPx(tess.Lines[i + 1].pos, x1, y1);
            const ColorRgba8 col = MapVertexColor(tess.Lines[i].color, darkBg);

            if (lw <= 1.25f)
                DrawLineRgbaAA(outImage, x0, y0, x1, y1, col);
            else
                DrawThickLineRgba(outImage, x0, y0, x1, y1, lw, col);
        }

        // 3. 光栅化独立点
        for (const auto& pt : tess.Points)
        {
            double sx, sy;
            toPx(pt.pos, sx, sy);
            const ColorRgba8 col = MapVertexColor(pt.color, darkBg);
            BlendPixel(outImage, static_cast<int>(std::lround(sx)), static_cast<int>(std::lround(sy)), col, 1.0f);
        }
    }

    uint64_t RasterizeSceneMask(const TessellatedScene& tess,
                                const CanvasLayout& canvas,
                                float lineWidthPx,
                                std::vector<uint8_t>& outMask)
    {
        const size_t total = static_cast<size_t>(canvas.WidthPx) * static_cast<size_t>(canvas.HeightPx);
        outMask.assign(total, 0u);
        if (canvas.WidthPx <= 0 || canvas.HeightPx <= 0 || !(canvas.MmPerPx > 0.0))
            return 0;

        const double invRes = 1.0 / canvas.MmPerPx;
        const double offX   = tess.RenderOrigin.x - canvas.Bbox[0];
        const double offY   = canvas.Bbox[3] - tess.RenderOrigin.y;
        const float  lw     = std::clamp(lineWidthPx, 1.0f, 4.0f);
        const int    w      = canvas.WidthPx;
        const int    h      = canvas.HeightPx;

        auto toPx = [&](const Math::Float3& p, double& sx, double& sy) {
            sx = (offX + static_cast<double>(p.x)) * invRes;
            sy = (offY - static_cast<double>(p.y)) * invRes;
        };

        for (size_t i = 0; i + 2 < tess.Fills.size(); i += 3)
        {
            double x0, y0, x1, y1, x2, y2;
            toPx(tess.Fills[i].pos,     x0, y0);
            toPx(tess.Fills[i + 1].pos, x1, y1);
            toPx(tess.Fills[i + 2].pos, x2, y2);
            const int plotted = RasterizeTriangleImpl(
                x0, y0, x1, y1, x2, y2, w, h,
                [&](int x, int y) { PlotMask(outMask, w, h, x, y); });
            if (plotted == 0)
            {
                const double d01 = (x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0);
                const double d12 = (x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1);
                const double d20 = (x0 - x2) * (x0 - x2) + (y0 - y2) * (y0 - y2);
                if (d01 >= d12 && d01 >= d20)      DrawLineMask1Px(outMask, w, h, x0, y0, x1, y1);
                else if (d12 >= d20)               DrawLineMask1Px(outMask, w, h, x1, y1, x2, y2);
                else                               DrawLineMask1Px(outMask, w, h, x2, y2, x0, y0);
            }
        }

        for (size_t i = 0; i + 1 < tess.Lines.size(); i += 2)
        {
            double x0, y0, x1, y1;
            toPx(tess.Lines[i].pos,     x0, y0);
            toPx(tess.Lines[i + 1].pos, x1, y1);
            if (lw <= 1.25f)
                DrawLineMask1Px(outMask, w, h, x0, y0, x1, y1);
            else
                DrawThickLineMask(outMask, w, h, x0, y0, x1, y1, lw);
        }

        for (const auto& pt : tess.Points)
        {
            double sx, sy;
            toPx(pt.pos, sx, sy);
            PlotMask(outMask, w, h, static_cast<int>(std::lround(sx)), static_cast<int>(std::lround(sy)));
        }

        uint64_t count = 0;
        for (uint8_t v : outMask)
            count += v;
        return count;
    }

    void CompositeCompareOverlay(const std::vector<uint8_t>& maskA,
                                 const std::vector<uint8_t>& maskB,
                                 int width,
                                 int height,
                                 bool darkBg,
                                 RgbaImage& outImage)
    {
        outImage.Resize(width, height);
        const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
        const uint8_t bg = darkBg ? 0u : 255u;
        uint8_t* dst = outImage.Pixels.data();

        for (size_t i = 0; i < n; ++i)
        {
            const bool a = i < maskA.size() && maskA[i] != 0;
            const bool b = i < maskB.size() && maskB[i] != 0;
            if (a && b)
            {
                dst[0] = 0x8Cu; dst[1] = 0x8Cu; dst[2] = 0x8Cu; dst[3] = 255u;   // 重合深灰 #8C8C8C
            }
            else if (a)
            {
                dst[0] = 0x00u; dst[1] = 0x9Eu; dst[2] = 0xB8u; dst[3] = 255u;   // A 独有青 #009EB8
            }
            else if (b)
            {
                dst[0] = 0xD9u; dst[1] = 0x1Fu; dst[2] = 0x1Fu; dst[3] = 255u;   // B 独有红 #D91F1F
            }
            else
            {
                dst[0] = bg;    dst[1] = bg;    dst[2] = bg;    dst[3] = 255u;
            }
            dst += 4;
        }
    }

    void CompositeDiffOverlay(const std::vector<uint8_t>& maskA,
                              const std::vector<uint8_t>& maskB,
                              const std::vector<uint8_t>& aOnly,
                              const std::vector<uint8_t>& bOnly,
                              int width,
                              int height,
                              RgbaImage& outImage)
    {
        outImage.Resize(width, height);
        const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
        uint8_t* dst = outImage.Pixels.data();

        for (size_t i = 0; i < n; ++i)
        {
            const bool a  = i < maskA.size() && maskA[i] != 0;
            const bool b  = i < maskB.size() && maskB[i] != 0;
            const bool ao = i < aOnly.size() && aOnly[i] != 0;
            const bool bo = i < bOnly.size() && bOnly[i] != 0;

            if (ao && !bo)
            {
                dst[0] = 0x00u; dst[1] = 0x9Eu; dst[2] = 0xB8u; dst[3] = 255u;   // A 独有青 #009EB8
            }
            else if (bo && !ao)
            {
                dst[0] = 0xD9u; dst[1] = 0x1Fu; dst[2] = 0x1Fu; dst[3] = 255u;   // B 独有红 #D91F1F
            }
            else if (ao && bo)
            {
                dst[0] = 0x8Cu; dst[1] = 0x8Cu; dst[2] = 0x8Cu; dst[3] = 255u;
            }
            else if (a && b)
            {
                dst[0] = 0x8Cu; dst[1] = 0x8Cu; dst[2] = 0x8Cu; dst[3] = 255u;   // 精确重合深灰 #8C8C8C
            }
            else if (a || b)
            {
                dst[0] = 0xD1u; dst[1] = 0xD1u; dst[2] = 0xD1u; dst[3] = 255u;   // 容差内重合浅灰 #D1D1D1
            }
            else
            {
                dst[0] = 255u;  dst[1] = 255u;  dst[2] = 255u;  dst[3] = 255u;   // 背景白
            }
            dst += 4;
        }
    }

    void DrawRegionBoxWithLabel(RgbaImage& image,
                                int x0, int y0, int x1, int y1,
                                int labelNumber,
                                uint8_t r, uint8_t g, uint8_t b,
                                int thickness,
                                int outerPadPx)
    {
        if (image.Width <= 0 || image.Height <= 0)
            return;
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);

        const int t   = std::max(1, thickness);
        // 外框相对紧致差异框 [x0..x1, y0..y1] 向外扩充（默认外沿 +5px、线宽 3px -> 内侧保留 2px 呼吸留白）
        const int pad = std::max(outerPadPx, t + 2);
        const int ox0 = std::clamp(x0 - pad, 0, image.Width - 1);
        const int oy0 = std::clamp(y0 - pad, 0, image.Height - 1);
        const int ox1 = std::clamp(x1 + pad, 0, image.Width - 1);
        const int oy1 = std::clamp(y1 + pad, 0, image.Height - 1);

        auto isDiffInk = [](const uint8_t* p) -> bool {
            // 保护 A 独有青 (#009EB8) 与 B 独有红 (#D91F1F)，画布边缘夹紧时也绝不吞掉真实差异线
            return (p[0] == 0x00u && p[1] == 0x9Eu && p[2] == 0xB8u)
                || (p[0] == 0xD9u && p[1] == 0x1Fu && p[2] == 0x1Fu);
        };

        auto putBorder = [&](int x, int y, uint8_t cr, uint8_t cg, uint8_t cb) {
            if (x < 0 || x >= image.Width || y < 0 || y >= image.Height)
                return;
            uint8_t* p = image.Pixels.data()
                       + (static_cast<size_t>(y) * static_cast<size_t>(image.Width) + static_cast<size_t>(x)) * 4u;
            if (isDiffInk(p))
                return;
            p[0] = cr; p[1] = cg; p[2] = cb; p[3] = 255u;
        };

        auto putBadge = [&](int x, int y, uint8_t cr, uint8_t cg, uint8_t cb) {
            if (x < 0 || x >= image.Width || y < 0 || y >= image.Height)
                return;
            uint8_t* p = image.Pixels.data()
                       + (static_cast<size_t>(y) * static_cast<size_t>(image.Width) + static_cast<size_t>(x)) * 4u;
            if (isDiffInk(p))
                return;
            p[0] = cr; p[1] = cg; p[2] = cb; p[3] = 255u;
        };

        for (int k = 0; k < t; ++k)
        {
            for (int x = ox0; x <= ox1; ++x)
            {
                putBorder(x, oy0 + k, r, g, b);
                putBorder(x, oy1 - k, r, g, b);
            }
            for (int y = oy0; y <= oy1; ++y)
            {
                putBorder(ox0 + k, y, r, g, b);
                putBorder(ox1 - k, y, r, g, b);
            }
        }

        if (labelNumber <= 0)
            return;

        // 在外框左上角外侧画橙底白字角标（#1, #2...），顶部放不下时放到底部外侧，避免遮挡框内差异
        const std::string text = "#" + std::to_string(labelNumber);
        const int scale = 2;
        const int glyphW = 5 * scale;
        const int glyphH = 7 * scale;
        const int gap    = scale;
        const int bpad   = 3;
        const int badgeW = static_cast<int>(text.size()) * (glyphW + gap) - gap + bpad * 2;
        const int badgeH = glyphH + bpad * 2;
        const int bx0    = std::clamp(ox0, 0, std::max(0, image.Width - badgeW));
        int by0          = oy0 - badgeH;
        if (by0 < 0)
            by0 = (oy1 + 1 + badgeH <= image.Height) ? (oy1 + 1) : 0;

        for (int y = by0; y < by0 + badgeH; ++y)
            for (int x = bx0; x < bx0 + badgeW; ++x)
                putBadge(x, y, r, g, b);

        int cx = bx0 + bpad;
        const int cy = by0 + bpad;
        for (char ch : text)
        {
            const uint8_t* rows = Glyph5x7(ch);
            if (rows)
            {
                for (int row = 0; row < 7; ++row)
                {
                    for (int col = 0; col < 5; ++col)
                    {
                        if ((rows[row] & (1u << (4 - col))) != 0)
                        {
                            for (int sy = 0; sy < scale; ++sy)
                                for (int sx = 0; sx < scale; ++sx)
                                    putBadge(cx + col * scale + sx, cy + row * scale + sy, 255u, 255u, 255u);
                        }
                    }
                }
            }
            cx += glyphW + gap;
        }
    }

    bool RunRenderCommand(const RenderOptions& opts,
                          RenderResult& result,
                          std::string* errorCode,
                          std::string* errorMsg,
                          bool* isUsageError)
    {
        const auto t0 = std::chrono::steady_clock::now();
        result = {};
        if (isUsageError) *isUsageError = false;

        auto fail = [&](const char* code, const std::string& msg, bool usage) -> bool {
            if (errorCode) *errorCode = code;
            if (errorMsg)  *errorMsg  = msg;
            if (isUsageError) *isUsageError = usage;
            return false;
        };

        // 先提前校验正则表达式
        std::string err;
        if (!ValidateRegexPattern(opts.IncludeLayersRegex, &err))
            return fail("INVALID_REGEX", err, true);
        if (!ValidateRegexPattern(opts.ExcludeLayersRegex, &err))
            return fail("INVALID_REGEX", err, true);

        Scene sceneA;
        CadSemanticSnapshot snapA;
        if (!LoadCadFileWithSemantics(opts.InputPath, sceneA, snapA, &err))
            return fail("LOAD_FAILED", err.empty() ? ("无法加载主图：" + opts.InputPath) : err, false);

        const int layoutIdxA = ResolveLayoutIndex(sceneA, opts.LayoutName, &err);
        if (layoutIdxA < 0)
            return fail("LAYOUT_NOT_FOUND", err, true);

        std::array<double, 4> bbox{};
        if (opts.HasBbox)
        {
            bbox = opts.Bbox;
        }
        else
        {
            if (!ComputeLayoutBbox(sceneA, layoutIdxA, bbox, /*expandMargin*/ true))
                return fail("EMPTY_LAYOUT", "目标布局中没有可渲染的可见图元", false);
        }

        CanvasLayout canvas;
        if (!ComputeCanvasLayout(bbox, opts.MmPerPx, opts.MaxPx, kHardMaxTotalPx, canvas, &err))
            return fail("INVALID_CANVAS", err, true);

        FontSystem fontSystem;
        InitHeadlessFontSystem(fontSystem, opts.FontsDir);

        TessellateFilter filter;
        filter.IncludeLayersRegex = opts.IncludeLayersRegex;
        filter.ExcludeLayersRegex = opts.ExcludeLayersRegex;
        filter.ExcludeAnnotations = opts.ExcludeAnnotations;
        filter.ExcludeFrames      = opts.ExcludeFrames;

        TessellatedScene tessA;
        if (!TessellateSceneLayout(sceneA, layoutIdxA, canvas.Bbox, canvas.MmPerPx,
                                   static_cast<double>(canvas.HeightPx), filter, fontSystem, tessA, &err))
            return fail("TESSELLATE_FAILED", err, false);

        RgbaImage image;
        const bool isCompare = !opts.ComparePath.empty();
        int totalRendered = tessA.Stats.EntitiesRendered;
        int totalFiltered = tessA.Stats.EntitiesFiltered;
        int totalFrames   = tessA.Stats.FramesExcluded;
        std::string layoutNameB;

        if (!isCompare)
        {
            RasterizeSceneRgba(tessA, canvas, opts.DarkBg, opts.LineWidthPx, image);
        }
        else
        {
            Scene sceneB;
            CadSemanticSnapshot snapB;
            if (!LoadCadFileWithSemantics(opts.ComparePath, sceneB, snapB, &err))
                return fail("LOAD_FAILED", err.empty() ? ("无法加载对比图：" + opts.ComparePath) : err, false);

            const int layoutIdxB = ResolveCompareLayoutIndex(sceneA, layoutIdxA, sceneB, opts.LayoutNameB, &err);
            if (layoutIdxB < 0)
                return fail("LAYOUT_NOT_FOUND", err, true);
            layoutNameB = sceneB.Layouts()[static_cast<size_t>(layoutIdxB)].Model
                            ? "Model"
                            : sceneB.Layouts()[static_cast<size_t>(layoutIdxB)].Name;

            // B 侧窗口按 -(dx, dy) 平移，使 B + (dx, dy) 与 A 像素对齐
            CanvasLayout canvasB = canvas;
            canvasB.Bbox = {
                canvas.Bbox[0] - opts.OffsetDx,
                canvas.Bbox[1] - opts.OffsetDy,
                canvas.Bbox[2] - opts.OffsetDx,
                canvas.Bbox[3] - opts.OffsetDy
            };

            TessellatedScene tessB;
            if (!TessellateSceneLayout(sceneB, layoutIdxB, canvasB.Bbox, canvasB.MmPerPx,
                                       static_cast<double>(canvasB.HeightPx), filter, fontSystem, tessB, &err))
                return fail("TESSELLATE_FAILED", err, false);

            totalRendered += tessB.Stats.EntitiesRendered;
            totalFiltered += tessB.Stats.EntitiesFiltered;
            totalFrames   += tessB.Stats.FramesExcluded;

            std::vector<uint8_t> maskA, maskB;
            RasterizeSceneMask(tessA, canvas,  opts.LineWidthPx, maskA);
            RasterizeSceneMask(tessB, canvasB, opts.LineWidthPx, maskB);
            CompositeCompareOverlay(maskA, maskB, canvas.WidthPx, canvas.HeightPx, opts.DarkBg, image);
        }

        if (!WritePngFile(opts.OutputPngPath, image, &err))
            return fail("WRITE_PNG_FAILED", err, false);

        const auto t1 = std::chrono::steady_clock::now();
        result.Mode             = isCompare ? "compare" : "single";
        result.OutputPath       = opts.OutputPngPath;
        result.LayoutName       = sceneA.Layouts()[static_cast<size_t>(layoutIdxA)].Model
                                    ? "Model"
                                    : sceneA.Layouts()[static_cast<size_t>(layoutIdxA)].Name;
        result.LayoutNameB      = layoutNameB;
        result.Bbox             = canvas.Bbox;
        result.OffsetBToA       = { opts.OffsetDx, opts.OffsetDy };
        result.WidthPx          = canvas.WidthPx;
        result.HeightPx         = canvas.HeightPx;
        result.MmPerPx          = canvas.MmPerPx;
        result.Clamped          = canvas.Clamped;
        result.Bg               = opts.DarkBg ? "black" : "white";
        result.EntitiesRendered = totalRendered;
        result.EntitiesFiltered = totalFiltered;
        result.FramesExcluded   = totalFrames;
        result.ElapsedMs        = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        return true;
    }

    std::string FormatRenderResultJson(const RenderResult& r, bool pretty)
    {
        JsonWriter j(pretty);
        j.BeginObject();
        j.KeyBool("ok", true);
        j.KeyString("command", "render");
        j.KeyString("mode", r.Mode);
        j.KeyString("output_path", r.OutputPath);
        j.KeyString("layout", r.LayoutName);
        j.KeyOptString("layout_b", r.LayoutNameB);

        j.Key("bbox");
        j.BeginArray();
        for (double v : r.Bbox) j.Double(v, 4);
        j.EndArray();

        j.Key("offset_b_to_a");
        j.BeginArray();
        j.Double(r.OffsetBToA[0], 4);
        j.Double(r.OffsetBToA[1], 4);
        j.EndArray();

        j.Key("canvas_px");
        j.BeginArray();
        j.Int(r.WidthPx);
        j.Int(r.HeightPx);
        j.EndArray();

        j.KeyDouble("mm_per_px", r.MmPerPx, 4);
        j.KeyBool("clamped", r.Clamped);
        j.KeyString("bg", r.Bg);
        j.KeyInt("entities_rendered", r.EntitiesRendered);
        j.KeyInt("entities_filtered", r.EntitiesFiltered);
        j.KeyInt("frames_excluded", r.FramesExcluded);
        j.KeyInt("elapsed_ms", r.ElapsedMs);
        j.EndObject();
        return j.Take();
    }
}
