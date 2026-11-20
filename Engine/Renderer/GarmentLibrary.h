#pragma once
// Strati dei vestiti sul modello animato: costruzione (una volta per corpo + strato) e texture, in cache.
// Vedi GarmentShell.h per come nasce il guscio.
#include "GarmentShell.h"
#include "GearMesh.h"
#include "SkinnedModel.h"
#include "TeutonicArmor.h"
#include "Core/Texture.h"
#include "../Core/Logger/Log.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <utility>

namespace SunEngine {

    class GarmentLibrary {
    public:
        // root = cartella GameData (con la barra finale): i percorsi dei JSON sono relativi a questa
        void SetRoot(const std::string& root) { m_Root = root; }

        // Nome del corpo (cartella in GameData/Gear/: footman, candidate, vrepigh, pighcib...): i pezzi costruiti
        // dalla forgia (GearMesh.h) per quel corpo prendono il posto dei gusci generati
        void RegisterBody(const SkinnedModel& body, const std::string& folder) { m_BodyFolder[&body] = folder; }

        // Strato 'd' sul corpo 'body'. Ritorna false se non c'e' niente da disegnare (zona vuota, file mancante).
        bool Get(const SkinnedModel& body, const GarmentDef& d, GarmentDraw& out) {
            Entry* e = FindGear(body, d);   // pezzo della forgia, se c'e'
            if (!e) {
                if (body.authored) return false;   // modello d'autore senza pezzi della forgia: niente gusci sul suo corpo
                e = Find(body, d);
            }
            if (!e || !e->mesh) return false;
            out.mesh = e->mesh.get();
            out.texture = d.texture.empty() ? nullptr : Texture(d.texture);
            out.color = d.color;
            out.roughness = d.roughness;
            out.metallic = d.metallic;
            out.texScale = d.texTiles / e->height;
            out.alphaCut = out.texture ? d.alphaCut : 0.0f;
            out.invHeight = 1.0f / std::max(1e-4f, e->height);
            out.detail = d.detail;
            out.emissive = d.emissive;
            out.baked = e->gear ? 1 : 0;   // pezzo della forgia: occlusione nel vertice, pieghe vere (niente pieghe finte)
            return true;
        }

    private:
        struct Entry {
            std::unique_ptr<SkinnedMesh> mesh;   // null = provato, niente da disegnare
            float height = 1.0f;
            bool gear = false;                   // pezzo della forgia (GearMesh.h)
        };

        // Indice dei pezzi della forgia di un corpo: GameData/Gear/<corpo>/index.json -> {"pieces": {id: file}}
        struct GearIndex {
            bool loaded = false;
            std::unordered_map<std::string, std::string> files;   // id dello strato -> percorso del .gear
        };

        const GearIndex& IndexOf(const std::string& folder) {
            GearIndex& gi = m_GearIndex[folder];
            if (gi.loaded) return gi;
            gi.loaded = true;
            const std::string dir = m_Root + "Gear/" + folder + "/";
            std::ifstream in(dir + "index.json");
            if (!in) return gi;
            try {
                const nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
                if (j.contains("pieces") && j["pieces"].is_object())
                    for (auto it = j["pieces"].begin(); it != j["pieces"].end(); ++it)
                        if (it.value().is_string()) gi.files[it.key()] = dir + it.value().get<std::string>();
                SUN_CORE_INFO("Forgia: {0} pezzi per il corpo {1}", gi.files.size(), folder);
            }
            catch (const std::exception& ex) { SUN_CORE_WARN("Forgia: {0}index.json non valido ({1})", dir, ex.what()); }
            return gi;
        }

