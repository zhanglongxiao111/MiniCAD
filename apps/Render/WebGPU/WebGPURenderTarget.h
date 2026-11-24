#pragma once
#include "Render/IRenderTarget.h"
#include <cstdint>
#include <webgpu/webgpu.h>

namespace MiniCAD
{
    // WebGPU 离屏渲染目标：RGBA8 颜色纹理 + Depth32Float 深度纹理，与 GLRenderTarget / D3D11RenderTarget 对应。
    // 原生句柄：
    //   GetNativeHandle()         → 颜色纹理的视图（WGPUTextureView，渲染附件）
    //   GetNativeShaderResource() → 同一个视图（界面层把视口画面当作纹理显示）
    // 调整尺寸时重建纹理，视图随之改变：界面层发现句柄变了会重新登记（MainFrame）。
    // 先建新的再释放旧的，同时递增全局原子代次 m_generation（Release 归零），供合成缓存精确判别纹理重建，避免指针地址复用 ABA
    class WebGPURenderTarget : public IRenderTarget
    {
    public:
        explicit WebGPURenderTarget(WGPUDevice device);
        ~WebGPURenderTarget() override;

        WebGPURenderTarget(const WebGPURenderTarget&) = delete;
        WebGPURenderTarget& operator=(const WebGPURenderTarget&) = delete;

        void Create(int width, int height) override;
        void Resize(int width, int height) override;
        void Release() override;

        int      GetWidth()      const override { return m_width; }
        int      GetHeight()     const override { return m_height; }
        void*    GetNativeHandle()         const override { return m_colorView; }
        void*    GetNativeShaderResource() const override { return m_colorView; }
        uint64_t GetGeneration() const { return m_generation; }

        WGPUTexture     GetColorTexture() const { return m_color; }
        WGPUTextureView GetColorView()    const { return m_colorView; }
        WGPUTextureView GetDepthView()    const { return m_depthView; }

        static constexpr WGPUTextureFormat kColorFormat = WGPUTextureFormat_RGBA8Unorm;
        static constexpr WGPUTextureFormat kDepthFormat = WGPUTextureFormat_Depth32Float;

    private:
        WGPUDevice      m_device     = nullptr;
        WGPUTexture     m_color      = nullptr;
        WGPUTextureView m_colorView  = nullptr;
        WGPUTexture     m_depth      = nullptr;
        WGPUTextureView m_depthView  = nullptr;
        int             m_width      = 0;
        int             m_height     = 0;
        uint64_t        m_generation = 0;
    };
}
