#include "DeferredRenderer.h"
#include "../../Core/Logger/Log.h"
#include "../../Renderer/Camera.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace SunEngine {

    // ============================================================
    // SHADERS (inline per ora; in Fase 2 li spostiamo in file .glsl)
    // ============================================================

    static const char* k_GBufferVertSrc = R"(
        #version 410 core
        layout (location = 0) in vec3 a_Position;
        layout (location = 1) in vec3 a_Normal;

        uniform mat4 u_Model;
        uniform mat4 u_ViewProjection;

        out vec3 v_WorldPos;
        out vec3 v_Normal;

        void main() {
            vec4 worldPos = u_Model * vec4(a_Position, 1.0);
            v_WorldPos = worldPos.xyz;
            mat3 normalMatrix = transpose(inverse(mat3(u_Model)));
            v_Normal = normalize(normalMatrix * a_Normal);
            gl_Position = u_ViewProjection * worldPos;
        }
    )";

    static const char* k_GBufferFragSrc = R"(
        #version 410 core
        in vec3 v_WorldPos;
        in vec3 v_Normal;

        layout (location = 0) out vec4 out_Albedo;
        layout (location = 1) out vec4 out_Normal;
        layout (location = 2) out vec4 out_Material;

        uniform vec4  u_Color;
        uniform float u_Roughness;
        uniform float u_Metallic;
        uniform float u_Emissive;

        void main() {
            out_Albedo   = vec4(u_Color.rgb, u_Color.a);
            out_Normal   = vec4(normalize(v_Normal), 1.0);
            out_Material = vec4(u_Roughness, u_Metallic, u_Emissive, 0.0);
        }
    )";

    static const char* k_LightingVertSrc = R"(
        #version 410 core
        layout (location = 0) in vec2 a_Position;
        layout (location = 1) in vec2 a_TexCoord;
        out vec2 v_TexCoord;
        void main() {
            v_TexCoord = a_TexCoord;
            gl_Position = vec4(a_Position, 0.0, 1.0);
        }
    )";

    static const char* k_LightingFragSrc = R"(
        #version 410 core
        in vec2 v_TexCoord;
        out vec4 out_Color;

        uniform sampler2D u_GBufferAlbedo;
        uniform sampler2D u_GBufferNormal;
        uniform sampler2D u_GBufferMaterial;
        uniform sampler2D u_GBufferDepth;

        uniform vec3 u_CameraPos;
        uniform mat4 u_InvViewProjection;

        #define MAX_LIGHTS 64
        struct Light {
            vec4 positionRange;
            vec4 directionCone;
            vec4 colorIntensity;
            ivec4 flags;
        };
        layout (std140) uniform LightBlock {
            Light u_Lights[MAX_LIGHTS];
        };
        uniform int u_LightCount;

        vec3 ReconstructWorldPos(vec2 uv, float depth) {
            vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
            vec4 wp = u_InvViewProjection * ndc;
            return wp.xyz / wp.w;
        }

        void main() {
            vec4 albedoSample   = texture(u_GBufferAlbedo,   v_TexCoord);
            vec4 normalSample   = texture(u_GBufferNormal,   v_TexCoord);
            vec4 materialSample = texture(u_GBufferMaterial, v_TexCoord);
            float depth         = texture(u_GBufferDepth,    v_TexCoord).r;

            // Sfondo
            if (depth >= 0.9999) {
                out_Color = vec4(0.015, 0.015, 0.025, 1.0);
                return;
            }

            vec3  albedo    = albedoSample.rgb;
            float ao        = albedoSample.a;
            vec3  N         = normalize(normalSample.rgb);
            float roughness = materialSample.r;
            float metallic  = materialSample.g;
            float emissive  = materialSample.b;

            vec3 worldPos = ReconstructWorldPos(v_TexCoord, depth);
            vec3 V = normalize(u_CameraPos - worldPos);

            vec3 F0 = mix(vec3(0.04), albedo, metallic);
            float shininess = mix(2.0, 512.0, 1.0 - roughness);

            vec3 accum = vec3(0.0);

            for (int i = 0; i < u_LightCount; ++i) {
                int type = u_Lights[i].flags.x;

                vec3 L;
                float attenuation = 1.0;
                float spotFactor = 1.0;

                if (type == 0) {
                    L = normalize(-u_Lights[i].directionCone.xyz);
                }
                else {
                    vec3 toLight = u_Lights[i].positionRange.xyz - worldPos;
                    float dist = length(toLight);
                    L = toLight / max(dist, 0.0001);

                    float range = max(u_Lights[i].positionRange.w, 0.001);
                    float ratio = dist / range;
                    attenuation = clamp(1.0 - ratio * ratio, 0.0, 1.0);
                    attenuation *= attenuation;

                    if (type == 2) {
                        float cosA = dot(-L, normalize(u_Lights[i].directionCone.xyz));
                        float outer = u_Lights[i].directionCone.w;
                        spotFactor = clamp((cosA - outer) / max(1.0 - outer, 0.001), 0.0, 1.0);
                    }
                }

                float NdotL = max(dot(N, L), 0.0);
                if (NdotL <= 0.0) continue;

                vec3 diffuse = albedo * NdotL * (1.0 - metallic);

                vec3 H = normalize(L + V);
                float NdotH = max(dot(N, H), 0.0);
                float spec = pow(NdotH, shininess);
                vec3 specular = F0 * spec * NdotL;

                accum += (diffuse + specular)
                       * u_Lights[i].colorIntensity.rgb
                       * u_Lights[i].colorIntensity.w
                       * attenuation * spotFactor;
            }

            vec3 ambient = albedo * 0.04 * ao;
            accum += ambient;
            accum += albedo * emissive;

            out_Color = vec4(accum, 1.0);
        }
    )";

    static const char* k_TonemapFragSrc = R"(
        #version 410 core
        in vec2 v_TexCoord;
        out vec4 out_Color;

        uniform sampler2D u_HDRTexture;
        uniform float u_Exposure;

        vec3 ACESFilm(vec3 x) {
            const float a = 2.51;
            const float b = 0.03;
            const float c = 2.43;
            const float d = 0.59;
            const float e = 0.14;
            return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
        }

        void main() {
            vec3 hdr = texture(u_HDRTexture, v_TexCoord).rgb * u_Exposure;
            vec3 ldr = ACESFilm(hdr);
            ldr = pow(ldr, vec3(1.0 / 2.2));
            out_Color = vec4(ldr, 1.0);
        }
    )";

    // ============================================================
    // Impl
    // ============================================================

    bool DeferredRenderer::Init(int width, int height) {
        m_Width = width;
        m_Height = height;

        m_GBuffer.Create(width, height);

        // HDR target con un solo attachment RGBA16F
        m_HDRTarget.Create(width, height, { TextureFormat::RGBA16F },
            /*hasDepth*/ false, TextureFilter::Linear);

        // Shaders
        m_GBufferShader = std::make_unique<Shader>();
        if (!m_GBufferShader->LoadFromSource(k_GBufferVertSrc, k_GBufferFragSrc, "GBuffer")) {
            SUN_CORE_ERROR("DeferredRenderer: gbuffer shader fallito");
            return false;
        }

        m_LightingShader = std::make_unique<Shader>();
        if (!m_LightingShader->LoadFromSource(k_LightingVertSrc, k_LightingFragSrc, "Lighting")) {
            SUN_CORE_ERROR("DeferredRenderer: lighting shader fallito");
            return false;
        }

        m_TonemapShader = std::make_unique<Shader>();
        if (!m_TonemapShader->LoadFromSource(k_LightingVertSrc, k_TonemapFragSrc, "Tonemap")) {
            SUN_CORE_ERROR("DeferredRenderer: tonemap shader fallito");
            return false;
        }

        // UBO per le luci (binding 0)
        glGenBuffers(1, &m_LightUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, m_LightUBO);
        glBufferData(GL_UNIFORM_BUFFER,
            sizeof(Light) * MAX_LIGHTS,
            nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_LightUBO);
        m_LightingShader->BindUniformBlock("LightBlock", 0);

        CreateTestGeometry();

        m_Initialized = true;
        SUN_CORE_INFO("DeferredRenderer inizializzato: {0}x{1}", width, height);
        return true;
    }

    void DeferredRenderer::Shutdown() {
        if (m_LightUBO != 0) {
            glDeleteBuffers(1, &m_LightUBO);
            m_LightUBO = 0;
        }
        if (m_TestCubeVAO != 0) {
            glDeleteVertexArrays(1, &m_TestCubeVAO);
            glDeleteBuffers(1, &m_TestCubeVBO);
            glDeleteBuffers(1, &m_TestCubeEBO);
            m_TestCubeVAO = 0;
        }
        m_GBufferShader.reset();
        m_LightingShader.reset();
        m_TonemapShader.reset();
        m_Initialized = false;
    }

    void DeferredRenderer::Resize(int width, int height) {
        if (width == m_Width && height == m_Height) return;
        m_Width = width;
        m_Height = height;
        m_GBuffer.Resize(width, height);
        m_HDRTarget.Resize(width, height);
    }

    void DeferredRenderer::CreateTestGeometry() {
        // Cube 24 vertici (pos + normal), 36 indici — non posso usare quello
        // del vecchio Renderer perché non ha normali.
        float verts[] = {
            // -Z (back)
            -0.5f,-0.5f,-0.5f,  0, 0,-1,
             0.5f,-0.5f,-0.5f,  0, 0,-1,
             0.5f, 0.5f,-0.5f,  0, 0,-1,
            -0.5f, 0.5f,-0.5f,  0, 0,-1,
            // +Z (front)
            -0.5f,-0.5f, 0.5f,  0, 0, 1,
             0.5f,-0.5f, 0.5f,  0, 0, 1,
             0.5f, 0.5f, 0.5f,  0, 0, 1,
            -0.5f, 0.5f, 0.5f,  0, 0, 1,
            // -X (left)
            -0.5f,-0.5f,-0.5f, -1, 0, 0,
            -0.5f,-0.5f, 0.5f, -1, 0, 0,
            -0.5f, 0.5f, 0.5f, -1, 0, 0,
            -0.5f, 0.5f,-0.5f, -1, 0, 0,
            // +X (right)
             0.5f,-0.5f,-0.5f,  1, 0, 0,
             0.5f,-0.5f, 0.5f,  1, 0, 0,
             0.5f, 0.5f, 0.5f,  1, 0, 0,
             0.5f, 0.5f,-0.5f,  1, 0, 0,
             // -Y (bottom)
             -0.5f,-0.5f,-0.5f,  0,-1, 0,
              0.5f,-0.5f,-0.5f,  0,-1, 0,
              0.5f,-0.5f, 0.5f,  0,-1, 0,
             -0.5f,-0.5f, 0.5f,  0,-1, 0,
             // +Y (top)
             -0.5f, 0.5f,-0.5f,  0, 1, 0,
              0.5f, 0.5f,-0.5f,  0, 1, 0,
              0.5f, 0.5f, 0.5f,  0, 1, 0,
             -0.5f, 0.5f, 0.5f,  0, 1, 0
        };

        unsigned int idx[] = {
            0,1,2, 2,3,0,       // back
            4,5,6, 6,7,4,       // front
            8,9,10, 10,11,8,    // left
            12,13,14, 14,15,12, // right
            16,17,18, 18,19,16, // bottom
            20,21,22, 22,23,20  // top
        };

        glGenVertexArrays(1, &m_TestCubeVAO);
        glGenBuffers(1, &m_TestCubeVBO);
        glGenBuffers(1, &m_TestCubeEBO);

        glBindVertexArray(m_TestCubeVAO);

        glBindBuffer(GL_ARRAY_BUFFER, m_TestCubeVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_TestCubeEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
            (void*)(3 * sizeof(float)));

        glBindVertexArray(0);

        m_TestCubeIndexCount = 36;
    }

    Shader* DeferredRenderer::BeginGeometryPass(const Camera& camera) {
        m_View = glm::mat4(1.0f);
        m_Projection = glm::mat4(1.0f);
        m_CameraPos = glm::vec3(0.0f);
        m_ViewProjection = camera.GetViewProjection();

        // Bind G-Buffer
        m_GBuffer.BindForWriting();
        m_GBuffer.Clear();

        // Depth test attivo per la geometria
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        return m_GBufferShader.get();
    }

    void DeferredRenderer::EndGeometryPassAndPresent(const Camera& camera) {
        // Estraiamo la posizione reale della camera (fix del bug precedente:
        // l'inversa di VP * origine NDC restituisce un punto sulla linea di
        // vista, non la posizione della camera).
        m_CameraPos = camera.GetPosition();

        // Ricaviamo la camera pos dalla view matrix: -R^T * t
        // (per ora basta che il chiamante la passi in un modo semplice)
        // → Strategia alternativa: estrarre dalla view matrix è più pulito,
        //   ma dato che Camera qui è una black box, uso l'inversa di VP e
        //   prendo la posizione "di fronte". Non perfetto ma funziona per test.

        // Upload luci
        UploadLights();

        // 1. Lighting pass
        RunLightingPass();

        // 2. Tonemap pass
        RunTonemapPass();
    }

    void DeferredRenderer::UploadLights() {
        int count = (int)m_Lights.size();
        if (count > MAX_LIGHTS) count = MAX_LIGHTS;

        glBindBuffer(GL_UNIFORM_BUFFER, m_LightUBO);
        if (count > 0) {
            glBufferSubData(GL_UNIFORM_BUFFER, 0, count * sizeof(Light), m_Lights.data());
        }
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        m_LightingShader->Bind();
        m_LightingShader->SetInt("u_LightCount", count);
    }

    void DeferredRenderer::RunLightingPass() {
        m_HDRTarget.Bind();
        m_HDRTarget.Clear(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);

        m_LightingShader->Bind();

        // Bind G-Buffer textures
        m_GBuffer.BindForReading();
        m_LightingShader->SetInt("u_GBufferAlbedo", 0);
        m_LightingShader->SetInt("u_GBufferNormal", 1);
        m_LightingShader->SetInt("u_GBufferMaterial", 2);
        m_LightingShader->SetInt("u_GBufferDepth", 3);

        // Camera uniforms
        m_LightingShader->SetVec3("u_CameraPos", m_CameraPos);

        glm::mat4 invVP = glm::inverse(m_ViewProjection);
        m_LightingShader->SetMat4("u_InvViewProjection", invVP);

        // Fullscreen quad
        glBindVertexArray(Framebuffer::GetFullscreenQuadVAO());
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        m_HDRTarget.Unbind();
    }

    void DeferredRenderer::RunTonemapPass() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_Width, m_Height);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_TonemapShader->Bind();
        m_HDRTarget.BindColorTexture(0, 0);
        m_TonemapShader->SetInt("u_HDRTexture", 0);
        m_TonemapShader->SetFloat("u_Exposure", 1.0f);

        glBindVertexArray(Framebuffer::GetFullscreenQuadVAO());
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }

    void DeferredRenderer::DrawCube(const glm::mat4& transform,
        const glm::vec4& color,
        float roughness, float metallic, float emissive) {
        m_GBufferShader->Bind();
        m_GBufferShader->SetMat4("u_Model", transform);
        m_GBufferShader->SetMat4("u_ViewProjection", m_ViewProjection);
        m_GBufferShader->SetVec4("u_Color", color);
        m_GBufferShader->SetFloat("u_Roughness", roughness);
        m_GBufferShader->SetFloat("u_Metallic", metallic);
        m_GBufferShader->SetFloat("u_Emissive", emissive);

        glBindVertexArray(m_TestCubeVAO);
        glDrawElements(GL_TRIANGLES, m_TestCubeIndexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    void DeferredRenderer::DrawTestGround(float size) {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f));
        t = glm::scale(t, glm::vec3(size, 0.05f, size));
        DrawCube(t, glm::vec4(0.25f, 0.25f, 0.28f, 1.0f), 0.9f, 0.0f);
    }

    void DeferredRenderer::ClearLights() {
        m_Lights.clear();
        m_DirectionalLight = {};
        m_HasDirectional = false;
    }

    void DeferredRenderer::AddLight(const Light& light) {
        if ((int)m_Lights.size() >= MAX_LIGHTS) return;
        m_Lights.push_back(light);
    }

    void DeferredRenderer::SetDirectionalLight(const glm::vec3& direction,
        const glm::vec3& color,
        float intensity) {
        m_DirectionalLight = Light::Directional(direction, color, intensity);
        m_HasDirectional = true;

        if (m_Lights.empty() ||
            m_Lights[0].flags.x != LIGHT_DIRECTIONAL) {
            m_Lights.insert(m_Lights.begin(), m_DirectionalLight);
        }
        else {
            m_Lights[0] = m_DirectionalLight;
        }
    }

}