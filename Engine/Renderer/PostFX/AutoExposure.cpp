
#include "AutoExposure.h"
#include "../../Core/Logger/Log.h"
#include <algorithm>
#include <cmath>

namespace SunEngine {

    static const char* k_LuminanceComp = R"(
        #version 430 core
        layout (local_size_x = 16, local_size_y = 16) in;

        uniform sampler2D u_HDRTexture;
        uniform vec2 u_TargetSize;

        // Output: texture 16x16 R32F
        layout (rgba32f, binding = 0) uniform image2D u_LumOutput;

        void main() {
            ivec2 gid = ivec2(gl_GlobalInvocationID.xy);

            // Ogni cella 16x16 media un blocco 4x4 di texel del target.
            // Su 1280x720 sono 320x180 per cella -> campioniamo 4x4 = 16 punti.
            vec2 cellSize = u_TargetSize / vec2(16.0);
            ivec2 basePix = ivec2(vec2(gid) * cellSize);

            float lumSum = 0.0;
            int samples = 0;
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    ivec2 p = basePix + ivec2(x, y) * (ivec2(cellSize) / 4);
                    if (p.x >= int(u_TargetSize.x) || p.y >= int(u_TargetSize.y)) continue;
                    vec2 uv = (vec2(p) + 0.5) / u_TargetSize;
                    vec3 c = texture(u_HDRTexture, uv).rgb;
                    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
                    // Clamp per evitare che un pixel emissivo estremo domini
                    lum = clamp(lum, 1e-4, 1e4);
                    lumSum += log(lum);
                    samples++;
                }
            }
            float logAvg = (samples > 0) ? (lumSum / float(samples)) : log(1e-4);

            imageStore(u_LumOutput, gid, vec4(logAvg, 0.0, 0.0, 0.0));
        }
    )";

    static const char* k_AdaptComp = R"(
        #version 430 core
        layout (local_size_x = 1, local_size_y = 1) in;

        layout (rgba32f, binding = 0) uniform image2D u_LumInput;
        layout (rgba32f, binding = 1) uniform image2D u_ExposureInOut;

        uniform float u_DeltaTime;
        uniform float u_Speed;         // 1/sec
        uniform float u_KeyValue;      // target middle gray
        uniform float u_MinExposure;
        uniform float u_MaxExposure;
        uniform int   u_LumSize;

        void main() {
            // 1. Media le LUM_SIZE*LUM_SIZE celle di log-luminanza
            float logSum = 0.0;
            int count = 0;
            for (int y = 0; y < u_LumSize; ++y) {
                for (int x = 0; x < u_LumSize; ++x) {
                    logSum += imageLoad(u_LumInput, ivec2(x, y)).r;
                    count++;
                }
            }
            float logAvg = logSum / float(count);
            float avgLum = exp(logAvg);

            // 2. Exposure target = key / avgLum  (Reinhard-style)
            float targetExposure = u_KeyValue / max(avgLum, 1e-4);
            targetExposure = clamp(targetExposure, u_MinExposure, u_MaxExposure);

            // 3. Smoothing esponenziale nel tempo
            float currentExposure = imageLoad(u_ExposureInOut, ivec2(0, 0)).r;
            float alpha = 1.0 - exp(-u_Speed * u_DeltaTime);
            alpha = clamp(alpha, 0.0, 1.0);
            float newExposure = mix(currentExposure, targetExposure, alpha);

            imageStore(u_ExposureInOut, ivec2(0, 0), vec4(newExposure, 0.0, 0.0, 0.0));
        }
    )";

    bool AutoExposure::Init(int width, int height) {
        m_Width = width;
        m_Height = height;

        // Le compute (4.3) e le image unit (4.2) richiedono che glad abbia caricato queste funzioni.
        // Se un puntatore e' nullo, chiamarlo = access violation a 0x00000000. In quel caso si
        // disattiva l'auto-esposizione (DeferredRenderer prosegue con esposizione fissa).
        if (!glDispatchCompute || !glBindImageTexture || !glMemoryBarrier) {
            SUN_CORE_WARN("AutoExposure: funzioni compute/image non caricate da glad (serve OpenGL 4.3+), disattivo");
            return false;
        }
        if (!glDispatchCompute || !glBindImageTexture || !glMemoryBarrier) {
            SUN_CORE_WARN("AutoExposure: funzioni compute/image non caricate da glad (serve OpenGL 4.3+), disattivo");
            return false;
        }
        // Luminance 16x16 R32F
        glGenTextures(1, &m_LuminanceTex);
        glBindTexture(GL_TEXTURE_2D, m_LuminanceTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, LUM_SIZE, LUM_SIZE, 0,
            GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Exposure 1x1 R32F
        glGenTextures(1, &m_ExposureTex);
        glBindTexture(GL_TEXTURE_2D, m_ExposureTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 1, 1, 0,
            GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // Inizializza exposure a 1.0
        float init = 1.0f;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RED, GL_FLOAT, &init);

        glBindTexture(GL_TEXTURE_2D, 0);

        // Shaders
        m_LuminancePass = std::make_unique<Shader>();
        if (!m_LuminancePass->LoadComputeFromSource(k_LuminanceComp, "AutoExposureLuminance")) {
            SUN_CORE_ERROR("AutoExposure: luminance pass fallito");
            return false;
        }

        m_AdaptPass = std::make_unique<Shader>();
        if (!m_AdaptPass->LoadComputeFromSource(k_AdaptComp, "AutoExposureAdapt")) {
            SUN_CORE_ERROR("AutoExposure: adapt pass fallito");
            return false;
        }

        SUN_CORE_INFO("AutoExposure inizializzato: {}x{} cell luminance + 1x1 exposure",
            LUM_SIZE, LUM_SIZE);
        return true;
    }

    void AutoExposure::Shutdown() {
        if (m_LuminanceTex) { glDeleteTextures(1, &m_LuminanceTex); m_LuminanceTex = 0; }
        if (m_ExposureTex) { glDeleteTextures(1, &m_ExposureTex); m_ExposureTex = 0; }
        m_LuminancePass.reset();
        m_AdaptPass.reset();
    }

    void AutoExposure::Resize(int width, int height) {
        m_Width = width;
        m_Height = height;
    }

    void AutoExposure::Update(GLuint hdrTexture, float deltaTime) {
        if (!m_Enabled) return;

        // ---- PASS 1: log-luminanza in 16x16 ----
        m_LuminancePass->Bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdrTexture);
        m_LuminancePass->SetInt("u_HDRTexture", 0);
        m_LuminancePass->SetVec2("u_TargetSize", glm::vec2(m_Width, m_Height));

        glBindImageTexture(0, m_LuminanceTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);

        glDispatchCompute(LUM_SIZE, LUM_SIZE, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

        // ---- PASS 2: adatta exposure ----
        m_AdaptPass->Bind();
        glBindImageTexture(0, m_LuminanceTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(1, m_ExposureTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);

        m_AdaptPass->SetFloat("u_DeltaTime", deltaTime);
        m_AdaptPass->SetFloat("u_Speed", m_Speed);
        m_AdaptPass->SetFloat("u_KeyValue", m_KeyValue);
        m_AdaptPass->SetFloat("u_MinExposure", m_MinExposure);
        m_AdaptPass->SetFloat("u_MaxExposure", m_MaxExposure);
        m_AdaptPass->SetInt("u_LumSize", LUM_SIZE);

        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

        // Legge l'exposure appena calcolata per debug/HUD
        glBindTexture(GL_TEXTURE_2D, m_ExposureTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, &m_CurrentExposure);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void AutoExposure::BindExposureTexture(int unit) const {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, m_ExposureTex);
    }

}