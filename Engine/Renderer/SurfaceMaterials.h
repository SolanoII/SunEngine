#pragma once
// Materiali procedurali delle mappe (shader dei cubi nel DeferredRenderer, proiezione triplanare nel mondo).
// Le mappe descrivono le strutture solo con colore e dimensioni: il materiale si deduce da quelli (vedi
// GuessSurfaceMaterial), oppure si indica nel JSON con "material": "mud" / "concrete" / "metal" / "sandbags" /
// "wood" / "foliage" / "earthworks" / "rubble" / "none".
// Tavolozza grimdark: niente colori accesi, tutto sporco, consumato, bruciato.
#include <glm/glm.hpp>
#include <algorithm>
#include <string>

namespace SunEngine {

    enum SurfaceMaterial : int {
        Surface_None = 0,        // tinta unita (come prima)
        Surface_Mud = 1,         // terreno: fango, pozzanghere, crateri, ghiaia
        Surface_Concrete = 2,    // rockcrete: pannelli, crepe, colature, colpi, fuliggine alla base
        Surface_Metal = 3,       // lamiere rivettate: vernice scrostata, ruggine, colature di ruggine
        Surface_Sandbags = 4,    // sacchi di sabbia: file sfalsate, juta, fango
        Surface_Wood = 5,        // legno: venature, bruciature
        Surface_Foliage = 6,     // chiome secche e morte
        Surface_Earthworks = 7,  // terrapieni della trincea: terra battuta e assi di contenimento
        Surface_Rubble = 8,      // detriti: cemento spezzato e sporco
        Surface_Count
    };

    inline int SurfaceMaterialFromName(const std::string& n) {
        if (n == "mud" || n == "terrain") return Surface_Mud;
        if (n == "concrete" || n == "rockcrete") return Surface_Concrete;
        if (n == "metal" || n == "plates") return Surface_Metal;
        if (n == "sandbags") return Surface_Sandbags;
        if (n == "wood") return Surface_Wood;
        if (n == "foliage") return Surface_Foliage;
        if (n == "earthworks" || n == "earth") return Surface_Earthworks;
        if (n == "rubble") return Surface_Rubble;
        if (n == "none") return Surface_None;
        return -1;
    }

    // Materiale dedotto dal colore e dalla forma del blocco
    inline int GuessSurfaceMaterial(const glm::vec4& color, const glm::vec3& scale, bool terrain) {
        if (terrain) return Surface_Mud;
        const float r = color.r, g = color.g, b = color.b;
        const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
        const float sat = mx - mn;
        const float h = scale.y, footprint = std::max(scale.x, scale.z), thin = std::min(scale.x, scale.z);
        if (g > r + 0.06f && g > b + 0.06f) return Surface_Foliage;                          // verde: chiome
        if (sat > 0.08f && r > g && g > b) {                                                   // marroni
            if (thin < 1.0f && h > 1.8f && footprint < 1.2f) return Surface_Wood;             // tronchi, pali
            if (r > 0.45f && g > 0.40f && h <= 1.4f) return Surface_Sandbags;                 // beige basso: sacchi
            if (h <= 2.0f) return Surface_Earthworks;                                          // terrapieni
            return Surface_Wood;
        }
        if (r > 0.45f && g > 0.40f && b < 0.42f && h <= 1.4f) return Surface_Sandbags;
        if (sat < 0.08f) {                                                                     // grigi
            if (h >= 5.0f || footprint >= 40.0f) return Surface_Metal;                        // muraglioni di lamiera
            return Surface_Concrete;
        }
        return Surface_Concrete;
    }
}
