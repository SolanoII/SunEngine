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

uniform vec3  u_CameraPos;
uniform mat4  u_InvViewProjection;
uniform float u_AmbientIntensity;
uniform vec3  u_AmbientSkyColor;
uniform vec3  u_AmbientGroundColor;

#define PI 3.14159265359

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

// ============================================================
// SHADOW BLOCK — binding point 1
// ============================================================
layout (std140) uniform ShadowBlock {
    mat4 u_LightSpaceMatrices[4];
};

uniform sampler2D u_ShadowMap0;
uniform sampler2D u_ShadowMap1;
uniform sampler2D u_ShadowMap2;
uniform sampler2D u_ShadowMap3;
uniform float u_ShadowBias;
uniform float u_ShadowNormalBias;
uniform int   u_CascadeCount;
uniform float u_CascadeSplits[4];
uniform float u_CascadeFade;   // 0.0 = nessun blend, 0.15 = blend ultimo 15%
vec3 ReconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = u_InvViewProjection * ndc;
    return wp.xyz / wp.w;
}

// ============================================================
// PBR — Cook-Torrance GGX
// ============================================================
float DistributionGGX(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

float GeometrySchlickGGX(float NdotV, float k) {
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 ComputeAmbient(vec3 N, vec3 V, vec3 albedo, float roughness, float metallic) {
    vec3 irradiance = mix(u_AmbientGroundColor, u_AmbientSkyColor, N.y * 0.5 + 0.5);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 kS = FresnelSchlick(max(dot(N, V), 0.0), F0);
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    vec3 diffuseAmb = kD * albedo * irradiance;

    vec3 R = reflect(-V, N);
    vec3 envColor = mix(u_AmbientGroundColor, u_AmbientSkyColor, R.y * 0.5 + 0.5);
    float fresnelBoost = pow(1.0 - max(dot(N, V), 0.0), 5.0);
    vec3 specAmb = mix(envColor, envColor * 2.0, fresnelBoost) * F0;

    return (diffuseAmb + specAmb) * u_AmbientIntensity;
}

// ============================================================
// SHADOW SAMPLING
// ============================================================
// Seleziona la texture di shadow corretta via branch esplicito.
// Non si possono copiare sampler in variabili locali in GLSL: va fatto
// direttamente dentro la texture() con l'if-chain.
float SampleShadowTex(int cascade, vec2 uv) {
    if (cascade == 0) return texture(u_ShadowMap0, uv).r;
    if (cascade == 1) return texture(u_ShadowMap1, uv).r;
    if (cascade == 2) return texture(u_ShadowMap2, uv).r;
    return texture(u_ShadowMap3, uv).r;
}

float SampleShadowCascade(int cascade, vec3 worldPos, vec3 N, float NdotL) {
    float normalOffsetScale = u_ShadowNormalBias * (1.0 - NdotL);
    vec3 offsetPos = worldPos + N * normalOffsetScale;

    vec4 lightSpace = u_LightSpaceMatrices[cascade] * vec4(offsetPos, 1.0);
    vec3 projCoords = lightSpace.xyz / lightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0) return 1.0;
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) return 1.0;

    float currentDepth = projCoords.z;

    // Tutte le 4 cascade hanno la stessa risoluzione (2048x2048), quindi
    // il texelSize di una vale per tutte. Uso la texture #0 come riferimento.
    vec2 texelSize = 1.0 / vec2(textureSize(u_ShadowMap0, 0));

    float bias = max(u_ShadowBias * (1.0 - NdotL), u_ShadowBias * 0.1);

    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = SampleShadowTex(cascade, projCoords.xy + vec2(x, y) * texelSize);
            shadow += (currentDepth - bias > pcfDepth) ? 0.0 : 1.0;
        }
    }
    shadow /= 9.0;
    return shadow;
}
float GetShadow(vec3 worldPos, vec3 N, float NdotL, float viewDistance) {
    // Trova la cascade primaria
    int cascade = u_CascadeCount - 1;
    for (int i = 0; i < u_CascadeCount; ++i) {
        if (viewDistance <= u_CascadeSplits[i]) {
            cascade = i;
            break;
        }
    }

    float shadowPrimary = SampleShadowCascade(cascade, worldPos, N, NdotL);

    // Blend con la cascade successiva se siamo nell'ultima frazione (u_CascadeFade)
    // dell'intervallo corrente. Elimina lo "scalino" visibile ai bordi.
    if (cascade < u_CascadeCount - 1) {
        float cascadeEnd = u_CascadeSplits[cascade];
        float fadeStart = cascadeEnd * (1.0 - u_CascadeFade);

        if (viewDistance > fadeStart) {
            float t = (viewDistance - fadeStart) / max(cascadeEnd - fadeStart, 0.001);
            t = clamp(t, 0.0, 1.0);

            float shadowNext = SampleShadowCascade(cascade + 1, worldPos, N, NdotL);
            return mix(shadowPrimary, shadowNext, t);
        }
    }

    return shadowPrimary;
}

