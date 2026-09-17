#include "Framebuffer.h"
#include "../../Core/Logger/Log.h"

namespace SunEngine {

    GLuint Framebuffer::s_FullscreenQuadVAO = 0;
    GLuint Framebuffer::s_FullscreenQuadVBO = 0;

    Framebuffer::~Framebuffer() {
        if (m_FBO != 0) glDeleteFramebuffers(1, &m_FBO);
        if (m_RBO != 0) glDeleteRenderbuffers(1, &m_RBO);
    }

    Framebuffer::Framebuffer(Framebuffer&& other) noexcept
        : m_FBO(other.m_FBO), m_RBO(other.m_RBO),
        m_Width(other.m_Width), m_Height(other.m_Height),
        m_ColorTextures(std::move(other.m_ColorTextures)),
        m_DepthTexture(std::move(other.m_DepthTexture)) {
        other.m_FBO = 0;
        other.m_RBO = 0;
    }

    Framebuffer& Framebuffer::operator=(Framebuffer&& other) noexcept {
        if (this != &other) {
            if (m_FBO != 0) glDeleteFramebuffers(1, &m_FBO);
            if (m_RBO != 0) glDeleteRenderbuffers(1, &m_RBO);

            m_FBO = other.m_FBO;
            m_RBO = other.m_RBO;
            m_Width = other.m_Width;
            m_Height = other.m_Height;
            m_ColorTextures = std::move(other.m_ColorTextures);
            m_DepthTexture = std::move(other.m_DepthTexture);

            other.m_FBO = 0;
            other.m_RBO = 0;
        }
        return *this;
    }

    void Framebuffer::Create(int width, int height,
        const std::vector<TextureFormat>& colorFormats,
        bool hasDepth, TextureFilter filter) {
        if (m_FBO != 0) glDeleteFramebuffers(1, &m_FBO);
        if (m_RBO != 0) glDeleteRenderbuffers(1, &m_RBO);

        m_Width = width;
        m_Height = height;
        m_ColorTextures.clear();
        m_DepthTexture.reset();

        glGenFramebuffers(1, &m_FBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);

        std::vector<GLenum> attachments;

        for (size_t i = 0; i < colorFormats.size(); ++i) {
            auto tex = std::make_unique<GpuTexture>();   // <-- era Texture
            tex->CreateEmpty(width, height, colorFormats[i], filter);
            tex->Bind(0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                GL_TEXTURE_2D, tex->GetID(), 0);
            attachments.push_back(GL_COLOR_ATTACHMENT0 + i);
            m_ColorTextures.push_back(std::move(tex));
        }

        if (attachments.size() > 1) {
            glDrawBuffers((GLsizei)attachments.size(), attachments.data());
        }
        else if (attachments.empty()) {
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);
        }

        if (hasDepth) {
            m_DepthTexture = std::make_unique<GpuTexture>();  // <-- era Texture
            m_DepthTexture->CreateEmpty(width, height, TextureFormat::Depth24,
                TextureFilter::Nearest);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                GL_TEXTURE_2D, m_DepthTexture->GetID(), 0);
        }

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            SUN_CORE_ERROR("Framebuffer incompleto! Status: 0x{0:x}", status);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void Framebuffer::Resize(int width, int height) {
        if (width == m_Width && height == m_Height) return;

        std::vector<TextureFormat> formats;
        for (auto& tex : m_ColorTextures) {
            formats.push_back(tex->GetFormat());
        }

        bool hadDepth = (m_DepthTexture != nullptr);

        Create(width, height, formats, hadDepth);
    }

    void Framebuffer::Bind() const {
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glViewport(0, 0, m_Width, m_Height);
    }

    void Framebuffer::Unbind() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void Framebuffer::BindColorTexture(int index, int unit) const {
        if (index >= 0 && index < (int)m_ColorTextures.size()) {
            m_ColorTextures[index]->Bind(unit);
        }
    }

    void Framebuffer::BindDepthTexture(int unit) const {
        if (m_DepthTexture) {
            m_DepthTexture->Bind(unit);
        }
    }

    void Framebuffer::Clear(const glm::vec4& color) {
        Bind();

        bool hasColor = !m_ColorTextures.empty();
        if (hasColor) {
            glClearColor(color.r, color.g, color.b, color.a);
        }
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // NON chiamare Unbind() qui: chi chiama Clear si aspetta che il FBO
        // resti bindato per poterci disegnare sopra subito dopo.
    }

    GLuint Framebuffer::GetFullscreenQuadVAO() {
        if (s_FullscreenQuadVAO == 0) {
            float quadVertices[] = {
                // pos            // uv
                -1.0f,  1.0f,     0.0f, 1.0f,
                -1.0f, -1.0f,     0.0f, 0.0f,
                 1.0f, -1.0f,     1.0f, 0.0f,
                -1.0f,  1.0f,     0.0f, 1.0f,
                 1.0f, -1.0f,     1.0f, 0.0f,
                 1.0f,  1.0f,     1.0f, 1.0f
            };

            glGenVertexArrays(1, &s_FullscreenQuadVAO);
            glGenBuffers(1, &s_FullscreenQuadVBO);

            glBindVertexArray(s_FullscreenQuadVAO);
            glBindBuffer(GL_ARRAY_BUFFER, s_FullscreenQuadVBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

            glBindVertexArray(0);
        }
        return s_FullscreenQuadVAO;
    }

    void Framebuffer::DrawFullscreenQuad() const {
        glBindVertexArray(GetFullscreenQuadVAO());
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }

}