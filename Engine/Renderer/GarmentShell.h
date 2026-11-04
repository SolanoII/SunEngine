#pragma once
// Vestiti e armature sul modello animato: strati skinnati SOPRA il corpo, non cubi agganciati alle ossa.
//
// Ogni strato ("garment") e' una mesh che usa le stesse ossa (e gli stessi pesi) del corpo, quindi segue
// ogni animazione senza compenetrare. Due modi di ottenerla:
//   1) guscio generato dal corpo stesso: si prendono i triangoli delle zone indicate (ossa, fascia di
//      altezza, lato davanti/dietro) e si gonfiano lungo la normale. Rumore e "piastre" danno spessori
//      irregolari, la texture (triplanare, con alpha per strappi e buchi) da' il materiale.
//   2) mesh esterna ("model": FBX skinnato sullo stesso scheletro mixamo): le ossa vengono rimappate per
//      nome su quelle del corpo. Per vestiti modellati a mano in Blender.
// Questo file non usa OpenGL: la costruzione e' testabile offline. La parte GPU sta in GarmentLibrary.h.
#include "SkinnedVertex.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SunEngine {

    class SkinnedMesh;
    class GpuTexture;

    // Cosa il render deve disegnare per uno strato (i puntatori restano validi finche' vive la GarmentLibrary)
    struct GarmentDraw {
        const SkinnedMesh* mesh = nullptr;
        const GpuTexture* texture = nullptr;   // null = solo colore
        glm::vec4 color{ 1.0f };
        float roughness = 0.8f;
        float metallic = 0.0f;
        float texScale = 1.0f;                 // ripetizioni per unita' del modello (bind pose)
        float alphaCut = 0.0f;
    };

    struct GarmentDef {
        std::string id;
        std::vector<std::string> bones;      // ossa della zona, senza prefisso ("Spine1", "LeftUpLeg"...)
        float minWeight = 0.5f;              // peso minimo delle ossa della zona sul vertice
        glm::vec2 heightRange{ 0.0f, 1.0f }; // fascia di altezza (frazione dell'altezza del modello in posa T)
        int side = 0;                        // +1 solo il lato sinistro del modello (x >= 0), -1 solo il destro
        float maxAbsX = 0.0f;                // >0: solo |x| <= maxAbsX (frazione dell'altezza), es. elmetto senza orecchie
        glm::vec3 facing{ 0.0f };            // se non nullo: solo i vertici con normale . facing >= facingMin
        float facingMin = -1.0f;             // (es. facing [0,0,1] e 0.15 = solo il davanti)
        float inflate = 0.008f;              // spessore (frazione dell'altezza del modello)
        float noise = 0.0f;                  // irregolarita' dello spessore (frazione di inflate)
        float noiseScale = 14.0f;            // frequenza del rumore (onde sull'altezza del modello)
        int plateCells = 0;                  // >0: piastre a scalini (celle sull'altezza del modello)
        float plateStep = 0.0f;              // scalino massimo fra piastre (frazione dell'altezza)
        std::string texture;                 // png (percorso relativo a GameData)
        float texTiles = 6.0f;               // ripetizioni della texture sull'altezza del modello
        glm::vec4 color{ 1.0f };             // tinta moltiplicata alla texture
        float roughness = 0.8f;
        float metallic = 0.0f;
        float alphaCut = 0.0f;               // >0: dove l'alpha della texture e' sotto soglia c'e' un buco
        std::string model;                   // opzionale: mesh skinnata esterna (sostituisce il guscio)
    };

    struct GarmentShell {
        std::vector<SkinnedVertex> vertices;
        std::vector<unsigned int> indices;
    };

    struct GarmentBodyInfo {
        float minY = 0.0f, maxY = 1.0f;
        float Height() const { return std::max(0.001f, maxY - minY); }
    };

    // Nome dell'osso senza prefisso mixamo ("mixamorig1:Spine1" -> "Spine1")
    inline std::string GarmentBoneSuffix(const std::string& name) {
        const auto p = name.rfind(':');
        return p == std::string::npos ? name : name.substr(p + 1);
    }

    inline uint32_t GarmentHash(int x, int y, int z) {
        uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(z) * 83492791u;
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        return h;
    }
    inline float GarmentHash01(int x, int y, int z) { return (GarmentHash(x, y, z) & 0xFFFFFFu) / 16777215.0f; }

    // Rumore 3D liscio in [0,1] (value noise trilineare): stesso valore per vertici nella stessa posizione,
    // quindi niente crepe sulle cuciture UV del modello.
    inline float GarmentValueNoise(const glm::vec3& p) {
        const glm::vec3 f = glm::floor(p);
        const glm::vec3 t = p - f;
        const glm::vec3 s = t * t * (3.0f - 2.0f * t);
        const int x = static_cast<int>(f.x), y = static_cast<int>(f.y), z = static_cast<int>(f.z);
        auto h = [&](int dx, int dy, int dz) { return GarmentHash01(x + dx, y + dy, z + dz); };
        const float a = glm::mix(glm::mix(h(0, 0, 0), h(1, 0, 0), s.x), glm::mix(h(0, 1, 0), h(1, 1, 0), s.x), s.y);
        const float b = glm::mix(glm::mix(h(0, 0, 1), h(1, 0, 1), s.x), glm::mix(h(0, 1, 1), h(1, 1, 1), s.x), s.y);
        return glm::mix(a, b, s.z);
    }

    inline GarmentBodyInfo GarmentMeasure(const std::vector<const std::vector<SkinnedVertex>*>& meshes) {
        GarmentBodyInfo info{ FLT_MAX, -FLT_MAX };
        for (const auto* m : meshes) for (const auto& v : *m) { info.minY = std::min(info.minY, v.Position.y); info.maxY = std::max(info.maxY, v.Position.y); }
        if (info.minY > info.maxY) info = GarmentBodyInfo{};
        return info;
    }

    // Guscio di una mesh del corpo per lo strato 'd'. boneIds = id delle ossa (del corpo) della zona.
    inline void GarmentBuildShell(const std::vector<SkinnedVertex>& verts, const std::vector<unsigned int>& indices,
                                  const std::vector<int>& boneIds, const GarmentDef& d, const GarmentBodyInfo& body,
                                  GarmentShell& out) {
        const float H = body.Height();
        const size_t n = verts.size();
        std::vector<char> keep(n, 0);
        const bool useFacing = glm::dot(d.facing, d.facing) > 1e-6f;
        const glm::vec3 facing = useFacing ? glm::normalize(d.facing) : glm::vec3(0.0f);
        for (size_t i = 0; i < n; ++i) {
            const auto& v = verts[i];
            float w = 0.0f;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k)
                if (v.BoneIDs[k] >= 0 && std::find(boneIds.begin(), boneIds.end(), v.BoneIDs[k]) != boneIds.end()) w += v.Weights[k];
            if (w < d.minWeight) continue;
            const float hy = (v.Position.y - body.minY) / H;
            if (hy < d.heightRange.x || hy > d.heightRange.y) continue;
            if (d.side != 0 && v.Position.x * static_cast<float>(d.side) < -0.01f * H) continue;   // piccola sovrapposizione al centro
            if (d.maxAbsX > 0.0f && std::abs(v.Position.x) / H > d.maxAbsX) continue;
            if (useFacing && glm::dot(glm::normalize(v.Normal), facing) < d.facingMin) continue;
            keep[i] = 1;
        }
        // Normali saldate: molti FBX hanno i vertici duplicati per triangolo (o sulle cuciture UV). Gonfiando
        // ognuno lungo la sua normale il guscio si aprirebbe in crepe: si usa la media delle normali dei
        // vertici nella stessa posizione.
        const float q = H * 1e-4f;
        auto key = [&](const glm::vec3& p) {
            const int64_t x = static_cast<int64_t>(std::llround(p.x / q)), y = static_cast<int64_t>(std::llround(p.y / q)), z = static_cast<int64_t>(std::llround(p.z / q));
            return static_cast<uint64_t>((x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL));
        };
        std::unordered_map<uint64_t, glm::vec3> welded;
        for (size_t i = 0; i < n; ++i) if (keep[i]) welded[key(verts[i].Position)] += verts[i].Normal;
        std::vector<int> remap(n, -1);
        auto emit = [&](unsigned int i) -> unsigned int {
            if (remap[i] < 0) {
                SkinnedVertex v = verts[i];
                glm::vec3 nrm = welded[key(v.Position)];
                nrm = glm::length(nrm) > 1e-6f ? glm::normalize(nrm) : glm::vec3(0.0f, 1.0f, 0.0f);
                const glm::vec3 pn = v.Position / H;   // posizione normalizzata: stesso aspetto su corpi di taglia diversa
                float thick = d.inflate;
                if (d.noise > 0.0f) thick *= 1.0f + d.noise * (GarmentValueNoise(pn * d.noiseScale) * 2.0f - 1.0f);
                if (d.plateCells > 0 && d.plateStep > 0.0f) {
                    const glm::vec3 c = glm::floor(pn * static_cast<float>(d.plateCells));
                    thick += d.plateStep * GarmentHash01(static_cast<int>(c.x), static_cast<int>(c.y), static_cast<int>(c.z));
                }
                v.Position += nrm * (thick * H);
                remap[i] = static_cast<int>(out.vertices.size());
                out.vertices.push_back(v);
            }
            return static_cast<unsigned int>(remap[i]);
        };
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
            if (a >= n || b >= n || c >= n || !keep[a] || !keep[b] || !keep[c]) continue;
            out.indices.push_back(emit(a));
            out.indices.push_back(emit(b));
            out.indices.push_back(emit(c));
        }
    }

    // Id (nel corpo) delle ossa indicate dallo strato, confrontando i nomi senza prefisso.
    inline std::vector<int> GarmentBoneIds(const std::unordered_map<std::string, BoneInfo>& boneMap, const GarmentDef& d) {
        std::vector<int> ids;
        for (const auto& [name, info] : boneMap)
            if (std::find(d.bones.begin(), d.bones.end(), GarmentBoneSuffix(name)) != d.bones.end()) ids.push_back(info.id);
        return ids;
    }

    // Mesh esterna: gli id delle ossa della mesh (sue) diventano quelli del corpo con lo stesso nome.
    // Ossa che il corpo non ha: il peso viene tolto e i rimanenti rinormalizzati.
    inline void GarmentRemapBones(std::vector<SkinnedVertex>& verts, const std::unordered_map<std::string, BoneInfo>& garmentBones,
                                  const std::unordered_map<std::string, BoneInfo>& bodyBones) {
        std::unordered_map<std::string, int> bodyBySuffix;
        for (const auto& [name, info] : bodyBones) bodyBySuffix[GarmentBoneSuffix(name)] = info.id;
        std::unordered_map<int, int> map;
        for (const auto& [name, info] : garmentBones) {
            auto it = bodyBySuffix.find(GarmentBoneSuffix(name));
            map[info.id] = it == bodyBySuffix.end() ? -1 : it->second;
        }
        for (auto& v : verts) {
            float total = 0.0f;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                if (v.BoneIDs[k] < 0) continue;
                auto it = map.find(v.BoneIDs[k]);
                v.BoneIDs[k] = it == map.end() ? -1 : it->second;
                if (v.BoneIDs[k] < 0) v.Weights[k] = 0.0f;
                total += v.Weights[k];
            }
            if (total > 1e-4f) for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) v.Weights[k] /= total;
        }
    }
}
