#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <array>
#include <memory>
#include "../Core/Shader.h"

namespace SunEngine {

    class Camera;

    // 4 cascade. Ogni cascade ha la sua texture depth + light-space matrix.
    class ShadowMap {
    public:
        static constexpr int CASCADE_COUNT = 4;
        static constexpr int SHADOW_RESOLUTION = 2048;   // risoluzione di partenza (opzioni grafiche: SetResolution)

        bool Init();
        // Risoluzione delle mappe d'ombra (1024 / 2048 / 4096): ricrea le texture se cambia
        void SetResolution(int resolution);
        int GetResolution() const { return m_Resolution; }
        void Shutdown();

        // Da chiamare ogni frame PRIMA della geometry pass.
        // Calcola le light-space matrices per le 4 cascade a partire dal
        // frustum della camera e dalla direzione del sole.
        void UpdateCascades(const Camera& camera,
            const glm::vec3& lightDirection,
            float shadowDistance = 60.0f);

        // Bind del framebuffer per il rendering della cascade c.
        void BeginCascade(int cascadeIndex);
        void EndCascade();

        // Per la lighting pass: bind delle 4 texture depth.
        // Occupano le unità texture consecutive a partire da firstUnit.
        void BindDepthTextures(int firstUnit) const;

        // Upload delle matrici nel UBO delle shadow (binding point 1)
        void UploadMatrices();

        const glm::mat4& GetLightSpaceMatrix(int cascade) const {
            return m_LightSpaceMatrices[cascade];
        }

        const std::array<float, CASCADE_COUNT>& GetCascadeSplits() const {
            return m_CascadeSplits;
        }

        GLuint GetShadowUBO() const { return m_ShadowUBO; }

        float GetShadowBias() const { return m_ShadowBias; }
        void  SetShadowBias(float b) { m_ShadowBias = b; }
        float GetNormalBias() const { return m_NormalBias; }
        void  SetNormalBias(float b) { m_NormalBias = b; }

    private:
        void CreateDepthTextures();
        void DeleteDepthTextures();
        int m_Resolution = SHADOW_RESOLUTION;
        GLuint m_FBO = 0;
        GLuint m_DepthTextures[CASCADE_COUNT] = {};
        // 4 mat4 = 256 bytes
        GLuint m_ShadowUBO = 0;

        std::array<glm::mat4, CASCADE_COUNT> m_LightSpaceMatrices{};
        std::array<float, CASCADE_COUNT>     m_CascadeSplits{};
        float m_ShadowBias = 0.0035f;
        float m_NormalBias = 0.20f;
        // Calcola i corner del frustum per una cascade
        std::array<glm::vec3, 8> ComputeFrustumCornersWorldSpace(
            const glm::mat4& proj, const glm::mat4& view,
            float nearZ, float farZ) const;
    };

}