        // Pezzo della forgia per lo strato 'd' su questo corpo (null = non c'e', si usa il guscio generato)
        Entry* FindGear(const SkinnedModel& body, const GarmentDef& d) {
            auto bf = m_BodyFolder.find(&body);
            if (bf == m_BodyFolder.end() || d.id.empty() || !d.procedural.empty()) return nullptr;
            const GearIndex& gi = IndexOf(bf->second);
            auto fit = gi.files.find(d.id);
            if (fit == gi.files.end()) return nullptr;
            const std::string key = "gear|" + bf->second + "|" + d.id;
            auto it = m_Entries.find(key);
            if (it != m_Entries.end()) return it->second.mesh ? &it->second : nullptr;
            Entry& e = m_Entries[key];
            e.gear = true;
            std::vector<const std::vector<SkinnedVertex>*> parts;
            for (const auto& m : body.meshes) parts.push_back(&m.vertices);
            e.height = GarmentMeasure(parts).Height();
            std::vector<SkinnedVertex> v;
            std::vector<unsigned int> idx;
            std::string err;
            if (!ReadGearPiece(fit->second, body.boneInfoMap, v, idx, &err) || idx.empty()) {
                SUN_CORE_WARN("Forgia: pezzo '{0}' ({1}) non caricato: {2}", d.id, fit->second, err);
                return nullptr;   // si torna al guscio generato
            }
            SUN_CORE_INFO("Forgia: '{0}' sul corpo {1}, {2} triangoli", d.id, bf->second, idx.size() / 3);
            e.mesh = std::make_unique<SkinnedMesh>(std::move(v), std::move(idx));
            return &e;
        }

        static std::string Key(const SkinnedModel& body, const GarmentDef& d) {
            // Stesso id ma parametri diversi (es. due armature che riusano l'id): la chiave include i campi
            // che cambiano la forma
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%p|%s|%s%s|%.4f|%.3f|%.3f|%.3f|%.3f|%.3f|%.3f|%.2f|%d|%.4f|%.3f|%d|%zu",
                static_cast<const void*>(&body), d.id.c_str(), d.model.c_str(), d.procedural.c_str(), d.inflate, d.minWeight, d.heightRange.x, d.heightRange.y,
                d.facing.x + d.facing.y * 3.0f + d.facing.z * 7.0f, d.facingMin, d.noise, d.noiseScale, d.plateCells, d.plateStep,
                d.maxAbsX + d.minAbsX * 3.0f, d.side, d.bones.size());
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
            if (!d.procedural.empty()) {
                // Pezzo costruito su questo corpo (armatura teutonica: elmo, sopravveste, mantello, spallacci...)
                if (!BuildProceduralGarment(body, d.procedural, shell))
                    SUN_CORE_WARN("Vestito '{0}': pezzo procedurale '{1}' sconosciuto o corpo senza le ossa che servono", d.id, d.procedural);
            }
            else if (!d.model.empty()) {
                // Mesh modellata a parte, skinnata sullo stesso scheletro: ossa rimappate per nome
                SkinnedModel garment;
                const std::string path = m_Root + d.model;
                std::error_code ec;
                if (std::filesystem::exists(path, ec) && garment.LoadModel(path)) {
                    for (auto& m : garment.meshes) {
                        std::vector<SkinnedVertex> v = m.vertices;
                        GarmentRemapBones(v, garment.boneInfoMap, body.boneInfoMap);
                        for (auto& gv : v) gv.TexCoords = glm::vec2(1.0f, 1.0f);   // niente orli calcolati: tutto "lontano dal bordo"
                        const unsigned int base = static_cast<unsigned int>(shell.vertices.size());
                        shell.vertices.insert(shell.vertices.end(), v.begin(), v.end());
                        for (unsigned int i : m.indices) shell.indices.push_back(base + i);
                    }
                }
                else SUN_CORE_WARN("Vestito '{0}': modello {1} non trovato, uso il guscio generato", d.id, path);
            }
            if (shell.indices.empty() && d.procedural.empty()) {
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
        std::unordered_map<const SkinnedModel*, std::string> m_BodyFolder;
        std::unordered_map<std::string, GearIndex> m_GearIndex;
        std::map<std::string, std::unique_ptr<GpuTexture>> m_Textures;
    };
}
