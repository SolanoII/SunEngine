#include "ShadowMap.h"
#include "../../Core/Logger/Log.h"
#include "../../Renderer/Camera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace SunEngine {

    void ShadowMap::DeleteDepthTextures() {
        for (int i = 0; i < CASCADE_COUNT; ++i) {
            if (m_DepthTextures[i] != 0) { glDeleteTextures(1, &m_DepthTextures[i]); m_DepthTextures[i] = 0; }
        }
    }

    void ShadowMap::SetResolution(int resolution) {
        resolution = std::clamp(resolution, 512, 4096);
        if (resolution == m_Resolution) return;
        m_Resolution = resolution;
        if (m_FBO == 0) return;   // non ancora inizializzata: Init usera' la nuova risoluzione
        DeleteDepthTextures();
        CreateDepthTextures();
        SUN_CORE_INFO("ShadowMap: risoluzione {}x{}", m_Resolution, m_Resolution);
    }

    bool ShadowMap::Init() {
        // Crea FBO
        glGenFramebuffers(1, &m_FBO);
        CreateDepthTextures();

        // UBO con 4 mat4
        glGenBuffers(1, &m_ShadowUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, m_ShadowUBO);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(glm::mat4) * CASCADE_COUNT,
            nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        SUN_CORE_INFO("ShadowMap: {} cascade, risoluzione {}x{}",
            CASCADE_COUNT, m_Resolution, m_Resolution);

        return true;
    }

    void ShadowMap::CreateDepthTextures() {
        // Crea le texture depth
        for (int i = 0; i < CASCADE_COUNT; ++i) {
            glGenTextures(1, &m_DepthTextures[i]);
            glBindTexture(GL_TEXTURE_2D, m_DepthTextures[i]);

            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
                m_Resolution, m_Resolution, 0,
                GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
            // IMPORTANTE: il bordo deve ritornare 1.0 (far) così fuori dallo shadow
            // map il test non ombreggia
            float borderColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

            // Nessun compare mode (sample esplicito nel fragment shader)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void ShadowMap::Shutdown() {
        for (int i = 0; i < CASCADE_COUNT; ++i) {
            if (m_DepthTextures[i] != 0) {
                glDeleteTextures(1, &m_DepthTextures[i]);
                m_DepthTextures[i] = 0;
            }
        }
        if (m_FBO != 0) { glDeleteFramebuffers(1, &m_FBO); m_FBO = 0; }
        if (m_ShadowUBO != 0) { glDeleteBuffers(1, &m_ShadowUBO); m_ShadowUBO = 0; }
    }

    std::array<glm::vec3, 8> ShadowMap::ComputeFrustumCornersWorldSpace(
        const glm::mat4& proj, const glm::mat4& view,
        float nearZ, float farZ) const
    {
        // NDC corner del frustum a near/far
        // Usiamo una proiezione fittizia con near/far sostituiti
        glm::mat4 p = proj;
        // Sostituisci le near/far nella proiezione.
        // (glm::perspective mette in [2][2] e [3][2] i coefficienti dipendenti da near/far)
        // Rebuild perspective con stessi fov/aspect ma near/far custom.
        float fovRad = glm::radians(50.0f);
        float aspect = 16.0f / 9.0f;
        // Ricaviamo fov/aspect dalla proj originaria:
        // p[1][1] = 1/tan(fov/2), p[0][0] = p[1][1]/aspect
        if (std::abs(p[1][1]) > 1e-5f) {
            fovRad = 2.0f * std::atan(1.0f / p[1][1]);
            if (std::abs(p[0][0]) > 1e-5f) aspect = p[1][1] / p[0][0];
        }
        glm::mat4 customProj = glm::perspective(fovRad, aspect, nearZ, farZ);

        glm::mat4 invVP = glm::inverse(customProj * view);

        std::array<glm::vec3, 8> corners{};
        int idx = 0;
        for (int x = 0; x < 2; ++x) {
            for (int y = 0; y < 2; ++y) {
                for (int z = 0; z < 2; ++z) {
                    glm::vec4 pt = invVP * glm::vec4(
                        2.0f * x - 1.0f,
                        2.0f * y - 1.0f,
                        2.0f * z - 1.0f,
                        1.0f);
                    corners[idx++] = glm::vec3(pt) / pt.w;
                }
            }
        }
        return corners;
    }

    void ShadowMap::UpdateCascades(const Camera& camera,
        const glm::vec3& lightDirection,
        float shadowDistance)
    {
        glm::mat4 view = camera.GetView();
        glm::mat4 proj = camera.GetProjection();

        float nearZ = camera.GetNearPlane();
        float farZ = shadowDistance;
        const float lambda = 0.75f; // 0 = uniform, 1 = full logaritmico

        // Array locale: [0] = near globale, [1..CASCADE_COUNT] = far di ogni cascade.
        // Deve avere CASCADE_COUNT+1 elementi: il loop sotto scrive fino a
        // splits[CASCADE_COUNT].
        float splits[CASCADE_COUNT + 1];
        splits[0] = nearZ;
        for (int i = 1; i <= CASCADE_COUNT; ++i) {
            float p = static_cast<float>(i) / CASCADE_COUNT;
            float logSplit = nearZ * std::pow(farZ / nearZ, p);
            float uniSplit = nearZ + (farZ - nearZ) * p;
            splits[i] = glm::mix(uniSplit, logSplit, lambda);
        }

        // Copia i far di ogni cascade (splits[1..4]) in m_CascadeSplits[0..3].
        for (int i = 0; i < CASCADE_COUNT; ++i) {
            m_CascadeSplits[i] = splits[i + 1];
        }

        float cascadeNear = nearZ;
        for (int c = 0; c < CASCADE_COUNT; ++c) {
            float cascadeFar = m_CascadeSplits[c];

            auto corners = ComputeFrustumCornersWorldSpace(proj, view, cascadeNear, cascadeFar);

            glm::vec3 center(0.0f);
            for (const auto& corner : corners) center += corner;
            center /= 8.0f;

            // Calcola raggio della sfera che contiene il frustum
            float radius = 0.0f;
            for (const auto& corner : corners) {
                radius = std::max(radius, glm::length(corner - center));
            }
            // Snap del raggio a multipli di 16 unità texel per ridurre shimmering
            float texelSize = (2.0f * radius) / static_cast<float>(m_Resolution);
            radius = std::ceil(radius / texelSize) * texelSize;

            // Light view: guarda dalla direzione del sole verso il centro
            glm::vec3 lightDir = glm::normalize(lightDirection);
            glm::vec3 up = std::abs(lightDir.y) > 0.99f
                ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);

            glm::vec3 eye = center - lightDir * (radius + 20.0f);
            glm::mat4 lightView = glm::lookAt(eye, center, up);

            // Snap X/Y a pixel interi in light space per eliminare shimmering
            glm::vec4 originShift = lightView * glm::vec4(0, 0, 0, 1);
            float shadowHalf = radius;
            float offsetX = originShift.x / (shadowHalf * 2.0f) * static_cast<float>(m_Resolution);
            float offsetY = originShift.y / (shadowHalf * 2.0f) * static_cast<float>(m_Resolution);
            float roundedX = std::round(offsetX);
            float roundedY = std::round(offsetY);
            originShift.x = roundedX * (shadowHalf * 2.0f) / static_cast<float>(m_Resolution);
            originShift.y = roundedY * (shadowHalf * 2.0f) / static_cast<float>(m_Resolution);
            lightView[3] = originShift;

            glm::mat4 lightProj = glm::ortho(
                -radius, radius,
                -radius, radius,
                0.1f, radius * 2.0f + 40.0f);

            m_LightSpaceMatrices[c] = lightProj * lightView;

            cascadeNear = cascadeFar;
        }
    }

    void ShadowMap::BeginCascade(int cascadeIndex) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
            GL_TEXTURE_2D, m_DepthTextures[cascadeIndex], 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);

        glViewport(0, 0, m_Resolution, m_Resolution);
        glClear(GL_DEPTH_BUFFER_BIT);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT); // Front-face culling riduce peter-panning
        glFrontFace(GL_CCW);
    }

    void ShadowMap::EndCascade() {
        glCullFace(GL_BACK);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void ShadowMap::BindDepthTextures(int firstUnit) const {
        for (int i = 0; i < CASCADE_COUNT; ++i) {
            glActiveTexture(GL_TEXTURE0 + firstUnit + i);
            glBindTexture(GL_TEXTURE_2D, m_DepthTextures[i]);
        }
    }

    void ShadowMap::UploadMatrices() {
        glBindBuffer(GL_UNIFORM_BUFFER, m_ShadowUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0,
            sizeof(glm::mat4) * CASCADE_COUNT,
            m_LightSpaceMatrices.data());
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_ShadowUBO);
    }

}