// ============================================================
// MAIN
// ============================================================
void main() {
    vec4  albedoSample   = texture(u_GBufferAlbedo,   v_TexCoord);
    vec4  normalSample   = texture(u_GBufferNormal,   v_TexCoord);
    vec4  materialSample = texture(u_GBufferMaterial, v_TexCoord);
    float depth          = texture(u_GBufferDepth,    v_TexCoord).r;

    if (depth >= 0.9999) {
        out_Color = vec4(0.01, 0.01, 0.015, 1.0);
        return;
    }

    vec3  albedo    = albedoSample.rgb;
    float ao        = albedoSample.a;
    vec3  N         = normalize(normalSample.rgb);
    float roughness = clamp(materialSample.r, 0.04, 1.0);
    float metallic  = clamp(materialSample.g, 0.0, 1.0);
    float emissive  = materialSample.b;

    vec3 worldPos = ReconstructWorldPos(v_TexCoord, depth);
    vec3 V = normalize(u_CameraPos - worldPos);
    float NdotV = max(dot(N, V), 1e-4);
    float viewDist = length(worldPos - u_CameraPos);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);

    for (int i = 0; i < u_LightCount; ++i) {
        int type = u_Lights[i].flags.x;

        vec3  L;
        float attenuation = 1.0;
        float spotFactor  = 1.0;

        if (type == 0) {
            L = normalize(-u_Lights[i].directionCone.xyz);
        } else {
            vec3 toLight = u_Lights[i].positionRange.xyz - worldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 1e-4);

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

        vec3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        float HdotV = max(dot(H, V), 0.0);

        // Cook-Torrance
        float NDF = DistributionGGX(NdotH, roughness * roughness);
        float G   = GeometrySmith(NdotV, NdotL, roughness);
        vec3  F   = FresnelSchlick(HdotV, F0);

        vec3 numerator    = NDF * G * F;
        float denominator = 4.0 * NdotV * NdotL + 1e-6;
        vec3 specular = numerator / denominator;

        vec3 kS = F;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

        // Shadow SOLO per la direzionale (type == 0)
        float shadowFactor = 1.0;
        if (type == 0) {
            shadowFactor = GetShadow(worldPos, N, NdotL, viewDist);
        }

        vec3 radiance = u_Lights[i].colorIntensity.rgb * u_Lights[i].colorIntensity.w;
        Lo += (kD * albedo / PI + specular) * radiance * NdotL
              * attenuation * spotFactor * shadowFactor;
    }

    vec3 ambient = ComputeAmbient(N, V, albedo, roughness, metallic) * ao;

    vec3 color = ambient + Lo + albedo * emissive;

    out_Color = vec4(color, 1.0);
}
)";

    static const char* k_TonemapFragSrc = R"(
        #version 410 core
in vec2 v_TexCoord;
out vec4 out_Color;

