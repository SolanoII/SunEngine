#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include "ShadowMap.h"
#include "../Core/Shader.h"
#include "../Core/Framebuffer.h"
#include "GBuffer.h"
#include "Light.h"
#include "../PostFX/Bloom.h"
#include "../PostFX/AutoExposure.h"
#include "../SkinnedMesh.h"   // in cima al file
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
        Bloom m_Bloom;
        bool m_BloomEnabled = true;
        AutoExposure m_AutoExposure;
        bool m_AutoExposureEnabled = true;
        bool IsInitialized() const { return m_Initialized; }
        GBuffer& GetGBuffer() { return m_GBuffer; }
        const glm::mat4& GetViewProjection() const { return m_ViewProjection; }
        const glm::mat4& GetView() const { return m_View; }
        const glm::mat4& GetProjection() const { return m_Projection; }
        const glm::vec3& GetCameraPos() const { return m_CameraPos; }
        ShadowMap& GetShadowMap() { return m_ShadowMap; }
        const ShadowMap& GetShadowMap() const { return m_ShadowMap; }

        // Da chiamare PRIMA di BeginGeometryPass.
        // Imposta la direzione del sole e aggiorna le cascade.
        void SetSunDirection(const glm::vec3& dir) { m_SunDirection = glm::normalize(dir); }

        // Espone lo shader depth per il rendering shadow (usato dal chiamante
        // per fare DrawShadowCube per ogni mesh)
        Shader* GetShadowDepthShader() const { return m_ShadowDepthShader.get(); }
        void RenderShadowPassCube(const glm::mat4& transform, int cascadeIndex);
        void SetAmbient(float intensity, const glm::vec3& sky, const glm::vec3& ground) {
            m_AmbientIntensity = intensity;
            m_AmbientSkyColor = sky;
            m_AmbientGroundColor = ground;
        }
        void SetCascadeFade(float f) { m_CascadeFade = glm::clamp(f, 0.0f, 0.5f); }
        AutoExposure& GetAutoExposure() { return m_AutoExposure; }
        void SetAutoExposureEnabled(bool e) { m_AutoExposureEnabled = e; }
        // Esposti per debug HUD
        GpuTexture* GetHDRResult() { return m_HDRTarget.GetColorTexture(0); }
        // boneMatrices = transform di ogni osso (dal Animator).
// Il chiamante deve aver già bindato il GBuffer (BeginGeometryPass).
        void DrawSkinnedMesh(
            const SkinnedMesh& mesh,
            const glm::mat4& model,
            const std::vector<glm::mat4>& boneMatrices,
            const glm::vec4& color,
            float roughness = 0.7f,
            float metallic = 0.0f,
            float emissive = 0.0f,
            const GpuTexture* texture = nullptr,   // strato di vestito: texture triplanare in posa T
            float texScale = 1.0f,
            float alphaCut = 0.0f,
            int detail = 0,          // dettagli procedurali: 1 pelle, 2 stoffa, 3 armatura (0 = niente)
            float invHeight = 0.0f); // 1 / altezza del corpo in posa T (scala dei dettagli; 0 = niente dettagli)
        void SetFrameDelta(float dt) { m_LastFrameDelta = dt; }

        // Mimetismo (camo cloak): fra BeginCloak ed EndCloak i cubi e le mesh skinnate non vanno nel G-Buffer ma in
        // una coda; dopo l'illuminazione vengono disegnati come "liquido trasparente": rifrangono la scena dietro
        // (copia dell'immagine HDR, spostata lungo la normale) con un leggero bordo luminoso. amount 0..1 = intensita'.
        void BeginCloak(float amount) { m_CloakCapture = amount > 0.001f; m_CloakAmount = amount; }
        void EndCloak() { m_CloakCapture = false; }
    private:
        void RunCloakPass();
        struct CloakCube { glm::mat4 model; float amount; };
        struct CloakSkinned { const SkinnedMesh* mesh; glm::mat4 model; std::vector<glm::mat4> bones; float amount; };
        bool m_CloakCapture = false;
        float m_CloakAmount = 0.0f;
        float m_CloakTime = 0.0f;
        std::vector<CloakCube> m_CloakCubes;
        std::vector<CloakSkinned> m_CloakSkinned;
        Framebuffer m_CloakCopy;                       // copia della scena illuminata (da rifrangere)
        std::unique_ptr<Shader> m_CloakShader;
        std::unique_ptr<Shader> m_CloakSkinnedShader;
        void RunLightingPass();
        void RunTonemapPass();
        void UploadLights();
        float m_CascadeFade = 0.15f;  // ultimo 15% di ogni cascade fa blend
        // Ambient IBL fittizia (finché non hai cubemap vere)
        float m_AmbientIntensity = 0.55f;
        glm::vec3 m_AmbientSkyColor = glm::vec3(0.35f, 0.42f, 0.55f);
        glm::vec3 m_AmbientGroundColor = glm::vec3(0.10f, 0.08f, 0.06f);
        bool m_Initialized = false;
        int m_Width = 0;
        int m_Height = 0;
        ShadowMap m_ShadowMap;
        std::unique_ptr<Shader> m_ShadowDepthShader;
        glm::vec3 m_SunDirection = glm::normalize(glm::vec3(-0.4f, -1.0f, 0.3f));
        std::unique_ptr<Shader> m_GBufferSkinnedShader;
        static constexpr int MAX_BONES = 128;
        // Aggiungi il VBO/EBO del cubo shadow (condiviso col cube gbuffer)
        // → riutilizza m_TestCubeVAO/m_TestCubeEBO già presenti
        GBuffer m_GBuffer;
        Framebuffer m_HDRTarget;   // RGBA16F + depth (per depth test nei passi forward futuri)

        std::unique_ptr<Shader> m_GBufferShader;
        std::unique_ptr<Shader> m_LightingShader;
        std::unique_ptr<Shader> m_TonemapShader;
        float m_LastFrameDelta = 0.016f;
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