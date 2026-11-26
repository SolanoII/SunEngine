#pragma once
// Cielo fotografico (HDRI equirettangolare, es. Poly Haven CC0) e luce d'ambiente che ne deriva.
//
//   - cielo visibile: cubemap 512 con tutti i mipmap (anche per la nebbia, che ne legge un livello sfocato)
//   - riflessi: cubemap prefiltrata GGX, un livello per ruvidita' (0 = specchio, ultimo = opaco)
//   - luce diffusa: armoniche sferiche di 3 bande (9 coefficienti) calcolate sulla CPU dall'immagine
// Il passaggio di illuminazione li usa al posto del cielo procedurale (DeferredRenderer, Atmosphere::hdri).
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace SunEngine {

    class Shader;

    class EnvironmentMap {
    public:
        ~EnvironmentMap();
        // Carica l'HDRI (.hdr) e prepara cubemap, riflessi e armoniche. false = niente cielo fotografico.
        bool Load(const std::string& path);
        bool IsReady() const { return m_SpecCube != 0; }
        GLuint SkyCube() const { return m_SkyCube; }
        GLuint SpecCube() const { return m_SpecCube; }
        int SpecLevels() const { return kSpecLevels; }
        const glm::vec3* SH() const { return m_SH; }
        const std::string& Path() const { return m_Path; }
        void Release();

    private:
        static constexpr int kSkySize = 512;
        static constexpr int kSpecSize = 128;
        static constexpr int kSpecLevels = 6;
        GLuint m_Equirect = 0, m_SkyCube = 0, m_SpecCube = 0, m_FBO = 0;
        glm::vec3 m_SH[9]{};
        std::string m_Path;
        std::unique_ptr<Shader> m_ToCube, m_Prefilter;
    };
}
