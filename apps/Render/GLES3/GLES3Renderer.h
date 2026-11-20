#pragma once
#include "Render/IRenderer.h"
#include <GLES3/gl3.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace MiniCAD
{
    // OpenGL ES 3 / WebGL2 的 CAD 视口渲染器，与 D3D11Renderer 对应：
    //   - 线 / 三角形：顶点色；文字：纹理 alpha × 顶点色；光栅图像：纹理 × 顶点色
    //   - 不用深度缓冲（与 D3D11 版相同，后提交的盖在先提交的上面），depth 参数忽略
    //   - (slot, version) 缓存提交：version 未变时复用上次上传的顶点缓冲
    //   - 缓存层：CreateLayerTarget 返回 GLRenderTarget，DrawLayer 用 glBlitFramebuffer 整块复制
    // 纹理句柄（nativeSRV）约定为按值放进指针的 GL 纹理名，见 GLRenderTarget。
    // 上下文由宿主创建并设为当前。
    class GLES3Renderer : public IRenderer
    {
    public:
        GLES3Renderer();
        ~GLES3Renderer() override;

        GLES3Renderer(const GLES3Renderer&) = delete;
        GLES3Renderer& operator=(const GLES3Renderer&) = delete;

        void BeginFrame(IRenderTarget& target, const ViewportDesc& viewport) override;
        void EndFrame() override;
        void Submit(std::span<const Vertex_P3_C4> verts, const Math::Mat4& viewProj, PrimitiveType type, bool depth = true, bool blend = false) override;
        void SubmitTextured(std::span<const Vertex_P3_C4_UV> verts, const Math::Mat4& viewProj, void* nativeSRV, bool depth = false, bool blend = true) override;
        void SubmitImage(const ImageData& image, std::span<const Vertex_P3_C4_UV> quad, const Math::Mat4& viewProj) override;
        void SubmitImageForSlot(const ImageData& image, std::span<const Vertex_P3_C4_UV> quad, const Math::Mat4& viewProj, uint32_t slotOffset) override;

        void SubmitCached(uint32_t slot, uint64_t version,
                          std::span<const Vertex_P3_C4> verts,
                          const Math::Mat4& viewProj, PrimitiveType type,
                          bool depth = true, bool blend = false) override;
        void SubmitTexturedCached(uint32_t slot, uint64_t version,
                                  std::span<const Vertex_P3_C4_UV> verts,
                                  const Math::Mat4& viewProj, void* nativeSRV,
                                  bool depth = false, bool blend = true) override;

        bool IsCached(uint32_t slot, uint64_t version, bool textured) const override;
        void SubmitGlyphsCached(uint32_t slot, uint64_t version, const GlyphMeshView& meshes,
                                std::span<const GlyphInstance> instances, std::span<const GlyphBatch> batches,
                                const Math::Mat4& viewProj, PrimitiveType type, bool depth = true, bool blend = false) override;
        bool IsGlyphCached(uint32_t slot, uint64_t version, uint64_t meshVersion) const override;

        std::unique_ptr<IRenderTarget> CreateLayerTarget() override;
        void  DrawLayer(IRenderTarget& layer) override;
        void  SetLightBackground(bool light) override { m_lightBackground = light; }
        void  SetPaintStyle(const PaintStyle& style) override { m_paintStyle = style; }
        void  SetScissorRect(const ScissorRect& rect) override;
        void  SetClearColor(float r, float g, float b, float a) override { m_clearColor[0] = r; m_clearColor[1] = g; m_clearColor[2] = b; m_clearColor[3] = a; }
        void  ReleaseCachedResources() override;
        void  ReleaseSlotRange(uint32_t firstSlot, uint32_t lastSlot) override;
        void* GetNativeDevice() override { return nullptr; }

    private:
        enum class Program { Color, Text, Image, Glyph };

        struct ProgramInfo
        {
            GLuint program   = 0;
            GLint  viewProj  = -1;
            GLint  light     = -1;
            GLint  paintMode = -1;
            GLint  tintColor = -1;
        };

        // 一个顶点数组对象 + 顶点缓冲（持久缓存或每帧流式上传）
        struct VertexBuffer
        {
            GLuint   vao      = 0;
            GLuint   vbo      = 0;
            size_t   capacity = 0;      // 字节
            uint64_t version  = 0;
            GLsizei  count    = 0;
            bool     valid    = false;
        };

        void UseProgram(Program p, const Math::Mat4& viewProj, bool depth, bool blend);
        void InitVertexBuffer(VertexBuffer& vb, bool textured);
        void Upload(VertexBuffer& vb, const void* data, size_t bytes, GLenum usage);
        void Draw(VertexBuffer& vb, GLenum mode, GLsizei count);
        VertexBuffer& Cached(std::unordered_map<uint32_t, VertexBuffer>& cache, uint32_t slot, bool textured);

    private:
        ProgramInfo  m_programs[4];
        VertexBuffer m_stream;          // Vertex_P3_C4，每次提交重新上传
        VertexBuffer m_textStream;      // Vertex_P3_C4_UV
        std::unordered_map<uint32_t, VertexBuffer> m_cached;
        std::unordered_map<uint32_t, VertexBuffer> m_cachedText;

        // 字形实例（GlyphInstancing.hpp）：每个槽位独立持有实例 VBO 与网格库
        struct GlyphSlot
        {
            VertexBuffer                instances;   // 只用 vbo / capacity / version / count / valid
            std::vector<GlyphBatch>     batches;
            std::vector<GlyphMeshRange> meshes;
            GLuint                      fillVbo     = 0;
            GLuint                      lineVbo     = 0;
            uint64_t                    meshVersion = 0;
            bool                        meshValid   = false;
        };
        void ReleaseGlyphResources();
        void EnsureGlyphMeshes(GlyphSlot& gs, const GlyphMeshView& meshes);
        std::unordered_map<uint32_t, GlyphSlot> m_glyphSlots;
        GLuint   m_glyphVao     = 0;
        struct ImageTexture
        {
            GLuint   tex       = 0;   // 0 = 创建失败，不再重试
            uint32_t ownerBits = 0;   // 按 ImageSlotOwnerMask 记录使用方槽位组
        };
        std::unordered_map<uint64_t, ImageTexture> m_imageTextures;   // ImageData::Key → 纹理
        IRenderTarget* m_currentTarget   = nullptr;
        bool           m_lightBackground = false;
        PaintStyle     m_paintStyle{};
        ScissorRect    m_scissor{};
        bool           m_scissorEmpty    = false;
        float          m_clearColor[4]   = { 0.0f, 0.0f, 0.0f, 0.0f };
    };
}
