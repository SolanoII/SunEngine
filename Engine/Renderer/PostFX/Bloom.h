#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>
#include "../Core/Shader.h"
#include "../Core/Framebuffer.h"

namespace SunEngine {

    // Bloom multi-mip additive:
    // 1. Bright pass -> half-res
    // 2. 5 livelli di downsample con blur gaussiano
    // 3. Upsample additive fino a risoluzione half-res
    // 4. Composite additivo sopra l'HDR target
    class Bloom {
    public:
        static constexpr int MIP_COUNT = 5;

        bool Init(int width, int height);
        void Shutdown();
        void Resize(int width, int height);

        // Esegue la pipeline completa: prende HDR tex (luminanza in rgb)
        // e produce un bloom da compositare sopra.
        // Nota: il composite avviene dentro, bloom è già sommato all'HDR
        // nell'ultimo pass. Se vuoi bloom separato, esponi GetBloomTexture().
        void Render(GLuint hdrTexture);

        void SetThreshold(float t) { m_Threshold = t; }
        void SetIntensity(float i) { m_Intensity = i; }
        void SetSoftKnee(float k) { m_SoftKnee = k; }

        float GetThreshold() const { return m_Threshold; }
        float GetIntensity() const { return m_Intensity; }

        GpuTexture* GetBloomTexture() {
            return m_MipTargets[0].GetColorTexture(0);
        }
        GpuTexture* GetCompositeTexture() {
            return m_CompositeTarget.GetColorTexture(0);
        }
    private:
        int m_Width = 0;
        int m_Height = 0;

        // Half-res del main target
        int m_HalfWidth = 0;
        int m_HalfHeight = 0;

        // Mip targets: 0 è il più grande, 4 il più piccolo (half-res → /32)
        Framebuffer m_MipTargets[MIP_COUNT];
        Framebuffer m_CompositeTarget;

        std::unique_ptr<Shader> m_BrightPassShader;
        std::unique_ptr<Shader> m_BlurShader;
        std::unique_ptr<Shader> m_UpsampleShader;
        std::unique_ptr<Shader> m_CompositeShader;

        float m_Threshold = 1.0f;
        float m_Intensity = 1.2f;
        float m_SoftKnee = 0.5f;

        void RenderFullscreenQuad() const;
    };

}