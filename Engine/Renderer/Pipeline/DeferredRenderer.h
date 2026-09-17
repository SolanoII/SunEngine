#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>

#include "../Core/Shader.h"
#include "../Core/Framebuffer.h"
#include "GBuffer.h"
#include "Light.h"

namespace SunEngine {

    class Camera;

    // Orchestratore della pipeline HDR deferred.
    // Step 1B: 1 directional + N point/spot, Blinn-Phong placeholder.
    // Step 1C: sostituire lighting con Cook-Torrance PBR + IBL.
    class DeferredRenderer {
    public:
        bool Init(int width, int height);
        void Shutdown();
        void Resize(int width, int height);

        // Chiamato prima di disegnare la geometria: bind del GBuffer,
        // clear, config degli stati GL. Ritorna lo shader gbuffer per
        // settare u_Model e u_Color per ogni mesh.
        Shader* BeginGeometryPass(const Camera& camera);

        // Chiude il geometry pass, esegue lighting + tonemap e presenta a schermo.
        void EndGeometryPassAndPresent(const Camera& camera);

        // Disegna un cube di test (usato solo in Step 1B per validare).
                // Disegna un cubo nel G-Buffer corrente. Chiamare SOLO tra
        // BeginGeometryPass() ed EndGeometryPassAndPresent().
        void DrawCube(const glm::mat4& transform,
            const glm::vec4& color,
            float roughness = 0.5f,
            float metallic = 0.0f,
            float emissive = 0.0f);

        void DrawTestGround(float size);

        // Luci (gestite dal chiamante)
        void ClearLights();
        void AddLight(const Light& light);
        void SetDirectionalLight(const glm::vec3& direction,
            const glm::vec3& color,
            float intensity);

        bool IsInitialized() const { return m_Initialized; }
        GBuffer& GetGBuffer() { return m_GBuffer; }
        const glm::mat4& GetViewProjection() const { return m_ViewProjection; }
        const glm::mat4& GetView() const { return m_View; }
        const glm::mat4& GetProjection() const { return m_Projection; }
        const glm::vec3& GetCameraPos() const { return m_CameraPos; }

        // Esposti per debug HUD
        GpuTexture* GetHDRResult() { return m_HDRTarget.GetColorTexture(0); }
    private:
        void RunLightingPass();
        void RunTonemapPass();
        void UploadLights();

        bool m_Initialized = false;
        int m_Width = 0;
        int m_Height = 0;

        GBuffer m_GBuffer;
        Framebuffer m_HDRTarget;   // RGBA16F + depth (per depth test nei passi forward futuri)

        std::unique_ptr<Shader> m_GBufferShader;
        std::unique_ptr<Shader> m_LightingShader;
        std::unique_ptr<Shader> m_TonemapShader;

        // Camera state
        glm::mat4 m_View{ 1.0f };
        glm::mat4 m_Projection{ 1.0f };
        glm::mat4 m_ViewProjection{ 1.0f };
        glm::vec3 m_CameraPos{ 0.0f };

        // Test geometry (Step 1B)
        unsigned int m_TestCubeVAO = 0;
        unsigned int m_TestCubeVBO = 0;
        unsigned int m_TestCubeEBO = 0;
        unsigned int m_TestCubeIndexCount = 0;
        void CreateTestGeometry();

        // Lights
        std::vector<Light> m_Lights;
        Light m_DirectionalLight{};
        bool m_HasDirectional = false;

        unsigned int m_LightUBO = 0;
        static constexpr int MAX_LIGHTS = 64;
    };

}