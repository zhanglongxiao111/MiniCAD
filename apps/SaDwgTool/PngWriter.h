#pragma once
// 无外部依赖的 RGBA8 → PNG 写出器（UTF-8 路径，支持中文与 canonical UNC 路径）。
#include <cstdint>
#include <string>
#include <vector>

namespace MiniCAD::SaTool
{
    struct RgbaImage
    {
        int                  Width  = 0;
        int                  Height = 0;
        std::vector<uint8_t> Pixels;   // RGBA8，行主序，大小 = Width * Height * 4

        void Resize(int w, int h)
        {
            Width  = w > 0 ? w : 0;
            Height = h > 0 ? h : 0;
            Pixels.assign(static_cast<size_t>(Width) * static_cast<size_t>(Height) * 4u, 0u);
        }

        void Clear(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
        {
            const size_t n = static_cast<size_t>(Width) * static_cast<size_t>(Height);
            uint8_t* p = Pixels.data();
            for (size_t i = 0; i < n; ++i)
            {
                p[0] = r; p[1] = g; p[2] = b; p[3] = a;
                p += 4;
            }
        }
    };

    // 将 RGBA8 图像编码为标准 8 位 RGBA PNG 字节流；失败返回空数组。
    std::vector<uint8_t> EncodePngRgba(const uint8_t* rgba, int width, int height);

    // 将 RGBA8 图像写盘到 UTF-8 路径（自动创建父目录）；失败写入 error 并返回 false。
    bool WritePngFile(const std::string& utf8Path, const uint8_t* rgba, int width, int height, std::string* error = nullptr);
    bool WritePngFile(const std::string& utf8Path, const RgbaImage& image, std::string* error = nullptr);
}
