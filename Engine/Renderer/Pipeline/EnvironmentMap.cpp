#include "EnvironmentMap.h"
#include "../Core/Shader.h"
#include "../Core/Framebuffer.h"
#include "../../Core/Logger/Log.h"
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace SunEngine {

    namespace {
        const char* kQuadVert = R"(
#version 410 core
layout (location = 0) in vec2 a_Position;
layout (location = 1) in vec2 a_TexCoord;
out vec2 v_TexCoord;
void main() { v_TexCoord = a_TexCoord; gl_Position = vec4(a_Position, 0.0, 1.0); }
)";
        // direzione del texel di una faccia della cubemap (convenzione di OpenGL)
        const char* kCubeCommon = R"(
#version 410 core
in vec2 v_TexCoord;
out vec4 out_Color;
uniform int u_Face;
vec3 FaceDir(int f, vec2 uv) {
    vec2 p = uv * 2.0 - 1.0;
    if (f == 0) return normalize(vec3(1.0, -p.y, -p.x));
    if (f == 1) return normalize(vec3(-1.0, -p.y, p.x));
    if (f == 2) return normalize(vec3(p.x, 1.0, p.y));
    if (f == 3) return normalize(vec3(p.x, -1.0, -p.y));
    if (f == 4) return normalize(vec3(p.x, -p.y, 1.0));
    return normalize(vec3(-p.x, -p.y, -1.0));
}
#define PI 3.14159265359
)";
        const char* kToCubeFrag = R"(
uniform sampler2D u_Equirect;
void main() {
    vec3 d = FaceDir(u_Face, v_TexCoord);
    vec2 uv = vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5);
    out_Color = vec4(textureLod(u_Equirect, uv, 0.0).rgb, 1.0);
}
)";
        // Prefiltro GGX con campionamento d'importanza e lettura da un mipmap scelto dalla densita' del campione
        // (Karis 2013, "filtered importance sampling"): niente puntini anche con pochi campioni
        const char* kPrefilterFrag = R"(
