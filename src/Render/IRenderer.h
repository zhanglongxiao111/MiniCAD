#pragma once
#include <memory>
#include <span>
#include <cstdint>
#include "Render/IRenderTarget.h"
#include "Render/VertexTypes.hpp"
#include "Render/GlyphInstancing.hpp"
#include <vector>
#include "Core/Image/ImageData.hpp"
#include "Core/Math/Mat4.hpp"

namespace MiniCAD 
{
    // Line / Triangle 是线段与三角形；Point 是**点图元**（一个顶点 = 一个点）。
    // 点图元由后端保证恒定 1 屏幕像素，与缩放无关 —— 图案填充的"点"笔划
    // （.pat 的 dash = 0）必须走这条路：画成世界尺寸的短线段会在缩小视图时
    // 小于 1px 而整片消失。
    enum class PrimitiveType { Line, Triangle, Point };

    struct ViewportDesc
    {
        float x        = 0.0f;
        float y        = 0.0f;
        float width    = 0.0f;
        float height   = 0.0f;
        float minDepth = 0.0f;
        float maxDepth = 1.0f;
    };

    enum class PaintTintMode : uint8_t { None = 0, Monochrome = 1 };
    enum class PaintBlendMode : uint8_t { Alpha = 0, Screen = 1 };

    struct PaintStyle
    {
        PaintTintMode  TintMode  = PaintTintMode::None;
        PaintBlendMode BlendMode = PaintBlendMode::Alpha;
        float TintR   = 1.0f;
        float TintG   = 1.0f;
        float TintB   = 1.0f;
        float Opacity = 1.0f;
    };

    struct ScissorRect
    {
        bool Enabled = false;
        int  X       = 0;
        int  Y       = 0;
        int  Width   = 0;
        int  Height  = 0;
    };

    class IRenderer
    {
    public:
        virtual ~IRenderer() = default;

        // IRenderTarget 替换原来的 D3D11_VIEWPORT + RenderTarget
        virtual void BeginFrame(IRenderTarget& target, const ViewportDesc& viewport) = 0;
        virtual void EndFrame  () = 0; 
        virtual void Submit    (std::span<const Vertex_P3_C4>    verts, const Math::Mat4& viewProj, PrimitiveType type, bool depth = true, bool blend = false) = 0;
        virtual void SubmitTextured(std::span<const Vertex_P3_C4_UV> verts, const Math::Mat4& viewProj, void* nativeSRV, bool depth = false, bool blend = true) = 0;

        // 绘制一张光栅图像（quad 为 6 个带 UV 的顶点）。后端按 image.Key 缓存纹理；
        // 默认不支持（WASM 暂未实现），图像不显示，实体只画边框。
        virtual void SubmitImage(const ImageData& image, std::span<const Vertex_P3_C4_UV> quad, const Math::Mat4& viewProj)
        {
            (void)image; (void)quad; (void)viewProj;
        }

        // 带槽位偏移提交光栅图像（用于主图 0..31 / 对比图 32..63 按归属独立释放 GPU 纹理）
        virtual void SubmitImageForSlot(const ImageData& image, std::span<const Vertex_P3_C4_UV> quad, const Math::Mat4& viewProj, uint32_t slotOffset)
        {
            (void)slotOffset;
            SubmitImage(image, quad, viewProj);
        }

        // 按 slot/32 分桶的图像纹理归属位掩码（最多覆盖 32 组，前 0..31 主图 / 32..63 对比图）
        static constexpr uint32_t ImageSlotOwnerMask(uint32_t firstSlot, uint32_t lastSlot)
        {
            if (firstSlot > lastSlot)
                return 0u;
            const uint32_t startBucket = firstSlot / 32u;
            if (startBucket >= 32u)
                return 0u;
            const uint32_t rawEnd    = lastSlot / 32u;
            const uint32_t endBucket = rawEnd < 31u ? rawEnd : 31u;
            uint32_t mask = 0u;
            for (uint32_t b = startBucket; b <= endBucket; ++b)
                mask |= (1u << b);
            return mask;
        }

        // ── 缓存提交 ─────────────────────────────────────────────────────
        // 几何由 (slot, version) 标识：version 未变时后端可直接复用上次上传的
        // GPU 缓冲，避免每帧全量重新上传大体量场景顶点；version 变化才上传。
        // 默认实现回退到非缓存 Submit，无缓存能力的后端（WASM）仍然正确。
        virtual void SubmitCached(uint32_t slot, uint64_t version,
                                  std::span<const Vertex_P3_C4> verts,
                                  const Math::Mat4& viewProj, PrimitiveType type,
                                  bool depth = true, bool blend = false)
        {
            (void)slot; (void)version;
            Submit(verts, viewProj, type, depth, blend);
        }

        virtual void SubmitTexturedCached(uint32_t slot, uint64_t version,
                                          std::span<const Vertex_P3_C4_UV> verts,
                                          const Math::Mat4& viewProj, void* nativeSRV,
                                          bool depth = false, bool blend = true)
        {
            (void)slot; (void)version;
            SubmitTextured(verts, viewProj, nativeSRV, depth, blend);
        }

