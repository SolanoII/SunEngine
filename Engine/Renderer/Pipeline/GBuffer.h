#pragma once

#include <memory>
#include "../Core/Framebuffer.h"

namespace SunEngine {

    // Layout G-Buffer:
    //   0 : Albedo      (RGBA8)   rgb = albedo, a = AO
    //   1 : Normal      (RGBA16F) rgb = world-space normal
    //   2 : Material    (RGBA8)   r = roughness, g = metallic, b = emissive
    //   Depth           (Depth24)
    class GBuffer {
    public:
        void Create(int width, int height);
        void Resize(int width, int height);

        void BindForWriting();
        void BindForReading();       // bind su unità 0..3 (albedo/normal/material/depth)
        static void Unbind();
        void Clear();

        int GetWidth() const { return m_Width; }
        int GetHeight() const { return m_Height; }
        GpuTexture* GetAlbedo() { return m_FBO.GetColorTexture(0); }
        GpuTexture* GetNormal() { return m_FBO.GetColorTexture(1); }
        GpuTexture* GetMaterial() { return m_FBO.GetColorTexture(2); }
        GpuTexture* GetDepth() { return m_FBO.GetDepthTexture(); }
    private:
        Framebuffer m_FBO;
        int m_Width = 0;
        int m_Height = 0;
    };

}