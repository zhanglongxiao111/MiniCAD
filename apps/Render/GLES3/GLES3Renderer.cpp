#include "GLES3/GLES3Renderer.h"
#include "GLES3/GLRenderTarget.h"
#include "Core/Log.h"
#include <algorithm>
#include <cstddef>
#include <string>

namespace MiniCAD
{
    namespace
    {
        // PrimitiveType → GL 图元模式。点图元（.pat 的 dash = 0 等）走 GL_POINTS，
        // 一个顶点一个点。GLES3 / WebGL2 没有 glPointSize，点大小由顶点着色器的 gl_PointSize 给出（见 kVertexColor）。
        GLenum PrimitiveMode(PrimitiveType type)
        {
            switch (type)
            {
            case PrimitiveType::Line:     return GL_LINES;
            case PrimitiveType::Point:    return GL_POINTS;
            case PrimitiveType::Triangle:
            default:                      return GL_TRIANGLES;
            }
        }

        // 顶点着色器：Mat4 为行向量约定（P' = P · M），按行存放的 16 个数直接上传为 GLSL 的列主序矩阵，
        // 得到 M 的转置，于是 uViewProj * v 与 HLSL 的 mul(v, M) 相同
        const char* kVertexColor = R"(#version 300 es
            uniform mat4 uViewProj;
            uniform float uLight;       // 1 浅色背景，0 深色背景，-1 不换色（光栅图像）
            vec4 MapColor(vec4 c)
            {
                // 7 号色语义：浅色背景时纯白 → 纯黑，深色背景时纯黑 → 纯白
                if (uLight >= 0.0)
                {
                    vec3 d = abs(c.rgb - vec3(uLight));
                    if (max(d.x, max(d.y, d.z)) < 0.004)
                        c.rgb = 1.0 - c.rgb;
                }
                return c;
            }
            layout(location = 0) in vec3 aPos;
            layout(location = 1) in vec4 aColor;
            out vec4 vColor;
            void main()
            {
                vColor = MapColor(aColor);
                gl_Position = uViewProj * vec4(aPos, 1.0);
                gl_PointSize = 1.0;     // 点图元恒定 1 物理像素（与 D3D11 的 POINTLIST 一致）；线、三角形忽略此值
            })";

