#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>

namespace SunEngine {

    enum class TextureFormat {
        R8,
        RG8,
        RGBA8,
        RGBA16F,
        RGB16F,
        RG16F,
        R16F,
        Depth24,
        Depth24Stencil8
    };

    enum class TextureFilter {
        Nearest,
        Linear,
        LinearMipmapLinear
    };

    enum class TextureWrap {
        Repeat,
        ClampToEdge,
        ClampToBorder
    };

    // NOTA: nome "GpuTexture" per evitare collisione con la legacy
    // struct Texture in Engine/Renderer/Mesh.h. La legacy verra' rimossa
    // in Fase 2 (asset pipeline).
    class GpuTexture {
    public:
        GpuTexture() = default;
        ~GpuTexture();

        GpuTexture(const GpuTexture&) = delete;
        GpuTexture& operator=(const GpuTexture&) = delete;
        GpuTexture(GpuTexture&& other) noexcept;
        GpuTexture& operator=(GpuTexture&& other) noexcept;

        void CreateEmpty(int width, int height, TextureFormat format,
            TextureFilter filter = TextureFilter::Linear,
            TextureWrap wrap = TextureWrap::ClampToEdge);

        bool LoadFromFile(const std::string& path, bool flipVertically = true,
            bool srgb = false);

        void CreateFromData(int width, int height, int channels,
            const unsigned char* data,
            TextureFilter filter = TextureFilter::LinearMipmapLinear,
            bool srgb = false);

        void GenerateMipmaps();
        void Bind(int unit) const;

        GLuint GetID() const { return m_ID; }
        int GetWidth() const { return m_Width; }
        int GetHeight() const { return m_Height; }
        TextureFormat GetFormat() const { return m_Format; }

    private:
        GLuint m_ID = 0;
        int m_Width = 0;
        int m_Height = 0;
        TextureFormat m_Format = TextureFormat::RGBA8;

        static GLenum ToGLInternalFormat(TextureFormat fmt);
        static GLenum ToGLFormat(TextureFormat fmt);
        static GLenum ToGLType(TextureFormat fmt);
    };

}