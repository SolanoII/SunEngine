#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

#include "PlayerProfile.h"

namespace SunEngine {

    // Albero di ricerca delle unita', stile World of Tanks.
    //  - Ogni nodo e' un tipo di unita' (id blueprint) con un nodo "padre".
    //  - Sbloccare (ricercare) un nodo costa PUNTI STUDIO dell'unita' PADRE (si guadagnano giocando con
    //    quel tipo di unita'); i nodi radice sono gia' sbloccati.
    //  - Un nodo sbloccato si "acquista" con ENERGIA: crea un'unita' nuova nel garage (anche piu' volte).
    //  - Nodi OGGETTO ("item": id di un'arma o dotazione): si ricercano coi punti studio della prima unita'
    //    risalendo l'albero (es. la chiave inglese sotto il Gretchin usa i punti studio del Gretchin) e poi si
    //    comprano nel garage, unita' per unita'. Gli oggetti senza nodo (dotazione base) sono sempre disponibili.
    // Solo dati e regole: la UI sta in GameLayer. Dati in GameData/Research/research_tree.json.
    struct ResearchNode {
        std::string id;
        std::string unitId;        // id del blueprint dell'unita' (vuoto nei nodi oggetto)
        std::string itemId;        // id del blueprint dell'arma/dotazione/armatura (nodi oggetto)
        std::vector<std::string> itemIds;   // tutti gli oggetti sbloccati dal nodo ("item" e/o "items")
        std::string name;          // nome mostrato nell'albero
        std::string parentId;      // vuoto = radice
        std::string defaultName = "Recluta";   // nome dato alle nuove unita' acquistate
        std::array<float, 4> color{ 0.6f, 0.6f, 0.6f, 1.0f };
        int tier = 1;
        float col = 0.0f;          // posizione nel disegno (colonne da sinistra a destra)
        float row = 0.0f;
        int64_t researchCost = 0;  // punti studio del padre
        int64_t purchaseCost = 0;  // energia
    };

    struct ResearchFaction {
        std::string id;
        std::string name;
        std::array<float, 3> accent{ 0.8f, 0.8f, 0.8f };
        std::vector<ResearchNode> nodes;
    };

    enum class ResearchStatus {
        Researched,        // sbloccato
        Researchable,      // sbloccabile adesso
        LockedParent,      // il padre non e' ancora sbloccato
        LockedPoints       // padre sbloccato ma punti studio insufficienti
    };

    class ResearchTree {
    public:
        bool IsLoaded() const { return !m_Factions.empty(); }
        const std::vector<ResearchFaction>& GetFactions() const { return m_Factions; }

        // Carica e valida il file. In caso di errore ritorna false e (se richiesto) descrive il problema.
        bool Load(const std::filesystem::path& file, std::string* error = nullptr) {
            m_Factions.clear();
            m_ById.clear();
            auto fail = [&](const std::string& msg) { if (error) *error = msg; m_Factions.clear(); m_ById.clear(); return false; };
            try {
                std::ifstream in(file, std::ios::binary);
                if (!in) return fail("file non apribile: " + file.string());
                nlohmann::json j = nlohmann::json::parse(in);
                if (!j.contains("factions") || !j["factions"].is_array()) return fail("manca l'array 'factions'");

                for (const auto& jf : j["factions"]) {
                    ResearchFaction f;
                    f.id = jf.value("id", std::string());
                    f.name = jf.value("name", f.id);
                    if (jf.contains("accent") && jf["accent"].is_array() && jf["accent"].size() >= 3) {
                        for (int i = 0; i < 3; ++i) f.accent[i] = jf["accent"][i].get<float>();
                    }
                    if (f.id.empty()) return fail("fazione senza 'id'");
                    for (const auto& jn : jf.value("nodes", nlohmann::json::array())) {
                        ResearchNode n;
                        n.id = jn.value("id", std::string());
                        n.unitId = jn.value("unit", std::string());
                        n.itemId = jn.value("item", std::string());
                        if (!n.itemId.empty()) n.itemIds.push_back(n.itemId);
                        if (jn.contains("items") && jn["items"].is_array()) {
                            for (const auto& it : jn["items"]) if (it.is_string()) n.itemIds.push_back(it.get<std::string>());
                        }
                        if (n.itemId.empty() && !n.itemIds.empty()) n.itemId = n.itemIds.front();
                        n.name = jn.value("name", n.unitId.empty() ? n.itemId : n.unitId);
                        n.parentId = jn.value("parent", std::string());
                        n.defaultName = jn.value("default_name", std::string("Recluta"));
                        n.tier = jn.value("tier", 1);
                        n.col = jn.value("col", 0.0f);
                        n.row = jn.value("row", 0.0f);
                        n.researchCost = std::max<int64_t>(0, jn.value("research_cost", 0));
                        n.purchaseCost = std::max<int64_t>(0, jn.value("purchase_cost", 0));
                        if (jn.contains("color") && jn["color"].is_array() && jn["color"].size() >= 4) {
                            for (int i = 0; i < 4; ++i) n.color[i] = jn["color"][i].get<float>();
                        }
                        if (n.id.empty() || (n.unitId.empty() && n.itemId.empty())) return fail("nodo senza 'id' o 'unit'/'item' nella fazione " + f.id);
                        if (!n.unitId.empty() && !n.itemId.empty()) return fail("nodo con sia 'unit' che 'item': " + n.id);
                        if (m_ById.count(n.id)) return fail("id nodo duplicato: " + n.id);
                        m_ById[n.id] = n;
                        f.nodes.push_back(n);
                    }
                    m_Factions.push_back(std::move(f));
                }

                // Validazione: padre esistente e nessun ciclo
                for (const auto& kv : m_ById) {
                    const ResearchNode* cur = &kv.second;
                    for (int depth = 0; !cur->parentId.empty(); ++depth) {
                        auto it = m_ById.find(cur->parentId);
                        if (it == m_ById.end()) return fail("padre inesistente '" + cur->parentId + "' per il nodo " + kv.first);
                        if (depth > 64) return fail("ciclo nell'albero vicino al nodo " + kv.first);
                        cur = &it->second;
                    }
                }
            }
            catch (const std::exception& e) {
                return fail(std::string("JSON non valido: ") + e.what());
            }
            return true;
        }

