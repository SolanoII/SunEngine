#include "Texture.h"
#include "../../Core/Logger/Log.h"

#include <stb_image.h>
#include <algorithm>

namespace SunEngine {

    GpuTexture::~GpuTexture() {
        if (m_ID != 0) glDeleteTextures(1, &m_ID);
    }

    GpuTexture::GpuTexture(GpuTexture&& other) noexcept
        : m_ID(other.m_ID), m_Width(other.m_Width),
        m_Height(other.m_Height), m_Format(other.m_Format) {
        other.m_ID = 0;
    }

    GpuTexture& GpuTexture::operator=(GpuTexture&& other) noexcept {
        if (this != &other) {
            if (m_ID != 0) glDeleteTextures(1, &m_ID);
            m_ID = other.m_ID;
            m_Width = other.m_Width;
            m_Height = other.m_Height;
            m_Format = other.m_Format;
            other.m_ID = 0;
        }
        return *this;
    }

    GLenum GpuTexture::ToGLInternalFormat(TextureFormat fmt) {
        switch (fmt) {
        case TextureFormat::R8:           return GL_R8;
        case TextureFormat::RG8:          return GL_RG8;
        case TextureFormat::RGBA8:        return GL_RGBA8;
        case TextureFormat::RGBA16F:      return GL_RGBA16F;
        case TextureFormat::RGB16F:       return GL_RGB16F;
        case TextureFormat::RG16F:        return GL_RG16F;
        case TextureFormat::R16F:         return GL_R16F;
        case TextureFormat::Depth24:      return GL_DEPTH_COMPONENT24;
        case TextureFormat::Depth24Stencil8: return GL_DEPTH24_STENCIL8;
        }
        return GL_RGBA8;
    }

    GLenum GpuTexture::ToGLFormat(TextureFormat fmt) {
        switch (fmt) {
        case TextureFormat::R8:
        case TextureFormat::R16F:         return GL_RED;
        case TextureFormat::RG8:
        case TextureFormat::RG16F:        return GL_RG;
        case TextureFormat::RGBA8:
        case TextureFormat::RGBA16F:      return GL_RGBA;
        case TextureFormat::RGB16F:       return GL_RGB;
        case TextureFormat::Depth24:      return GL_DEPTH_COMPONENT;
        case TextureFormat::Depth24Stencil8: return GL_DEPTH_STENCIL;
        }
        return GL_RGBA;
    }

    GLenum GpuTexture::ToGLType(TextureFormat fmt) {
        switch (fmt) {
        case TextureFormat::RGBA16F:
        case TextureFormat::RGB16F:
        case TextureFormat::RG16F:
        case TextureFormat::R16F:         return GL_FLOAT;
        case TextureFormat::Depth24Stencil8: return GL_UNSIGNED_INT_24_8;
        default:                          return GL_UNSIGNED_BYTE;
        }
    }

    void GpuTexture::CreateEmpty(int width, int height, TextureFormat format,
        TextureFilter filter, TextureWrap wrap) {
        if (m_ID != 0) glDeleteTextures(1, &m_ID);

        m_Width = width;
        m_Height = height;
        m_Format = format;

        glGenTextures(1, &m_ID);
        glBindTexture(GL_TEXTURE_2D, m_ID);

        glTexImage2D(
            GL_TEXTURE_2D, 0,
            ToGLInternalFormat(format),
            width, height, 0,
            ToGLFormat(format),
            ToGLType(format),
            nullptr
        );

        GLenum glFilter = (filter == TextureFilter::Nearest) ? GL_NEAREST :
            (filter == TextureFilter::LinearMipmapLinear) ? GL_LINEAR_MIPMAP_LINEAR :
            GL_LINEAR;
        GLenum glWrap = (wrap == TextureWrap::Repeat) ? GL_REPEAT :
            (wrap == TextureWrap::ClampToBorder) ? GL_CLAMP_TO_BORDER :
            GL_CLAMP_TO_EDGE;

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
            (filter == TextureFilter::Nearest) ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrap);

        if (wrap == TextureWrap::ClampToBorder) {
            float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
            glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
        }

        glBindTexture(GL_TEXTURE_2D, 0);
    }

    bool GpuTexture::LoadFromFile(const std::string& path, bool flipVertically, bool srgb) {
        stbi_set_flip_vertically_on_load(flipVertically);

        int w, h, channels;
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);

        if (!data) {
            SUN_CORE_ERROR("GpuTexture::LoadFromFile - fallito caricamento: {0}", path);
            return false;
        }

        m_Width = w;
        m_Height = h;
        m_Format = TextureFormat::RGBA8;

        if (m_ID != 0) glDeleteTextures(1, &m_ID);
        glGenTextures(1, &m_ID);
        glBindTexture(GL_TEXTURE_2D, m_ID);

        GLenum internalFmt = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;

        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        GLfloat maxAniso = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
        if (maxAniso > 0.0f) {
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, std::min(maxAniso, 8.0f));
        }

        glBindTexture(GL_TEXTURE_2D, 0);
        stbi_image_free(data);

        SUN_CORE_INFO("Texture caricata: {0} ({1}x{2})", path, w, h);
        return true;
    }

    void GpuTexture::CreateFromData(int width, int height, int channels,
        const unsigned char* data,
        TextureFilter filter, bool srgb) {
        if (m_ID != 0) glDeleteTextures(1, &m_ID);

        m_Width = width;
        m_Height = height;

        GLenum fmt = (channels == 1) ? GL_RED :
            (channels == 3) ? GL_RGB : GL_RGBA;
        GLenum internalFmt = srgb ? GL_SRGB8_ALPHA8
            : (channels == 1 ? GL_R8
                : (channels == 3 ? GL_RGB8 : GL_RGBA8));

        glGenTextures(1, &m_ID);
        glBindTexture(GL_TEXTURE_2D, m_ID);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, width, height, 0, fmt, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);

        GLenum glFilter = (filter == TextureFilter::Nearest) ? GL_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void GpuTexture::GenerateMipmaps() {
        glBindTexture(GL_TEXTURE_2D, m_ID);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void GpuTexture::Bind(int unit) const {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, m_ID);
    }

}