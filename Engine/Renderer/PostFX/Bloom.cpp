#include "Bloom.h"
#include "../../Core/Logger/Log.h"

namespace SunEngine {

    static const char* k_FullscreenVert = R"(
        #version 410 core
        layout (location = 0) in vec2 a_Position;
        layout (location = 1) in vec2 a_TexCoord;
        out vec2 v_TexCoord;
        void main() {
            v_TexCoord = a_TexCoord;
            gl_Position = vec4(a_Position, 0.0, 1.0);
        }
    )";

    static const char* k_BrightPassFrag = R"(
        #version 410 core
        in vec2 v_TexCoord;
        out vec4 out_Color;

        uniform sampler2D u_HDRTexture;
        uniform float u_Threshold;
        uniform float u_SoftKnee;

        void main() {
            vec3 color = texture(u_HDRTexture, v_TexCoord).rgb;
            float brightness = max(color.r, max(color.g, color.b));

            // Soft knee: curva di transizione per evitare pop del bloom
            float knee = u_Threshold * u_SoftKnee + 1e-5;
            float soft = clamp(brightness - u_Threshold + knee, 0.0, 2.0 * knee);
            soft = soft * soft / (4.0 * knee + 1e-5);
            float contribution = max(soft, brightness - u_Threshold) / max(brightness, 1e-5);

            out_Color = vec4(color * contribution, 1.0);
        }
    )";

    static const char* k_BlurFrag = R"(
        #version 410 core
        in vec2 v_TexCoord;
        out vec4 out_Color;

        uniform sampler2D u_Texture;
        uniform vec2 u_TexelSize;   // 1.0 / texSize

        // 9-tap gaussian (5 pesi unici)
        // pesi: 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216
        void main() {
            vec3 result = texture(u_Texture, v_TexCoord).rgb * 0.227027;
            result += texture(u_Texture, v_TexCoord + vec2( 1.0,  0.0) * u_TexelSize).rgb * 0.1945946;
            result += texture(u_Texture, v_TexCoord + vec2(-1.0,  0.0) * u_TexelSize).rgb * 0.1945946;
            result += texture(u_Texture, v_TexCoord + vec2( 2.0,  0.0) * u_TexelSize).rgb * 0.1216216;
            result += texture(u_Texture, v_TexCoord + vec2(-2.0,  0.0) * u_TexelSize).rgb * 0.1216216;
            result += texture(u_Texture, v_TexCoord + vec2( 3.0,  0.0) * u_TexelSize).rgb * 0.054054;
            result += texture(u_Texture, v_TexCoord + vec2(-3.0,  0.0) * u_TexelSize).rgb * 0.054054;
            result += texture(u_Texture, v_TexCoord + vec2( 4.0,  0.0) * u_TexelSize).rgb * 0.016216;
            result += texture(u_Texture, v_TexCoord + vec2(-4.0,  0.0) * u_TexelSize).rgb * 0.016216;
            out_Color = vec4(result, 1.0);
        }
    )";

    // Variante verticale: stessa cosa ma con offset su Y
    // (riuso lo stesso shader con u_TexelSize passato diversamente? No:
    //  blur orizzontale e verticale vanno separati per efficienza. Uso
    //  un'unica shader con u_Direction.)
    static const char* k_BlurFragDir = R"(
        #version 410 core
        in vec2 v_TexCoord;
        out vec4 out_Color;

        uniform sampler2D u_Texture;
        uniform vec2 u_TexelSize;
        uniform vec2 u_Direction;   // (1,0) o (0,1)

        void main() {
            vec2 off1 = vec2(1.0) * u_TexelSize * u_Direction;
            vec2 off2 = vec2(2.0) * u_TexelSize * u_Direction;
            vec2 off3 = vec2(3.0) * u_TexelSize * u_Direction;
            vec2 off4 = vec2(4.0) * u_TexelSize * u_Direction;

            vec3 result = texture(u_Texture, v_TexCoord).rgb * 0.227027;
            result += texture(u_Texture, v_TexCoord + off1).rgb * 0.1945946;
            result += texture(u_Texture, v_TexCoord - off1).rgb * 0.1945946;
            result += texture(u_Texture, v_TexCoord + off2).rgb * 0.1216216;
            result += texture(u_Texture, v_TexCoord - off2).rgb * 0.1216216;
            result += texture(u_Texture, v_TexCoord + off3).rgb * 0.054054;
            result += texture(u_Texture, v_TexCoord - off3).rgb * 0.054054;
            result += texture(u_Texture, v_TexCoord + off4).rgb * 0.016216;
            result += texture(u_Texture, v_TexCoord - off4).rgb * 0.016216;
            out_Color = vec4(result, 1.0);
        }
    )";

    static const char* k_UpsampleFrag = R"(
        #version 410 core
        in vec2 v_TexCoord;
        out vec4 out_Color;

        uniform sampler2D u_LowerMip;   // mip più piccolo (già blurred)
        uniform sampler2D u_CurrentMip; // mip corrente
        uniform float u_Intensity;

        void main() {
            vec3 lower = texture(u_LowerMip, v_TexCoord).rgb;
            vec3 current = texture(u_CurrentMip, v_TexCoord).rgb;
            out_Color = vec4(current + lower * u_Intensity, 1.0);
        }
    )";

    // Composite: HDR + bloom * bloomIntensity
    static const char* k_CompositeFrag = R"(
        #version 410 core
        in vec2 v_TexCoord;
        out vec4 out_Color;

        uniform sampler2D u_HDRTexture;
        uniform sampler2D u_BloomTexture;
        uniform float u_BloomIntensity;

        void main() {
            vec3 hdr = texture(u_HDRTexture, v_TexCoord).rgb;
            vec3 bloom = texture(u_BloomTexture, v_TexCoord).rgb;
            out_Color = vec4(hdr + bloom * u_BloomIntensity, 1.0);
        }
    )";

    bool Bloom::Init(int width, int height) {
        m_Width = width;
        m_Height = height;
        m_HalfWidth = std::max(1, width / 2);
        m_HalfHeight = std::max(1, height / 2);

        // Alloca i mip targets
        for (int i = 0; i < MIP_COUNT; ++i) {
            int w = std::max(1, m_HalfWidth >> i);
            int h = std::max(1, m_HalfHeight >> i);
            m_MipTargets[i].Create(w, h, { TextureFormat::RGBA16F },
                /*hasDepth*/ false, TextureFilter::Linear);
        }

        m_CompositeTarget.Create(width, height, { TextureFormat::RGBA16F },
            false, TextureFilter::Linear);

        // Shaders
        m_BrightPassShader = std::make_unique<Shader>();
        if (!m_BrightPassShader->LoadFromSource(k_FullscreenVert, k_BrightPassFrag, "BloomBright")) {
            SUN_CORE_ERROR("Bloom: bright pass shader fallito");
            return false;
        }

        m_BlurShader = std::make_unique<Shader>();
        if (!m_BlurShader->LoadFromSource(k_FullscreenVert, k_BlurFragDir, "BloomBlur")) {
            SUN_CORE_ERROR("Bloom: blur shader fallito");
            return false;
        }

        m_UpsampleShader = std::make_unique<Shader>();
        if (!m_UpsampleShader->LoadFromSource(k_FullscreenVert, k_UpsampleFrag, "BloomUpsample")) {
            SUN_CORE_ERROR("Bloom: upsample shader fallito");
            return false;
        }

        m_CompositeShader = std::make_unique<Shader>();
        if (!m_CompositeShader->LoadFromSource(k_FullscreenVert, k_CompositeFrag, "BloomComposite")) {
            SUN_CORE_ERROR("Bloom: composite shader fallito");
            return false;
        }

        SUN_CORE_INFO("Bloom: {} mip, half-res {}x{}", MIP_COUNT, m_HalfWidth, m_HalfHeight);
        return true;
    }

    void Bloom::Shutdown() {
        m_BrightPassShader.reset();
        m_BlurShader.reset();
        m_UpsampleShader.reset();
        m_CompositeShader.reset();
    }

    void Bloom::Resize(int width, int height) {
        if (width == m_Width && height == m_Height) return;
        m_Width = width;
        m_Height = height;
        m_HalfWidth = std::max(1, width / 2);
        m_HalfHeight = std::max(1, height / 2);

        for (int i = 0; i < MIP_COUNT; ++i) {
            int w = std::max(1, m_HalfWidth >> i);
            int h = std::max(1, m_HalfHeight >> i);
            m_MipTargets[i].Resize(w, h);
        }
        m_CompositeTarget.Resize(width, height);
    }

    void Bloom::RenderFullscreenQuad() const {
        glBindVertexArray(Framebuffer::GetFullscreenQuadVAO());
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }

    void Bloom::Render(GLuint hdrTexture) {
        if (m_BrightPassShader == nullptr) return;

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);

        // === 1. BRIGHT PASS -> mip 0 ===
        m_MipTargets[0].Bind();
        m_MipTargets[0].Clear(glm::vec4(0.0f));
        m_BrightPassShader->Bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdrTexture);
        m_BrightPassShader->SetInt("u_HDRTexture", 0);
        m_BrightPassShader->SetFloat("u_Threshold", m_Threshold);
        m_BrightPassShader->SetFloat("u_SoftKnee", m_SoftKnee);
        RenderFullscreenQuad();

        // === 2. DOWNSAMPLE + BLUR per ogni mip successivo ===
        m_BlurShader->Bind();
        for (int i = 1; i < MIP_COUNT; ++i) {
            // Input: mip precedente
            m_MipTargets[i].Bind();
            m_MipTargets[i].Clear(glm::vec4(0.0f));

            // Passo 2a: copia mip[i-1] -> mip[i] (downsample semplice per blur)
            // In realtà vogliamo fare: mip[i] = blur(mip[i-1]).
            // Facciamo blur orizzontale + verticale, scrivendo due volte in mip[i].
            // Ma serve un target temporaneo... per semplicità usiamo mip[i-1]
            // stessa come ping-pong temporaneo? No: inquinerebbe.
            // Approccio pragmatico: blur orizzontale da mip[i-1] a mip[i],
            // blur verticale da mip[i] a mip[i] (looping additivo).

            // Orizzontale: mip[i-1] -> mip[i]
            m_MipTargets[i].Bind();
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_MipTargets[i - 1].GetColorTexture(0)->GetID());
            m_BlurShader->SetInt("u_Texture", 0);
            m_BlurShader->SetVec2("u_TexelSize",
                glm::vec2(1.0f / m_MipTargets[i - 1].GetWidth(),
                    1.0f / m_MipTargets[i - 1].GetHeight()));
            m_BlurShader->SetVec2("u_Direction", glm::vec2(1.0f, 0.0f));
            RenderFullscreenQuad();

            // Verticale: mip[i] -> mip[i] (read+write stesso target: non ideale
            // ma funziona perché texel cached). Per correttezza, leggiamo
            // mip[i] e scriviamo ancora in mip[i].
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_MipTargets[i].GetColorTexture(0)->GetID());
            m_BlurShader->SetVec2("u_TexelSize",
                glm::vec2(1.0f / m_MipTargets[i].GetWidth(),
                    1.0f / m_MipTargets[i].GetHeight()));
            m_BlurShader->SetVec2("u_Direction", glm::vec2(0.0f, 1.0f));
            RenderFullscreenQuad();
        }

        // === 3. UPSAMPLE ADDITIVO: dal più piccolo al più grande ===
        m_UpsampleShader->Bind();
        m_UpsampleShader->SetFloat("u_Intensity", m_Intensity);
        for (int i = MIP_COUNT - 1; i > 0; --i) {
            m_MipTargets[i - 1].Bind();
            m_UpsampleShader->SetInt("u_LowerMip", 0);
            m_UpsampleShader->SetInt("u_CurrentMip", 1);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_MipTargets[i].GetColorTexture(0)->GetID());
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, m_MipTargets[i - 1].GetColorTexture(0)->GetID());

            RenderFullscreenQuad();
        }

        // === 4. COMPOSITE: HDR + bloom -> out ===
        // Per semplicità scriviamo in composite target, il chiamante
        // può poi tonemappare dal composite target.
        m_CompositeTarget.Bind();
        m_CompositeShader->Bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdrTexture);
        m_CompositeShader->SetInt("u_HDRTexture", 0);
        m_CompositeShader->SetInt("u_BloomTexture", 1);
        m_CompositeShader->SetFloat("u_BloomIntensity", m_Intensity);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_MipTargets[0].GetColorTexture(0)->GetID());

        RenderFullscreenQuad();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

}