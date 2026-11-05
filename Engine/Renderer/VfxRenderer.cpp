#include "VfxRenderer.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>

namespace SunEngine {

    unsigned int VfxRenderer::s_VAO = 0, VfxRenderer::s_VBO = 0, VfxRenderer::s_Program = 0;
    int VfxRenderer::s_VPLoc = -1;
    glm::mat4 VfxRenderer::s_ViewProj(1.0f);
    glm::vec3 VfxRenderer::s_CamRight(1, 0, 0), VfxRenderer::s_CamUp(0, 1, 0), VfxRenderer::s_CamPos(0.0f);
    std::vector<VfxRenderer::Vertex> VfxRenderer::s_Additive, VfxRenderer::s_Alpha;

    static unsigned int CompileVfx(unsigned int type, const char* src) {
        const unsigned int s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        return s;
    }

    void VfxRenderer::Init() {
        if (s_Program) return;
        const char* vs = R"(
            #version 330 core
            layout (location = 0) in vec3 aPos;
            layout (location = 1) in vec2 aUV;
            layout (location = 2) in vec4 aColor;
            layout (location = 3) in float aShape;
            layout (location = 4) in float aSeed;
            uniform mat4 u_VP;
            out vec2 vUV; out vec4 vColor; flat out int vShape; out float vSeed;
            void main() {
                vUV = aUV; vColor = aColor; vShape = int(aShape + 0.5); vSeed = aSeed;
                gl_Position = u_VP * vec4(aPos, 1.0);
            }
        )";
        const char* fs = R"(
            #version 330 core
            in vec2 vUV; in vec4 vColor; flat in int vShape; in float vSeed;
            out vec4 FragColor;
            float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
            float noise(vec2 p) {
                vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
                return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
            }
            void main() {
                vec2 q = vUV * 2.0 - 1.0;
                float r = length(q);
                vec3 rgb = vColor.rgb;
                float a = 0.0;
                if (vShape == 0) {            // bagliore
                    a = exp(-r * r * 4.5);
                    float core = exp(-r * r * 30.0);
                    rgb = mix(rgb, vec3(1.0), core * 0.7);
                    a = clamp(a + core * 0.5, 0.0, 1.0);
                } else if (vShape == 1) {     // striscia luminosa: uv.y attraverso
                    float across = abs(q.y);
                    float ends = smoothstep(0.0, 0.08, vUV.x) * smoothstep(1.0, 0.92, vUV.x);
                    float core = exp(-across * across * 40.0);
                    a = (exp(-across * across * 5.0) * 0.75 + core) * ends;
                    rgb = mix(rgb, vec3(1.0), core * 0.75);
                } else if (vShape == 2) {     // fumo
                    float n = noise(q * 2.5 + vSeed * 17.0) * 0.6 + noise(q * 5.0 - vSeed * 9.0) * 0.4;
                    a = smoothstep(1.0, 0.15, r + (n - 0.5) * 0.55);
                } else if (vShape == 3) {     // anello
                    float d = (r - 0.78) * 9.0;
                    a = exp(-d * d) * step(r, 1.0);
                    rgb = mix(rgb, vec3(1.0), exp(-d * d * 4.0) * 0.4);
                } else if (vShape == 4) {     // disco
                    a = smoothstep(1.0, 0.75, r);
                } else if (vShape == 5) {     // nastro
                    float across = abs(q.y);
                    float core = exp(-across * across * 18.0);
                    a = smoothstep(1.0, 0.25, across) * 0.65 + core * 0.6;
                    rgb = mix(rgb, vec3(1.0), core * 0.5);
                } else {                      // fiamma
                    float n = noise(q * 3.0 + vec2(vSeed * 13.0, -vSeed * 7.0));
                    a = smoothstep(1.0, 0.1, r + (n - 0.5) * 0.7);
                    rgb = mix(rgb, vec3(1.0, 0.95, 0.7), exp(-r * r * 6.0) * 0.6);
                }
                a *= vColor.a;
                if (a < 0.003) discard;
                FragColor = vec4(rgb, a);
            }
        )";
        const unsigned int v = CompileVfx(GL_VERTEX_SHADER, vs), f = CompileVfx(GL_FRAGMENT_SHADER, fs);
        s_Program = glCreateProgram();
        glAttachShader(s_Program, v); glAttachShader(s_Program, f);
        glLinkProgram(s_Program);
        glDeleteShader(v); glDeleteShader(f);
        s_VPLoc = glGetUniformLocation(s_Program, "u_VP");

        glGenVertexArrays(1, &s_VAO);
        glGenBuffers(1, &s_VBO);
        glBindVertexArray(s_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, s_VBO);
        const GLsizei stride = sizeof(Vertex);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, pos));   glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, uv));    glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, color)); glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, shape)); glEnableVertexAttribArray(3);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, seed));  glEnableVertexAttribArray(4);
        glBindVertexArray(0);
    }

    void VfxRenderer::Shutdown() {
        if (s_VBO) glDeleteBuffers(1, &s_VBO);
        if (s_VAO) glDeleteVertexArrays(1, &s_VAO);
        if (s_Program) glDeleteProgram(s_Program);
        s_VBO = s_VAO = s_Program = 0;
    }

    bool VfxRenderer::Ready() { return s_Program != 0; }

    void VfxRenderer::Begin(const glm::mat4& view, const glm::mat4& viewProj) {
        s_ViewProj = viewProj;
        // Righe della view = assi della camera in coordinate mondo
        s_CamRight = glm::vec3(view[0][0], view[1][0], view[2][0]);
        s_CamUp = glm::vec3(view[0][1], view[1][1], view[2][1]);
        const glm::mat4 inv = glm::inverse(view);
        s_CamPos = glm::vec3(inv[3]);
        s_Additive.clear(); s_Alpha.clear();
    }

    void VfxRenderer::Quad(std::vector<Vertex>& out, const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
        const glm::vec4& c0, const glm::vec4& c1, const glm::vec4& c2, const glm::vec4& c3, float shape, float seed,
        const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec2& uv2, const glm::vec2& uv3) {
        out.push_back({ p0, uv0, c0, shape, seed }); out.push_back({ p1, uv1, c1, shape, seed }); out.push_back({ p2, uv2, c2, shape, seed });
        out.push_back({ p0, uv0, c0, shape, seed }); out.push_back({ p2, uv2, c2, shape, seed }); out.push_back({ p3, uv3, c3, shape, seed });
    }

    void VfxRenderer::Billboard(const glm::vec3& pos, float size, const glm::vec4& color, Shape shape, bool additive, float rotation, float seed) {
        if (size <= 0.0f || color.a <= 0.002f) return;
        const float c = std::cos(rotation), s = std::sin(rotation);
        const glm::vec3 r = (s_CamRight * c + s_CamUp * s) * (size * 0.5f);
        const glm::vec3 u = (-s_CamRight * s + s_CamUp * c) * (size * 0.5f);
        Quad(additive ? s_Additive : s_Alpha, pos - r - u, pos + r - u, pos + r + u, pos - r + u, color, color, color, color,
            static_cast<float>(shape), seed);
    }

    void VfxRenderer::Flat(const glm::vec3& pos, float size, const glm::vec4& color, Shape shape, bool additive, float rotation, float seed) {
        if (size <= 0.0f || color.a <= 0.002f) return;
        const float c = std::cos(rotation), s = std::sin(rotation);
        const glm::vec3 r = glm::vec3(c, 0.0f, s) * (size * 0.5f), u = glm::vec3(-s, 0.0f, c) * (size * 0.5f);
        const glm::vec3 p = pos + glm::vec3(0.0f, 0.03f, 0.0f);   // appena sopra il terreno
        Quad(additive ? s_Additive : s_Alpha, p - r - u, p + r - u, p + r + u, p - r + u, color, color, color, color, static_cast<float>(shape), seed);
    }

    void VfxRenderer::Streak(const glm::vec3& a, const glm::vec3& b, float width, const glm::vec4& colA, const glm::vec4& colB, Shape shape, bool additive) {
        glm::vec3 d = b - a;
        const float len = glm::length(d);
        if (len < 1e-4f || width <= 0.0f) return;
        d /= len;
        glm::vec3 toCam = s_CamPos - (a + b) * 0.5f;
        glm::vec3 side = glm::cross(d, toCam);
        if (glm::length(side) < 1e-5f) side = s_CamRight;
        side = glm::normalize(side) * (width * 0.5f);
        // Estremi allungati di mezza larghezza: le punte arrotondate non vengono tagliate
        const glm::vec3 a2 = a - d * (width * 0.5f), b2 = b + d * (width * 0.5f);
        Quad(additive ? s_Additive : s_Alpha, a2 - side, b2 - side, b2 + side, a2 + side, colA, colB, colB, colA, static_cast<float>(shape), 0.0f,
            { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 });
    }

    void VfxRenderer::RibbonStrip(const std::vector<glm::vec3>& pts, const std::vector<float>& widths, const std::vector<glm::vec4>& colors, Shape shape, bool additive) {
        const size_t n = pts.size();
        if (n < 2 || widths.size() < n || colors.size() < n) return;
        auto& out = additive ? s_Additive : s_Alpha;
        std::vector<glm::vec3> sides(n);
        for (size_t i = 0; i < n; ++i) {
            const glm::vec3 t = pts[std::min(i + 1, n - 1)] - pts[i > 0 ? i - 1 : 0];
            glm::vec3 side = glm::cross(t, s_CamPos - pts[i]);
            if (glm::length(side) < 1e-6f) side = s_CamUp;
            sides[i] = glm::normalize(side) * (widths[i] * 0.5f);
        }
        for (size_t i = 0; i + 1 < n; ++i) {
            const float u0 = static_cast<float>(i) / static_cast<float>(n - 1), u1 = static_cast<float>(i + 1) / static_cast<float>(n - 1);
            Quad(out, pts[i] - sides[i], pts[i + 1] - sides[i + 1], pts[i + 1] + sides[i + 1], pts[i] + sides[i],
                colors[i], colors[i + 1], colors[i + 1], colors[i], static_cast<float>(shape), 0.0f,
                { u0, 0 }, { u1, 0 }, { u1, 1 }, { u0, 1 });
        }
    }

    void VfxRenderer::Flush(std::vector<Vertex>& verts, bool additive) {
        if (verts.empty()) return;
        glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        glBindBuffer(GL_ARRAY_BUFFER, s_VBO);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(Vertex)), verts.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));
    }

    void VfxRenderer::End() {
        if (!s_Program || (s_Additive.empty() && s_Alpha.empty())) return;
        GLboolean depthMask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        const GLboolean blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE), depthTest = glIsEnabled(GL_DEPTH_TEST);
        glEnable(GL_BLEND); glDisable(GL_CULL_FACE); glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE); glDepthFunc(GL_LEQUAL);
        glUseProgram(s_Program);
        glUniformMatrix4fv(s_VPLoc, 1, GL_FALSE, glm::value_ptr(s_ViewProj));
        glBindVertexArray(s_VAO);
        Flush(s_Alpha, false);      // prima fumo e sangue, poi la luce sopra
        Flush(s_Additive, true);
        glBindVertexArray(0);
        glDepthMask(depthMask); glDepthFunc(GL_LESS);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (!blend) glDisable(GL_BLEND);
        if (cull) glEnable(GL_CULL_FACE);
        if (!depthTest) glDisable(GL_DEPTH_TEST);
        s_Additive.clear(); s_Alpha.clear();
    }

}
