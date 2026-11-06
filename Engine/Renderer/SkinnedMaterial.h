#pragma once
// Materiale di un modello d'autore (fatto da un artista): texture PBR legate alle coordinate UV del modello.
// Lo shader dei modelli animati lo usa al posto dei dettagli procedurali (pelle, stoffa, armatura generate).
//   albedo   colore (sRGB), alfa = trasparenza per capelli/stracci se alphaCut > 0
//   normal   normal map in spazio tangente (convenzione OpenGL, verde = su; DirectX con normalDirectX)
//   orm      R = occlusione ambientale, G = rugosita', B = metallo (come "OcclusionRoughnessMetallic" di Unreal
//            e la texture metallicRoughness del glTF). Le mappe separate vengono unite in questa al caricamento.
//   emissive parti che si illuminano (lenti, display, plasma)
#include "Core/Texture.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace SunEngine {

    enum SkinnedMaterialKind : int {
        SkinMat_Generic = 0,
        SkinMat_Skin = 1,     // pelle: luce sotto la superficie (opzione "Luce sotto la pelle")
        SkinMat_Cloth = 2,    // stoffa, cuoio
        SkinMat_Metal = 3,    // armature, armi, metallo
        SkinMat_Eye = 4,
        SkinMat_Hair = 5,     // capelli, peli: ritaglio con l'alfa
    };

    struct SkinnedMaterial {
        std::string name;
        std::shared_ptr<GpuTexture> albedo, normal, orm, emissive;
        glm::vec4 baseColor{ 1.0f };      // moltiplica la texture colore (o e' il colore se non c'e' texture)
        float roughness = 1.0f;           // moltiplica il canale G dell'ORM (senza ORM: valore diretto)
        float metallic = 1.0f;            // moltiplica il canale B dell'ORM (senza ORM: valore diretto)
        glm::vec3 emissiveColor{ 0.0f };  // senza texture emissiva: colore emesso
        float emissiveStrength = 1.0f;
        float normalStrength = 1.0f;
        bool normalDirectX = false;       // normal map in convenzione DirectX (verde invertito: Unreal, 3ds Max)
        int kind = SkinMat_Generic;
        float alphaCut = 0.0f;            // > 0: i pixel con alfa sotto soglia sono buchi
        bool tint = false;                // moltiplica il colore per quello dell'unita' (colore scelto nel garage)
        float grime = 0.0f;               // sporco grimdark del motore sopra le texture (0 = niente, 1 = pieno)
        bool hidden = false;              // non disegnare (es. un corpo di riferimento lasciato nel file)
    };
}