        const ResearchNode* FindNode(const std::string& id) const {
            auto it = m_ById.find(id);
            return it == m_ById.end() ? nullptr : &it->second;
        }
        const ResearchNode* FindParent(const ResearchNode& n) const {
            return n.parentId.empty() ? nullptr : FindNode(n.parentId);
        }
        const ResearchNode* FindNodeByUnit(const std::string& unitId) const {
            if (unitId.empty()) return nullptr;
            for (const auto& kv : m_ById) if (kv.second.unitId == unitId) return &kv.second;
            return nullptr;
        }
        const ResearchNode* FindNodeByItem(const std::string& itemId) const {
            if (itemId.empty()) return nullptr;
            for (const auto& kv : m_ById)
                for (const auto& id : kv.second.itemIds) if (id == itemId) return &kv.second;
            return nullptr;
        }

        // Nodo unita' i cui punti studio pagano la ricerca di n: il primo antenato che e' un'unita'
        const ResearchNode* FindStudySource(const ResearchNode& n) const {
            const ResearchNode* p = FindParent(n);
            while (p && p->unitId.empty()) p = FindParent(*p);
            return p;
        }

        // Oggetto da ricercare e comprare? (false = dotazione base, sempre disponibile)
        bool IsItemLocked(const std::string& itemId) const { return FindNodeByItem(itemId) != nullptr; }
        bool IsItemResearched(const PlayerProfile& p, const std::string& itemId) const {
            const ResearchNode* n = FindNodeByItem(itemId);
            return !n || IsResearched(p, *n);
        }

        static int64_t GetStudyPoints(const PlayerProfile& p, const std::string& unitId) {
            auto it = p.studyPoints.find(unitId);
            return it == p.studyPoints.end() ? 0 : it->second;
        }

        bool IsResearched(const PlayerProfile& p, const ResearchNode& n) const {
            return p.researchedNodes.count(n.id) > 0;
        }

        ResearchStatus GetStatus(const PlayerProfile& p, const ResearchNode& n) const {
            if (IsResearched(p, n)) return ResearchStatus::Researched;
            const ResearchNode* parent = FindParent(n);
            if (!parent) return ResearchStatus::Researchable;          // radice
            if (!IsResearched(p, *parent)) return ResearchStatus::LockedParent;
            const ResearchNode* source = FindStudySource(n);
            if (source && GetStudyPoints(p, source->unitId) < n.researchCost) return ResearchStatus::LockedPoints;
            return ResearchStatus::Researchable;
        }

        // Sblocca il nodo spendendo i punti studio del PADRE. false se non sbloccabile.
        bool Research(PlayerProfile& p, const ResearchNode& n) const {
            if (GetStatus(p, n) != ResearchStatus::Researchable) return false;
            if (const ResearchNode* source = FindStudySource(n)) {
                p.studyPoints[source->unitId] -= n.researchCost;
            }
            p.researchedNodes.insert(n.id);
            return true;
        }

        // Addebita l'energia per creare un'unita'. false se non sbloccato o energia insufficiente.
        // (Aggiungere l'unita' al garage spetta al chiamante, che conosce TrainedUnit.)
        bool Pay(PlayerProfile& p, const ResearchNode& n) const {
            if (!IsResearched(p, n)) return false;
            if (p.energy < n.purchaseCost) return false;
            p.energy -= n.purchaseCost;
            return true;
        }

        // I nodi radice sono sempre sbloccati.
        void EnsureRootsResearched(PlayerProfile& p) const {
            for (const auto& kv : m_ById) if (kv.second.parentId.empty()) p.researchedNodes.insert(kv.first);
        }

        // Compatibilita' con i salvataggi esistenti: le unita' gia' possedute (e i loro antenati) risultano
        // sbloccate, cosi' chi ha gia' un garage non perde nulla.
        void GrandfatherUnits(PlayerProfile& p, const std::vector<std::string>& ownedUnitIds) const {
            for (const auto& unit : ownedUnitIds) {
                const ResearchNode* n = FindNodeByUnit(unit);
                while (n) {
                    p.researchedNodes.insert(n->id);
                    n = FindParent(*n);
                }
            }
        }

    private:
        std::vector<ResearchFaction> m_Factions;
        std::unordered_map<std::string, ResearchNode> m_ById;
    };

}