        // 字形实例（见 GlyphInstancing.hpp）：instances 按网格排序，batches 为同一网格的连续区间。
        // type 为 Triangle 画字形的三角形，Line 画轮廓线；两次提交共用同一 (slot, version) 的实例。
        // 默认实现在 CPU 上展开成顶点后走 Submit（无实例化能力的后端）
        virtual void SubmitGlyphsCached(uint32_t slot, uint64_t version, const GlyphMeshView& meshes,
                                        std::span<const GlyphInstance> instances, std::span<const GlyphBatch> batches,
                                        const Math::Mat4& viewProj, PrimitiveType type, bool depth = true, bool blend = false)
        {
            (void)slot; (void)version; (void)batches;
            std::vector<Vertex_P3_C4> verts;
            for (const GlyphInstance& g : instances)
                ExpandGlyph(g, meshes, type == PrimitiveType::Triangle, [&](const Vertex_P3_C4& v) { verts.push_back(v); });
            Submit(verts, viewProj, type, depth, blend);
        }

        // 后端是否已持有 (slot, version) 的字形实例与 meshes.Version 的字形网格（同 IsCached）
        virtual bool IsGlyphCached(uint32_t slot, uint64_t version, uint64_t meshVersion) const
        {
            (void)slot; (void)version; (void)meshVersion;
            return false;
        }

        // 后端是否已持有 (slot, version) 的几何：上传成功，之后同一版本的提交不再读调用方传入的顶点。
        // 为 true 时调用方可以释放自己的那份顶点（几百万顶点的图纸省下一整份副本）；
        // 默认 false：无缓存能力的后端每次提交都要读顶点
        virtual bool IsCached(uint32_t slot, uint64_t version, bool textured) const
        {
            (void)slot; (void)version; (void)textured;
            return false;
        }

        // ── 接到已缓存的几何末尾（可选能力）────────────────────────────────
        // 调用方只持有新增部分的顶点时（大图纸的拼接流上传后已释放，之后又画了一个图形），把 verts 接到
        // (slot, baseVersion) 缓冲的末尾并把它升级为 version；之后同一 version 的 SubmitCached 直接画这块缓冲，
        // 几百 MB 的场景流不再经过 CPU 重新拼接、整体重新上传。已经是 version 时返回 true（同一帧里可能调两次）；
        // 后端没有 baseVersion 的缓冲或失败返回 false，调用方按 baseVersion 画旧内容并改走全量提交。
        // 默认不支持：调用方先看 SupportsCachedAppend() 再准备追加段
        virtual bool SupportsCachedAppend() const { return false; }
        virtual bool AppendCached(uint32_t slot, uint64_t baseVersion, uint64_t version, std::span<const Vertex_P3_C4> verts)
        {
            (void)slot; (void)baseVersion; (void)version; (void)verts;
            return false;
        }
        // 原地覆盖 (slot, baseVersion) 缓冲里从 offset 起的 verts.size() 个顶点，版本不变：修改 / 删除一个实体时调用方
        // 只重写它那一段（删除写成透明的退化图元），再经 AppendCached 升级版本。越界或没有 baseVersion 的缓冲返回 false。
        // 与 AppendCached 一起支持（SupportsCachedAppend 为真时两者都可用）
        virtual bool WriteCached(uint32_t slot, uint64_t baseVersion, size_t offset, std::span<const Vertex_P3_C4> verts)
        {
            (void)slot; (void)baseVersion; (void)offset; (void)verts;
            return false;
        }

        // ── 离屏缓存层（可选能力）────────────────────────────────────────
        // Viewport 把不常变化的内容（网格、坐标轴、场景）画进缓存层，之后每帧
        // 只复制缓存层、再画光标 / 夹点 / 选中等动态内容，鼠标移动不再重画整张图。
        // 不支持的后端返回 nullptr，Viewport 退回每帧全量绘制。
        virtual std::unique_ptr<IRenderTarget> CreateLayerTarget() { return nullptr; }
        // 把缓存层的内容原样复制到当前帧的渲染目标（BeginFrame 之后调用，尺寸必须相同）
        virtual void DrawLayer(IRenderTarget& layer) { (void)layer; }

        // 背景深浅（AutoCAD 的 7 号色语义）：浅色背景时纯白顶点画成纯黑，深色背景时纯黑画成纯白，
        // 白色的实体、图层颜色、十字光标在两种背景下都看得见。只影响线 / 三角形 / 文字，不影响光栅图像
        virtual void SetLightBackground(bool light) { (void)light; }

        // 叠图对比染色/透明/混合与裁剪矩形（只影响随后的提交，BeginFrame 复位）
        virtual void SetPaintStyle(const PaintStyle& style) { (void)style; }
        virtual void SetScissorRect(const ScissorRect& rect) { (void)rect; }
        virtual void SetClearColor(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }

        // 释放缓存提交的顶点缓冲与图像纹理（关闭文档后调用）：之后的提交按需重新创建、上传
        virtual void ReleaseCachedResources() {}
        // 只释放 [firstSlot, lastSlot] 槽位范围的缓存（如主图 0..31、对比图 32..63 各自独立释放）
        virtual void ReleaseSlotRange(uint32_t firstSlot, uint32_t lastSlot) { (void)firstSlot; (void)lastSlot; }

        virtual void* GetNativeDevice() = 0;
    };
}