uniform samplerCube u_Sky;
uniform float u_Roughness;
uniform float u_SrcSize;
float RadicalInverse(uint b) {
    b = (b << 16u) | (b >> 16u);
    b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
    b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
    b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
    b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
    return float(b) * 2.3283064365386963e-10;
}
void main() {
    vec3 N = FaceDir(u_Face, v_TexCoord);
    vec3 V = N;
    float a = u_Roughness * u_Roughness;
    vec3 up = abs(N.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 T = normalize(cross(up, N)); vec3 B = cross(N, T);
    const uint COUNT = 96u;
    vec3 acc = vec3(0.0); float w = 0.0;
    for (uint i = 0u; i < COUNT; ++i) {
        vec2 Xi = vec2(float(i) / float(COUNT), RadicalInverse(i));
        float phi = 2.0 * PI * Xi.x;
        float cosT = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
        float sinT = sqrt(1.0 - cosT * cosT);
        vec3 H = normalize(T * cos(phi) * sinT + B * sin(phi) * sinT + N * cosT);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = dot(N, L);
        if (NdotL > 0.0) {
            float NdotH = max(dot(N, H), 0.0);
            float d = NdotH * NdotH * (a * a - 1.0) + 1.0;
            float D = a * a / (PI * d * d);
            float pdf = D * NdotH / (4.0 * NdotH) + 1e-4;
            float saTexel = 4.0 * PI / (6.0 * u_SrcSize * u_SrcSize);
            float saSample = 1.0 / (float(COUNT) * pdf + 1e-4);
            float mip = u_Roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel) + 1.0;
            acc += textureLod(u_Sky, L, mip).rgb * NdotL;
            w += NdotL;
        }
    }
    out_Color = vec4(acc / max(w, 1e-4), 1.0);
}
)";

        GLuint MakeCube(int size, bool mips) {
            GLuint t = 0;
            glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_CUBE_MAP, t);
            const int levels = mips ? static_cast<int>(std::floor(std::log2(static_cast<float>(size)))) + 1 : 1;
            for (int l = 0; l < levels; ++l) {
                const int s = std::max(1, size >> l);
                for (int f = 0; f < 6; ++f)
                    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, l, GL_RGB16F, s, s, 0, GL_RGB, GL_FLOAT, nullptr);
            }
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, levels - 1);
            return t;
        }
    }

    EnvironmentMap::~EnvironmentMap() { Release(); }

    void EnvironmentMap::Release() {
        if (m_Equirect) glDeleteTextures(1, &m_Equirect);
        if (m_SkyCube) glDeleteTextures(1, &m_SkyCube);
        if (m_SpecCube) glDeleteTextures(1, &m_SpecCube);
        if (m_FBO) glDeleteFramebuffers(1, &m_FBO);
        m_Equirect = m_SkyCube = m_SpecCube = m_FBO = 0;
    }

    bool EnvironmentMap::Load(const std::string& path) {
        Release();
        int w = 0, h = 0, c = 0;
        stbi_set_flip_vertically_on_load(1);
        float* px = stbi_loadf(path.c_str(), &w, &h, &c, 3);
        stbi_set_flip_vertically_on_load(0);
        if (!px) { SUN_CORE_WARN("Cielo: HDRI {0} non caricato", path); return false; }
        m_Path = path;

        // Armoniche sferiche (irradianza, Ramamoorthi-Hanrahan): somma pesata per l'angolo solido di ogni texel
        for (auto& s : m_SH) s = glm::vec3(0.0f);
        const int step = std::max(1, w / 256);
        float wsum = 0.0f;
        for (int y = 0; y < h; y += step) {
            const float v = (y + 0.5f) / h;                  // 0 = in basso (immagine capovolta)
            const float theta = (v - 0.5f) * 3.14159265f;    // elevazione
            const float cosEl = std::cos(theta);
            for (int x = 0; x < w; x += step) {
                const float u = (x + 0.5f) / w;
                const float phi = (u - 0.5f) * 2.0f * 3.14159265f;
                const glm::vec3 d(std::cos(phi) * cosEl, std::sin(theta), std::sin(phi) * cosEl);
                const float* p = px + (static_cast<size_t>(y) * w + x) * 3;
                const glm::vec3 col(std::min(p[0], 64.0f), std::min(p[1], 64.0f), std::min(p[2], 64.0f));
                const float sa = cosEl;
                const float b[9] = { 0.282095f, 0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x,
                                     1.092548f * d.x * d.y, 1.092548f * d.y * d.z, 0.315392f * (3.0f * d.z * d.z - 1.0f),
                                     1.092548f * d.x * d.z, 0.546274f * (d.x * d.x - d.y * d.y) };
                for (int k = 0; k < 9; ++k) m_SH[k] += col * b[k] * sa;
                wsum += sa;
            }
        }
        for (auto& s : m_SH) s *= 4.0f * 3.14159265f / std::max(wsum, 1e-6f);

        glGenTextures(1, &m_Equirect);
        glBindTexture(GL_TEXTURE_2D, m_Equirect);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, w, h, 0, GL_RGB, GL_FLOAT, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        stbi_image_free(px);

        if (!m_ToCube) {
            m_ToCube = std::make_unique<Shader>();
            m_Prefilter = std::make_unique<Shader>();
            if (!m_ToCube->LoadFromSource(kQuadVert, std::string(kCubeCommon) + kToCubeFrag, "EnvToCube") ||
                !m_Prefilter->LoadFromSource(kQuadVert, std::string(kCubeCommon) + kPrefilterFrag, "EnvPrefilter")) {
                SUN_CORE_WARN("Cielo: shader della cubemap non compilati");
                Release(); return false;
            }
        }
        GLint prevFbo = 0, vp[4];
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glGetIntegerv(GL_VIEWPORT, vp);
        glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND);
        glGenFramebuffers(1, &m_FBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glBindVertexArray(Framebuffer::GetFullscreenQuadVAO());

        // 1. cielo: equirettangolare -> cubemap, poi tutti i mipmap
        m_SkyCube = MakeCube(kSkySize, true);
        m_ToCube->Bind();
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_Equirect);
        m_ToCube->SetInt("u_Equirect", 0);
        glViewport(0, 0, kSkySize, kSkySize);
        for (int f = 0; f < 6; ++f) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, m_SkyCube, 0);
            m_ToCube->SetInt("u_Face", f);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        glBindTexture(GL_TEXTURE_CUBE_MAP, m_SkyCube);
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

        // 2. riflessi prefiltrati per ruvidita'
        m_SpecCube = MakeCube(kSpecSize, false);
        glBindTexture(GL_TEXTURE_CUBE_MAP, m_SpecCube);
        for (int l = 1; l < kSpecLevels; ++l) {
            const int s = std::max(1, kSpecSize >> l);
            for (int f = 0; f < 6; ++f) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, l, GL_RGB16F, s, s, 0, GL_RGB, GL_FLOAT, nullptr);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, kSpecLevels - 1);
        m_Prefilter->Bind();
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_CUBE_MAP, m_SkyCube);
        m_Prefilter->SetInt("u_Sky", 0);
        m_Prefilter->SetFloat("u_SrcSize", static_cast<float>(kSkySize));
        for (int l = 0; l < kSpecLevels; ++l) {
            const int s = std::max(1, kSpecSize >> l);
            glViewport(0, 0, s, s);
            m_Prefilter->SetFloat("u_Roughness", static_cast<float>(l) / (kSpecLevels - 1));
            for (int f = 0; f < 6; ++f) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, m_SpecCube, l);
                m_Prefilter->SetInt("u_Face", f);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        glDeleteTextures(1, &m_Equirect); m_Equirect = 0;
        SUN_CORE_INFO("Cielo fotografico: {0} ({1}x{2})", path, w, h);
        return true;
    }
}
