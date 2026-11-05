#pragma once

#include <vector>
#include <glm/glm.hpp>

namespace SunEngine {

    // Effetti visivi "luminosi": sprite sempre rivolti alla camera, strisce (raggi, traccianti) e nastri (scie dei
    // fendenti), con forme morbide calcolate nello shader. Due gruppi: additivo (luce: lampi, scintille, laser,
    // plasma, fuoco) e trasparente normale (fumo, polvere, sangue). Si disegna dopo la scena, col depth test ma
    // senza scrivere la profondita'.
    class VfxRenderer {
    public:
        enum Shape {
            Glow = 0,     // bagliore morbido con nucleo
            Spark = 1,    // striscia sottile e luminosa (scintille, traccianti, raggi): nucleo bianco
            Smoke = 2,    // sbuffo con bordo irregolare
            Ring = 3,     // anello (onde d'urto, parata)
            Disc = 4,     // disco pieno dal bordo netto (gocce, schegge)
            Ribbon = 5,   // nastro: sfuma ai bordi e lungo la lunghezza (scie dei fendenti)
            Flame = 6     // lingua di fuoco: bagliore con bordo irregolare
        };

        static void Init();
        static void Shutdown();
        static bool Ready();

        static void Begin(const glm::mat4& view, const glm::mat4& viewProj);
        // Sprite rivolto alla camera. rotation in radianti, seed per il rumore delle forme irregolari.
        static void Billboard(const glm::vec3& pos, float size, const glm::vec4& color, Shape shape, bool additive = true,
            float rotation = 0.0f, float seed = 0.0f);
        // Quadrato steso orizzontale (onde d'urto a terra, bruciature, pozze di sangue)
        static void Flat(const glm::vec3& pos, float size, const glm::vec4& color, Shape shape, bool additive = false,
            float rotation = 0.0f, float seed = 0.0f);
        // Striscia da a a b larga width, girata verso la camera
        static void Streak(const glm::vec3& a, const glm::vec3& b, float width, const glm::vec4& colA, const glm::vec4& colB,
            Shape shape = Spark, bool additive = true);
        // Nastro lungo una serie di punti (larghezza e colore per punto)
        static void RibbonStrip(const std::vector<glm::vec3>& pts, const std::vector<float>& widths,
            const std::vector<glm::vec4>& colors, Shape shape = Ribbon, bool additive = true);
        static void End();   // disegna tutto e ripristina lo stato di OpenGL

    private:
        struct Vertex { glm::vec3 pos; glm::vec2 uv; glm::vec4 color; float shape; float seed; };
        static void Quad(std::vector<Vertex>& out, const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
            const glm::vec4& c0, const glm::vec4& c1, const glm::vec4& c2, const glm::vec4& c3, float shape, float seed,
            const glm::vec2& uv0 = { 0, 0 }, const glm::vec2& uv1 = { 1, 0 }, const glm::vec2& uv2 = { 1, 1 }, const glm::vec2& uv3 = { 0, 1 });
        static void Flush(std::vector<Vertex>& verts, bool additive);

        static unsigned int s_VAO, s_VBO, s_Program;
        static int s_VPLoc;
        static glm::mat4 s_ViewProj;
        static glm::vec3 s_CamRight, s_CamUp, s_CamPos;
        static std::vector<Vertex> s_Additive, s_Alpha;
    };

}
