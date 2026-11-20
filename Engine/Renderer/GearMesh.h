#pragma once
// Vesti e armature costruite fuori dal gioco (Tools/GearForge, script di Blender) sul corpo vero del gioco.
//
// Il giro e' questo:
//   1) il gioco, avviato con la variabile d'ambiente SUN_GEAR_DUMP=<cartella>, scrive ogni corpo animato cosi'
//      come viene disegnato (dopo adattamento, muscoli e sagoma) in <cartella>/<corpo>.body;
//   2) la forgia costruisce in Blender i pezzi di ogni armatura su quel corpo (stoffa simulata, piastre
//      modellate, elmi misurati sul cranio) e li scrive in GameData/Gear/<corpo>/*.gear con un index.json;
//   3) qui il gioco li carica al posto dei gusci generati (GarmentShell.h) e li disegna con le stesse ossa.
// Se cambia un corpo (per esempio arriva quello dell'artista) si rifanno i passi 1 e 2.
//
// Formato dei file (binario, little endian):
//   .gear : "SGEAR001", u32 ossa, per osso u16 lunghezza + nome;
//           u32 vertici, per vertice f32 pos[3], f32 normale[3], f32 uv[2], i16 osso[4], f32 peso[4];
//           u32 indici, u32 indice[]
//           Le ossa del vertice sono posizioni nella tabella del file. uv.x = distanza dal bordo del pezzo
//           (frazione dell'altezza del corpo: orli, cuciture, spigoli consumati), uv.y = occlusione ambientale.
//   .body : "SBODY001", u32 ossa, per osso u16 lunghezza + nome + f32 offset[16] (colonne, come glm);
//           u32 vertici come sopra (uv.x = 1 per le parti aggiunte, es. muso e zanne del vrepigh); indici.
// Niente OpenGL qui: lettura e scrittura sono provabili fuori dal gioco.
#include "SkinnedVertex.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace SunEngine {

    namespace GearIO {
        inline bool WriteU32(FILE* f, uint32_t v) { return std::fwrite(&v, 4, 1, f) == 1; }
        inline bool ReadU32(FILE* f, uint32_t& v) { return std::fread(&v, 4, 1, f) == 1; }
        inline bool WriteName(FILE* f, const std::string& s) {
            const uint16_t n = static_cast<uint16_t>(std::min<size_t>(s.size(), 65535));
            return std::fwrite(&n, 2, 1, f) == 1 && (n == 0 || std::fwrite(s.data(), 1, n, f) == n);
        }
        inline bool ReadName(FILE* f, std::string& s) {
            uint16_t n = 0;
            if (std::fread(&n, 2, 1, f) != 1) return false;
            s.assign(n, '\0');
            return n == 0 || std::fread(&s[0], 1, n, f) == n;
        }
        inline bool WriteVertex(FILE* f, const SkinnedVertex& v) {
            float p[8] = { v.Position.x, v.Position.y, v.Position.z, v.Normal.x, v.Normal.y, v.Normal.z, v.TexCoords.x, v.TexCoords.y };
            int16_t b[4];
            for (int k = 0; k < 4; ++k) b[k] = static_cast<int16_t>(v.BoneIDs[k]);
            return std::fwrite(p, 4, 8, f) == 8 && std::fwrite(b, 2, 4, f) == 4 && std::fwrite(v.Weights, 4, 4, f) == 4;
        }
        inline bool ReadVertex(FILE* f, SkinnedVertex& v) {
            float p[8]; int16_t b[4];
            if (std::fread(p, 4, 8, f) != 8 || std::fread(b, 2, 4, f) != 4 || std::fread(v.Weights, 4, 4, f) != 4) return false;
            v.Position = glm::vec3(p[0], p[1], p[2]);
            v.Normal = glm::vec3(p[3], p[4], p[5]);
            v.TexCoords = glm::vec2(p[6], p[7]);
            for (int k = 0; k < 4; ++k) v.BoneIDs[k] = b[k];
            return true;
        }
    }

    // Corpo da esportare: le sue mesh (o le parti aggiunte, aux = true) con le ossa del modello
    struct GearBodyPart {
        const std::vector<SkinnedVertex>* vertices = nullptr;
        const std::vector<unsigned int>* indices = nullptr;
        bool aux = false;
    };

    // Scrive il corpo per la forgia. boneMap = nome -> (id, offset) del modello.
    inline bool WriteGearBody(const std::string& path, const std::unordered_map<std::string, BoneInfo>& boneMap, const std::vector<GearBodyPart>& parts) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        int maxId = -1;
        for (const auto& [name, info] : boneMap) maxId = std::max(maxId, info.id);
        std::vector<const std::string*> names(static_cast<size_t>(maxId + 1), nullptr);
        std::vector<glm::mat4> offs(static_cast<size_t>(maxId + 1), glm::mat4(1.0f));
        for (const auto& [name, info] : boneMap) if (info.id >= 0) { names[info.id] = &name; offs[info.id] = info.offsetMatrix; }
        bool ok = std::fwrite("SBODY001", 1, 8, f) == 8 && GearIO::WriteU32(f, static_cast<uint32_t>(names.size()));
        static const std::string kEmpty;
        for (size_t i = 0; ok && i < names.size(); ++i)
            ok = GearIO::WriteName(f, names[i] ? *names[i] : kEmpty) && std::fwrite(&offs[i][0][0], 4, 16, f) == 16;
        uint32_t nv = 0, ni = 0;
        for (const auto& p : parts) { nv += static_cast<uint32_t>(p.vertices->size()); ni += static_cast<uint32_t>(p.indices->size()); }
        ok = ok && GearIO::WriteU32(f, nv);
        for (const auto& p : parts) for (const auto& v0 : *p.vertices) {
            if (!ok) break;
            SkinnedVertex v = v0;
            v.TexCoords = glm::vec2(p.aux ? 1.0f : 0.0f, 0.0f);
            ok = GearIO::WriteVertex(f, v);
        }
        ok = ok && GearIO::WriteU32(f, ni);
        uint32_t base = 0;
        for (const auto& p : parts) {
            for (unsigned int i : *p.indices) { const uint32_t k = base + i; if (ok) ok = GearIO::WriteU32(f, k); }
            base += static_cast<uint32_t>(p.vertices->size());
        }
        std::fclose(f);
        return ok;
    }

    // Legge un pezzo .gear; le ossa dei vertici diventano quelle del corpo con lo stesso nome (senza prefisso
    // mixamo). Ossa che il corpo non ha: peso tolto e rinormalizzato.
    inline bool ReadGearPiece(const std::string& path, const std::unordered_map<std::string, BoneInfo>& bodyBones,
                              std::vector<SkinnedVertex>& verts, std::vector<unsigned int>& indices, std::string* error = nullptr) {
        auto fail = [&](const char* why) { if (error) *error = why; return false; };
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return fail("file non trovato");
        char magic[8];
        uint32_t nb = 0, nv = 0, ni = 0;
        if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "SGEAR001", 8) != 0 || !GearIO::ReadU32(f, nb) || nb > 4096) { std::fclose(f); return fail("intestazione non valida"); }
        auto suffix = [](const std::string& n) { const auto p = n.rfind(':'); return p == std::string::npos ? n : n.substr(p + 1); };
        std::unordered_map<std::string, int> bodyBySuffix;
        for (const auto& [name, info] : bodyBones) bodyBySuffix[suffix(name)] = info.id;
        std::vector<int> map(nb, -1);
        for (uint32_t i = 0; i < nb; ++i) {
            std::string name;
            if (!GearIO::ReadName(f, name)) { std::fclose(f); return fail("ossa illeggibili"); }
            auto it = bodyBySuffix.find(suffix(name));
            if (it != bodyBySuffix.end()) map[i] = it->second;
        }
        if (!GearIO::ReadU32(f, nv) || nv > 4000000) { std::fclose(f); return fail("vertici illeggibili"); }
        verts.resize(nv);
        for (uint32_t i = 0; i < nv; ++i) {
            SkinnedVertex& v = verts[i];
            if (!GearIO::ReadVertex(f, v)) { std::fclose(f); return fail("vertici troncati"); }
            float total = 0.0f;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                const int b = v.BoneIDs[k];
                v.BoneIDs[k] = (b >= 0 && b < static_cast<int>(nb)) ? map[b] : -1;
                if (v.BoneIDs[k] < 0) v.Weights[k] = 0.0f;
                total += v.Weights[k];
            }
            if (total > 1e-4f) for (float& w : v.Weights) w /= total;
        }
        if (!GearIO::ReadU32(f, ni) || ni > 12000000) { std::fclose(f); return fail("indici illeggibili"); }
        indices.resize(ni);
        const bool ok = ni == 0 || std::fread(indices.data(), 4, ni, f) == ni;
        std::fclose(f);
        if (!ok) return fail("indici troncati");
        for (unsigned int& i : indices) if (i >= nv) return fail("indice fuori dai vertici");
        return true;
    }
}
