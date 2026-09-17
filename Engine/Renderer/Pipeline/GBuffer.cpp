#include "GBuffer.h"
#include "../../Core/Logger/Log.h"

namespace SunEngine {

    void GBuffer::Create(int width, int height) {
        m_Width = width;
        m_Height = height;

        std::vector<TextureFormat> formats = {
            TextureFormat::RGBA8,
            TextureFormat::RGBA16F,
            TextureFormat::RGBA8
        };

        m_FBO.Create(width, height, formats, /*hasDepth*/ true, TextureFilter::Nearest);

        SUN_CORE_INFO("GBuffer creato: {0}x{1} (3 attachments + depth)", width, height);
    }

    void GBuffer::Resize(int width, int height) {
        if (width == m_Width && height == m_Height) return;
        m_FBO.Resize(width, height);
        m_Width = width;
        m_Height = height;
    }

    void GBuffer::BindForWriting() {
        m_FBO.Bind();
    }

    void GBuffer::BindForReading() {
        // Unità fisse: 0=albedo, 1=normal, 2=material, 3=depth
        m_FBO.BindColorTexture(0, 0);
        m_FBO.BindColorTexture(1, 1);
        m_FBO.BindColorTexture(2, 2);
        m_FBO.BindDepthTexture(3);
    }

    void GBuffer::Unbind() {
        Framebuffer::Unbind();
    }

    void GBuffer::Clear() {
        // Colore neutro: albedo nero, normal 0 (poi normalizziamo), material 0.
        // Così i pixel non coperti restano "vuoti" e la lighting pass
        // può scartarli con un check su depth.
        m_FBO.Clear(glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
    }

}