uniform sampler2D u_HDRTexture;
uniform sampler2D u_ExposureTex;   // 1x1 R32F dall'AutoExposure
uniform float u_ExposureFallback;  // usato se auto-exposure disabilitata

vec3 ACESFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    float exposure = texture(u_ExposureTex, vec2(0.5)).r;
    if (exposure < 0.001) exposure = u_ExposureFallback;

    vec3 hdr = texture(u_HDRTexture, v_TexCoord).rgb * exposure;
    vec3 ldr = ACESFilm(hdr);
    ldr = pow(ldr, vec3(1.0 / 2.2));
    out_Color = vec4(ldr, 1.0);
}
    )";

    // ============================================================
    // Impl
    // ============================================================
    static const char* k_GBufferSkinnedVertSrc = R"(
#version 410 core
layout (location = 0) in vec3 a_Position;
layout (location = 1) in vec3 a_Normal;
layout (location = 2) in vec2 a_TexCoord;
layout (location = 3) in ivec4 a_BoneIDs;
layout (location = 4) in vec4 a_Weights;

uniform mat4 u_Model;
uniform mat4 u_ViewProjection;
uniform mat4 u_BoneMatrices[128];

out vec3 v_WorldPos;
out vec3 v_Normal;

void main() {
    vec4 skinnedPos = vec4(0.0);
    vec3 skinnedNormal = vec3(0.0);
    float totalWeight = 0.0;

    for (int i = 0; i < 4; ++i) {
        int bID = a_BoneIDs[i];
        if (bID < 0 || bID >= 128) continue;
        float w = a_Weights[i];
        if (w <= 0.0001) continue;

        mat4 boneMat = u_BoneMatrices[bID];
        skinnedPos += (boneMat * vec4(a_Position, 1.0)) * w;
        skinnedNormal += (mat3(boneMat) * a_Normal) * w;
        totalWeight += w;
    }

    // Fallback: vertice senza pesi validi
    if (totalWeight < 0.001) {
        skinnedPos = vec4(a_Position, 1.0);
        skinnedNormal = a_Normal;
    }

    vec4 worldPos = u_Model * skinnedPos;
    v_WorldPos = worldPos.xyz;

    mat3 normalMatrix = transpose(inverse(mat3(u_Model)));
    v_Normal = normalize(normalMatrix * skinnedNormal);

    gl_Position = u_ViewProjection * worldPos;
}
)";

    static const char* k_GBufferSkinnedFragSrc = R"(
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
        m_GBufferSkinnedShader = std::make_unique<Shader>();
        if (!m_GBufferSkinnedShader->LoadFromSource(
            k_GBufferSkinnedVertSrc, k_GBufferSkinnedFragSrc, "GBufferSkinned")) {
            SUN_CORE_ERROR("DeferredRenderer: gbuffer skinned shader fallito");
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
        if (!m_Bloom.Init(width, height)) {
            SUN_CORE_WARN("DeferredRenderer: Bloom init fallito, proseguo senza");
            m_BloomEnabled = false;
        }
        if (!m_AutoExposure.Init(width, height)) {
            SUN_CORE_WARN("DeferredRenderer: AutoExposure init fallito, proseguo senza");
            m_AutoExposureEnabled = false;
        }
        // Shadow shader
        m_ShadowDepthShader = std::make_unique<Shader>();
        {
            std::string shadowVert = R"(
        #version 410 core
        layout (location = 0) in vec3 a_Position;
        uniform mat4 u_Model;
        uniform mat4 u_LightSpaceMatrix;
        void main() { gl_Position = u_LightSpaceMatrix * u_Model * vec4(a_Position, 1.0); }
    )";
            std::string shadowFrag = R"(
        #version 410 core
        void main() { }
    )";
            if (!m_ShadowDepthShader->LoadFromSource(shadowVert, shadowFrag, "ShadowDepth")) {
                SUN_CORE_ERROR("DeferredRenderer: shadow depth shader fallito");
                return false;
            }
        }

        if (!m_ShadowMap.Init()) {
            SUN_CORE_ERROR("DeferredRenderer: ShadowMap init fallita");
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
        if (m_BloomEnabled) m_Bloom.Shutdown();
        if (m_AutoExposureEnabled) m_AutoExposure.Shutdown();
        m_Initialized = false;
        m_ShadowMap.Shutdown();
        m_ShadowDepthShader.reset();
        m_GBufferSkinnedShader.reset();
    }

    void DeferredRenderer::Resize(int width, int height) {
        if (width == m_Width && height == m_Height) return;
        m_Width = width;
        m_Height = height;
        m_GBuffer.Resize(width, height);
        m_HDRTarget.Resize(width, height);
        if (m_BloomEnabled) m_Bloom.Resize(width, height);
        if (m_AutoExposureEnabled) m_AutoExposure.Resize(width, height);
        // FIX #8: nessun FBO lasciato bindato (Resize puo' arrivare durante il frame ImGui)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
     0, 3, 2, 2, 1, 0,       // back (-Z)    [FIXED]
     4, 5, 6, 6, 7, 4,       // front (+Z)
     8, 9, 10, 10, 11, 8,    // left (-X)
     14, 13, 12, 12, 15, 14, // right (+X)   [FIXED]
     16, 17, 18, 18, 19, 16, // bottom (-Y)
     22, 21, 20, 20, 23, 22  // top (+Y)     [FIXED]
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
        m_ViewProjection = camera.GetViewProjection();
        m_CameraPos = camera.GetPosition();

        // --- SHADOW PASS ---
        // Calcola le light-space matrices, poi il chiamante disegna
        // la scena 4 volte con RenderShadowPassCube. Per ora la facciamo
        // in automatico qui usando i cubi di test.
        m_ShadowMap.UpdateCascades(camera, m_SunDirection, /*shadowDistance*/ 60.0f);

        // Bind dello shadow UBO (binding point 1)
        m_ShadowMap.UploadMatrices();

        // (Il chiamante, se ha una scena reale, dovrebbe iterare qui.)
        // Nel tuo GameLayer il loop shadow va fatto PRIMA di BeginGeometryPass.
        // Vedi patch a GameLayer::OnRender.

        // --- G-BUFFER ---
        m_GBuffer.BindForWriting();
        m_GBuffer.Clear();

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

        m_ShadowMap.BindDepthTextures(4);
        m_LightingShader->SetInt("u_ShadowMap0", 4);
        m_LightingShader->SetInt("u_ShadowMap1", 5);
        m_LightingShader->SetInt("u_ShadowMap2", 6);
        m_LightingShader->SetInt("u_ShadowMap3", 7);
        m_LightingShader->SetFloat("u_ShadowBias", m_ShadowMap.GetShadowBias());
        m_LightingShader->SetFloat("u_ShadowNormalBias", m_ShadowMap.GetNormalBias());
        m_LightingShader->SetFloat("u_CascadeCount", (float)ShadowMap::CASCADE_COUNT);

        // Le splits le passiamo come array
        float splits[ShadowMap::CASCADE_COUNT];
        for (int i = 0; i < ShadowMap::CASCADE_COUNT; ++i)
            splits[i] = m_ShadowMap.GetCascadeSplits()[i];
        glUniform1fv(glGetUniformLocation(m_LightingShader->GetID(), "u_CascadeSplits"),
            ShadowMap::CASCADE_COUNT, splits);
        m_LightingShader->SetFloat("u_CascadeFade", m_CascadeFade);
        // Camera uniforms
        m_LightingShader->SetVec3("u_CameraPos", m_CameraPos);

        glm::mat4 invVP = glm::inverse(m_ViewProjection);
        m_LightingShader->SetMat4("u_InvViewProjection", invVP);

        // Fullscreen quad
        glBindVertexArray(Framebuffer::GetFullscreenQuadVAO());
        m_LightingShader->SetFloat("u_AmbientIntensity", m_AmbientIntensity);
        m_LightingShader->SetVec3("u_AmbientSkyColor", m_AmbientSkyColor);
        m_LightingShader->SetVec3("u_AmbientGroundColor", m_AmbientGroundColor);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        m_HDRTarget.Unbind();
    }
    void DeferredRenderer::RunTonemapPass() {
        GLuint hdrSource = m_HDRTarget.GetColorTexture(0)->GetID();
        if (m_BloomEnabled) {
            m_Bloom.Render(hdrSource);
            hdrSource = m_Bloom.GetCompositeTexture()->GetID();
        }

        // Auto-exposure: analizza il composite HDR e aggiorna l'expousure 1x1
        if (m_AutoExposureEnabled) {
            float dt = m_LastFrameDelta;  // vedi sotto
            m_AutoExposure.Update(hdrSource, dt);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_Width, m_Height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_TonemapShader->Bind();
        m_TonemapShader->SetInt("u_HDRTexture", 0);
        m_TonemapShader->SetInt("u_ExposureTex", 1);
        m_TonemapShader->SetFloat("u_ExposureFallback", 1.0f);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdrSource);
        if (m_AutoExposureEnabled) {
            m_AutoExposure.BindExposureTexture(1);
        }
        else {
            // bind dummy 1x1 R32F con valore 1
            static GLuint s_DummyExposureTex = 0;
            if (s_DummyExposureTex == 0) {
                glGenTextures(1, &s_DummyExposureTex);
                glBindTexture(GL_TEXTURE_2D, s_DummyExposureTex);
                float one = 1.0f;
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 1, 1, 0, GL_RED, GL_FLOAT, &one);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            }
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, s_DummyExposureTex);
        }

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
    void DeferredRenderer::RenderShadowPassCube(const glm::mat4& transform, int cascadeIndex) {
        m_ShadowDepthShader->Bind();
        m_ShadowDepthShader->SetMat4("u_Model", transform);
        m_ShadowDepthShader->SetMat4("u_LightSpaceMatrix",
            m_ShadowMap.GetLightSpaceMatrix(cascadeIndex));

        glBindVertexArray(m_TestCubeVAO);
        glDrawElements(GL_TRIANGLES, m_TestCubeIndexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }
    void DeferredRenderer::DrawSkinnedMesh(
        const SkinnedMesh& mesh,
        const glm::mat4& model,
        const std::vector<glm::mat4>& boneMatrices,
        const glm::vec4& color,
        float roughness, float metallic, float emissive)
    {
        if (!m_GBufferSkinnedShader || !m_GBufferSkinnedShader->IsValid()) return;

        m_GBufferSkinnedShader->Bind();
        m_GBufferSkinnedShader->SetMat4("u_Model", model);
        m_GBufferSkinnedShader->SetMat4("u_ViewProjection", m_ViewProjection);
        m_GBufferSkinnedShader->SetVec4("u_Color", color);
        m_GBufferSkinnedShader->SetFloat("u_Roughness", roughness);
        m_GBufferSkinnedShader->SetFloat("u_Metallic", metallic);
        m_GBufferSkinnedShader->SetFloat("u_Emissive", emissive);

        int count = std::min((int)boneMatrices.size(), MAX_BONES);
        if (count > 0) {
            glUniformMatrix4fv(
                glGetUniformLocation(m_GBufferSkinnedShader->GetID(), "u_BoneMatrices"),
                count, GL_FALSE, glm::value_ptr(boneMatrices[0]));
        }
        else {
            // Bind identity matrices se boneMatrices è vuoto (fallback)
            static std::vector<glm::mat4> s_Identity(MAX_BONES, glm::mat4(1.0f));
            glUniformMatrix4fv(
                glGetUniformLocation(m_GBufferSkinnedShader->GetID(), "u_BoneMatrices"),
                MAX_BONES, GL_FALSE, glm::value_ptr(s_Identity[0]));
        }

        mesh.Draw();
    }
}