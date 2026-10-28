#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace SunEngine {

    // Profilo locale del giocatore: impostazioni, valute, esperienza e roster.
    // Salvato come JSON in %APPDATA%\ProjectTrazyn\profile.json (Linux/macOS: ~/.local/share/...),
    // cosi' sopravvive a una ricompilazione/pulizia della cartella di build.
    //
    // ATTENZIONE: e' un file locale, quindi modificabile dal giocatore. Va bene per energia, exp e
    // impostazioni; la "Pietra Nera" (valuta a pagamento) e' qui solo un SALDO segnaposto. Quando ci
    // sara' un acquisto reale il saldo dovra' vivere lato server.
    struct PlayerProfile {
        static constexpr int kVersion = 1;

        // --- Impostazioni ---
        int   resolutionIdx = 0;
        bool  fullscreen = false;
        float volumeSFX = 0.8f;
        float volumeVoice = 0.8f;
        float volumeMusic = 0.8f;

        // --- Valute ---
        int64_t energy = 0;       // valuta principale (1 per ogni orco ucciso)
        int64_t blackStone = 0;   // "Pietra Nera": valuta premium (a pagamento)

        // --- Progressione ---
        std::unordered_map<std::string, int64_t> weaponExp;   // id blueprint arma -> exp

        // --- Selezioni del garage ---
        std::vector<int> squadIndices;
        std::string selectedMapId;
        int selectedEventIndex = 0;

        // Roster serializzato da GameLayer (array di oggetti): il profilo non conosce TrainedUnit.
        nlohmann::json roster = nlohmann::json::array();

        // ---------------------------------------------------------------
        static std::filesystem::path GetProfilePath() {
            std::string base;
#ifdef _WIN32
            char* buf = nullptr; size_t len = 0;
            if (_dupenv_s(&buf, &len, "APPDATA") == 0 && buf) { base = buf; free(buf); }
#else
            if (const char* home = std::getenv("HOME")) base = std::string(home) + "/.local/share";
#endif
            std::filesystem::path dir = base.empty() ? std::filesystem::path("SaveData")
                                                      : std::filesystem::path(base) / "ProjectTrazyn";
            return dir / "profile.json";
        }

        nlohmann::json ToJson() const {
            nlohmann::json j;
            j["version"] = kVersion;
            j["settings"] = { {"resolutionIdx", resolutionIdx}, {"fullscreen", fullscreen},
                              {"volumeSFX", volumeSFX}, {"volumeVoice", volumeVoice}, {"volumeMusic", volumeMusic} };
            j["currency"] = { {"energy", energy}, {"blackStone", blackStone} };
            // std::map per ottenere un ordine stabile (serve al confronto "e' cambiato qualcosa?")
            nlohmann::json w = nlohmann::json::object();
            std::vector<std::string> keys;
            for (const auto& kv : weaponExp) keys.push_back(kv.first);
            std::sort(keys.begin(), keys.end());
            for (const auto& k : keys) w[k] = weaponExp.at(k);
            j["weaponExp"] = w;
            j["garage"] = { {"squadIndices", squadIndices}, {"selectedMapId", selectedMapId},
                            {"selectedEventIndex", selectedEventIndex} };
            j["roster"] = roster;
            return j;
        }

        // Tollerante: campi mancanti = valori di default; valori fuori range = clamp.
        static PlayerProfile FromJson(const nlohmann::json& j) {
            PlayerProfile p;
            if (!j.is_object()) return p;
            auto clamp01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
            if (j.contains("settings") && j["settings"].is_object()) {
                const auto& s = j["settings"];
                p.resolutionIdx = std::max(0, s.value("resolutionIdx", 0));
                p.fullscreen = s.value("fullscreen", false);
                p.volumeSFX = clamp01(s.value("volumeSFX", 0.8f));
                p.volumeVoice = clamp01(s.value("volumeVoice", 0.8f));
                p.volumeMusic = clamp01(s.value("volumeMusic", 0.8f));
            }
            if (j.contains("currency") && j["currency"].is_object()) {
                p.energy = std::max<int64_t>(0, j["currency"].value("energy", 0));
                p.blackStone = std::max<int64_t>(0, j["currency"].value("blackStone", 0));
            }
            if (j.contains("weaponExp") && j["weaponExp"].is_object()) {
                for (auto it = j["weaponExp"].begin(); it != j["weaponExp"].end(); ++it) {
                    if (it.value().is_number_integer()) p.weaponExp[it.key()] = std::max<int64_t>(0, it.value().get<int64_t>());
                }
            }
            if (j.contains("garage") && j["garage"].is_object()) {
                const auto& g = j["garage"];
                if (g.contains("squadIndices") && g["squadIndices"].is_array()) {
                    for (const auto& v : g["squadIndices"]) if (v.is_number_integer()) p.squadIndices.push_back(v.get<int>());
                }
                p.selectedMapId = g.value("selectedMapId", std::string());
                p.selectedEventIndex = std::max(0, g.value("selectedEventIndex", 0));
            }
            if (j.contains("roster") && j["roster"].is_array()) p.roster = j["roster"];
            return p;
        }

        std::string Serialize() const { return ToJson().dump(2); }

        // false = nessun file (primo avvio) oppure file illeggibile. Un file corrotto viene messo da
        // parte come profile.corrupt.json (cosi' non va perso e non viene sovrascritto subito).
        static bool Load(PlayerProfile& out, const std::filesystem::path& path = GetProfilePath()) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) return false;
            try {
                std::ifstream in(path, std::ios::binary);
                nlohmann::json j = nlohmann::json::parse(in);
                out = FromJson(j);
                return true;
            }
            catch (...) {
                std::filesystem::path bad = path; bad.replace_filename("profile.corrupt.json");
                std::filesystem::rename(path, bad, ec);
                return false;
            }
        }

        // Scrittura atomica: file temporaneo + rename, cosi' un crash a meta' non lascia un JSON troncato.
        static bool WriteText(const std::string& text, const std::filesystem::path& path = GetProfilePath()) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::filesystem::path tmp = path; tmp += ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out) return false;
                out << text;
                if (!out.good()) return false;
            }
            std::filesystem::rename(tmp, path, ec);
            if (ec) {   // alcuni filesystem non sovrascrivono con rename
                std::filesystem::copy_file(tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove(tmp);
                return !ec;
            }
            return true;
        }
    };

}
