#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>
#include "Texture.h"

namespace SunEngine {

    class Framebuffer {
    public:
        Framebuffer() = default;
        ~Framebuffer();

        Framebuffer(const Framebuffer&) = delete;
        Framebuffer& operator=(const Framebuffer&) = delete;
        Framebuffer(Framebuffer&& other) noexcept;
        Framebuffer& operator=(Framebuffer&& other) noexcept;

        void Create(int width, int height,
            const std::vector<TextureFormat>& colorFormats,
            bool hasDepth = true,
            TextureFilter filter = TextureFilter::Nearest);

        void Resize(int width, int height);
        void Bind() const;
        static void Unbind();
        void BindColorTexture(int index, int unit) const;
        void BindDepthTexture(int unit) const;

        void Clear(const glm::vec4& color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        void DrawFullscreenQuad() const;

        GLuint GetID() const { return m_FBO; }
        int GetWidth() const { return m_Width; }
        int GetHeight() const { return m_Height; }
        int GetColorAttachmentCount() const { return (int)m_ColorTextures.size(); }

        GpuTexture* GetColorTexture(int index) {
            return (index >= 0 && index < (int)m_ColorTextures.size())
                ? m_ColorTextures[index].get() : nullptr;
        }
        GpuTexture* GetDepthTexture() { return m_DepthTexture.get(); }

        static GLuint GetFullscreenQuadVAO();

    private:
        GLuint m_FBO = 0;
        GLuint m_RBO = 0;
        int m_Width = 0;
        int m_Height = 0;

        std::vector<std::unique_ptr<GpuTexture>> m_ColorTextures;
        std::unique_ptr<GpuTexture> m_DepthTexture;

        static GLuint s_FullscreenQuadVAO;
        static GLuint s_FullscreenQuadVBO;
    };

}