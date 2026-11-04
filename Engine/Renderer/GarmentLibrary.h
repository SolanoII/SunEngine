#pragma once
// Strati dei vestiti sul modello animato: costruzione (una volta per corpo + strato) e texture, in cache.
// Vedi GarmentShell.h per come nasce il guscio.
#include "GarmentShell.h"
#include "SkinnedModel.h"
#include "Core/Texture.h"
#include "../Core/Logger/Log.h"
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace SunEngine {

    class GarmentLibrary {
    public:
        // root = cartella GameData (con la barra finale): i percorsi dei JSON sono relativi a questa
        void SetRoot(const std::string& root) { m_Root = root; }

        // Strato 'd' sul corpo 'body'. Ritorna false se non c'e' niente da disegnare (zona vuota, file mancante).
        bool Get(const SkinnedModel& body, const GarmentDef& d, GarmentDraw& out) {
            Entry* e = Find(body, d);
            if (!e || !e->mesh) return false;
            out.mesh = e->mesh.get();
            out.texture = d.texture.empty() ? nullptr : Texture(d.texture);
            out.color = d.color;
            out.roughness = d.roughness;
            out.metallic = d.metallic;
            out.texScale = d.texTiles / e->height;
            out.alphaCut = out.texture ? d.alphaCut : 0.0f;
            return true;
        }

    private:
        struct Entry {
            std::unique_ptr<SkinnedMesh> mesh;   // null = provato, niente da disegnare
            float height = 1.0f;
        };

        static std::string Key(const SkinnedModel& body, const GarmentDef& d) {
            // Stesso id ma parametri diversi (es. due armature che riusano l'id): la chiave include i campi
            // che cambiano la forma
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%p|%s|%s|%.4f|%.3f|%.3f|%.3f|%.3f|%.3f|%.3f|%.2f|%d|%.4f|%.3f|%d|%zu",
                static_cast<const void*>(&body), d.id.c_str(), d.model.c_str(), d.inflate, d.minWeight, d.heightRange.x, d.heightRange.y,
                d.facing.x + d.facing.y * 3.0f + d.facing.z * 7.0f, d.facingMin, d.noise, d.noiseScale, d.plateCells, d.plateStep,
                d.maxAbsX, d.side, d.bones.size());
            return buf;
        }

        Entry* Find(const SkinnedModel& body, const GarmentDef& d) {
            const std::string key = Key(body, d);
            auto it = m_Entries.find(key);
            if (it != m_Entries.end()) return &it->second;
            Entry& e = m_Entries[key];

            std::vector<const std::vector<SkinnedVertex>*> parts;
            for (const auto& m : body.meshes) parts.push_back(&m.vertices);
            const GarmentBodyInfo info = GarmentMeasure(parts);
            e.height = info.Height();

            GarmentShell shell;
            if (!d.model.empty()) {
                // Mesh modellata a parte, skinnata sullo stesso scheletro: ossa rimappate per nome
                SkinnedModel garment;
                const std::string path = m_Root + d.model;
                std::error_code ec;
                if (std::filesystem::exists(path, ec) && garment.LoadModel(path)) {
                    for (auto& m : garment.meshes) {
                        std::vector<SkinnedVertex> v = m.vertices;
                        GarmentRemapBones(v, garment.boneInfoMap, body.boneInfoMap);
                        const unsigned int base = static_cast<unsigned int>(shell.vertices.size());
                        shell.vertices.insert(shell.vertices.end(), v.begin(), v.end());
                        for (unsigned int i : m.indices) shell.indices.push_back(base + i);
                    }
                }
                else SUN_CORE_WARN("Vestito '{0}': modello {1} non trovato, uso il guscio generato", d.id, path);
            }
            if (shell.indices.empty()) {
                const std::vector<int> boneIds = GarmentBoneIds(body.boneInfoMap, d);
                for (const auto& m : body.meshes) GarmentBuildShell(m.vertices, m.indices, boneIds, d, info, shell);
            }
            if (shell.indices.empty()) {
                SUN_CORE_WARN("Vestito '{0}': nessun triangolo nella zona indicata", d.id);
                return &e;
            }
            SUN_CORE_INFO("Vestito '{0}': {1} triangoli", d.id, shell.indices.size() / 3);
            e.mesh = std::make_unique<SkinnedMesh>(std::move(shell.vertices), std::move(shell.indices));
            return &e;
        }

        const GpuTexture* Texture(const std::string& rel) {
            auto it = m_Textures.find(rel);
            if (it != m_Textures.end()) return it->second.get();
            auto tex = std::make_unique<GpuTexture>();
            const bool ok = tex->LoadFromFile(m_Root + rel, false, true);   // la texture e' gia' tileabile: niente flip
            if (!ok) tex.reset();
            const GpuTexture* p = tex.get();
            m_Textures[rel] = std::move(tex);
            return p;
        }

        std::string m_Root;
        std::map<std::string, Entry> m_Entries;
        std::map<std::string, std::unique_ptr<GpuTexture>> m_Textures;
    };
}
