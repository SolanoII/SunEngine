#pragma once
// Modelli d'autore: personaggi fatti da un artista (FBX, glTF/GLB) con le loro texture PBR.
//
// Dove si mettono: GameData/Models/Characters/<corpo>/  (corpo = guardsman, ork, ogryn, gretchin, neophyte, astartes)
//   - il modello: model.fbx / model.glb / model.gltf (o qualunque nome con quelle estensioni)
//   - le texture: accanto al modello o nella sottocartella Textures/, oppure dentro il file (FBX/GLB "embedded")
//   - model.json (facoltativo): scala, altezza, materiali e texture indicati a mano
//   - Animations/ (facoltativo): clip proprie con i nomi standard del gioco (vedi la guida per gli artisti)
// Se la cartella contiene un modello, il gioco usa quello al posto del corpo procedurale di quel tipo.
//
// Texture di ogni materiale, in quest'ordine:
//   1. model.json -> "materials" -> nome del materiale
//   2. quelle indicate nel file del modello (anche incorporate)
//   3. per nome del file: <Materiale>_BaseColor.png, _Normal, _OcclusionRoughnessMetallic / _ORM, _Roughness,
//      _Metallic, _AO, _Emissive, _Opacity (nomi di esportazione di Substance Painter e simili)
#include "SkinnedModel.h"
#include <string>

namespace SunEngine {

    struct AuthoredCharacterInfo {
        std::string folder;       // cartella del corpo (con / finale)
        std::string modelPath;    // file del modello trovato
        std::string animsDir;     // cartella Animations/ propria (vuota = usa quelle del gioco)
    };

    // true se nella cartella c'e' un modello d'autore
    bool FindAuthoredCharacter(const std::string& folder, AuthoredCharacterInfo& out);

    // Carica modello, materiali, texture e impostazioni di model.json. targetHeightCm = altezza del corpo che
    // sostituisce (per riconoscere le unita' del file: metri, centimetri, millimetri), 0 = sconosciuta.
    bool LoadAuthoredCharacter(const AuthoredCharacterInfo& info, SkinnedModel& out, float targetHeightCm = 0.0f);

    // Altezza in posa di riposo nelle unita' del file (estensione verticale dei vertici)
    float SkinnedModelHeight(const SkinnedModel& model);
    // Punto piu' basso e piu' alto in posa di riposo (unita' del file), come viene disegnato
    void SkinnedBindBounds(const SkinnedModel& model, float& lo, float& hi);
}
