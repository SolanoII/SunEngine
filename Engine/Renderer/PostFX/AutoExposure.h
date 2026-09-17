#pragma once

#include <glad/glad.h>
#include <memory>
#include "../Core/Shader.h"
#include "../Core/Framebuffer.h"

namespace SunEngine {

    // Auto-exposure con log-average luminance (metodo Reinhard/Duiker).
    // 1. Compute pass 16x16: ogni cella media la log-luminanza di un blocco
    //    4x4 del composite HDR, scrivendola in m_LuminanceTex.
    // 2. Compute pass 1x1: media le 256 celle + adatta temporalmente
    //    m_ExposureTex verso il target (smoothing esponenziale).
    class AutoExposure {
    public:
        bool Init(int width, int height);
        void Shutdown();
        void Resize(int width, int height);

        // Da chiamare dopo il bloom, prima del tonemap.
        // hdrTexture : composite HDR target (RGBA16F)
        // deltaTime  : per lo smoothing temporale
        void Update(GLuint hdrTexture, float deltaTime);

        // Binding dell'immagine exposure per il tonemap shader.
        // Occupa una texture unit.
        void BindExposureTexture(int unit) const;

        void SetEnabled(bool e) { m_Enabled = e; }
        bool IsEnabled() const { return m_Enabled; }

        void SetSpeed(float s) { m_Speed = s; }            // unità: 1/secondo
        void SetRange(float minE, float maxE) { m_MinExposure = minE; m_MaxExposure = maxE; }
        void SetKeyValue(float k) { m_KeyValue = k; }       // moltiplicatore target

        float GetCurrentExposure() const { return m_CurrentExposure; }

    private:
        bool m_Enabled = true;
        int m_Width = 0;
        int m_Height = 0;

        // 16x16 luminanza media di blocchi
        GLuint m_LuminanceTex = 0;
        // 1x1 exposure adattata
        GLuint m_ExposureTex = 0;

        std::unique_ptr<Shader> m_LuminancePass;
        std::unique_ptr<Shader> m_AdaptPass;

        float m_CurrentExposure = 1.0f;
        float m_Speed = 1.5f;
        float m_MinExposure = 0.05f;
        float m_MaxExposure = 8.0f;
        float m_KeyValue = 0.18f;  // mid-gray

        static constexpr int LUM_SIZE = 16;
    };

}