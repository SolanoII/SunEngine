#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>


namespace SunEngine {

    // Modello "a blocchi" di un oggetto (es. un'arma): piu' cubi colorati con propri materiali, nello
    // stesso stile dello scheletro a cubi. Si disegna con DrawCube, quindi non servono texture ne' UV.
    //
    // Spazio del modello: origine = centro dell'ingombro; la canna punta verso -Z (come il "davanti" del
    // personaggio nel motore); +Y e' l'alto, +X la destra di chi impugna l'arma. Unita' = metri.
    struct BlockPart {
        std::string name;
        glm::vec3 offset{ 0.0f };      // centro del blocco
        glm::vec3 size{ 0.1f };        // dimensioni (x, y, z)
        glm::vec3 rotation{ 0.0f };    // gradi, ordine Y, X, Z (come TransformComponent)
        glm::vec4 color{ 0.5f, 0.5f, 0.5f, 1.0f };
        float roughness = 0.6f;
        float metallic = 0.0f;
        float emissive = 0.0f;
    };

    struct BlockModel {
        std::string id;
        std::vector<BlockPart> parts;
        // Punti di ancoraggio (spazio del modello) usati per far combaciare l'arma con le mani
        glm::vec3 gripRight{ 0.0f };   // impugnatura (mano destra)
        glm::vec3 gripLeft{ 0.0f };    // appoggio della mano sinistra (guardamano)
        glm::vec3 muzzle{ 0.0f };      // bocca della canna
        bool hasGripRight = false;
        bool hasGripLeft = false;
        bool hasMuzzle = false;
    };

    // Collegato a un'entity con TransformComponent: al posto del cubo unico vengono disegnati i blocchi.
    // La Scale dell'entity moltiplica tutto il modello (1 = misure reali, 0 = nascosto).
    struct BlockModelComponent {
        const BlockModel* model = nullptr;
    };

    // Matrice mondo di un blocco: entityMatrix * T(offset) * R(Y,X,Z) * S(size)
    inline glm::mat4 BlockPartMatrix(const glm::mat4& entityMatrix, const BlockPart& p) {
        glm::mat4 m = glm::translate(entityMatrix, p.offset);
        m = glm::rotate(m, glm::radians(p.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::rotate(m, glm::radians(p.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, glm::radians(p.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        return glm::scale(m, p.size);
    }

    class BlockModelLibrary {
    public:
        // Carica tutti i *.json della cartella (non ricorsivo). Ritorna quanti ne ha caricati;
        // i file non validi vengono saltati e descritti in `errors` (se fornito).
        int LoadDirectory(const std::filesystem::path& dir, std::vector<std::string>* errors = nullptr) {
            int loaded = 0;
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) return 0;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                std::string err;
                BlockModel model;
                if (ParseFile(entry.path(), model, err)) {
                    m_Models[model.id] = std::move(model);
                    ++loaded;
                }
                else if (errors) {
                    errors->push_back(entry.path().filename().string() + ": " + err);
                }
            }
            return loaded;
        }

        const BlockModel* Find(const std::string& id) const {
            auto it = m_Models.find(id);
            return it == m_Models.end() ? nullptr : &it->second;   // i puntatori restano validi (mappa a nodi)
        }

        static bool ParseFile(const std::filesystem::path& file, BlockModel& out, std::string& error) {
            try {
                std::ifstream in(file, std::ios::binary);
                if (!in) { error = "file non apribile"; return false; }
                nlohmann::json j = nlohmann::json::parse(in);
                out = BlockModel{};
                out.id = j.value("id", std::string());
                if (out.id.empty()) { error = "manca 'id'"; return false; }

                auto vec3 = [](const nlohmann::json& a, glm::vec3 def) {
                    if (a.is_array() && a.size() >= 3) return glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
                    return def;
                };
                if (!j.contains("parts") || !j["parts"].is_array() || j["parts"].empty()) { error = "manca 'parts'"; return false; }
                for (const auto& jp : j["parts"]) {
                    BlockPart p;
                    p.name = jp.value("name", std::string());
                    p.offset = vec3(jp.value("offset", nlohmann::json()), glm::vec3(0.0f));
                    p.size = vec3(jp.value("size", nlohmann::json()), glm::vec3(0.1f));
                    p.rotation = vec3(jp.value("rotation", nlohmann::json()), glm::vec3(0.0f));
                    if (jp.contains("color") && jp["color"].is_array() && jp["color"].size() >= 3) {
                        p.color = glm::vec4(jp["color"][0].get<float>(), jp["color"][1].get<float>(), jp["color"][2].get<float>(),
                            jp["color"].size() >= 4 ? jp["color"][3].get<float>() : 1.0f);
                    }
                    p.roughness = jp.value("roughness", 0.6f);
                    p.metallic = jp.value("metallic", 0.0f);
                    p.emissive = jp.value("emissive", 0.0f);
                    out.parts.push_back(p);
                }
                if (j.contains("anchors") && j["anchors"].is_object()) {
                    const auto& a = j["anchors"];
                    if (a.contains("grip_right")) { out.gripRight = vec3(a["grip_right"], glm::vec3(0.0f)); out.hasGripRight = true; }
                    if (a.contains("grip_left")) { out.gripLeft = vec3(a["grip_left"], glm::vec3(0.0f)); out.hasGripLeft = true; }
                    if (a.contains("muzzle")) { out.muzzle = vec3(a["muzzle"], glm::vec3(0.0f)); out.hasMuzzle = true; }
                }
                return true;
            }
            catch (const std::exception& e) {
                error = e.what();
                return false;
            }
        }

    private:
        std::unordered_map<std::string, BlockModel> m_Models;
    };

}