        const char* kVertexTextured = R"(#version 300 es
            uniform mat4 uViewProj;
            uniform float uLight;       // 1 浅色背景，0 深色背景，-1 不换色（光栅图像）
            vec4 MapColor(vec4 c)
            {
                // 7 号色语义：浅色背景时纯白 → 纯黑，深色背景时纯黑 → 纯白
                if (uLight >= 0.0)
                {
                    vec3 d = abs(c.rgb - vec3(uLight));
                    if (max(d.x, max(d.y, d.z)) < 0.004)
                        c.rgb = 1.0 - c.rgb;
                }
                return c;
            }
            layout(location = 0) in vec3 aPos;
            layout(location = 1) in vec4 aColor;
            layout(location = 2) in vec2 aUV;
            out vec4 vColor;
            out vec2 vUV;
            void main()
            {
                vColor = MapColor(aColor);
                vUV    = aUV;
                gl_Position = uViewProj * vec4(aPos, 1.0);
            })";

        // 字形实例：aPos 为字形网格的局部坐标，其余为逐实例属性（divisor 1）
        const char* kVertexGlyph = R"(#version 300 es
            uniform mat4 uViewProj;
            uniform float uLight;
            vec4 MapColor(vec4 c)
            {
                if (uLight >= 0.0)
                {
                    vec3 d = abs(c.rgb - vec3(uLight));
                    if (max(d.x, max(d.y, d.z)) < 0.004)
                        c.rgb = 1.0 - c.rgb;
                }
                return c;
            }
            layout(location = 0) in vec2 aPos;
            layout(location = 1) in vec4 aM;       // m00 m01 m10 m11
            layout(location = 2) in vec3 aT;       // tx ty z
            layout(location = 3) in vec4 aColor;
            out vec4 vColor;
            void main()
            {
                vColor = MapColor(aColor);
                vec2 w = vec2(aPos.x * aM.x + aPos.y * aM.z + aT.x, aPos.x * aM.y + aPos.y * aM.w + aT.y);
                gl_Position = uViewProj * vec4(w, aT.z, 1.0);
            })";

        #define MINICAD_GL_APPLY_PAINT \
            "uniform float uOpacity;\n" \
            "uniform float uFullCoverage;\n" \
            "float VertexCoverage(float a) {\n" \
            "    return uFullCoverage > 0.5 ? (a > 0.001 ? 1.0 : 0.0) : a;\n" \
            "}\n" \
            "vec4 ApplyPaint(vec4 c) {\n" \
            "    return vec4(c.rgb, clamp(c.a * uOpacity, 0.0, 1.0));\n" \
            "}\n"

        const char* kFragmentColor =
            "#version 300 es\nprecision mediump float;\n"
            MINICAD_GL_APPLY_PAINT
            "in vec4 vColor;\nout vec4 outColor;\n"
            "void main() { outColor = ApplyPaint(vec4(vColor.rgb, VertexCoverage(vColor.a))); }";

        // 文字：字形图集只用 alpha
        const char* kFragmentText =
            "#version 300 es\nprecision mediump float;\nuniform sampler2D uTex;\n"
            MINICAD_GL_APPLY_PAINT
            "in vec4 vColor;\nin vec2 vUV;\nout vec4 outColor;\n"
            "void main() { outColor = ApplyPaint(vec4(vColor.rgb, VertexCoverage(vColor.a) * texture(uTex, vUV).a)); }";

        const char* kFragmentImage =
            "#version 300 es\nprecision mediump float;\nuniform sampler2D uTex;\n"
            MINICAD_GL_APPLY_PAINT
            "in vec4 vColor;\nin vec2 vUV;\nout vec4 outColor;\n"
            "void main() { vec4 t = texture(uTex, vUV); outColor = ApplyPaint(vec4(t.rgb * vColor.rgb, t.a * VertexCoverage(vColor.a))); }";

        // 全屏三角形：双离屏底片三态差异合成（按像素覆盖度判定 重合 / 仅主图 / 仅对比图）
        const char* kVertexCompare = R"(#version 300 es
            void main()
            {
                vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
                gl_Position = vec4(p, 0.0, 1.0);
            })";

        const char* kFragmentCompare = R"(#version 300 es
            precision mediump float;
            uniform sampler2D uMainTex;
            uniform sampler2D uCompTex;
            uniform vec4 uMainColor;
            uniform vec4 uCompColor;
            uniform vec4 uOverlapColor;
            out vec4 outColor;
            void main()
            {
                ivec2 dims = textureSize(uMainTex, 0);
                ivec2 c = clamp(ivec2(gl_FragCoord.xy), ivec2(0), max(dims - ivec2(1), ivec2(0)));
                float a = texelFetch(uMainTex, c, 0).a;
                float b = texelFetch(uCompTex, c, 0).a;
                float wOverlap = min(a, b) * uOverlapColor.a;
                float wMain    = max(0.0, a - b) * uMainColor.a;
                float wComp    = max(0.0, b - a) * uCompColor.a;
                float totalW   = wOverlap + wMain + wComp;
                if (totalW < 0.0005)
                    discard;
                vec3 rgb = (uOverlapColor.rgb * wOverlap + uMainColor.rgb * wMain + uCompColor.rgb * wComp) / totalW;
                outColor = vec4(rgb, clamp(totalW, 0.0, 1.0));
            })";

        GLuint Compile(GLenum type, const char* source)
        {
            GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &source, nullptr);
            glCompileShader(s);
            GLint ok = GL_FALSE;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[1024] = {};
                glGetShaderInfoLog(s, sizeof(log), nullptr, log);
                LOG_ERROR_TR("GLES3Renderer: 着色器编译失败：{0}", log);
            }
            return s;
        }

        GLuint Link(const char* vsSource, const char* fsSource)
        {
            const GLuint vs = Compile(GL_VERTEX_SHADER, vsSource);
            const GLuint fs = Compile(GL_FRAGMENT_SHADER, fsSource);
            const GLuint p  = glCreateProgram();
            glAttachShader(p, vs);
            glAttachShader(p, fs);
            glLinkProgram(p);
            glDeleteShader(vs);
            glDeleteShader(fs);
            GLint ok = GL_FALSE;
            glGetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                char log[1024] = {};
                glGetProgramInfoLog(p, sizeof(log), nullptr, log);
                LOG_ERROR_TR("GLES3Renderer: 着色器链接失败：{0}", log);
            }
            return p;
        }

        GLuint TextureFromHandle(void* nativeSRV)
        {
            return static_cast<GLuint>(reinterpret_cast<uintptr_t>(nativeSRV));
        }
    }

    GLES3Renderer::GLES3Renderer()
    {
        const char* fragments[3] = { kFragmentColor, kFragmentText, kFragmentImage };
        for (int i = 0; i < 3; ++i)
        {
            ProgramInfo& info = m_programs[i];
            info.program  = Link(i == 0 ? kVertexColor : kVertexTextured, fragments[i]);
            info.viewProj     = glGetUniformLocation(info.program, "uViewProj");
            info.light        = glGetUniformLocation(info.program, "uLight");
            info.opacity      = glGetUniformLocation(info.program, "uOpacity");
            info.fullCoverage = glGetUniformLocation(info.program, "uFullCoverage");
            if (i != 0)
            {
                glUseProgram(info.program);
                glUniform1i(glGetUniformLocation(info.program, "uTex"), 0);
            }
        }
        {
            ProgramInfo& info = m_programs[static_cast<int>(Program::Glyph)];
            info.program      = Link(kVertexGlyph, kFragmentColor);
            info.viewProj     = glGetUniformLocation(info.program, "uViewProj");
            info.light        = glGetUniformLocation(info.program, "uLight");
            info.opacity      = glGetUniformLocation(info.program, "uOpacity");
            info.fullCoverage = glGetUniformLocation(info.program, "uFullCoverage");
        }
        InitVertexBuffer(m_stream, false);
        InitVertexBuffer(m_textStream, true);
        glGenVertexArrays(1, &m_glyphVao);
    }

    GLES3Renderer::~GLES3Renderer()
    {
        auto destroy = [](VertexBuffer& vb)
        {
            glDeleteBuffers(1, &vb.vbo);
            glDeleteVertexArrays(1, &vb.vao);
        };
        destroy(m_stream);
        destroy(m_textStream);
        for (auto& [slot, vb] : m_cached)     destroy(vb);
        for (auto& [slot, vb] : m_cachedText) destroy(vb);
        ReleaseGlyphResources();
        glDeleteVertexArrays(1, &m_glyphVao);
        for (auto& [key, im] : m_imageTextures)
            if (im.tex)
                glDeleteTextures(1, &im.tex);
        for (ProgramInfo& p : m_programs)
            glDeleteProgram(p.program);
        if (m_compareProgram.program)
            glDeleteProgram(m_compareProgram.program);
        if (m_compareProgram.vao)
            glDeleteVertexArrays(1, &m_compareProgram.vao);
    }

    void GLES3Renderer::ReleaseCachedResources()
    {
        auto destroy = [](VertexBuffer& vb)
        {
            glDeleteBuffers(1, &vb.vbo);
            glDeleteVertexArrays(1, &vb.vao);
        };
        for (auto& [slot, vb] : m_cached)     destroy(vb);
        for (auto& [slot, vb] : m_cachedText) destroy(vb);
        m_cached.clear();
        m_cachedText.clear();
        ReleaseGlyphResources();
        for (auto& [key, im] : m_imageTextures)
            if (im.tex)
                glDeleteTextures(1, &im.tex);
        m_imageTextures.clear();
    }

    void GLES3Renderer::ReleaseSlotRange(uint32_t firstSlot, uint32_t lastSlot)
    {
        auto eraseBuffers = [&](std::unordered_map<uint32_t, VertexBuffer>& cache)
        {
            for (auto it = cache.begin(); it != cache.end();)
            {
                if (it->first >= firstSlot && it->first <= lastSlot)
                {
                    glDeleteBuffers(1, &it->second.vbo);
                    glDeleteVertexArrays(1, &it->second.vao);
                    it = cache.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        };
        eraseBuffers(m_cached);
        eraseBuffers(m_cachedText);
        for (auto it = m_glyphSlots.begin(); it != m_glyphSlots.end();)
        {
            if (it->first >= firstSlot && it->first <= lastSlot)
            {
                glDeleteBuffers(1, &it->second.instances.vbo);
                glDeleteBuffers(1, &it->second.fillVbo);
                glDeleteBuffers(1, &it->second.lineVbo);
                it = m_glyphSlots.erase(it);
            }
            else
            {
                ++it;
            }
        }
        const uint32_t clearBits = IRenderer::ImageSlotOwnerMask(firstSlot, lastSlot);
        if (clearBits)
        {
            for (auto it = m_imageTextures.begin(); it != m_imageTextures.end();)
            {
                it->second.ownerBits &= ~clearBits;
                if (it->second.ownerBits == 0)
                {
                    if (it->second.tex)
                        glDeleteTextures(1, &it->second.tex);
                    it = m_imageTextures.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }

    void GLES3Renderer::InitVertexBuffer(VertexBuffer& vb, bool textured)
    {
        glGenVertexArrays(1, &vb.vao);
        glGenBuffers(1, &vb.vbo);
        glBindVertexArray(vb.vao);
        glBindBuffer(GL_ARRAY_BUFFER, vb.vbo);
        if (textured)
        {
            const GLsizei stride = sizeof(Vertex_P3_C4_UV);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex_P3_C4_UV, pos)));
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex_P3_C4_UV, color)));
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex_P3_C4_UV, uv)));
        }
        else
        {
            const GLsizei stride = sizeof(Vertex_P3_C4);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex_P3_C4, pos)));
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void*>(offsetof(Vertex_P3_C4, color)));   // VertexColor：RGBA 各 8 位
        }
        glBindVertexArray(0);
    }

    void GLES3Renderer::Upload(VertexBuffer& vb, const void* data, size_t bytes, GLenum usage)
    {
        glBindBuffer(GL_ARRAY_BUFFER, vb.vbo);
        // 容量不足时按 1.5 倍扩容，减少反复重新分配；用量不到容量的 1/4 时缩容（归还显存）
        const bool shrink = vb.capacity > (1u << 20) && bytes * 4 < vb.capacity;
        if (bytes > vb.capacity || shrink)
        {
            vb.capacity = bytes + bytes / 2;
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vb.capacity), nullptr, usage);
        }
        if (bytes > 0)
            glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), data);
    }

    void GLES3Renderer::Draw(VertexBuffer& vb, GLenum mode, GLsizei count)
    {
        if (count <= 0)
            return;
        glBindVertexArray(vb.vao);
        glDrawArrays(mode, 0, count);
        glBindVertexArray(0);
    }

    GLES3Renderer::VertexBuffer& GLES3Renderer::Cached(std::unordered_map<uint32_t, VertexBuffer>& cache, uint32_t slot, bool textured)
    {
        auto it = cache.find(slot);
        if (it == cache.end())
        {
            it = cache.emplace(slot, VertexBuffer{}).first;
            InitVertexBuffer(it->second, textured);
        }
        return it->second;
    }

    void GLES3Renderer::UseProgram(Program p, const Math::Mat4& viewProj, bool depth, bool blend)
    {
        const ProgramInfo& info = m_programs[static_cast<int>(p)];
        float m[16];
        for (int i = 0; i < 16; ++i)
            m[i] = static_cast<float>(viewProj.m[i]);
        const bool effectiveBlend = blend || (m_paintStyle.Opacity < 0.999f);

        glUseProgram(info.program);
        glUniformMatrix4fv(info.viewProj, 1, GL_FALSE, m);
        glUniform1f(info.light, p == Program::Image ? -1.0f : (m_lightBackground ? 1.0f : 0.0f));
        glUniform1f(info.opacity, m_paintStyle.Opacity);
        glUniform1f(info.fullCoverage, m_paintStyle.FullCoverage ? 1.0f : 0.0f);

        // 深度：与 D3D11Renderer 相同——不测试 / 混合的只测试不写入 / 不混合的测试并写入（三维视图的着色面）
        if (!depth)
            glDisable(GL_DEPTH_TEST);
        else
        {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(effectiveBlend ? GL_FALSE : GL_TRUE);
        }

        if (effectiveBlend)
        {
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        else
        {
            glDisable(GL_BLEND);
        }
    }

    // =========================================================
    // 帧
    // =========================================================
    void GLES3Renderer::BeginFrame(IRenderTarget& target, const ViewportDesc& vp)
    {
        m_currentTarget = &target;
        auto& gl = static_cast<GLRenderTarget&>(target);
        glBindFramebuffer(GL_FRAMEBUFFER, gl.GetFramebuffer());

        // GL 的视口原点在左下角
        const int height = target.GetHeight();
        glViewport(static_cast<GLint>(vp.x), static_cast<GLint>(height - (vp.y + vp.height)),
                   static_cast<GLsizei>(vp.width), static_cast<GLsizei>(vp.height));
        m_paintStyle   = {};
        m_scissor      = {};
        m_scissorEmpty = false;
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
        glDepthMask(GL_TRUE);
        glClearDepthf(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void GLES3Renderer::SetScissorRect(const ScissorRect& rect)
    {
        m_scissor = rect;
        if (!m_currentTarget || !m_scissor.Enabled)
        {
            m_scissorEmpty = false;
            glDisable(GL_SCISSOR_TEST);
            return;
        }
        const int tw = m_currentTarget->GetWidth();
        const int th = m_currentTarget->GetHeight();
        if (tw <= 0 || th <= 0)
        {
            m_scissorEmpty = true;
            glDisable(GL_SCISSOR_TEST);
            return;
        }
        const int x0 = std::clamp(m_scissor.X, 0, tw);
        const int y0 = std::clamp(m_scissor.Y, 0, th);
        const int x1 = std::clamp(m_scissor.X + m_scissor.Width, 0, tw);
        const int y1 = std::clamp(m_scissor.Y + m_scissor.Height, 0, th);
        if (x1 <= x0 || y1 <= y0)
        {
            m_scissorEmpty = true;
            glDisable(GL_SCISSOR_TEST);
            return;
        }
        m_scissorEmpty = false;
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0, th - y1, x1 - x0, y1 - y0);
    }

    void GLES3Renderer::EndFrame()
    {
        m_currentTarget = nullptr;
        m_scissorEmpty  = false;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);   // 与界面层共用上下文：恢复默认深度状态
        glDepthMask(GL_TRUE);
    }

    // =========================================================
    // 提交
    // =========================================================
    void GLES3Renderer::Submit(std::span<const Vertex_P3_C4> verts, const Math::Mat4& viewProj, PrimitiveType type, bool depth, bool blend)
    {
        if (verts.empty() || m_scissorEmpty)
            return;
        UseProgram(Program::Color, viewProj, depth, blend);
        Upload(m_stream, verts.data(), verts.size_bytes(), GL_STREAM_DRAW);
        Draw(m_stream, PrimitiveMode(type), static_cast<GLsizei>(verts.size()));
    }

    void GLES3Renderer::SubmitTextured(std::span<const Vertex_P3_C4_UV> verts, const Math::Mat4& viewProj, void* nativeSRV, bool depth, bool blend)
    {
        if (verts.empty() || !nativeSRV || m_scissorEmpty)
            return;
        UseProgram(Program::Text, viewProj, depth, blend);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, TextureFromHandle(nativeSRV));
        Upload(m_textStream, verts.data(), verts.size_bytes(), GL_STREAM_DRAW);
        Draw(m_textStream, GL_TRIANGLES, static_cast<GLsizei>(verts.size()));
    }

    void GLES3Renderer::SubmitImage(const ImageData& image, std::span<const Vertex_P3_C4_UV> quad, const Math::Mat4& viewProj)
    {
        SubmitImageForSlot(image, quad, viewProj, 0);
    }

    void GLES3Renderer::SubmitImageForSlot(const ImageData& image, std::span<const Vertex_P3_C4_UV> quad, const Math::Mat4& viewProj, uint32_t slotOffset)
    {
        if (quad.empty() || image.Width == 0 || image.Height == 0 || image.Rgba.empty() || m_scissorEmpty)
            return;

        const uint32_t ownerBit = IRenderer::ImageSlotOwnerMask(slotOffset, slotOffset);
        auto it = m_imageTextures.find(image.Key);
        if (it == m_imageTextures.end())
        {
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(image.Width), static_cast<GLsizei>(image.Height),
                         0, GL_RGBA, GL_UNSIGNED_BYTE, image.Rgba.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            it = m_imageTextures.emplace(image.Key, ImageTexture{ tex, ownerBit }).first;
        }
        else
        {
            it->second.ownerBits |= ownerBit;
        }
        if (!it->second.tex)
            return;

        UseProgram(Program::Image, viewProj, false, true);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, it->second.tex);
        Upload(m_textStream, quad.data(), quad.size_bytes(), GL_STREAM_DRAW);
        Draw(m_textStream, GL_TRIANGLES, static_cast<GLsizei>(quad.size()));
    }

    bool GLES3Renderer::IsCached(uint32_t slot, uint64_t version, bool textured) const
    {
        const auto& cache = textured ? m_cachedText : m_cached;
        auto it = cache.find(slot);
        return it != cache.end() && it->second.valid && it->second.version == version;
    }

    void GLES3Renderer::ReleaseGlyphResources()
    {
        for (auto& [slot, gs] : m_glyphSlots)
        {
            glDeleteBuffers(1, &gs.instances.vbo);
            glDeleteBuffers(1, &gs.fillVbo);
            glDeleteBuffers(1, &gs.lineVbo);
        }
        m_glyphSlots.clear();
    }

    // 字形网格库：版本变化时整体重新上传（按槽位独立持有）
    void GLES3Renderer::EnsureGlyphMeshes(GlyphSlot& gs, const GlyphMeshView& meshes)
    {
        if (gs.meshValid && gs.meshVersion == meshes.Version)
            return;
        auto upload = [](GLuint& vbo, std::span<const Math::Float2> verts)
        {
            if (!vbo) glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size_bytes()), verts.data(), GL_STATIC_DRAW);
        };
        upload(gs.fillVbo, meshes.FillVerts);
        upload(gs.lineVbo, meshes.LineVerts);
        gs.meshes.assign(meshes.Meshes.begin(), meshes.Meshes.end());
        gs.meshVersion = meshes.Version;
        gs.meshValid   = true;
    }

    void GLES3Renderer::SubmitGlyphsCached(uint32_t slot, uint64_t version, const GlyphMeshView& meshes,
                                           std::span<const GlyphInstance> instances, std::span<const GlyphBatch> batches,
                                           const Math::Mat4& viewProj, PrimitiveType type, bool depth, bool blend)
    {
        GlyphSlot& gs = m_glyphSlots[slot];
        if (!gs.instances.valid || gs.instances.version != version)
        {
            if (!gs.instances.vbo) glGenBuffers(1, &gs.instances.vbo);
            Upload(gs.instances, instances.data(), instances.size_bytes(), GL_STATIC_DRAW);
            gs.instances.count   = static_cast<GLsizei>(instances.size());
            gs.instances.version = version;
            gs.instances.valid   = true;
            gs.batches.assign(batches.begin(), batches.end());
        }
        EnsureGlyphMeshes(gs, meshes);
        if (gs.instances.count == 0 || m_scissorEmpty)
            return;
        const GLuint meshVbo = type == PrimitiveType::Triangle ? gs.fillVbo : gs.lineVbo;

        UseProgram(Program::Glyph, viewProj, depth, blend);
        glBindVertexArray(m_glyphVao);
        glBindBuffer(GL_ARRAY_BUFFER, meshVbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Math::Float2), nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, gs.instances.vbo);
        for (GLuint a = 1; a <= 3; ++a)
        {
            glEnableVertexAttribArray(a);
            glVertexAttribDivisor(a, 1);
        }
        const GLenum mode = type == PrimitiveType::Triangle ? GL_TRIANGLES : GL_LINES;
        constexpr GLsizei stride = sizeof(GlyphInstance);
        for (const GlyphBatch& b : gs.batches)
        {
            if (b.mesh >= gs.meshes.size()) continue;
            const GlyphMeshRange& m = gs.meshes[b.mesh];
            const GLsizei n     = static_cast<GLsizei>(type == PrimitiveType::Triangle ? m.fillCount : m.lineCount);
            const GLint   start = static_cast<GLint>(type == PrimitiveType::Triangle ? m.fillStart : m.lineStart);
            if (n == 0) continue;
            // WebGL2 没有 baseInstance：逐批把实例属性指到这一批的起点
            const size_t base = static_cast<size_t>(b.first) * sizeof(GlyphInstance);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(base + offsetof(GlyphInstance, m00)));
            glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(base + offsetof(GlyphInstance, tx)));
            glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void*>(base + offsetof(GlyphInstance, color)));
            glDrawArraysInstanced(mode, start, n, static_cast<GLsizei>(b.count));
        }
        glBindVertexArray(0);
    }

    bool GLES3Renderer::IsGlyphCached(uint32_t slot, uint64_t version, uint64_t meshVersion) const
    {
        auto it = m_glyphSlots.find(slot);
        return it != m_glyphSlots.end() && it->second.instances.valid && it->second.instances.version == version
            && it->second.meshValid && it->second.meshVersion == meshVersion;
    }

    void GLES3Renderer::SubmitCached(uint32_t slot, uint64_t version, std::span<const Vertex_P3_C4> verts,
                                     const Math::Mat4& viewProj, PrimitiveType type, bool depth, bool blend)
    {
        VertexBuffer& vb = Cached(m_cached, slot, false);
        if (!vb.valid || vb.version != version)
        {
            Upload(vb, verts.data(), verts.size_bytes(), GL_STATIC_DRAW);
            vb.count   = static_cast<GLsizei>(verts.size());
            vb.version = version;
            vb.valid   = true;
        }
        if (vb.count == 0 || m_scissorEmpty)
            return;
        UseProgram(Program::Color, viewProj, depth, blend);
        Draw(vb, PrimitiveMode(type), vb.count);
    }

    void GLES3Renderer::SubmitTexturedCached(uint32_t slot, uint64_t version, std::span<const Vertex_P3_C4_UV> verts,
                                             const Math::Mat4& viewProj, void* nativeSRV, bool depth, bool blend)
    {
        if (!nativeSRV)
            return;
        VertexBuffer& vb = Cached(m_cachedText, slot, true);
        if (!vb.valid || vb.version != version)
        {
            Upload(vb, verts.data(), verts.size_bytes(), GL_STATIC_DRAW);
            vb.count   = static_cast<GLsizei>(verts.size());
            vb.version = version;
            vb.valid   = true;
        }
        if (vb.count == 0 || m_scissorEmpty)
            return;
        UseProgram(Program::Text, viewProj, depth, blend);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, TextureFromHandle(nativeSRV));
        Draw(vb, GL_TRIANGLES, vb.count);
    }

    // =========================================================
    // 缓存层
    // =========================================================
    std::unique_ptr<IRenderTarget> GLES3Renderer::CreateLayerTarget()
    {
        return std::make_unique<GLRenderTarget>();
    }

    void GLES3Renderer::DrawLayer(IRenderTarget& layer)
    {
        // 整块复制缓存层：比重新绘制几十万条线便宜得多
        if (!m_currentTarget || &layer == m_currentTarget)
            return;
        const int w = layer.GetWidth();
        const int h = layer.GetHeight();
        if (w != m_currentTarget->GetWidth() || h != m_currentTarget->GetHeight())
            return;

        const GLuint src = static_cast<GLRenderTarget&>(layer).GetFramebuffer();
        const GLuint dst = static_cast<GLRenderTarget*>(m_currentTarget)->GetFramebuffer();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, dst);
    }

    bool GLES3Renderer::CompositeCompareLayers(IRenderTarget& mainLayer, IRenderTarget& compareLayer,
                                               const CompareCompositeStyle& style)
    {
        if (!m_currentTarget || &mainLayer == m_currentTarget || &compareLayer == m_currentTarget || m_scissorEmpty)
            return false;
        const int w = m_currentTarget->GetWidth();
        const int h = m_currentTarget->GetHeight();
        if (w <= 0 || h <= 0 || mainLayer.GetWidth() != w || mainLayer.GetHeight() != h
            || compareLayer.GetWidth() != w || compareLayer.GetHeight() != h)
            return false;

        if (!m_compareProgram.program)
        {
            m_compareProgram.program      = Link(kVertexCompare, kFragmentCompare);
            m_compareProgram.mainColor    = glGetUniformLocation(m_compareProgram.program, "uMainColor");
            m_compareProgram.compColor    = glGetUniformLocation(m_compareProgram.program, "uCompColor");
            m_compareProgram.overlapColor = glGetUniformLocation(m_compareProgram.program, "uOverlapColor");
            glUseProgram(m_compareProgram.program);
            glUniform1i(glGetUniformLocation(m_compareProgram.program, "uMainTex"), 0);
            glUniform1i(glGetUniformLocation(m_compareProgram.program, "uCompTex"), 1);
            glGenVertexArrays(1, &m_compareProgram.vao);
        }
        if (!m_compareProgram.program)
            return false;

        const GLuint mainTex = static_cast<GLRenderTarget&>(mainLayer).GetTexture();
        const GLuint compTex = static_cast<GLRenderTarget&>(compareLayer).GetTexture();
        if (!mainTex || !compTex)
            return false;

        glUseProgram(m_compareProgram.program);
        glUniform4f(m_compareProgram.mainColor,    style.MainR,    style.MainG,    style.MainB,    std::clamp(style.MainOpacity, 0.0f, 1.0f));
        glUniform4f(m_compareProgram.compColor,    style.CompareR, style.CompareG, style.CompareB, std::clamp(style.CompareOpacity, 0.0f, 1.0f));
        glUniform4f(m_compareProgram.overlapColor, style.OverlapR, style.OverlapG, style.OverlapB, std::clamp(style.OverlapOpacity, 0.0f, 1.0f));

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mainTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, compTex);

        glBindVertexArray(m_compareProgram.vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        return true;
    }
}
