#define NOMINMAX
#include <glad/glad.h>

#include "GameLayer.h"
#include "Engine/Core/Application/Application.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/MathUtils.h"
#include "Engine/Core/Input.h"
#include "Engine/Core/Logger/Log.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Tags.h"
#include "Engine/ECS/DamageResolver.h"
#include "Engine/ECS/RaycastUtils.h"
#include "Engine/AssetManager/BlueprintManager.h"
#include "Engine/Physics/SkeletonLoader.h"
#include "Engine/Scene/MapLoader.h"
#include "Engine/Navigation/NavMeshQuery.h"

#include <cstdlib>
#include <cfloat>
#include <cstring>
#include <cctype>
#include <cstdarg>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#include <memory>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <mutex>
#include <nlohmann/json.hpp>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

extern std::string FindValidDir(const std::string& targetDirName);

namespace SunEngine {

    // Cartella del modello d'autore per ogni corpo (GameData/Models/Characters/<cartella>/)
    static std::string AuthoredFolderOf(const std::string& label) {
        if (label == "neofita") return "neophyte";
        return label;   // guardsman, ork, ogryn, gretchin, astartes
    }
    // Altezza indicativa di ogni corpo (cm): serve solo a riconoscere le unita' del file dell'artista (m, cm, mm)
    static float DefaultBodyHeightCm(const std::string& label) {
        if (label == "ork") return 195.0f;
        if (label == "ogryn") return 275.0f;
        if (label == "gretchin") return 90.0f;
        if (label == "neofita" || label == "neophyte") return 195.0f;
        if (label == "astartes") return 220.0f;
        return 180.0f;
    }


    // Versione delle dotazioni base salvata nel profilo: le unita' salvate con una versione piu' vecchia
    // ricevono la dotazione base del loro blueprint (se ne ha una) al caricamento. Alzala quando cambi
    // le dotazioni base e vuoi che arrivino anche alle unita' gia' nel garage.
    static constexpr int kLoadoutVersion = 2;   // 1 = armi e dotazione base, 2 = armatura/vesti base

    // FIX #3: observer on_destroy di HordeAgentComponent. Quando un'unita' nemica viene distrutta
    // (timeout cadavere, reload mappa, ritorno al garage...) distrugge anche l'arma "figlia"
    // (orkWeaponEntity, entity separata Mesh+Color) che altrimenti restava a fluttuare per sempre.
    // Distrugge un'entity DIVERSA da quella in corso di rimozione, quindi e' sicuro dentro il segnale.
    static void OnHordeAgentDestroyed(entt::registry& reg, entt::entity enemy) {
        if (!reg.all_of<HordeAgentComponent>(enemy)) return;
        entt::entity weapon = reg.get<HordeAgentComponent>(enemy).weaponEntity;
        if (weapon != entt::null && weapon != enemy && reg.valid(weapon)) {
            reg.destroy(weapon);
        }
    }

    // Bot della modalita' Vs Bot: anche la loro arma visibile sparisce con loro
    static void OnBotDestroyed(entt::registry& reg, entt::entity bot) {
        if (!reg.all_of<BotBrainComponent>(bot)) return;
        entt::entity weapon = reg.get<BotBrainComponent>(bot).weaponEntity;
        if (weapon != entt::null && weapon != bot && reg.valid(weapon)) reg.destroy(weapon);
    }

    static glm::vec2 WorldToScreen(const glm::vec3& worldPos, const glm::mat4& viewProj, float screenWidth, float screenHeight) {
        glm::vec4 clipPos = viewProj * glm::vec4(worldPos, 1.0f);
        if (clipPos.w <= 0.001f) return glm::vec2(-10000.0f);
        glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
        float x = (ndc.x + 1.0f) * 0.5f * screenWidth;
        float y = (1.0f - ndc.y) * 0.5f * screenHeight;
        return glm::vec2(x, y);
    }

    struct ResOption {
        int width;
        int height;
        const char* name;
    };

    static const ResOption g_Resolutions[] = {
        { 1280, 720,  "1280 x 720 (HD)" },
        { 1600, 900,  "1600 x 900 (HD+)" },
        { 1920, 1080, "1920 x 1080 (Full HD)" },
        { 2560, 1440, "2560 x 1440 (2K QHD)" }
    };

    static bool g_WaaaghActive = false;
    static float g_WaaaghTimer = 0.0f;
    static int g_WaaaghSpawnCount = 1;

    struct OrkVoicePoolItem {
        std::string filename{};
        ma_sound sound{};
    };

    struct PendingShout {
        entt::entity orkEntity = entt::null;
        float delayTimer = 0.0f;
    };

    struct OrkVoiceManager {
        ma_sound startWaaaghSound;
        bool startSoundLoaded = false;

        std::vector<std::unique_ptr<OrkVoicePoolItem>> voicePool;
        std::vector<std::string> variationFiles;

        float nextShoutTimer = 3.0f;
        std::vector<PendingShout> pendingShouts;
    } g_OrkVoices;

    static std::mutex g_OrkVoiceMutex;
    static float CalculateSpatialVolume(float distance) {
        if (distance <= 1.0f) return 1.0f;
        if (distance >= 101.0f) return 0.0f;
        return 1.0f - ((distance - 1.0f) * 0.01f);
    }

    static void SpawnSubBlock(Scene& scene, PhysicsSystem& physicsSystem,
        const glm::vec3& parentPos, const glm::vec3& parentRotDeg,
        const glm::vec3& localPos, const glm::vec3& localScale, const glm::vec3& localRotDeg,
        const glm::vec4& color, bool isSelected) {

        auto entity = scene.CreateEntity();

        glm::mat4 parentMat = glm::translate(glm::mat4(1.0f), parentPos);
        parentMat = glm::rotate(parentMat, glm::radians(parentRotDeg.y), glm::vec3(0.0f, 1.0f, 0.0f));
        parentMat = glm::rotate(parentMat, glm::radians(parentRotDeg.x), glm::vec3(1.0f, 0.0f, 0.0f));
        parentMat = glm::rotate(parentMat, glm::radians(parentRotDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));

        glm::mat4 localMat = glm::translate(glm::mat4(1.0f), localPos);
        localMat = glm::rotate(localMat, glm::radians(localRotDeg.y), glm::vec3(0.0f, 1.0f, 0.0f));
        localMat = glm::rotate(localMat, glm::radians(localRotDeg.x), glm::vec3(1.0f, 0.0f, 0.0f));
        localMat = glm::rotate(localMat, glm::radians(localRotDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));

        glm::mat4 worldMat = parentMat * localMat;

        auto& trans = scene.GetRegistry().emplace<TransformComponent>(entity);
        trans.Position = glm::vec3(worldMat[3]);
        trans.Rotation = parentRotDeg + localRotDeg;
        trans.Scale = localScale;

        scene.GetRegistry().emplace<MeshComponent>(entity, 0u);
        auto& col = scene.GetRegistry().emplace<ColliderComponent>(entity, glm::vec3(0.5f));
        // FIX #1: localPos dei sub-blocchi e' gia' il centro (es. scale.y*0.5 sopra la base),
        // quindi nessun offset: altrimenti Jolt si sposta ancora di 0.5*Scale.y rispetto al visibile.
        col.localCenterOffset = glm::vec3(0.0f);
        scene.GetRegistry().emplace<RigidBodyComponent>(entity, glm::vec3(0.0f), 0.0f, true);

        auto& c = scene.GetRegistry().emplace<ColorComponent>(entity);
        c.Color = isSelected ? glm::vec4(1.0f, 0.85f, 0.1f, 1.0f) : color;
        c.BaseColor = c.Color;
        if (!isSelected) scene.GetRegistry().emplace<SurfaceMaterialComponent>(entity, SurfaceMaterialComponent{ GuessSurfaceMaterial(color, localScale, false) });
    }

    void GameLayer::OnAttach() {
        SUN_CORE_INFO("GameLayer agganciato con successo!");
    }

    void GameLayer::OnDetach() {
        SUN_CORE_INFO("GameLayer sganciato!");
    }

    void GameLayer::SyncEditorEntitiesWithScene() {
        auto view = m_Scene.GetRegistry().view<ColliderComponent>(entt::exclude<PlayerTag>);
        for (auto e : view) {
            m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
            m_Scene.DestroyEntity(e);
        }
        {   // detriti della mappa caricata in partita (nell'editor non ci sono)
            std::vector<entt::entity> decor;
            for (auto e : m_Scene.GetRegistry().view<MapDecorTag>()) decor.push_back(e);
            for (auto e : decor) m_Scene.DestroyEntity(e);
        }

        auto terrainEntity = m_Scene.CreateEntity();
        auto& tTrans = m_Scene.GetRegistry().emplace<TransformComponent>(terrainEntity);
        tTrans.Position = glm::vec3(0.0f, -0.5f, 0.0f);
        tTrans.Scale = m_EditorMap.terrainSize;

        m_Scene.GetRegistry().emplace<MeshComponent>(terrainEntity, 0u);
        m_Scene.GetRegistry().emplace<ColliderComponent>(terrainEntity, glm::vec3(0.5f));
        m_Scene.GetRegistry().emplace<RigidBodyComponent>(terrainEntity, glm::vec3(0.0f), 0.0f, true);

        auto& tColor = m_Scene.GetRegistry().emplace<ColorComponent>(terrainEntity);
        tColor.Color = m_EditorMap.terrainColor;
        tColor.BaseColor = m_EditorMap.terrainColor;
        m_Scene.GetRegistry().emplace<SurfaceMaterialComponent>(terrainEntity, SurfaceMaterialComponent{ Surface_Mud });

        for (size_t i = 0; i < m_EditorMap.structures.size(); ++i) {
            const auto& st = m_EditorMap.structures[i];
            bool isSel = (static_cast<int>(i) == m_SelectedStructureIdx);

            if (st.type == "block") {
                if (st.shape == "doorway") {
                    float pW = 0.6f;
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(-st.scale.x * 0.5f + pW * 0.5f, st.scale.y * 0.5f, 0.0f), glm::vec3(pW, st.scale.y, st.scale.z), glm::vec3(0.0f), st.color, isSel);
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(st.scale.x * 0.5f - pW * 0.5f, st.scale.y * 0.5f, 0.0f), glm::vec3(pW, st.scale.y, st.scale.z), glm::vec3(0.0f), st.color, isSel);
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(0.0f, st.scale.y - 0.3f, 0.0f), glm::vec3(st.scale.x, 0.6f, st.scale.z), glm::vec3(0.0f), st.color, isSel);
                }
                else if (st.shape == "arch") {
                    float pW = 0.6f;
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(-st.scale.x * 0.5f + pW * 0.5f, st.scale.y * 0.4f, 0.0f), glm::vec3(pW, st.scale.y * 0.8f, st.scale.z), glm::vec3(0.0f), st.color, isSel);
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(st.scale.x * 0.5f - pW * 0.5f, st.scale.y * 0.4f, 0.0f), glm::vec3(pW, st.scale.y * 0.8f, st.scale.z), glm::vec3(0.0f), st.color, isSel);
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(-st.scale.x * 0.25f, st.scale.y * 0.85f, 0.0f), glm::vec3(st.scale.x * 0.4f, 0.5f, st.scale.z), glm::vec3(0.0f, 0.0f, 20.0f), st.color, isSel);
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(st.scale.x * 0.25f, st.scale.y * 0.85f, 0.0f), glm::vec3(st.scale.x * 0.4f, 0.5f, st.scale.z), glm::vec3(0.0f, 0.0f, -20.0f), st.color, isSel);
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(0.0f, st.scale.y, 0.0f), glm::vec3(st.scale.x * 0.4f, 0.5f, st.scale.z), glm::vec3(0.0f), st.color, isSel);
                }
                else if (st.shape == "pyramid") {
                    for (int layer = 0; layer < 4; ++layer) {
                        float factor = 1.0f - (layer * 0.22f);
                        float layerH = st.scale.y * 0.25f;
                        glm::vec3 lScale(st.scale.x * factor, layerH, st.scale.z * factor);
                        glm::vec3 lPos(0.0f, layerH * layer + layerH * 0.5f, 0.0f);
                        SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, lPos, lScale, glm::vec3(0.0f), st.color, isSel);
                    }
                }
                else if (st.shape == "ramp") {
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(0.0f, st.scale.y * 0.5f, 0.0f), glm::vec3(st.scale.x, st.scale.y * 0.3f, st.scale.z * 1.1f), glm::vec3(22.0f, 0.0f, 0.0f), st.color, isSel);
                }
                else if (st.shape == "stairs") {
                    int steps = 5;
                    for (int s = 0; s < steps; ++s) {
                        float stepH = st.scale.y / steps;
                        float stepD = st.scale.z / steps;
                        glm::vec3 lScale(st.scale.x, stepH * (s + 1), stepD);
                        glm::vec3 lPos(0.0f, (stepH * (s + 1)) * 0.5f, -st.scale.z * 0.5f + stepD * s + stepD * 0.5f);
                        SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, lPos, lScale, glm::vec3(0.0f), st.color, isSel);
                    }
                }
                else {
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(0.0f, st.scale.y * 0.5f, 0.0f), st.scale, glm::vec3(0.0f), st.color, isSel);
                }
            }
            else if (st.type == "building") {
                float W = st.width, D = st.depth, H = st.floorHeight, T = st.wallThickness;
                float DW = st.doorWidth, DH = st.doorHeight;

                SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(-W * 0.5f + T * 0.5f, H * 0.5f, 0.0f), glm::vec3(T, H, D), glm::vec3(0.0f), st.color, isSel);
                SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(W * 0.5f - T * 0.5f, H * 0.5f, 0.0f), glm::vec3(T, H, D), glm::vec3(0.0f), st.color, isSel);

                SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(0.0f, H * 0.5f, -D * 0.5f + T * 0.5f), glm::vec3(W - 2 * T, H, T), glm::vec3(0.0f), st.color, isSel);

                float sideWallW = (W - DW) * 0.5f;
                SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(-W * 0.5f + sideWallW * 0.5f, H * 0.5f, D * 0.5f - T * 0.5f), glm::vec3(sideWallW, H, T), glm::vec3(0.0f), st.color, isSel);
                SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(W * 0.5f - sideWallW * 0.5f, H * 0.5f, D * 0.5f - T * 0.5f), glm::vec3(sideWallW, H, T), glm::vec3(0.0f), st.color, isSel);

                float lintelH = H - DH;
                if (lintelH > 0.1f) {
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(0.0f, DH + lintelH * 0.5f, D * 0.5f - T * 0.5f), glm::vec3(DW, lintelH, T), glm::vec3(0.0f), st.color, isSel);
                }

                if (st.shape == "flat_roof") {
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(0.0f, H + T * 0.5f, 0.0f), glm::vec3(W + 0.4f, T, D + 0.4f), glm::vec3(0.0f), st.color, isSel);
                }
                else if (st.shape == "gabled_roof") {
                    float roofSlopeH = 2.0f;
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(-W * 0.25f, H + roofSlopeH * 0.5f, 0.0f), glm::vec3(W * 0.55f, T, D + 0.4f), glm::vec3(0.0f, 0.0f, 22.0f), st.color, isSel);
                    SpawnSubBlock(m_Scene, m_PhysicsSystem, st.position, st.rotation, glm::vec3(W * 0.25f, H + roofSlopeH * 0.5f, 0.0f), glm::vec3(W * 0.55f, T, D + 0.4f), glm::vec3(0.0f, 0.0f, -22.0f), st.color, isSel);
                }
            }
        }

        m_PhysicsSystem.RefreshBodies(m_Scene);
    }

    void GameLayer::CreateNewMap() {
        m_EditorMap = EditorMapData();

        int randomId = Random::RangeInt(1000, 9999);
        m_EditorMap.id = "custom_map_" + std::to_string(randomId);
        m_EditorMap.name = "Mappa Personalizzata " + std::to_string(randomId);
        m_EditorMap.terrainSize = glm::vec3(150.0f, 1.0f, 150.0f);
        m_EditorMap.terrainColor = glm::vec4(0.22f, 0.22f, 0.25f, 1.0f);

        m_EditorMap.playerSpawns.clear();
        m_EditorMap.enemySpawns.clear();
        m_EditorMap.structures.clear();

        m_EditorMap.playerSpawns.push_back(glm::vec3(0.0f, 0.5f, 30.0f));
        m_EditorMap.enemySpawns.push_back(glm::vec3(0.0f, 0.5f, -30.0f));

        m_SelectedStructureIdx = -1;
        m_EditorCameraTarget = glm::vec3(0.0f, 2.0f, 0.0f);
        m_LastExportPathStatus = "";

        SyncEditorEntitiesWithScene();
        SUN_CORE_INFO("Nuova mappa creata da zero con ID: {0}", m_EditorMap.id);
    }

    void GameLayer::SaveMapToJSON() {
        nlohmann::json j;
        j["id"] = m_EditorMap.id;
        j["name"] = m_EditorMap.name;

        j["terrain"]["size"] = { m_EditorMap.terrainSize.x, m_EditorMap.terrainSize.y, m_EditorMap.terrainSize.z };
        j["terrain"]["color"] = { m_EditorMap.terrainColor.r, m_EditorMap.terrainColor.g, m_EditorMap.terrainColor.b, m_EditorMap.terrainColor.a };

        j["player_spawn_points"] = nlohmann::json::array();
        for (const auto& sp : m_EditorMap.playerSpawns) j["player_spawn_points"].push_back({ sp.x, sp.y, sp.z });

        j["enemy_spawn_points"] = nlohmann::json::array();
        for (const auto& sp : m_EditorMap.enemySpawns) j["enemy_spawn_points"].push_back({ sp.x, sp.y, sp.z });

        j["structures"] = nlohmann::json::array();
        for (const auto& st : m_EditorMap.structures) {
            nlohmann::json sj;
            sj["type"] = st.type;
            sj["shape"] = st.shape;
            sj["position"] = { st.position.x, st.position.y, st.position.z };
            sj["rotation"] = { st.rotation.x, st.rotation.y, st.rotation.z };
            sj["color"] = { st.color.r, st.color.g, st.color.b, st.color.a };

            if (st.type == "block") {
                sj["scale"] = { st.scale.x, st.scale.y, st.scale.z };
            }
            else if (st.type == "building") {
                sj["width"] = st.width;
                sj["depth"] = st.depth;
                sj["floor_height"] = st.floorHeight;
                sj["wall_thickness"] = st.wallThickness;
                sj["door_width"] = st.doorWidth;
                sj["door_height"] = st.doorHeight;
            }
            j["structures"].push_back(sj);
        }

        std::string mapsDir = FindValidDir("Maps");
        if (mapsDir.empty()) mapsDir = "GameData/Maps/";

        std::string filepath = mapsDir + m_EditorMap.id + ".json";
        std::ofstream file(filepath);
        if (file.is_open()) {
            file << j.dump(2);
            file.close();
            SUN_CORE_INFO("Mappa salvata con successo in: {0}", filepath);
        }
        else {
            SUN_CORE_ERROR("Impossibile salvare la mappa in: {0}", filepath);
        }
    }

    void GameLayer::ExportMapToExternal() {
        SaveMapToJSON();

        namespace fs = std::filesystem;
        std::string exportDir = "ExportedMaps/";
        if (!fs::exists(exportDir)) {
            fs::create_directories(exportDir);
        }

        std::string exportFilePath = exportDir + m_EditorMap.id + ".json";

        nlohmann::json j;
        j["id"] = m_EditorMap.id;
        j["name"] = m_EditorMap.name;
        j["terrain"]["size"] = { m_EditorMap.terrainSize.x, m_EditorMap.terrainSize.y, m_EditorMap.terrainSize.z };
        j["terrain"]["color"] = { m_EditorMap.terrainColor.r, m_EditorMap.terrainColor.g, m_EditorMap.terrainColor.b, m_EditorMap.terrainColor.a };

        j["player_spawn_points"] = nlohmann::json::array();
        for (const auto& sp : m_EditorMap.playerSpawns) j["player_spawn_points"].push_back({ sp.x, sp.y, sp.z });

        j["enemy_spawn_points"] = nlohmann::json::array();
        for (const auto& sp : m_EditorMap.enemySpawns) j["enemy_spawn_points"].push_back({ sp.x, sp.y, sp.z });

        j["structures"] = nlohmann::json::array();
        for (const auto& st : m_EditorMap.structures) {
            nlohmann::json sj;
            sj["type"] = st.type;
            sj["shape"] = st.shape;
            sj["position"] = { st.position.x, st.position.y, st.position.z };
            sj["rotation"] = { st.rotation.x, st.rotation.y, st.rotation.z };
            sj["color"] = { st.color.r, st.color.g, st.color.b, st.color.a };

            if (st.type == "block") {
                sj["scale"] = { st.scale.x, st.scale.y, st.scale.z };
            }
            else if (st.type == "building") {
                sj["width"] = st.width;
                sj["depth"] = st.depth;
                sj["floor_height"] = st.floorHeight;
                sj["wall_thickness"] = st.wallThickness;
                sj["door_width"] = st.doorWidth;
                sj["door_height"] = st.doorHeight;
            }
            j["structures"].push_back(sj);
        }

        std::ofstream file(exportFilePath);
        if (file.is_open()) {
            file << j.dump(2);
            file.close();

            m_LastExportPathStatus = fs::absolute(exportFilePath).string();
            SUN_CORE_INFO("Mappa esportata con successo in: {0}", m_LastExportPathStatus);
        }
        else {
            m_LastExportPathStatus = "ERRORE durante l'esportazione!";
        }
    }

    void GameLayer::LoadMapIntoPreview() {
        SaveMapToJSON();
        SyncEditorEntitiesWithScene();
    }

    void GameLayer::HandleEditor3DInteraction(float deltaTime) {
        if (m_GameState != GameState::MapEditor) return;

        if (Input::IsKeyPressed(GLFW_KEY_W) && !Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_GizmoMode = EditorGizmoMode::Translate;
        if (Input::IsKeyPressed(GLFW_KEY_E) && !Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_GizmoMode = EditorGizmoMode::Rotate;
        if (Input::IsKeyPressed(GLFW_KEY_R) && !Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_GizmoMode = EditorGizmoMode::Scale;

        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse) return;

        bool leftClicked = Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);

        if (leftClicked && !m_IsDraggingGizmo) {
            double mouseXDouble = 0.0, mouseYDouble = 0.0;
            GLFWwindow* window = glfwGetCurrentContext();
            if (window) glfwGetCursorPos(window, &mouseXDouble, &mouseYDouble);
            glm::vec2 mousePos(static_cast<float>(mouseXDouble), static_cast<float>(mouseYDouble));

            float screenW = m_WindowWidth, screenH = m_WindowHeight;
            float ndcX = (2.0f * mousePos.x) / screenW - 1.0f;
            float ndcY = 1.0f - (2.0f * mousePos.y) / screenH;

            glm::mat4 invViewProj = glm::inverse(m_Camera.GetViewProjection());
            glm::vec4 rayStartWorld = invViewProj * glm::vec4(ndcX, ndcY, -1.0f, 1.0f); rayStartWorld /= rayStartWorld.w;
            glm::vec4 rayEndWorld = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);     rayEndWorld /= rayEndWorld.w;

            glm::vec3 rayOrigin = glm::vec3(rayStartWorld);
            glm::vec3 rayDir = glm::normalize(glm::vec3(rayEndWorld - rayStartWorld));

            float closestT = 10000.0f;
            int hitIdx = -1;

            for (size_t i = 0; i < m_EditorMap.structures.size(); ++i) {
                const auto& st = m_EditorMap.structures[i];
                glm::vec3 size = (st.type == "block") ? st.scale : glm::vec3(st.width, st.floorHeight, st.depth);
                glm::vec3 minBox = st.position - (size * 0.5f);
                glm::vec3 maxBox = st.position + (size * 0.5f);

                float tMin = 0.0f, tMax = 10000.0f;
                bool hit = true;
                for (int a = 0; a < 3; ++a) {
                    float invD = 1.0f / rayDir[a];
                    float t0 = (minBox[a] - rayOrigin[a]) * invD;
                    float t1 = (maxBox[a] - rayOrigin[a]) * invD;
                    if (invD < 0.0f) std::swap(t0, t1);
                    tMin = std::max(tMin, t0);
                    tMax = std::min(tMax, t1);
                    if (tMax < tMin) { hit = false; break; }
                }

                if (hit && tMin < closestT) {
                    closestT = tMin;
                    hitIdx = static_cast<int>(i);
                }
            }

            if (hitIdx != -1) {
                m_SelectedStructureIdx = hitIdx;
                SyncEditorEntitiesWithScene();
            }
        }

        if (m_SelectedStructureIdx >= 0 && m_SelectedStructureIdx < (int)m_EditorMap.structures.size()) {
            auto& st = m_EditorMap.structures[m_SelectedStructureIdx];
            glm::vec2 mouseDelta = Input::GetMouseDelta();

            if (leftClicked && (std::abs(mouseDelta.x) > 0.01f || std::abs(mouseDelta.y) > 0.01f)) {
                float speed = 0.15f;

                if (m_GizmoMode == EditorGizmoMode::Translate) {
                    if (Input::IsKeyPressed(GLFW_KEY_LEFT_SHIFT)) {
                        st.position.y -= mouseDelta.y * speed;
                    }
                    else {
                        glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(m_EditorCameraYaw), glm::vec3(0.0f, 1.0f, 0.0f));
                        glm::vec3 fwd = glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
                        glm::vec3 rgt = glm::vec3(rot * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
                        st.position += (rgt * mouseDelta.x - fwd * mouseDelta.y) * speed;
                    }
                }
                else if (m_GizmoMode == EditorGizmoMode::Rotate) {
                    st.rotation.y += mouseDelta.x * 1.5f;
                }
                else if (m_GizmoMode == EditorGizmoMode::Scale) {
                    if (st.type == "block") {
                        st.scale.x = std::max(0.5f, st.scale.x + mouseDelta.x * speed);
                        st.scale.z = std::max(0.5f, st.scale.z - mouseDelta.y * speed);
                    }
                    else {
                        st.width = std::max(1.0f, st.width + mouseDelta.x * speed);
                        st.depth = std::max(1.0f, st.depth - mouseDelta.y * speed);
                    }
                }

                SyncEditorEntitiesWithScene();
            }
        }
    }

    void GameLayer::RenderEditorGizmos3D() {
        if (m_GameState != GameState::MapEditor || m_SelectedStructureIdx < 0 || m_SelectedStructureIdx >= (int)m_EditorMap.structures.size()) return;

        const auto& st = m_EditorMap.structures[m_SelectedStructureIdx];
        glm::vec3 origin = st.position;

        glm::mat4 vp = m_Camera.GetViewProjection();
        float screenW = m_WindowWidth, screenH = m_WindowHeight;

        glm::vec2 center2D = WorldToScreen(origin, vp, screenW, screenH);
        glm::vec2 posX2D = WorldToScreen(origin + glm::vec3(3.0f, 0.0f, 0.0f), vp, screenW, screenH);
        glm::vec2 posY2D = WorldToScreen(origin + glm::vec3(0.0f, 3.0f, 0.0f), vp, screenW, screenH);
        glm::vec2 posZ2D = WorldToScreen(origin + glm::vec3(0.0f, 0.0f, 3.0f), vp, screenW, screenH);

        // FIX #14: foreground -> i gizmo stanno SOPRA i pannelli ImGui (prima: dietro, coperti dalle finestre)
        ImDrawList* drawList = ImGui::GetForegroundDrawList();

        if (center2D.x > 0.0f && posX2D.x > 0.0f) {
            drawList->AddLine(ImVec2(center2D.x, center2D.y), ImVec2(posX2D.x, posX2D.y), IM_COL32(255, 50, 50, 255), 3.0f);
            drawList->AddCircleFilled(ImVec2(posX2D.x, posX2D.y), 6.0f, IM_COL32(255, 50, 50, 255));

            drawList->AddLine(ImVec2(center2D.x, center2D.y), ImVec2(posY2D.x, posY2D.y), IM_COL32(50, 255, 50, 255), 3.0f);
            drawList->AddCircleFilled(ImVec2(posY2D.x, posY2D.y), 6.0f, IM_COL32(50, 255, 50, 255));

            drawList->AddLine(ImVec2(center2D.x, center2D.y), ImVec2(posZ2D.x, posZ2D.y), IM_COL32(50, 150, 255, 255), 3.0f);
            drawList->AddCircleFilled(ImVec2(posZ2D.x, posZ2D.y), 6.0f, IM_COL32(50, 150, 255, 255));

            drawList->AddCircleFilled(ImVec2(center2D.x, center2D.y), 5.0f, IM_COL32(255, 255, 0, 255));
        }
    }

    static void InitOrkAudio(ma_engine* engine, ma_sound_group* voiceGroup) {   // voiceGroup: volume "Voci" delle opzioni
        std::string audioDir = FindValidDir("Audio");

        if (audioDir.empty()) {
            audioDir = "GameData/Audio/";
        }

        std::string startPath = audioDir + "i-say-waaagh.wav";
        if (ma_sound_init_from_file(engine, startPath.c_str(), MA_SOUND_FLAG_DECODE, voiceGroup, NULL, &g_OrkVoices.startWaaaghSound) == MA_SUCCESS) {
            g_OrkVoices.startSoundLoaded = true;
            // FIX #7: "I say waaagh" e' un annuncio globale senza posizione. Ora che il listener segue il
            // player, se restasse spazializzato (posizione di default = origine) verrebbe attenuato e
            // panneggiato in base alla distanza dall'origine della mappa.
            ma_sound_set_spatialization_enabled(&g_OrkVoices.startWaaaghSound, MA_FALSE);
        }

        g_OrkVoices.variationFiles = {
            audioDir + "waaagh.wav",
            audioDir + "waaagh(1).wav",
            audioDir + "waaagh(3).wav"
        };

        for (const auto& file : g_OrkVoices.variationFiles) {
            for (int i = 0; i < 3; ++i) {
                auto item = std::make_unique<OrkVoicePoolItem>();
                item->filename = file;

                if (ma_sound_init_from_file(engine, file.c_str(), MA_SOUND_FLAG_DECODE, voiceGroup, NULL, &item->sound) == MA_SUCCESS) {
                    g_OrkVoices.voicePool.push_back(std::move(item));
                }
            }
        }
    }
    static void PlayStartWaaagh(float voiceVolume) {
        if (g_OrkVoices.startSoundLoaded && voiceVolume > 0.001f) {
            std::lock_guard<std::mutex> lock(g_OrkVoiceMutex);   // ← AGGIUNGI
            ma_sound_seek_to_pcm_frame(&g_OrkVoices.startWaaaghSound, 0);
            ma_sound_set_volume(&g_OrkVoices.startWaaaghSound, voiceVolume);
            ma_sound_start(&g_OrkVoices.startWaaaghSound);
        }
    }

    static void PlayOrkVoiceAtPos(const glm::vec3& orkPos, const glm::vec3& playerPos, float voiceVolume, float pitchMin = 0.90f, float pitchMax = 1.10f) {
        if (g_OrkVoices.voicePool.empty() || voiceVolume <= 0.001f) return;

        float distToPlayer = glm::distance(orkPos, playerPos);
        if (distToPlayer > 35.0f) return;

        float volume = CalculateSpatialVolume(distToPlayer) * voiceVolume;
        if (volume <= 0.005f) return;

        std::lock_guard<std::mutex> lock(g_OrkVoiceMutex);   // ← AGGIUNGI
        for (auto& item : g_OrkVoices.voicePool) {
            if (!ma_sound_is_playing(&item->sound)) {
                float randomPitch = Random::Range(pitchMin, pitchMax);
                ma_sound_set_volume(&item->sound, volume);
                ma_sound_set_pitch(&item->sound, randomPitch);
                ma_sound_set_position(&item->sound, orkPos.x, orkPos.y, orkPos.z);
                // FIX #7: l'attenuazione per distanza e' gia' calcolata a mano (CalculateSpatialVolume):
                // miniaudio fa SOLO il panning rispetto al listener, senza una seconda attenuazione 1/d.
                ma_sound_set_attenuation_model(&item->sound, ma_attenuation_model_none);
                ma_sound_seek_to_pcm_frame(&item->sound, 0);
                ma_sound_start(&item->sound);
                break;
            }
        }
    }

    static void TriggerOrkShoutAndProvoke(entt::entity speakerEntity, Scene& scene, const glm::vec3& playerPos, float voiceVolume) {
        if (!scene.GetRegistry().valid(speakerEntity)) return;

        const auto& speakerPos = scene.GetRegistry().get<TransformComponent>(speakerEntity).Position;
        PlayOrkVoiceAtPos(speakerPos, playerPos, voiceVolume);

        auto orkView = scene.GetRegistry().view<TransformComponent, HealthComponent>(
            entt::exclude<PlayerTag, CorpseComponent>
        );

        for (auto otherEntity : orkView) {
            if (otherEntity == speakerEntity) continue;
            if (g_OrkVoices.pendingShouts.size() >= 6) break;

            const auto& otherPos = orkView.get<TransformComponent>(otherEntity).Position;
            float distToSpeaker = glm::distance(speakerPos, otherPos);
            float perceivedVolume = CalculateSpatialVolume(distToSpeaker);

            if (perceivedVolume > 0.05f) {
                float provocationChance = perceivedVolume * 0.65f;

                if (Random::Chance(provocationChance)) {
                    bool alreadyPending = false;
                    for (const auto& pending : g_OrkVoices.pendingShouts) {
                        if (pending.orkEntity == otherEntity) {
                            alreadyPending = true;
                            break;
                        }
                    }

                    if (!alreadyPending) {
                        PendingShout shout{};
                        shout.orkEntity = otherEntity;
                        shout.delayTimer = Random::Range(0.2f, 0.8f);
                        g_OrkVoices.pendingShouts.push_back(shout);
                    }
                }
            }
        }
    }

    static void UpdateOrkVoices(Scene& scene, entt::entity playerEntity, float deltaTime, float voiceVolume) {
        std::vector<entt::entity> livingOrks;
        auto orkView = scene.GetRegistry().view<TransformComponent, HealthComponent>(
            entt::exclude<PlayerTag, CorpseComponent>
        );
        const auto& playerPos = scene.GetRegistry().get<TransformComponent>(playerEntity).Position;

        for (auto entity : orkView) {
            const auto& orkPos = orkView.get<TransformComponent>(entity).Position;
            if (glm::distance(orkPos, playerPos) <= 35.0f) {
                livingOrks.push_back(entity);
            }
        }

        if (livingOrks.empty()) {
            g_OrkVoices.pendingShouts.clear();
            return;
        }

        std::vector<entt::entity> readyToShout;
        for (auto it = g_OrkVoices.pendingShouts.begin(); it != g_OrkVoices.pendingShouts.end();) {
            it->delayTimer -= deltaTime;
            if (it->delayTimer <= 0.0f) {
                if (scene.GetRegistry().valid(it->orkEntity)) {
                    readyToShout.push_back(it->orkEntity);
                }
                it = g_OrkVoices.pendingShouts.erase(it);
            }
            else {
                ++it;
            }
        }

        for (auto shoutingOrk : readyToShout) {
            TriggerOrkShoutAndProvoke(shoutingOrk, scene, playerPos, voiceVolume);
        }

        g_OrkVoices.nextShoutTimer -= deltaTime;
        if (g_OrkVoices.nextShoutTimer <= 0.0f) {
            entt::entity leaderOrk = livingOrks[Random::RangeInt(0, static_cast<int>(livingOrks.size()) - 1)];
            TriggerOrkShoutAndProvoke(leaderOrk, scene, playerPos, voiceVolume);

            float baseCooldown = Random::Range(4.0f, 7.0f);
            g_OrkVoices.nextShoutTimer = std::max(1.5f, baseCooldown / std::sqrt(static_cast<float>(livingOrks.size())));
        }
    }

    static entt::entity SpawnOrkBoy(Scene& scene, const glm::vec3& spawnPos) {
        auto enemyEntity = scene.CreateEntity();
        auto& enemyTransform = scene.GetRegistry().emplace<TransformComponent>(enemyEntity);

        // FIX #21: spawn fuori mappa -> resta alla quota richiesta invece di y=0
        float groundY = GetGroundHeightAt(scene, spawnPos, spawnPos.y).value_or(spawnPos.y);
        enemyTransform.Position = spawnPos;
        enemyTransform.Position.y = groundY;
        enemyTransform.Scale = glm::vec3(1.0f, 1.0f, 1.0f);

        scene.GetRegistry().emplace<MeshComponent>(enemyEntity, 0u);
        scene.GetRegistry().emplace<HealthComponent>(enemyEntity, 50.0f, 50.0f);

        auto& enemyCol = scene.GetRegistry().emplace<ColliderComponent>(enemyEntity, glm::vec3(0.35f, 0.65f, 0.35f));
        enemyCol.localCenterOffset = glm::vec3(0.0f, 0.65f, 0.0f);
        scene.GetRegistry().emplace<RigidBodyComponent>(enemyEntity, glm::vec3(0.0f), 0.0f, false);

        auto& enemyColor = scene.GetRegistry().emplace<ColorComponent>(enemyEntity);
        enemyColor.Color = glm::vec4(0.24f, 0.33f, 0.15f, 1.0f);
        enemyColor.BaseColor = enemyColor.Color;

        auto& enemyStats = scene.GetRegistry().emplace<UnitStatsComponent>(enemyEntity);
        enemyStats.armorSet.torso.armorRating = 2;
        enemyStats.armorSet.torso.maxDurability = 50.0f;
        enemyStats.armorSet.torso.currentDurability = 50.0f;

        auto& footstep = scene.GetRegistry().emplace<FootstepComponent>(enemyEntity);
        footstep.currentSurface = SurfaceType::Concrete;
        footstep.stepInterval = 0.50f;

        auto& inv = scene.GetRegistry().emplace<InventoryComponent>(enemyEntity);
        inv.meleeWeaponId = "ork_choppa_1h";
        inv.currentSlot = 2;

        scene.GetRegistry().emplace<WeaponBlueprintId>(enemyEntity, "ork_choppa_1h");
        scene.GetRegistry().emplace<MeleeStateComponent>(enemyEntity);

        auto orkWeaponEntity = scene.CreateEntity();
        scene.GetRegistry().emplace<TransformComponent>(orkWeaponEntity);
        scene.GetRegistry().emplace<MeshComponent>(orkWeaponEntity, 0u);
        auto& wColor = scene.GetRegistry().emplace<ColorComponent>(orkWeaponEntity);
        wColor.Color = glm::vec4(0.45f, 0.47f, 0.50f, 1.0f);
        wColor.BaseColor = wColor.Color;

        auto& hordeComp = scene.GetRegistry().emplace<HordeAgentComponent>(enemyEntity);
        hordeComp.state = HordeState::Waiting;
        hordeComp.weaponEntity = orkWeaponEntity;

        std::string skeletonsDir = FindValidDir("Skeletons");
        auto orkSkeleton = SkeletonLoader::LoadFromFile(skeletonsDir + "orks_boy.json");
        if (orkSkeleton) {
            auto& skelComp = scene.GetRegistry().emplace_or_replace<SkeletonComponent>(enemyEntity);
            skelComp.data = *orkSkeleton;
            skelComp.scale = 1.0f;
            skelComp.blockFlashTimers.assign(skelComp.data.blocks.size(), 0.0f);
        }
        return enemyEntity;
    }

    // Scheletro a cubi (hitbox) di un'unita': quello indicato nel blueprint ("skeleton", es. lo yoof usa
    // orks_boy) con la sua scala, altrimenti Skeletons/<id>.json. Se il file non esiste si usa quello del
    // guardsman, cosi' l'unita' non si tiene lo scheletro della precedente.
    static std::optional<SkeletonData> LoadUnitSkeleton(const std::string& unitId, float& outScale) {
        const auto* bp = BlueprintManager::Get().GetUnit(unitId);
        const std::string skeletonsDir = FindValidDir("Skeletons");
        const std::string skelId = (bp && !bp->skeleton_id.empty()) ? bp->skeleton_id : unitId;
        outScale = (bp && bp->skeleton_scale > 0.05f) ? bp->skeleton_scale : 1.0f;
        if (auto data = SkeletonLoader::LoadFromFile(skeletonsDir + skelId + ".json")) return data;
        SUN_CORE_WARN("Scheletro '{0}' per l'unita' '{1}' non trovato: uso quello del guardsman", skelId, unitId);
        outScale = 1.0f;
        return SkeletonLoader::LoadFromFile(skeletonsDir + "imperium_guardsman_recruit.json");
    }

    static void SwapPlayerUnit(Scene& scene, entt::entity playerEntity, const std::string& newUnitId) {
        const auto* bp = BlueprintManager::Get().GetUnit(newUnitId);
        if (!bp) return;

        scene.GetRegistry().emplace_or_replace<MeshComponent>(playerEntity, 0u);
        scene.GetRegistry().get_or_emplace<UnitBlueprintId>(playerEntity).id = newUnitId;

        auto& stats = scene.GetRegistry().get_or_emplace<UnitStatsComponent>(playerEntity);
        stats.baseSpeed = bp->base_speed;
        stats.stamina = bp->stamina;
        stats.maxStamina = bp->stamina;   // prima restava 100: la stamina recuperava solo fino a 100
        stats.staminaRegen = bp->stamina_regen;
        stats.maxHealth = bp->health;
        stats.currentHealth = bp->health;
        // La salute vera e' l'HealthComponent: prima restava 100 per qualsiasi unita' (un ogryn aveva 100 PV)
        auto& unitHealth = scene.GetRegistry().get_or_emplace<HealthComponent>(playerEntity);
        unitHealth.maxHealth = bp->health;
        unitHealth.currentHealth = bp->health;
        stats.toughness = bp->toughness;
        stats.strength = bp->strength;
        stats.climbHeight = bp->climb_height;
        stats.climbSpeed = bp->climb_speed > 0.05f ? bp->climb_speed : 0.6f;
        stats.accuracySkill = bp->accuracy_skill;
        stats.meleeSkill = bp->melee_skill;
        stats.morale = bp->morale;
        stats.suppressionThreshold = static_cast<float>(bp->suppression_threshold);
        stats.visionRange = bp->vision_range;
        stats.fovDegrees = bp->fov_degrees;
        stats.weightCapacity = bp->weight_capacity;
        float skeletonScale = 1.0f;
        auto skeletonData = LoadUnitSkeleton(newUnitId, skeletonScale);
        if (skeletonData) {
            auto& skelComp = scene.GetRegistry().emplace_or_replace<SkeletonComponent>(playerEntity);
            skelComp.data = *skeletonData;
            skelComp.scale = skeletonScale;
            skelComp.blockFlashTimers.assign(skelComp.data.blocks.size(), 0.0f);

            auto worldTransforms = skelComp.data.ComputeWorldTransforms(
                glm::vec3(0.0f), glm::vec3(0.0f), skelComp.scale);

            float minX = 9999.0f, maxX = -9999.0f;
            float minY = 9999.0f, maxY = -9999.0f;
            float minZ = 9999.0f, maxZ = -9999.0f;

            for (const auto& M : worldTransforms) {
                for (int bx = 0; bx < 2; ++bx)
                    for (int by = 0; by < 2; ++by)
                        for (int bz = 0; bz < 2; ++bz) {
                            glm::vec4 p = M * glm::vec4(
                                bx ? 0.5f : -0.5f,
                                by ? 0.5f : -0.5f,
                                bz ? 0.5f : -0.5f, 1.0f);
                            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
                            minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
                            minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z);
                        }
            }

            const glm::vec3 rawHalfExtents(
                std::max(0.05f, (maxX - minX) * 0.5f),
                std::max(0.05f, (maxY - minY) * 0.5f),
                std::max(0.05f, (maxZ - minZ) * 0.5f)
            );

            const glm::vec3 rawCenterOffset(
                (minX + maxX) * 0.5f,
                (minY + maxY) * 0.5f,
                (minZ + maxZ) * 0.5f
            );

            if (auto* existingCol = scene.GetRegistry().try_get<ColliderComponent>(playerEntity)) {
                existingCol->halfExtents = rawHalfExtents;
                existingCol->localCenterOffset = rawCenterOffset;
            }
            else {
                auto& newCol = scene.GetRegistry().emplace<ColliderComponent>(playerEntity);
                newCol.halfExtents = rawHalfExtents;
                newCol.localCenterOffset = rawCenterOffset;
            }
        }

        glm::vec4 unitColor(0.8f, 0.8f, 0.8f, 1.0f);
        if (newUnitId == "imperium_guardsman_recruit") unitColor = glm::vec4(0.65f, 0.50f, 0.35f, 1.0f);
        else if (newUnitId == "imperium_kasrkin") unitColor = glm::vec4(0.35f, 0.25f, 0.18f, 1.0f);
        else if (newUnitId == "spacemarines_neophyte") unitColor = glm::vec4(0.92f, 0.72f, 0.68f, 1.0f);
        else if (newUnitId == "spacemarines_battle_brother") unitColor = glm::vec4(0.74f, 0.58f, 0.48f, 1.0f);
        else if (newUnitId == "orks_boy") unitColor = glm::vec4(0.24f, 0.33f, 0.15f, 1.0f);
        else if (newUnitId == "orks_nob") unitColor = glm::vec4(0.17f, 0.25f, 0.11f, 1.0f);

        auto& colorComp = scene.GetRegistry().emplace_or_replace<ColorComponent>(playerEntity);
        colorComp.Color = unitColor;
        colorComp.BaseColor = unitColor;
    }
    void GameLayer::ApplyUnitVisuals(const TrainedUnit& unit) {
        SwapPlayerUnit(m_Scene, m_PlayerEntity, unit.blueprintId);

        if (m_Scene.GetRegistry().all_of<ColorComponent>(m_PlayerEntity)) {
            auto& color = m_Scene.GetRegistry().get<ColorComponent>(m_PlayerEntity);
            color.Color = unit.armorColor;
            color.BaseColor = unit.armorColor;
        }

        if (m_Scene.GetRegistry().all_of<UnitStatsComponent>(m_PlayerEntity)) {
            auto& stats = m_Scene.GetRegistry().get<UnitStatsComponent>(m_PlayerEntity);
            auto& set = stats.armorSet;

            auto applyPiece = [](const std::string& pieceId, auto& piece) {
                const auto* bp = BlueprintManager::Get().GetArmorPiece(pieceId);
                if (bp) {
                    piece.id = bp->id;
                    piece.name = bp->displayName;
                    piece.armorRating = bp->armorRating;
                    piece.coverage = bp->coverage;
                    piece.maxDurability = bp->maxDurability;
                    piece.currentDurability = bp->maxDurability;
                    piece.weight = bp->weight;
                    piece.pointCost = bp->pointCost;
                    piece.blocksHeadshotBonus = bp->blocks_headshot_bonus;
                }
                else {
                    piece = {};
                }
            };

            applyPiece(unit.armorHeadId, set.head);
            applyPiece(unit.armorTorsoId, set.torso);
            applyPiece(unit.armorLeftArmId, set.leftArm);
            applyPiece(unit.armorRightArmId, set.rightArm);
            applyPiece(unit.armorLeftLegId, set.leftLeg);
            applyPiece(unit.armorRightLegId, set.rightLeg);

            stats.currentWeight = unit.CalculateTotalWeight();
        }

        if (m_Scene.GetRegistry().all_of<InventoryComponent>(m_PlayerEntity)) {
            auto& inv = m_Scene.GetRegistry().get<InventoryComponent>(m_PlayerEntity);
            inv.primaryWeaponId = unit.primaryWeaponId;
            inv.secondaryWeaponId = unit.secondaryWeaponId;
            inv.meleeWeaponId = unit.meleeWeaponId;
            inv.tool1Id = unit.equipmentSlots[0];
            inv.tool2Id = unit.equipmentSlots[1];
            inv.tool3Id = unit.equipmentSlots[2];

            // Slot vuoto (es. il gretchin non ha primaria): si passa al primo slot non vuoto
            if (!inv.IsSlotSelectable(inv.currentSlot)) inv.currentSlot = inv.FirstUsableSlot();
            std::string activeId = inv.GetActiveWeaponId();
            if (inv.GetEquipStance() == EquipStance::HybridRangedMelee) activeId = inv.secondaryWeaponId;

            m_Scene.GetRegistry().get_or_emplace<WeaponBlueprintId>(m_PlayerEntity).id = activeId;

            if (m_Scene.GetRegistry().all_of<WeaponStateComponent>(m_PlayerEntity)) {
                auto& wState = m_Scene.GetRegistry().get<WeaponStateComponent>(m_PlayerEntity);
                wState.isReloading = false;
                wState.reloadTimer = 0.0f;
                wState.fireCooldown = 0.0f;
                wState.currentHeat = 0.0f;
                wState.isOverheating = false;
                wState.burstRemaining = 0;
                const auto* weaponBP = BlueprintManager::Get().GetWeapon(activeId);
                if (weaponBP) wState.currentAmmo = weaponBP->magazine_size;
                // Munizioni limitate: caricatori pieni e riserva iniziale dell'unita'
                Ammo::Init(m_Scene.GetRegistry(), m_PlayerEntity, unit.blueprintId);
            }
        }
        ApplyArmorSkin(m_PlayerEntity, unit);   // vesti / elmetto visibili sul campo
    }

    // Skin dell'armatura sul campo: colora i blocchi coperti dalle vesti e aggiunge i pezzi (es. elmetto).
    // Gretchin: nudo, vestito, nudo con elmetto o vestito con elmetto, a seconda di cosa indossa.
    void GameLayer::ApplyArmorSkin(entt::entity e, const TrainedUnit& unit) {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(e)) return;
        const auto* skel = reg.try_get<SkeletonComponent>(e);
        auto findBlock = [&](const std::string& name) {
            if (!skel) return -1;
            for (size_t i = 0; i < skel->data.blocks.size(); ++i) if (skel->data.blocks[i].name == name) return static_cast<int>(i);
            return -1;
        };
        SkinComponent skin;
        if (skel) skin.blockColors.assign(skel->data.blocks.size(), glm::vec4(0.0f));
        const std::string ids[6] = { unit.armorHeadId, unit.armorTorsoId, unit.armorLeftArmId, unit.armorRightArmId, unit.armorLeftLegId, unit.armorRightLegId };
        bool any = false;
        // Se la fonte (veste, armatura, dotazione) ha degli strati per il modello animato, quelli sostituiscono
        // i suoi cubi agganciati alle ossa: i cubi restano solo sullo scheletro a blocchi.
        bool sourceHasGarments = false;
        auto addGarments = [&](const std::vector<GarmentDef>& g) {
            sourceHasGarments = !g.empty();
            for (const auto& d : g) { skin.garments.push_back(d); any = true; }
        };
        auto addPart = [&](const ArmorSkinPart& sp) {
            if (sourceHasGarments && sp.block.empty()) return;   // pezzo solo per il modello animato: c'e' lo strato
            SkinPart p;
            p.block = findBlock(sp.block);
            p.offset = sp.offset; p.bone = sourceHasGarments ? std::string() : sp.bone; p.boneOffset = sp.boneOffset; p.size = sp.size;
            p.color = sp.color; p.roughness = sp.roughness; p.metallic = sp.metallic;
            skin.parts.push_back(p);
            any = true;
        };
        // Vestiti dell'unita' (es. calzoni degli orki), sotto l'armatura
        if (const auto* ubp = BlueprintManager::Get().GetUnit(unit.blueprintId)) {
            for (const auto& [blockName, col] : ubp->baseSkinBlockColors) {
                const int b = findBlock(blockName);
                if (b >= 0) { skin.blockColors[b] = col; any = true; }
            }
            addGarments(ubp->baseGarments);
            for (const auto& sp : ubp->baseSkinParts) addPart(sp);
        }
        // Dotazione visibile sul corpo (es. zaino munizioni)
        for (const auto& eqId : unit.equipmentSlots)
            if (const auto* eq = BlueprintManager::Get().GetWeapon(eqId)) { addGarments(eq->garments); for (const auto& sp : eq->skinParts) addPart(sp); }
        for (const auto& id : ids) {
            const auto* abp = BlueprintManager::Get().GetArmorPiece(id);
            if (!abp) continue;
            for (const auto& [blockName, col] : abp->skinBlockColors) {
                const int b = findBlock(blockName);
                if (b >= 0) { skin.blockColors[b] = col; any = true; }
            }
            addGarments(abp->garments);
            for (const auto& sp : abp->skinParts) addPart(sp);
        }
        if (any) reg.emplace_or_replace<SkinComponent>(e, std::move(skin));
        else if (reg.all_of<SkinComponent>(e)) reg.remove<SkinComponent>(e);
    }

    // Squig col modello skinnato: le ossa seguono i blocchi dello scheletro (o il ragdoll quando muore)
    void GameLayer::UpdateSquigVisuals() {
        auto& reg = m_Scene.GetRegistry();
        if (!m_SquigSkeleton || !m_SquigModel.Build(*m_SquigSkeleton)) return;
        std::vector<entt::entity> fresh;
        for (auto e : reg.view<SquigComponent, SkeletonComponent, TransformComponent>()) if (!reg.all_of<SquigVisualTag>(e)) fresh.push_back(e);
        for (auto e : fresh) {
            reg.emplace<SquigVisualTag>(e);
            auto& sk = reg.emplace_or_replace<SkinnedModelInstanceComponent>(e);
            sk.sourceModel = m_SquigModel.Model();
            sk.extraParts = m_SquigModel.Parts();
            sk.skinStyle = glm::vec4(0.6f, 0.9f, 0.5f, 0.2f);
            sk.roughness = 0.55f;
            m_SquigBones[e] = std::make_unique<std::vector<glm::mat4>>(8, glm::mat4(1.0f));
            sk.boneMatrices = m_SquigBones[e].get();
        }
        for (auto it = m_SquigBones.begin(); it != m_SquigBones.end();) {
            const entt::entity e = it->first;
            if (!reg.valid(e) || !reg.all_of<SquigVisualTag, SkinnedModelInstanceComponent, TransformComponent>(e)) { it = m_SquigBones.erase(it); continue; }
            auto& sk = reg.get<SkinnedModelInstanceComponent>(e);
            const auto& t = reg.get<TransformComponent>(e);
            const glm::mat4 em = t.GetTransform();
            if (const auto* rd = reg.try_get<RagdollComponent>(e); rd && !rd->worldTransforms.empty())
                m_SquigModel.ComputeBones(rd->worldTransforms, em, *it->second);
            else if (const auto* sc = reg.try_get<SkeletonComponent>(e))
                m_SquigModel.ComputeBones(sc->data.ComputeWorldTransforms(t.Position, t.Rotation, sc->scale), em, *it->second);
            sk.boneMatrices = it->second.get();
            if (const auto* c = reg.try_get<ColorComponent>(e)) sk.albedo = c->Color;
            ++it;
        }
    }

    const char* GameLayer::BodyLabelOf(int kind) {
        switch (static_cast<AnimatedBodyKind>(kind)) {
        case AnimatedBodyKind::Ork: return "ork";
        case AnimatedBodyKind::Ogryn: return "ogryn";
        case AnimatedBodyKind::Neophyte: return "neofita";
        case AnimatedBodyKind::Astartes: return "astartes";
        case AnimatedBodyKind::Gretchin: return "gretchin";
        default: return "guardsman";
        }
    }

    // Pelle della razza sul modello animato (chiazze, verruche, definizione muscolare, vene: vedi MuscleTone.h)
    void GameLayer::ApplySkinStyle(SkinnedModelInstanceComponent& sk, int kind) {
        sk.skinStyle = RaceSkinStyle(BodyLabelOf(kind));
    }
    bool GameLayer::ReloadMap(const std::string& mapId) {
        m_NavQuery.Destroy();
        {   // via i detriti della mappa precedente (non hanno collider: la pulizia per collider non li prende)
            std::vector<entt::entity> decor;
            for (auto e : m_Scene.GetRegistry().view<MapDecorTag>()) decor.push_back(e);
            for (auto e : decor) m_Scene.DestroyEntity(e);
        }

        std::string mapsDir = FindValidDir("Maps");
        if (mapsDir.empty()) mapsDir = "GameData/Maps/";

        m_CurrentMapData = MapLoader::LoadFromFile(m_Scene, mapsDir + mapId + ".json");
        m_PhysicsSystem.RefreshBodies(m_Scene);

        if (m_CurrentMapData.navMesh) {
            if (!m_NavQuery.Init(m_CurrentMapData.navMesh.get())) {
                SUN_CORE_ERROR("NavMeshQuery::Init fallita dopo reload mappa '{0}'", mapId);
                return false;
            }
        }
        else {
            SUN_CORE_WARN("Mappa '{0}' senza NavMesh: pathfinding disabilitato.", mapId);
        }

        SpawnTrainingDummies();
        return true;
    }
    int GameLayer::CalculateSquadPoints() const {
        int total = 0;
        for (int rosterIdx : m_SelectedSquadIndices) {
            if (rosterIdx >= 0 && rosterIdx < static_cast<int>(m_PlayerRoster.size())) {
                // Punti battaglia: unita' + equipaggiamento rivisto (vedi TrainedUnit::CalculateBattlePoints)
                total += m_PlayerRoster[rosterIdx].CalculateBattlePoints();
            }
        }
        return total;
    }
    void GameLayer::UpdateWindowSize() {
        GLFWwindow* window = glfwGetCurrentContext();
        if (!window) return;

        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);

        if (w > 0 && h > 0) {
            m_WindowWidth = static_cast<float>(w);
            m_WindowHeight = static_cast<float>(h);
            // FIX #6a: rete di sicurezza, il renderer segue SEMPRE la size reale del framebuffer.
            // Resize() esce subito se la size e' invariata, quindi il costo per frame e' un confronto.
            m_DeferredRenderer.Resize(w, h);
        }
    }

    // FIX #6d: arriva dal framebuffer-size callback GLFW via Application::OnEvent.
    void GameLayer::OnWindowResize(int width, int height) {
        if (width <= 0 || height <= 0) return;
        m_WindowWidth = static_cast<float>(width);
        m_WindowHeight = static_cast<float>(height);
        glViewport(0, 0, width, height);
        m_DeferredRenderer.Resize(width, height);
        m_Camera.SetViewportSize(m_WindowWidth, m_WindowHeight);
    }
    void GameLayer::ApplyDisplaySettings(int width, int height, bool fullscreen) {
        GLFWwindow* window = glfwGetCurrentContext();
        if (!window) return;

        if (fullscreen) {
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(window, monitor, 0, 0, width, height, mode->refreshRate);
        }
        else {
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            int posX = (mode->width - width) / 2;
            int posY = (mode->height - height) / 2;
            glfwSetWindowMonitor(window, NULL, std::max(50, posX), std::max(50, posY), width, height, 0);
        }
        // FIX #6b: la size reale del framebuffer puo' differire da quella richiesta (HiDPI, driver);
        // si legge dopo glfwSetWindowMonitor e si ridimensiona il renderer deferred di conseguenza.
        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        if (fbW <= 0 || fbH <= 0) { fbW = width; fbH = height; }
        glViewport(0, 0, fbW, fbH);

        m_WindowWidth = static_cast<float>(fbW);
        m_WindowHeight = static_cast<float>(fbH);
        m_DeferredRenderer.Resize(fbW, fbH);   // sicuro: chiamato da ImGui dopo il present del frame
        m_Camera.SetViewportSize(m_WindowWidth, m_WindowHeight);
    }

    void GameLayer::CheckUnitDeaths() {
        // FIX #22: prima si raccolgono le unita' morte, POI si modifica il registry (KillUnit fa
        // emplace/replace di Corpse/Ragdoll/JoltBody): niente mutazioni durante l'iterazione della view.
        auto& reg = m_Scene.GetRegistry();
        std::vector<entt::entity> dead;
        {
            // I manichini del campo da tiro non muoiono mai (la loro salute viene ripristinata)
            auto view = reg.view<HealthComponent, TransformComponent>(
                entt::exclude<CorpseComponent, PlayerTag, TrainingDummyComponent>
            );
            for (auto entity : view) {
                if (view.get<HealthComponent>(entity).currentHealth <= 0.0f) {
                    if (NetDeathBlocked(entity)) continue;   // copia in rete: muore quando lo dice chi la simula
                    dead.push_back(entity);
                }
            }
        }

        for (auto entity : dead) {
            if (!reg.valid(entity)) continue;

            // Squig ucciso: ragdoll a cubi, non conta come uccisione e per il padrone e' perso fino a fine partita
            if (const auto* sq = reg.try_get<SquigComponent>(entity)) {
                if (reg.valid(sq->owner)) {
                    if (auto* ammo = reg.try_get<AmmoComponent>(sq->owner)) {
                        if (sq->ownerSlot >= 4 && sq->ownerSlot <= 6) ammo->equipmentUses[sq->ownerSlot - 4] = 0;
                        ammo->feedback = "Il tuo grugn e' stato ucciso";
                        ammo->feedbackTimer = 2.5f;
                    }
                }
                reg.remove<SquigComponent>(entity);
                if (reg.all_of<EvasionComponent>(entity)) reg.remove<EvasionComponent>(entity);
                m_PhysicsSystem.KillUnit(m_Scene, entity, glm::vec3(0.0f, 2.0f, 0.0f));
                if (!reg.all_of<CorpseComponent>(entity)) m_Scene.DestroyEntity(entity);
                SUN_CORE_INFO("Squig ucciso");
                continue;
            }

            SUN_CORE_WARN("Unita' eliminata! Attivazione Ragdoll Jolt Physics...");

            // FIX #3: l'arma dell'ork sparisce alla morte (il ragdoll non la regge in mano).
            // L'observer on_destroy copre comunque i casi in cui l'ork sparisce senza passare di qui.
            if (auto* horde = reg.try_get<HordeAgentComponent>(entity)) {
                if (horde->weaponEntity != entt::null && reg.valid(horde->weaponEntity)) {
                    reg.destroy(horde->weaponEntity);
                }
                horde->weaponEntity = entt::null;
            }

            // Bot: l'arma visibile sparisce alla morte. Le uccisioni contano solo se il caduto e' un avversario
            // (o un'unita' senza squadra, come l'orda): un alleato morto non da' energia.
            bool countsAsKill = true;
            if (auto* bot = reg.try_get<BotBrainComponent>(entity)) {
                if (bot->weaponEntity != entt::null && reg.valid(bot->weaponEntity)) reg.destroy(bot->weaponEntity);
                bot->weaponEntity = entt::null;
            }
            if (const auto* team = reg.try_get<TeamComponent>(entity)) {
                countsAsKill = team->team != m_VsBotMyTeam;
                if (countsAsKill) ++m_VsBotEnemyKills;
            }

            // Partita in rete: l'host avvisa tutti (e dice chi l'ha ucciso), le copie si tolgono dagli elenchi
            const bool awardHere = NetOnUnitKilled(entity);

            glm::vec3 deathImpulse(0.0f, 1.5f, -3.5f);
            m_PhysicsSystem.KillUnit(m_Scene, entity, deathImpulse);

            if (!countsAsKill) {
                if (!reg.all_of<CorpseComponent>(entity)) {
                    m_PhysicsSystem.DestroyJoltBody(m_Scene, entity);
                    m_Scene.DestroyEntity(entity);
                }
            }
            else if (reg.all_of<CorpseComponent>(entity)) {
                // FIX #4/#19: la kill si conta QUI, nell'unico punto in cui un'unita' non-player muore
                // (la view esclude PlayerTag). Niente piu' scansione per-frame dei cadaveri ne'
                // ProcessedCorpseTag: il cadavere del player non puo' piu' essere contato per errore.
                if (awardHere) AwardKill();
            }
            else {
                // Nessuno scheletro -> KillUnit non puo' creare il ragdoll e l'unita' resterebbe
                // "morta ma viva" (rientrerebbe in questa lista ogni frame). La rimuovo.
                m_PhysicsSystem.DestroyJoltBody(m_Scene, entity);
                m_Scene.DestroyEntity(entity);
                if (awardHere) AwardKill();
            }
        }
    }

    // Ondate dell'orda (Difesa del Settore): ogni 10 s un gruppo che raddoppia, attorno al giocatore. In una partita in
    // rete le genera solo chi simula i nemici (l'host); gli altri ricevono le copie.
    void GameLayer::UpdateWaaaghWaves(float deltaTime, const glm::vec3& center) {
        if (!g_WaaaghActive || !NetIsAuthority()) return;
        g_WaaaghTimer += deltaTime;
        if (g_WaaaghTimer < 10.0f) return;
        g_WaaaghTimer = 0.0f;

        auto livingEnemyView = m_Scene.GetRegistry().view<HealthComponent>(
            entt::exclude<PlayerTag, CorpseComponent, TrainingDummyComponent, SquigComponent, NetRemotePlayerComponent>
        );
        size_t activeEnemyCount = static_cast<size_t>(std::distance(livingEnemyView.begin(), livingEnemyView.end()));
        constexpr size_t MAX_ACTIVE_ENEMIES = 60;

        if (activeEnemyCount < MAX_ACTIVE_ENEMIES) {
            const float minSpawnDistance = 20.0f;
            const float maxSpawnDistance = 60.0f;
            int spawnAmount = std::min(g_WaaaghSpawnCount, static_cast<int>(MAX_ACTIVE_ENEMIES - activeEnemyCount));
            for (int i = 0; i < spawnAmount; ++i) {
                float angle = Random::Range(0.0f, 6.28318530718f);
                float distance = Random::Range(minSpawnDistance, maxSpawnDistance);
                glm::vec3 spawnPos = center + glm::vec3(std::cos(angle) * distance, 0.0f, std::sin(angle) * distance);
                SpawnOrkBoy(m_Scene, spawnPos);
            }
        }
        g_WaaaghSpawnCount = std::min(g_WaaaghSpawnCount * 2, 12);
    }

    // Partita in rete: le copie degli orchi simulati dall'host (stessa unita' dell'ondata)
    entt::entity GameLayer::SpawnNetOrk(const glm::vec3& pos) { return SpawnOrkBoy(m_Scene, pos); }

    void GameLayer::GetWaveState(bool& active, float& timer, int& count) const { active = g_WaaaghActive; timer = g_WaaaghTimer; count = g_WaaaghSpawnCount; }
    void GameLayer::SetWaveState(bool active, float timer, int count) { g_WaaaghActive = active; g_WaaaghTimer = timer; g_WaaaghSpawnCount = std::clamp(count, 1, 12); }

    // FIX #4: pulisce lo stato "morto" del player (ragdoll Jolt + CorpseComponent).
    // Prima lo faceva solo il pulsante SCHIERA del respawn: tornando al garage a squadra distrutta
    // il player manteneva CorpseComponent+Ragdoll (ragdoll visibile in garage) e al redeploy il suo
    // cadavere veniva contato come kill. Idempotente.
    void GameLayer::ClearPlayerDeathState() {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(m_PlayerEntity)) return;
        if (reg.all_of<RagdollComponent>(m_PlayerEntity)) {
            m_PhysicsSystem.DestroyJoltBody(m_Scene, m_PlayerEntity);
        }
        if (reg.all_of<CorpseComponent>(m_PlayerEntity)) {
            reg.remove<CorpseComponent>(m_PlayerEntity);
        }
    }

    // Tutto quello che una partita lascia in giro. Si chiama da sola quando si esce da una partita (qualunque strada:
    // pausa, squadra distrutta, fine del Vs Bot) e quando se ne inizia una: prima ogni uscita resettava solo una parte
    // e il resto passava alla partita dopo (colpi in volo, scie, bersagli colpiti, cadaveri, mimetismo, scavalcamento
    // a meta', mischia o ricarica in corso, mira col mirino...).
    void GameLayer::ResetMatchState(bool leaving) {
        auto& reg = m_Scene.GetRegistry();
        m_WeaponSystem.ClearPendingShots();
        FeelEventQueue().clear();
        VfxEventQueue().clear();
        XpEventLog().clear();
        m_Feel.Reset();
        m_ParticleSystem.Clear();
        // entita' di passaggio della partita
        std::vector<entt::entity> junk;
        for (auto e : reg.view<TrailRequestComponent>()) junk.push_back(e);
        for (auto e : reg.view<HitMarkerComponent>()) if (e != m_PlayerEntity) junk.push_back(e);
        for (auto e : reg.view<ProjectileComponent>()) junk.push_back(e);
        for (auto e : reg.view<CorpseComponent>()) if (e != m_PlayerEntity) junk.push_back(e);
        std::sort(junk.begin(), junk.end());
        junk.erase(std::unique(junk.begin(), junk.end()), junk.end());
        for (auto e : junk) {
            if (!reg.valid(e)) continue;
            m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
            m_Scene.DestroyEntity(e);
        }
        if (leaving) {   // all'ingresso no: la partita che parte li ha appena impostati (ondata WAAAGH! della sopravvivenza)
            g_WaaaghActive = false;
            g_WaaaghTimer = 0.0f;
            g_WaaaghSpawnCount = 1;
            ResetFactionBuff();
        }
        m_IsPaused = false;
        m_PlayerDeathHandled = false;
        if (!reg.valid(m_PlayerEntity)) return;
        // il giocatore: niente stati a meta'
        ClearPlayerDeathState();
        reg.remove<VaultComponent, TurretMountComponent, SpeedBoostComponent, MoraleAuraComponent, HitMarkerComponent, ActionPoseComponent>(m_PlayerEntity);
        if (auto* cloak = reg.try_get<CloakComponent>(m_PlayerEntity)) { cloak->amount = 0.0f; cloak->timeLeft = 0.0f; }
        if (auto* crouch = reg.try_get<CrouchComponent>(m_PlayerEntity)) crouch->amount = 0.0f;
        if (auto* melee = reg.try_get<MeleeStateComponent>(m_PlayerEntity)) {
            melee->currentPhase = MeleePhase::None; melee->currentAttack = MeleeAttackType::None; melee->currentAttackNode.clear();
            melee->stateTimer = 0.0f; melee->stunTimer = 0.0f; melee->lungeTimer = 0.0f; melee->lungeVelocity = glm::vec3(0.0f);
            melee->bufferedInput.clear(); melee->hasHitInThisAttack = false; melee->whiffPenalty = 0.0f; melee->parrySuccessTimer = 0.0f;
        }
        if (auto* rc = reg.try_get<CameraRecoilComponent>(m_PlayerEntity)) { rc->recoilPitch = rc->recoilYaw = rc->shakeIntensity = rc->fovOffset = 0.0f; }
        if (auto* ws = reg.try_get<WeaponStateComponent>(m_PlayerEntity)) {
            ws->isReloading = false; ws->reloadTimer = 0.0f; ws->fireCooldown = 0.0f; ws->currentHeat = 0.0f; ws->isOverheating = false; ws->isScopeActive = false;
        }
        if (auto* am = reg.try_get<AmmoComponent>(m_PlayerEntity)) { am->useTimer = 0.0f; am->useSlot = -1; am->useTarget = entt::null; am->feedbackTimer = 0.0f; }
        if (auto* aim = reg.try_get<AimRayComponent>(m_PlayerEntity)) aim->valid = false;
        if (auto* t = reg.try_get<TransformComponent>(m_PlayerEntity)) { t->Rotation.x = 0.0f; t->Rotation.z = 0.0f; t->AimPitch = 0.0f; }
    }

    GameLayer::GameLayer() : Layer("Game Layer")
    {
        m_PhysicsSystem.Init(m_Scene);
        // FIX #3: pulizia automatica dell'arma nemica alla distruzione dell'ork
        m_Scene.GetRegistry().on_destroy<HordeAgentComponent>().connect<&OnHordeAgentDestroyed>();
        m_Scene.GetRegistry().on_destroy<BotBrainComponent>().connect<&OnBotDestroyed>();
        {
            // FIX #6: size iniziale = framebuffer reale (non 1280x720 hardcoded)
            int initW = 1280, initH = 720;
            if (GLFWwindow* w0 = glfwGetCurrentContext()) {
                int fw = 0, fh = 0;
                glfwGetFramebufferSize(w0, &fw, &fh);
                if (fw > 0 && fh > 0) { initW = fw; initH = fh; }
            }
            m_DeferredRenderer.Init(initW, initH);
        }
        m_StaticShader.Init();

        std::string modelsDir = FindValidDir("Models");
        if (!modelsDir.empty()) {
            // Vestiti del modello animato: i percorsi nei JSON sono relativi a GameData
            m_Garments.SetRoot(std::filesystem::path(modelsDir).parent_path().parent_path().string() + "/");
            std::string fbxPath = modelsDir + "Characters/guardsman_master.fbx";
            // Il modello skinnato serve al personaggio animato (UpdateAnimatedPlayer). Rimossi i modelli di
            // prova: la copia statica in T-pose, il dummy animato accanto al giocatore e l'entity all'origine.
            m_SkinnedShader.Init();
            // Modello d'autore in Characters/guardsman/ (vedi AuthoredModel.h e la guida per gli artisti): sostituisce il
            // corpo umano procedurale. Le altre razze allora non si adattano dal corpo umano (BodyFit).
            AuthoredCharacterInfo humanAsset;
            if (FindAuthoredCharacter(modelsDir + "Characters/guardsman", humanAsset) &&
                LoadAuthoredCharacter(humanAsset, m_SkinnedTestModel, DefaultBodyHeightCm("guardsman"))) {
                m_SkinnedModelLoaded = true;
                fbxPath = humanAsset.modelPath;
            }
            else {
                m_SkinnedModelLoaded = m_SkinnedTestModel.LoadModel(fbxPath);
            }
            if (m_SkinnedModelLoaded) {
                std::string animsDir = FindValidDir("Animations");
                const bool ownClips = !humanAsset.animsDir.empty() && m_SkinnedTestModel.authored;
                const std::string gameClips = animsDir.empty() ? std::string() : animsDir + "Human/";
                if (!animsDir.empty() || ownClips) {
                    m_RifleAnimLoaded = m_RifleAnimController.LoadClips(ownClips ? humanAsset.animsDir : gameClips, fbxPath, &m_SkinnedTestModel,
                                                                        ownClips ? gameClips : std::string());
                }
            }

            // Corpi animati delle altre unita' (stesso rig del guardsman)
            LoadAnimatedBody(Body(AnimatedBodyKind::Ork), modelsDir, "ork_master.fbx", "Orks", "ork");
            LoadAnimatedBody(Body(AnimatedBodyKind::Ogryn), modelsDir, "ogryn_master.fbx", "Ogryn", "ogryn");
            LoadAnimatedBody(Body(AnimatedBodyKind::Neophyte), modelsDir, "neophyte_master.fbx", "Neophyte", "neofita");
            LoadAnimatedBody(Body(AnimatedBodyKind::Astartes), modelsDir, "astartes_master.fbx", "Astartes", "astartes");
            LoadAnimatedBody(Body(AnimatedBodyKind::Gretchin), modelsDir, "gretchin_master.fbx", "Gretchin", "gretchin");
            // Il corpo umano base serve da sorgente al BodyFit delle altre razze: muscoli solo dopo
            if (m_SkinnedModelLoaded && !m_SkinnedTestModel.authored) {
                ApplyMuscleTone(m_SkinnedTestModel, RaceMuscleTone("guardsman"));
            }
        }

        GLFWwindow* window = glfwGetCurrentContext();
        if (window) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }

        if (ImGui::GetCurrentContext() == nullptr) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::StyleColorsDark();
            Ui::Apply(FindValidDir("Fonts"));   // tema dell'interfaccia: colori, misure, caratteri (UiTheme.h)

            if (window) {
                ImGui_ImplGlfw_InitForOpenGL(window, true);
                ImGui_ImplOpenGL3_Init("#version 410");
            }
        }

        int fbW = 0, fbH = 0;
        if (window) glfwGetFramebufferSize(window, &fbW, &fbH);
        m_WindowWidth = (fbW > 0) ? static_cast<float>(fbW) : 1280.0f;
        m_WindowHeight = (fbH > 0) ? static_cast<float>(fbH) : 720.0f;
        m_Camera.SetViewportSize(m_WindowWidth, m_WindowHeight);

        m_PlayerEntity = m_Scene.CreateEntity();
        auto& playerTransform = m_Scene.GetRegistry().emplace<TransformComponent>(m_PlayerEntity);
        m_Scene.GetRegistry().emplace<PlayerTag>(m_PlayerEntity);

        playerTransform.Position = glm::vec3(0.0f, 0.0f, 0.0f);
        playerTransform.Scale = glm::vec3(1.0f, 1.0f, 1.0f);

        auto& playerCol = m_Scene.GetRegistry().emplace<ColliderComponent>(m_PlayerEntity, glm::vec3(0.3f, 0.62f, 0.3f));
        playerCol.localCenterOffset = glm::vec3(0.0f, 0.62f, 0.0f);
        m_Scene.GetRegistry().emplace<RigidBodyComponent>(m_PlayerEntity, glm::vec3(0.0f), 0.0f, false);

        m_Scene.GetRegistry().emplace<HealthComponent>(m_PlayerEntity, 100.0f, 100.0f);
        m_Scene.GetRegistry().emplace<UnitBlueprintId>(m_PlayerEntity, "imperium_guardsman_recruit");
        m_Scene.GetRegistry().emplace<UnitStatsComponent>(m_PlayerEntity);
        m_Scene.GetRegistry().emplace<MeleeStateComponent>(m_PlayerEntity);
        m_Scene.GetRegistry().emplace<CameraRecoilComponent>(m_PlayerEntity);

        SwapPlayerUnit(m_Scene, m_PlayerEntity, "imperium_guardsman_recruit");

        auto& inv = m_Scene.GetRegistry().emplace<InventoryComponent>(m_PlayerEntity);
        inv.primaryWeaponId = "lasgun_standard";
        inv.secondaryWeaponId = "laspistol_standard";
        inv.meleeWeaponId = "combat_knife_1h";
        inv.currentSlot = 0;

        m_Scene.GetRegistry().emplace<WeaponBlueprintId>(m_PlayerEntity, inv.primaryWeaponId);
        m_Scene.GetRegistry().emplace<WeaponStateComponent>(m_PlayerEntity);

        m_WeaponEntity = m_Scene.CreateEntity();
        m_Scene.GetRegistry().emplace<TransformComponent>(m_WeaponEntity);
        m_Scene.GetRegistry().emplace<MeshComponent>(m_WeaponEntity, 0u);
        auto& wColor = m_Scene.GetRegistry().emplace<ColorComponent>(m_WeaponEntity);
        wColor.Color = glm::vec4(0.2f, 0.2f, 0.25f, 1.0f);
        wColor.BaseColor = wColor.Color;

        ReloadMap("warzone_sector4");

        if (!m_CurrentMapData.playerSpawnPoints.empty()) {
            playerTransform.Position = m_CurrentMapData.playerSpawnPoints[0];
            m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, playerTransform.Position);
        }
        // PROFILO: carico impostazioni (volumi, risoluzione, ...) PRIMA di inizializzare l'audio, cosi'
        // il gruppo SFX parte gia' col volume salvato.
        LoadProfile();

        if (ma_engine_init(NULL, &m_AudioEngine) != MA_SUCCESS) {
            SUN_CORE_ERROR("ERRORE: Impossibile inizializzare MiniAudio!");
            m_AudioEngineInitialized = false;
        }
        else {
            m_AudioEngineInitialized = true;

            // Tre canali, ognuno con la sua barra nelle opzioni: effetti (armi, passi), voci, musica
            if (ma_sound_group_init(&m_AudioEngine, 0, NULL, &m_SFXGroup) == MA_SUCCESS) m_SFXGroupInitialized = true;
            if (ma_sound_group_init(&m_AudioEngine, 0, NULL, &m_VoiceGroup) == MA_SUCCESS) m_VoiceGroupInitialized = true;
            if (ma_sound_group_init(&m_AudioEngine, 0, NULL, &m_MusicGroup) == MA_SUCCESS) m_MusicGroupInitialized = true;
            ApplyAudioVolumes();
            InitOrkAudio(&m_AudioEngine, m_VoiceGroupInitialized ? &m_VoiceGroup : nullptr);
            InitMusic();

            std::string audioDir = FindValidDir("Audio");
            m_WeaponSystem.InitAudio(
                &m_AudioEngine,
                m_SFXGroupInitialized ? &m_SFXGroup : nullptr,
                audioDir
            );
            // Suoni per tipo di arma, impatti, esplosioni, passi per corpo (GameData/Audio/Feel)
            m_Feel.Init(&m_AudioEngine, m_SFXGroupInitialized ? &m_SFXGroup : nullptr, audioDir);
        }
        m_Scene.GetRegistry().emplace<ParticleEmitterComponent>(m_PlayerEntity);

        // PROFILO: se esiste un salvataggio carica il roster e la squadra selezionata. Profilo nuovo (o roster
        // vuoto): LoadResearchTree crea il garage iniziale con la sola unita' radice di ogni ramo.
        LoadRosterFromProfile();
        LoadResearchTree();   // dopo il roster: le unita' gia' possedute risultano sbloccate
        if (m_PlayerRoster.empty()) BuildStarterRoster();   // albero di ricerca non leggibile: unita' di riserva
        LoadBlockModels();
        if (m_SelectedSquadIndices.empty()) m_SelectedSquadIndices.push_back(0);
        ApplyUnitVisuals(m_PlayerRoster[m_SelectedSquadIndices[0]]);

        // PROFILO: risoluzione/fullscreen salvati (la finestra parte sempre 1280x720 windowed)
        if (m_IsFullscreen || m_SelectedResolutionIdx != 0) {
            ApplyDisplaySettings(g_Resolutions[m_SelectedResolutionIdx].width,
                                 g_Resolutions[m_SelectedResolutionIdx].height, m_IsFullscreen);
        }
    }

    // ===================== IMPOSTAZIONI GRAFICHE <-> PROFILO =====================
    static void JsonFloat(const nlohmann::json& j, const char* k, float& out, float lo, float hi) {
        if (j.contains(k) && j[k].is_number()) out = std::clamp(j[k].get<float>(), lo, hi);
    }
    static void JsonBool(const nlohmann::json& j, const char* k, bool& out) {
        if (j.contains(k) && j[k].is_boolean()) out = j[k].get<bool>();
    }
    static void JsonVec3(const nlohmann::json& j, const char* k, glm::vec3& out) {
        if (!j.contains(k) || !j[k].is_array() || j[k].size() != 3) return;
        for (int i = 0; i < 3; ++i) if (j[k][i].is_number()) out[i] = std::clamp(j[k][i].get<float>(), 0.0f, 2.0f);
    }
    static nlohmann::json Vec3Json(const glm::vec3& v) { return nlohmann::json::array({ v.x, v.y, v.z }); }

    static nlohmann::json GraphicsToJson(DeferredRenderer& r) {
        const RenderQuality& q = r.GetQuality();
        const Atmosphere& a = r.GetAtmosphere();
        nlohmann::json j;
        j["quality"] = {
            {"sunShadows", q.sunShadows}, {"characterShadows", q.characterShadows}, {"shadowFilter", q.shadowFilter},
            {"shadowDistance", q.shadowDistance}, {"ssaoRadius", q.ssaoRadius}, {"ssaoStrength", q.ssaoStrength},
            {"bloom", q.bloom}, {"bloomIntensity", q.bloomIntensity}, {"brightness", q.brightness}, {"gamma", q.gamma},
            {"skinSSS", q.skinSSS}, {"skinDetail", q.skinDetail}, {"gearGrime", q.gearGrime}, {"desaturation", q.desaturation},
            {"mapMaterials", q.mapMaterials}, {"materialRelief", q.materialRelief}, {"mapDecor", q.mapDecor},
            {"renderScale", q.renderScale}, {"shadowResolution", q.shadowResolution}, {"ssaoHalfRes", q.ssaoHalfRes}, {"vsync", q.vsync} };
        j["atmosphere"] = {
            {"zenith", Vec3Json(a.zenith)}, {"horizon", Vec3Json(a.horizon)}, {"ground", Vec3Json(a.ground)},
            {"cloudCover", a.cloudCover}, {"fogDensity", a.fogDensity}, {"fogHeightFalloff", a.fogHeightFalloff},
            {"fogColor", Vec3Json(a.fogColor)}, {"dust", a.dust}, {"saturation", a.saturation}, {"contrast", a.contrast},
            {"vignette", a.vignette}, {"grain", a.grain}, {"shadowTint", Vec3Json(a.shadowTint)}, {"highlightTint", Vec3Json(a.highlightTint)} };
        return j;
    }

    // Tollerante: chiavi mancanti = valore attuale, valori fuori range = limitati
    static void GraphicsFromJson(DeferredRenderer& r, const nlohmann::json& j) {
        if (!j.is_object()) return;
        if (j.contains("quality") && j["quality"].is_object()) {
            const auto& g = j["quality"];
            RenderQuality& q = r.GetQuality();
            JsonBool(g, "sunShadows", q.sunShadows);
            JsonBool(g, "characterShadows", q.characterShadows);
            if (g.contains("shadowFilter") && g["shadowFilter"].is_number_integer()) q.shadowFilter = std::clamp(g["shadowFilter"].get<int>(), 0, 2);
            JsonFloat(g, "shadowDistance", q.shadowDistance, 20.0f, 150.0f);
            JsonFloat(g, "ssaoRadius", q.ssaoRadius, 0.1f, 2.0f);
            JsonFloat(g, "ssaoStrength", q.ssaoStrength, 0.0f, 3.0f);
            JsonBool(g, "bloom", q.bloom);
            JsonFloat(g, "bloomIntensity", q.bloomIntensity, 0.0f, 3.0f);
            JsonFloat(g, "brightness", q.brightness, 0.2f, 3.0f);
            JsonFloat(g, "gamma", q.gamma, 1.6f, 2.8f);
            JsonFloat(g, "skinSSS", q.skinSSS, 0.0f, 1.5f);
            JsonFloat(g, "skinDetail", q.skinDetail, 0.0f, 2.0f);
            JsonFloat(g, "gearGrime", q.gearGrime, 0.0f, 2.0f);
            JsonFloat(g, "desaturation", q.desaturation, 0.0f, 1.0f);
            JsonBool(g, "mapMaterials", q.mapMaterials);
            JsonFloat(g, "materialRelief", q.materialRelief, 0.0f, 2.0f);
            JsonBool(g, "mapDecor", q.mapDecor);
            JsonFloat(g, "renderScale", q.renderScale, 0.5f, 1.0f);
            if (g.contains("shadowResolution") && g["shadowResolution"].is_number_integer()) q.shadowResolution = std::clamp(g["shadowResolution"].get<int>(), 1024, 4096);
            JsonBool(g, "ssaoHalfRes", q.ssaoHalfRes);
            JsonBool(g, "vsync", q.vsync);
        }
        if (j.contains("atmosphere") && j["atmosphere"].is_object()) {
            const auto& g = j["atmosphere"];
            Atmosphere& a = r.GetAtmosphere();
            JsonVec3(g, "zenith", a.zenith);
            JsonVec3(g, "horizon", a.horizon);
            JsonVec3(g, "ground", a.ground);
            JsonFloat(g, "cloudCover", a.cloudCover, 0.0f, 1.0f);
            JsonFloat(g, "fogDensity", a.fogDensity, 0.0f, 0.05f);
            JsonFloat(g, "fogHeightFalloff", a.fogHeightFalloff, 0.005f, 0.5f);
            JsonVec3(g, "fogColor", a.fogColor);
            JsonFloat(g, "dust", a.dust, 0.0f, 1.5f);
            JsonFloat(g, "saturation", a.saturation, 0.0f, 1.5f);
            JsonFloat(g, "contrast", a.contrast, 0.5f, 1.8f);
            JsonFloat(g, "vignette", a.vignette, 0.0f, 1.0f);
            JsonFloat(g, "grain", a.grain, 0.0f, 0.1f);
            JsonVec3(g, "shadowTint", a.shadowTint);
            JsonVec3(g, "highlightTint", a.highlightTint);
        }
    }

    // Preset di qualita': toccano solo cio' che pesa sulle prestazioni, non le scelte di stile (pelle, sporco, atmosfera).
    // 0 Basso (integrate, GTX 1050), 1 Medio, 2 Alto (predefinito), 3 Max (schede di fascia alta)
    struct GraphicsPreset {
        const char* name;
        float renderScale; int shadowRes; int shadowFilter; float shadowDist; bool charShadows;
        bool ssao; bool ssaoHalf; float ssaoRadius; bool fxaa; bool bloom; bool decor; float relief;
    };
    static const GraphicsPreset k_GraphicsPresets[4] = {
        { "Basso", 0.70f, 1024, 0, 35.0f, false, false, true,  0.45f, true, false, false, 0.6f },
        { "Medio", 0.85f, 1024, 1, 45.0f, true,  true,  true,  0.45f, true, true,  true,  1.0f },
        { "Alto",  1.00f, 2048, 1, 60.0f, true,  true,  true,  0.45f, true, true,  true,  1.0f },
        { "Max",   1.00f, 4096, 2, 90.0f, true,  true,  false, 0.55f, true, true,  true,  1.2f },
    };

    static void ApplyGraphicsPreset(DeferredRenderer& r, int level) {
        const GraphicsPreset& p = k_GraphicsPresets[std::clamp(level, 0, 3)];
        RenderQuality& q = r.GetQuality();
        q.sunShadows = true;
        q.renderScale = p.renderScale; q.shadowResolution = p.shadowRes; q.shadowFilter = p.shadowFilter;
        q.shadowDistance = p.shadowDist; q.characterShadows = p.charShadows;
        q.ssaoHalfRes = p.ssaoHalf; q.ssaoRadius = p.ssaoRadius;
        q.bloom = p.bloom; q.mapDecor = p.decor; q.materialRelief = p.relief;
        r.SetSSAOEnabled(p.ssao); r.SetFXAAEnabled(p.fxaa);
    }

    // Quale preset corrisponde alle impostazioni attuali (-1 = personalizzate)
    static int CurrentGraphicsPreset(DeferredRenderer& r) {
        const RenderQuality& q = r.GetQuality();
        auto eq = [](float a, float b) { return std::abs(a - b) < 0.011f; };
        for (int i = 0; i < 4; ++i) {
            const GraphicsPreset& p = k_GraphicsPresets[i];
            if (q.sunShadows && eq(q.renderScale, p.renderScale) && q.shadowResolution == p.shadowRes && q.shadowFilter == p.shadowFilter &&
                eq(q.shadowDistance, p.shadowDist) && q.characterShadows == p.charShadows && r.IsSSAOEnabled() == p.ssao &&
                (!p.ssao || (q.ssaoHalfRes == p.ssaoHalf && eq(q.ssaoRadius, p.ssaoRadius))) && r.IsFXAAEnabled() == p.fxaa &&
                q.bloom == p.bloom && q.mapDecor == p.decor && eq(q.materialRelief, p.relief))
                return i;
        }
        return -1;
    }

    // Livello grafico consigliato per la scheda video (o integrata) di questo PC: nome del modello e memoria video.
    // Pensato per 1080p: sopra la 1440p si scende di un livello (tranne le schede piu' potenti).
    static int DetectGraphicsLevel(std::string& gpuName, std::string& reason) {
        const char* rend = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* vend = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        gpuName = rend ? rend : "sconosciuta";
        std::string n = gpuName + " " + (vend ? vend : "");
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
        // primo numero dopo una parola chiave ("rtx 3060" -> 3060)
        auto numAfter = [&](const char* key) -> int {
            const size_t p = n.find(key);
            if (p == std::string::npos) return -1;
            size_t i = p + std::strlen(key);
            while (i < n.size() && !std::isdigit(static_cast<unsigned char>(n[i]))) { if (n[i] != ' ' && n[i] != '-') return -1; ++i; }
            int v = 0, digits = 0;
            while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i])) && digits < 5) { v = v * 10 + (n[i] - '0'); ++i; ++digits; }
            return digits ? v : -1;
        };
        // memoria video dedicata (MB), se il driver la dice
        int vramMB = 0;
        {
            GLint count = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &count);
            bool nvx = false, ati = false;
            for (GLint i = 0; i < count; ++i) {
                const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
                if (!e) continue;
                if (std::strcmp(e, "GL_NVX_gpu_memory_info") == 0) nvx = true;
                if (std::strcmp(e, "GL_ATI_meminfo") == 0) ati = true;
            }
            if (nvx) { GLint kb = 0; glGetIntegerv(0x9047 /*GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX*/, &kb); vramMB = kb / 1024; }
            else if (ati) { GLint info[4] = {}; glGetIntegerv(0x87FC /*TEXTURE_FREE_MEMORY_ATI*/, info); vramMB = info[0] / 1024; }
            while (glGetError() != GL_NO_ERROR) {}
        }
        int level = -1;
        bool topTier = false;
        if (has("llvmpipe") || has("softpipe") || has("swiftshader") || has("microsoft basic") || has("gdi generic")) { level = 0; reason = "nessuna accelerazione 3D"; }
        else if (has("intel") && has(" arc")) { level = 2; reason = "Intel Arc"; }
        else if (has("intel") || has("radeon(tm) graphics") || has("radeon graphics") || has("radeon vega 3") || has("radeon vega 6") ||
                 has("radeon vega 8") || has("radeon vega 11") || has(" mx1") || has(" mx2") || has(" mx3") || has(" mx4") || has(" mx5")) {
            level = 0; reason = "scheda integrata o per portatili leggeri";
        }
        else if (int v = numAfter("rtx"); v > 0) {
            const int tier = v % 100, gen = v / 1000;   // 3060 -> serie 3, fascia 60; A2000/A4000 professionali: per memoria
            if (v < 1000) level = -1;
            else if (tier >= 80) { level = 3; topTier = true; }
            else if (tier >= 70) level = gen >= 3 ? 3 : 2;
            else if (tier >= 60) level = 2;
            else level = gen >= 3 ? 2 : 1;
            if (level >= 0) reason = "NVIDIA RTX " + std::to_string(v);
        }
        else if (int v = numAfter("gtx"); v > 0) {
            if (v >= 1600) level = (v % 100) >= 60 ? 2 : 1;      // 1660 / 1650
            else if (v >= 1000) level = (v % 100) >= 70 ? 2 : ((v % 100) >= 60 ? 1 : 0);   // 1070-1080 / 1060 / 1050
            else if (v >= 900) level = (v % 100) >= 70 ? 1 : 0;   // 970-980 / 950-960
            else level = 0;
            reason = "NVIDIA GTX " + std::to_string(v);
        }
        else if (int v = numAfter("gt"); v > 0 && has("geforce")) { level = 0; reason = "NVIDIA GT " + std::to_string(v); }
        else if (int v = numAfter("rx"); v > 0) {
            if (v >= 5000) {   // RX 5000-9000: 5500 / 5600-5700, 6400-6500 / 6600 / 6700+, ...
                const int tier = (v % 1000) / 100;
                if (tier >= 7) { level = 3; topTier = tier >= 8; }
                else if (tier >= 6) level = 2;
                else level = 1;
            }
            else if (v >= 470) level = 1;   // RX 470/480/570/580/590
            else level = 0;                 // RX 460/550/560
            reason = "AMD RX " + std::to_string(v);
        }
        else if (has("vega 56") || has("vega 64") || has("radeon vii")) { level = 2; reason = "AMD Vega"; }
        if (level < 0) {   // modello non riconosciuto: si decide dalla memoria video
            if (vramMB <= 0) { level = 1; reason = "modello non riconosciuto"; }
            else { level = vramMB < 2500 ? 0 : (vramMB < 5000 ? 1 : (vramMB < 9000 ? 2 : 3)); reason = "memoria video"; }
        }
        // risoluzione dello schermo: oltre la 1440p i pixel sono piu' del doppio della 1080p
        if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
            if (const GLFWvidmode* vm = glfwGetVideoMode(mon)) {
                if (vm->width * vm->height > 2560 * 1440 + 100000 && level > 0 && !topTier) { --level; reason += ", schermo 4K"; }
            }
        }
        if (vramMB > 0) reason += ", " + std::to_string((vramMB + 512) / 1024) + " GB";
        return std::clamp(level, 0, 3);
    }

    // ===================== AUDIO: VOLUMI E MUSICA =====================
    void GameLayer::ApplyAudioVolumes() {
        if (!m_AudioEngineInitialized) return;
        ma_engine_set_volume(&m_AudioEngine, std::clamp(m_VolumeMaster, 0.0f, 1.0f));
        if (m_SFXGroupInitialized) ma_sound_group_set_volume(&m_SFXGroup, std::clamp(m_VolumeSFX, 0.0f, 1.0f));
        if (m_VoiceGroupInitialized) ma_sound_group_set_volume(&m_VoiceGroup, std::clamp(m_VolumeVoice, 0.0f, 1.0f));
        if (m_MusicGroupInitialized) ma_sound_group_set_volume(&m_MusicGroup, std::clamp(m_VolumeMusic, 0.0f, 1.0f));
    }

    // Brani in GameData/Audio/Music (mp3, wav, flac): basta aggiungerne un file per averlo in gioco
    void GameLayer::InitMusic() {
        m_MusicTracks.clear();
        std::string audioDir = FindValidDir("Audio");
        if (audioDir.empty()) audioDir = "GameData/Audio/";
        const std::filesystem::path musicDir = std::filesystem::path(audioDir) / "Music";
        std::error_code ec;
        if (std::filesystem::is_directory(musicDir, ec)) {
            for (const auto& e : std::filesystem::directory_iterator(musicDir, ec)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".mp3" || ext == ".wav" || ext == ".flac") m_MusicTracks.push_back(e.path().string());
            }
        }
        std::sort(m_MusicTracks.begin(), m_MusicTracks.end());
        if (m_MusicTracks.empty()) { SUN_CORE_INFO("Nessun brano in {0}", musicDir.string()); return; }
        PlayMusicTrack(0);
    }

    void GameLayer::PlayMusicTrack(int idx) {
        if (!m_AudioEngineInitialized) return;
        if (m_MusicLoaded) {
            ma_sound_stop(&m_MusicSound);
            ma_sound_uninit(&m_MusicSound);
            m_MusicLoaded = false;
        }
        if (m_MusicTracks.empty()) return;
        const int n = static_cast<int>(m_MusicTracks.size());
        m_MusicTrackIdx = ((idx % n) + n) % n;
        // in streaming (non tutto in memoria), senza posizione nello spazio
        const ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;
        if (ma_sound_init_from_file(&m_AudioEngine, m_MusicTracks[m_MusicTrackIdx].c_str(), flags,
                m_MusicGroupInitialized ? &m_MusicGroup : nullptr, nullptr, &m_MusicSound) != MA_SUCCESS) {
            SUN_CORE_WARN("Musica: impossibile aprire {0}", m_MusicTracks[m_MusicTrackIdx]);
            return;
        }
        m_MusicLoaded = true;
        ma_sound_set_looping(&m_MusicSound, n == 1 ? MA_TRUE : MA_FALSE);   // un solo brano: a ciclo
        ma_sound_set_volume(&m_MusicSound, m_MusicGroupInitialized ? m_MusicDuck : m_MusicDuck * m_VolumeMusic);
        ma_sound_start(&m_MusicSound);
    }

    void GameLayer::UpdateMusic(float deltaTime) {
        if (!m_MusicLoaded) return;
        // in partita la musica scende sotto spari e voci, nei menu torna piena
        const float target = m_GameState == GameState::Gameplay ? 0.5f : 1.0f;
        m_MusicDuck += (target - m_MusicDuck) * std::min(1.0f, deltaTime * 0.8f);
        ma_sound_set_volume(&m_MusicSound, m_MusicGroupInitialized ? m_MusicDuck : m_MusicDuck * m_VolumeMusic);
        if (m_MusicTracks.size() > 1 && ma_sound_at_end(&m_MusicSound)) PlayMusicTrack(m_MusicTrackIdx + 1);
    }

    // ===================== PROFILO LOCALE =====================
    void GameLayer::LoadProfile() {
        PlayerProfile loaded;
        if (PlayerProfile::Load(loaded)) {
            m_Profile = loaded;
            SUN_CORE_INFO("Profilo caricato: {0}", PlayerProfile::GetProfilePath().string());
        }
        else {
            SUN_CORE_INFO("Nessun profilo valido in {0}: parto dai valori di default",
                PlayerProfile::GetProfilePath().string());
        }

        constexpr int kResCount = static_cast<int>(sizeof(g_Resolutions) / sizeof(g_Resolutions[0]));
        m_SelectedResolutionIdx = std::clamp(m_Profile.resolutionIdx, 0, kResCount - 1);
        m_IsFullscreen = m_Profile.fullscreen;
        m_VolumeSFX = m_Profile.volumeSFX;
        m_VolumeVoice = m_Profile.volumeVoice;
        m_VolumeMusic = m_Profile.volumeMusic;
        m_VolumeMaster = m_Profile.volumeMaster;
        m_Feel.shakeScale = m_Profile.cameraShake;
        m_Feel.hitStopEnabled = m_Profile.hitStop;
        InitParty();   // squadra e lobby: identita' in rete dal profilo, apertura della porta
        m_UseAnimatedPlayer = m_Profile.animatedPlayer;
        m_DeferredRenderer.SetSSAOEnabled(m_Profile.gfxSSAO);
        m_DeferredRenderer.SetFXAAEnabled(m_Profile.gfxFXAA);
        m_DeferredRenderer.GetAtmosphere().enabled = m_Profile.gfxAtmosphere;
        GraphicsFromJson(m_DeferredRenderer, m_Profile.graphics);
        // Primo avvio (o profilo di prima di questa funzione): qualita' grafica scelta in base alla scheda video
        if (!m_Profile.gfxAutoDone && glfwGetCurrentContext()) {
            std::string gpu, why;
            const int level = DetectGraphicsLevel(gpu, why);
            ApplyGraphicsPreset(m_DeferredRenderer, level);
            m_Profile.gfxAutoDone = true;
            m_GfxAutoNotice = "Grafica impostata su " + std::string(k_GraphicsPresets[level].name) + " per " + gpu + " (" + why +
                              "). Puoi cambiarla in Opzioni -> GRAFICA.";
            SUN_CORE_INFO("Qualita' grafica automatica: {0} ({1}: {2})", k_GraphicsPresets[level].name, gpu, why);
        }
        ApplyAudioVolumes();   // se l'audio e' gia' partito (ricarica del profilo)
        if (!m_Profile.selectedMapId.empty()) m_SelectedMapId = m_Profile.selectedMapId;
        m_SelectedEventIndex = std::clamp(m_Profile.selectedEventIndex, 0, 4);
        if (m_SelectedEventIndex == 1) m_SelectedEventIndex = 0;   // Recupero Dati non e' piu' selezionabile
    }

    void GameLayer::LoadRosterFromProfile() {
        if (!m_Profile.roster.is_array() || m_Profile.roster.empty()) return;

        std::vector<TrainedUnit> loaded;
        try {
            for (const auto& u : m_Profile.roster) {
                if (!u.is_object()) continue;
                if (static_cast<int>(loaded.size()) >= MAX_ROSTER_SIZE) break;

                TrainedUnit t;
                t.blueprintId = u.value("blueprintId", std::string());
                // Unita' non piu' presente nei blueprint (file rimosso/rinominato): la salto
                if (!BlueprintManager::Get().GetUnit(t.blueprintId)) {
                    SUN_CORE_WARN("Profilo: unita' '{0}' non trovata nei blueprint, ignorata", t.blueprintId);
                    continue;
                }
                t.customName = u.value("name", t.blueprintId);
                if (u.contains("armorColor") && u["armorColor"].is_array() && u["armorColor"].size() == 4) {
                    t.armorColor = glm::vec4(u["armorColor"][0].get<float>(), u["armorColor"][1].get<float>(),
                                             u["armorColor"][2].get<float>(), u["armorColor"][3].get<float>());
                }
                t.facePreset = u.value("facePreset", 0);
                // Armi non piu' presenti nei blueprint (es. "laspistol_m-g", mai esistita): torno al default
                auto weaponOr = [&](const char* key, const std::string& def) {
                    std::string id = u.value(key, def);
                    return (id.empty() || BlueprintManager::Get().GetWeapon(id)) ? id : def;
                };
                t.primaryWeaponId = weaponOr("primaryWeaponId", t.primaryWeaponId);
                t.secondaryWeaponId = weaponOr("secondaryWeaponId", t.secondaryWeaponId);
                t.meleeWeaponId = weaponOr("meleeWeaponId", t.meleeWeaponId);
                if (u.contains("equipmentSlots") && u["equipmentSlots"].is_array()) {
                    for (size_t i = 0; i < 3 && i < u["equipmentSlots"].size(); ++i)
                        t.equipmentSlots[i] = u["equipmentSlots"][i].get<std::string>();
                }
                if (u.contains("ownedItems") && u["ownedItems"].is_array()) {
                    for (const auto& it : u["ownedItems"]) if (it.is_string()) t.ownedItems.push_back(it.get<std::string>());
                }
                t.armorHeadId = u.value("armorHeadId", t.armorHeadId);
                t.armorTorsoId = u.value("armorTorsoId", t.armorTorsoId);
                t.armorLeftArmId = u.value("armorLeftArmId", t.armorLeftArmId);
                t.armorRightArmId = u.value("armorRightArmId", t.armorRightArmId);
                t.armorLeftLegId = u.value("armorLeftLegId", t.armorLeftLegId);
                t.armorRightLegId = u.value("armorRightLegId", t.armorRightLegId);
                t.exp = std::max(0, u.value("exp", 0));
                // Unita' salvata prima delle dotazioni base (es. gretchin col fucile laser): riceve la sua
                const int savedVersion = u.value("loadoutVersion", 0);
                const auto* tbp = BlueprintManager::Get().GetUnit(t.blueprintId);
                const int unitVersion = tbp ? tbp->loadout_version : 0;
                if (savedVersion < 1 || savedVersion < unitVersion) t.ApplyDefaultLoadout();   // armi, dotazione e armatura base
                else if (savedVersion < 2) t.ApplyDefaultArmor();     // solo le vesti/armatura base (le armi restano)
                if (t.SanitizeLoadout()) SUN_CORE_INFO("Profilo: '{0}' aveva equipaggiamento non piu' permesso, rimessa la dotazione base", t.customName);
                loaded.push_back(t);
            }
        }
        catch (const std::exception& e) {
            SUN_CORE_WARN("Profilo: roster non leggibile ({0}), uso quello di default", e.what());
            return;
        }
        if (loaded.empty()) return;

        m_PlayerRoster = std::move(loaded);
        m_SelectedVaultIndex = 0;
        m_SelectedSquadIndices.clear();
        for (int idx : m_Profile.squadIndices) {
            bool valid = idx >= 0 && idx < static_cast<int>(m_PlayerRoster.size());
            bool dup = std::find(m_SelectedSquadIndices.begin(), m_SelectedSquadIndices.end(), idx) != m_SelectedSquadIndices.end();
            if (valid && !dup) m_SelectedSquadIndices.push_back(idx);
        }
    }

    // Riallinea il profilo allo stato corrente e lo scrive solo se e' cambiato (confronto sul JSON
    // serializzato: non serve ricordarsi di segnare "dirty" in ogni punto della UI).
    void GameLayer::SaveProfileIfChanged() {
        m_Profile.resolutionIdx = m_SelectedResolutionIdx;
        m_Profile.fullscreen = m_IsFullscreen;
        m_Profile.volumeSFX = m_VolumeSFX;
        m_Profile.volumeVoice = m_VolumeVoice;
        m_Profile.volumeMusic = m_VolumeMusic;
        m_Profile.volumeMaster = m_VolumeMaster;
        m_Profile.cameraShake = m_Feel.shakeScale;
        m_Profile.hitStop = m_Feel.hitStopEnabled;
        m_Profile.animatedPlayer = m_UseAnimatedPlayer;
        m_Profile.gfxSSAO = m_DeferredRenderer.IsSSAOEnabled();
        m_Profile.gfxFXAA = m_DeferredRenderer.IsFXAAEnabled();
        m_Profile.gfxAtmosphere = m_DeferredRenderer.GetAtmosphere().enabled;
        m_Profile.graphics = GraphicsToJson(m_DeferredRenderer);
        m_Profile.gfxAutoDone = true;
        m_Profile.squadIndices = m_SelectedSquadIndices;
        m_Profile.selectedMapId = m_SelectedMapId;
        m_Profile.selectedEventIndex = m_SelectedEventIndex;

        m_Profile.roster = nlohmann::json::array();
        for (const auto& t : m_PlayerRoster) {
            nlohmann::json u;
            u["name"] = t.customName;
            u["blueprintId"] = t.blueprintId;
            u["armorColor"] = { t.armorColor.r, t.armorColor.g, t.armorColor.b, t.armorColor.a };
            u["facePreset"] = t.facePreset;
            u["primaryWeaponId"] = t.primaryWeaponId;
            u["secondaryWeaponId"] = t.secondaryWeaponId;
            u["meleeWeaponId"] = t.meleeWeaponId;
            u["equipmentSlots"] = { t.equipmentSlots[0], t.equipmentSlots[1], t.equipmentSlots[2] };
            {
                const auto* tbp = BlueprintManager::Get().GetUnit(t.blueprintId);
                u["loadoutVersion"] = std::max(kLoadoutVersion, tbp ? tbp->loadout_version : 0);
            }
            u["ownedItems"] = t.ownedItems;
            u["armorHeadId"] = t.armorHeadId;
            u["armorTorsoId"] = t.armorTorsoId;
            u["armorLeftArmId"] = t.armorLeftArmId;
            u["armorRightArmId"] = t.armorRightArmId;
            u["armorLeftLegId"] = t.armorLeftLegId;
            u["armorRightLegId"] = t.armorRightLegId;
            u["exp"] = t.exp;
            m_Profile.roster.push_back(std::move(u));
        }

        const std::string text = m_Profile.Serialize();
        if (text == m_LastSavedProfile) return;

        static bool s_WarnedWriteFailure = false;
        if (PlayerProfile::WriteText(text)) {
            m_LastSavedProfile = text;
            s_WarnedWriteFailure = false;
        }
        else if (!s_WarnedWriteFailure) {
            s_WarnedWriteFailure = true;   // niente spam nel log: riprova al prossimo intervallo
            SUN_CORE_WARN("Profilo: impossibile scrivere {0}", PlayerProfile::GetProfilePath().string());
        }
    }

    // Nemico eliminato: +1 al contatore e +1 exp all'unita' (energia e punti studio: vedi esperienza di partita)
    // attualmente in campo e all'arma attiva. La Pietra Nera NON si guadagna in partita.
    void GameLayer::AwardKill() {
        m_KillCount++;
        // Energia e punti studio non si danno piu' qui: arrivano dall'esperienza di partita (XpEvents)

        if (m_CurrentSquadMemberIdx >= 0 && m_CurrentSquadMemberIdx < static_cast<int>(m_ActiveSquad.size())) {
            int rosterIdx = m_ActiveSquad[m_CurrentSquadMemberIdx].rosterIndex;
            if (rosterIdx >= 0 && rosterIdx < static_cast<int>(m_PlayerRoster.size())) {
                m_PlayerRoster[rosterIdx].exp += 1;
            }
        }
        if (auto* wbp = m_Scene.GetRegistry().try_get<WeaponBlueprintId>(m_PlayerEntity)) {
            if (!wbp->id.empty()) m_Profile.weaponExp[wbp->id] += 1;
        }
    }

    std::string GameLayer::WeaponLabel(const std::string& name, const std::string& weaponId) const {
        auto it = m_Profile.weaponExp.find(weaponId);
        int64_t exp = (it != m_Profile.weaponExp.end()) ? it->second : 0;
        return name + "  [EXP " + std::to_string(exp) + "]";
    }

    GameLayer::~GameLayer()
    {
        SaveProfileIfChanged();   // ultimo salvataggio all'uscita

        // FIX #15: shutdown ImGui (ordine inverso all'init: backend renderer, backend platform,
        // contesto). Va fatto col contesto GL ancora valido, prima di renderer e audio.
        // Il controllo sui BackendUserData evita assert se un backend non era stato inizializzato.
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.BackendRendererUserData) ImGui_ImplOpenGL3_Shutdown();
            if (io.BackendPlatformUserData) ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }

        m_NavQuery.Destroy();
        m_DeferredRenderer.Shutdown();
        m_WeaponSystem.ShutdownAudio();
        m_MovementSystem.ShutdownAudio();

        if (g_OrkVoices.startSoundLoaded) {
            ma_sound_uninit(&g_OrkVoices.startWaaaghSound);
            g_OrkVoices.startSoundLoaded = false;
        }
        for (auto& item : g_OrkVoices.voicePool) {
            if (item) ma_sound_uninit(&item->sound);
        }
        g_OrkVoices.voicePool.clear();

        if (m_MusicLoaded) {
            ma_sound_uninit(&m_MusicSound);
            m_MusicLoaded = false;
        }
        m_Feel.Shutdown();   // prima del gruppo SFX a cui sono agganciati i suoi suoni
        if (m_SFXGroupInitialized) {
            ma_sound_group_uninit(&m_SFXGroup);
            m_SFXGroupInitialized = false;
        }
        if (m_VoiceGroupInitialized) {
            ma_sound_group_uninit(&m_VoiceGroup);
            m_VoiceGroupInitialized = false;
        }
        if (m_MusicGroupInitialized) {
            ma_sound_group_uninit(&m_MusicGroup);
            m_MusicGroupInitialized = false;
        }

        if (m_AudioEngineInitialized) {
            ma_engine_uninit(&m_AudioEngine);
            m_AudioEngineInitialized = false;
        }
    }

    // ===================== CAMPO DA TIRO (TESTING LOBBY) =====================
    static const char* HitLocationName(int location) {
        switch (location) {
        case static_cast<int>(HitboxLocation::Head):     return "Testa";
        case static_cast<int>(HitboxLocation::Torso):    return "Torso";
        case static_cast<int>(HitboxLocation::LeftArm):  return "Braccio SX";
        case static_cast<int>(HitboxLocation::RightArm): return "Braccio DX";
        case static_cast<int>(HitboxLocation::LeftLeg):  return "Gamba SX";
        case static_cast<int>(HitboxLocation::RightLeg): return "Gamba DX";
        default: return "Mischia";
        }
    }

    // Colore dei manichini: stessi colori delle unita' giocabili (vedi SwapPlayerUnit)
    static glm::vec4 TrainingDummyColor(const std::string& unitId) {
        if (unitId == "imperium_guardsman_recruit") return glm::vec4(0.65f, 0.50f, 0.35f, 1.0f);
        if (unitId == "imperium_kasrkin") return glm::vec4(0.35f, 0.25f, 0.18f, 1.0f);
        if (unitId == "imperium_ogryn") return glm::vec4(0.60f, 0.45f, 0.30f, 1.0f);
        if (unitId == "spacemarines_neophyte") return glm::vec4(0.92f, 0.72f, 0.68f, 1.0f);
        if (unitId == "spacemarines_battle_brother") return glm::vec4(0.74f, 0.58f, 0.48f, 1.0f);
        if (unitId == "orks_boy") return glm::vec4(0.24f, 0.33f, 0.15f, 1.0f);
        if (unitId == "orks_nob") return glm::vec4(0.17f, 0.25f, 0.11f, 1.0f);
        if (unitId == "orks_yoof") return glm::vec4(0.30f, 0.38f, 0.19f, 1.0f);
        if (unitId == "orks_gretchin") return glm::vec4(0.34f, 0.40f, 0.21f, 1.0f);
        return glm::vec4(0.75f, 0.75f, 0.78f, 1.0f);
    }

    // Crea un manichino per ogni voce "training_dummies" della mappa: scheletro dell'unita' in T-pose,
    // salute/robustezza/armatura dal blueprint (stessa formula di danno dei nemici veri, copertura
    // dell'armatura al 100% per avere numeri ripetibili), collider per non attraversarlo.
    void GameLayer::SpawnTrainingDummies() {
        if (m_CurrentMapData.trainingDummies.empty()) return;
        auto& reg = m_Scene.GetRegistry();
        const std::string skeletonsDir = FindValidDir("Skeletons");
        int spawned = 0;

        for (const auto& spawn : m_CurrentMapData.trainingDummies) {
            float skeletonScale = 1.0f;
            auto skeleton = LoadUnitSkeleton(spawn.unitId, skeletonScale);
            if (!skeleton) {
                SUN_CORE_WARN("Campo da tiro: scheletro '{0}' non trovato, manichino saltato", spawn.unitId);
                continue;
            }
            const auto* bp = BlueprintManager::Get().GetUnit(spawn.unitId);

            const entt::entity e = m_Scene.CreateEntity();
            auto& t = reg.emplace<TransformComponent>(e);
            t.Position = spawn.position;
            t.Rotation = glm::vec3(0.0f, spawn.yawDegrees, 0.0f);
            t.Scale = glm::vec3(1.0f);
            t.AimYaw = spawn.yawDegrees;

            // T-pose: braccia orizzontali, aperte verso l'esterno
            auto& skel = reg.emplace<SkeletonComponent>(e);
            skel.data = *skeleton;
            skel.scale = skeletonScale;
            for (auto& block : skel.data.blocks) {
                if (block.name == "Arm_L") block.jointRotation = glm::vec3(0.0f, 0.0f, -90.0f);
                else if (block.name == "Arm_R") block.jointRotation = glm::vec3(0.0f, 0.0f, 90.0f);
                else block.jointRotation = glm::vec3(0.0f);
            }
            skel.blockFlashTimers.assign(skel.data.blocks.size(), 0.0f);

            reg.emplace<MeshComponent>(e, 0u);
            auto& color = reg.emplace<ColorComponent>(e);
            color.Color = TrainingDummyColor(spawn.unitId);
            color.BaseColor = color.Color;

            const float maxHp = (bp && bp->health > 1.0f) ? bp->health : 100.0f;
            reg.emplace<HealthComponent>(e, maxHp, maxHp);

            auto& stats = reg.emplace<UnitStatsComponent>(e);
            stats.maxHealth = maxHp;
            stats.currentHealth = maxHp;
            if (bp) stats.toughness = bp->toughness;
            const int armorRating = bp ? bp->armor_rating : 0;
            for (ArmorPiece* piece : { &stats.armorSet.head, &stats.armorSet.torso, &stats.armorSet.leftArm,
                                       &stats.armorSet.rightArm, &stats.armorSet.leftLeg, &stats.armorSet.rightLeg }) {
                piece->armorRating = armorRating;
                piece->coverage = 1.0f;                 // numeri ripetibili: l'armatura protegge sempre
                piece->maxDurability = 100000.0f;       // non si consuma durante i test
                piece->currentDurability = piece->maxDurability;
            }

            // Collider (solo per non attraversare il manichino): ingombro della posa in T
            const auto mats = skel.data.ComputeWorldTransforms(glm::vec3(0.0f), glm::vec3(0.0f, spawn.yawDegrees, 0.0f), skel.scale);
            glm::vec3 mn(1e9f), mx(-1e9f);
            for (const auto& M : mats)
                for (int c = 0; c < 8; ++c) {
                    const glm::vec3 p(M * glm::vec4((c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 0.5f : -0.5f, 1.0f));
                    mn = glm::min(mn, p); mx = glm::max(mx, p);
                }
            t.Position.y = spawn.position.y - mn.y;   // piedi a terra (alcuni scheletri scendono sotto l'origine)
            auto& col = reg.emplace<ColliderComponent>(e);
            col.halfExtents = glm::max((mx - mn) * 0.5f, glm::vec3(0.05f));
            col.localCenterOffset = glm::vec3(0.0f, (mn.y + mx.y) * 0.5f, 0.0f);

            auto& dummy = reg.emplace<TrainingDummyComponent>(e);
            dummy.unitId = spawn.unitId;
            dummy.displayName = (bp && !bp->displayName.empty()) ? bp->displayName : spawn.unitId;
            dummy.prevHealth = maxHp;
            ++spawned;
        }
        SUN_CORE_INFO("Campo da tiro: {0} manichini creati", spawned);
    }

    // Registra il danno ricevuto (differenza di salute: vale per armi e mischia) e ripristina la salute
    // dopo una pausa senza colpi.
    void GameLayer::UpdateTrainingDummies(float deltaTime) {
        constexpr float kResetAfter = 3.0f;       // secondi senza colpi -> salute piena e totali azzerati
        constexpr float kLethalShowTime = 1.2f;   // a salute zero: mostra "LETALE" e poi ripristina
        auto& reg = m_Scene.GetRegistry();
        auto view = reg.view<TrainingDummyComponent, HealthComponent>();
        for (auto e : view) {
            auto& d = view.get<TrainingDummyComponent>(e);
            auto& h = view.get<HealthComponent>(e);

            if (h.currentHealth < d.prevHealth - 0.001f) {
                const float dmg = d.prevHealth - h.currentHealth;
                if (d.sinceHit > 0.0f) { d.lastDamage = dmg; ++d.hits; }
                else { d.lastDamage += dmg; }   // piu' colpi nello stesso frame (pallettoni) = un colpo
                d.totalDamage += dmg;
                d.sinceHit = 0.0f;
                if (h.currentHealth <= 0.0f) d.lethal = true;
            }
            else {
                d.sinceHit += deltaTime;
            }

            const bool resetLethal = d.lethal && d.sinceHit >= kLethalShowTime;
            const bool resetIdle = d.sinceHit >= kResetAfter && (d.hits > 0 || h.currentHealth < h.maxHealth);
            if (resetLethal || resetIdle) {
                h.currentHealth = h.maxHealth;
                d.totalDamage = 0.0f;
                d.hits = 0;
                d.lethal = false;
            }
            d.prevHealth = h.currentHealth;
        }
    }

    // Anello di parata sotto i piedi di chi para (Q), per tutta la durata della parata:
    //   windup   -> l'anello si allarga e si accende
    //   attiva   -> giallo pieno (finestra in cui la parata ferma i colpi)
    //   recovery -> si spegne e si stringe
    // Una parata riuscita lo fa lampeggiare quasi bianco. Fatto di piccoli blocchi emissivi piatti, disegnati
    // nel geometry pass: hanno profondita' (le gambe lo coprono) e prendono il bloom.
    void GameLayer::RenderParryRings(bool deferred) {
        auto& reg = m_Scene.GetRegistry();
        auto view = reg.view<MeleeStateComponent, TransformComponent>(entt::exclude<CorpseComponent>);
        constexpr float kWindupEnd = 0.08f;
        constexpr float kActiveEnd = DamageResolver::kParryWindowEnd;
        constexpr float kParryEnd = 0.48f;
        constexpr int kSegments = 40;

        for (auto e : view) {
            const auto& melee = view.get<MeleeStateComponent>(e);
            const bool parrying = melee.currentAttack == MeleeAttackType::Parry
                && melee.currentPhase != MeleePhase::None && melee.currentPhase != MeleePhase::Stunned;
            if (!parrying && melee.parrySuccessTimer <= 0.0f) continue;
            const auto& t = view.get<TransformComponent>(e);

            // Raggio dall'ingombro dell'unita' (collider), con un minimo per le unita' piccole
            // e quota dei piedi dal fondo del collider (Position non e' sempre ai piedi)
            float baseRadius = 0.65f;
            float footY = t.Position.y;
            if (const auto* col = reg.try_get<ColliderComponent>(e)) {
                baseRadius = std::clamp(std::min(col->halfExtents.x * t.Scale.x, col->halfExtents.z * t.Scale.z) + 0.35f, 0.55f, 1.4f);
                footY = t.Position.y + (col->localCenterOffset.y - col->halfExtents.y) * t.Scale.y;
            }

            float grow = 1.0f, intensity = 1.0f;
            const float st = melee.stateTimer;
            if (parrying) {
                if (st < kWindupEnd) { grow = 0.70f + 0.30f * (st / kWindupEnd); intensity = 0.6f + 0.4f * (st / kWindupEnd); }
                else if (st <= kActiveEnd) { grow = 1.0f; intensity = 1.0f; }
                else { const float k = std::clamp((st - kActiveEnd) / (kParryEnd - kActiveEnd), 0.0f, 1.0f); grow = 1.0f - 0.25f * k; intensity = 1.0f - 0.85f * k; }
            }
            else {
                intensity = 0.0f;
            }
            const float flash = std::clamp(melee.parrySuccessTimer / 0.25f, 0.0f, 1.0f);
            intensity = std::max(intensity, flash);
            if (intensity <= 0.02f) continue;

            const glm::vec3 yellow(1.0f, 0.82f, 0.10f);
            const glm::vec3 col3 = glm::mix(yellow, glm::vec3(1.0f, 0.97f, 0.85f), flash * 0.8f);
            const glm::vec4 color(col3 * (0.35f + 0.65f * intensity), 1.0f);
            const float radius = baseRadius * grow * (1.0f + 0.15f * flash);
            const float thickness = 0.035f + 0.045f * intensity + 0.04f * flash;   // larghezza della fascia
            const float y = footY + 0.015f;                                        // appena sopra il terreno

            // Due anelli concentrici: quello esterno pieno, uno interno sottile
            for (int ring = 0; ring < 2; ++ring) {
                const float r = (ring == 0) ? radius : radius * 0.82f;
                const float w = (ring == 0) ? thickness : thickness * 0.45f;
                const float segLen = 2.0f * glm::pi<float>() * r / kSegments * 1.08f;   // leggera sovrapposizione
                for (int i = 0; i < kSegments; ++i) {
                    const float a = 2.0f * glm::pi<float>() * (i + 0.5f) / kSegments;
                    const glm::vec3 p(t.Position.x + std::cos(a) * r, y, t.Position.z + std::sin(a) * r);
                    glm::mat4 m = glm::translate(glm::mat4(1.0f), p);
                    m = glm::rotate(m, -a, glm::vec3(0.0f, 1.0f, 0.0f));   // lato lungo tangente al cerchio
                    m = glm::scale(m, glm::vec3(w, 0.012f, segLen));
                    if (deferred) m_DeferredRenderer.DrawCube(m, color, 0.4f, 0.0f, intensity * (ring == 0 ? 1.0f : 0.7f));
                    else Renderer::DrawCube(m, color);
                }
            }
        }
    }

    // Cerchio verde a terra attorno allo zaino di cibo (raggio di cura), che si assottiglia col tempo
    void GameLayer::RenderHealZones(bool deferred) {
        auto& reg = m_Scene.GetRegistry();
        constexpr int kSegments = 56;
        for (auto e : reg.view<HealZoneComponent, TransformComponent>()) {
            const auto& z = reg.get<HealZoneComponent>(e);
            const auto& t = reg.get<TransformComponent>(e);
            const float k = z.duration > 0.0f ? std::clamp(z.timeLeft / z.duration, 0.0f, 1.0f) : 0.0f;
            const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(glfwGetTime()) * 4.0f);
            const glm::vec4 color(0.25f * pulse, 0.95f * pulse, 0.35f * pulse, 1.0f);
            const float w = 0.05f + 0.06f * k;
            const float segLen = 2.0f * glm::pi<float>() * z.radius / kSegments * 1.08f;
            for (int i = 0; i < kSegments; ++i) {
                const float a = 2.0f * glm::pi<float>() * (i + 0.5f) / kSegments;
                const glm::vec3 p(t.Position.x + std::cos(a) * z.radius, t.Position.y + 0.02f, t.Position.z + std::sin(a) * z.radius);
                glm::mat4 m = glm::translate(glm::mat4(1.0f), p);
                m = glm::rotate(m, -a, glm::vec3(0.0f, 1.0f, 0.0f));
                m = glm::scale(m, glm::vec3(w, 0.012f, segLen));
                if (deferred) m_DeferredRenderer.DrawCube(m, color, 0.4f, 0.0f, 0.6f * k + 0.2f);
                else Renderer::DrawCube(m, color);
            }
        }
    }

    // ===================== CORPI ANIMATI DI TUTTE LE UNITA' =====================
    // Bot, orki dell'orda e manichini del campo da tiro usano il corpo 3D animato della loro unita' (come il
    // giocatore); i cubi dello scheletro restano come hitbox ma non si disegnano (HideProceduralVisualTag).
    // Ogni unita' ha il suo controller con le clip condivise del corpo, quindi cammina, mira e spara per conto suo.
    // F2 (cubi) vale per tutti. Alla morte il corpo animato sparisce e resta il ragdoll a cubi.
    static std::string AnimUnitIdOf(entt::registry& reg, entt::entity e) {
        if (const auto* uid = reg.try_get<UnitBlueprintId>(e)) return uid->id;
        if (const auto* d = reg.try_get<TrainingDummyComponent>(e)) return d->unitId;
        if (reg.all_of<HordeAgentComponent>(e)) return "orks_boy";
        return std::string();
    }

    static float FootYOf(entt::registry& reg, entt::entity e) {
        const auto& t = reg.get<TransformComponent>(e);
        if (const auto* c = reg.try_get<ColliderComponent>(e)) return t.Position.y + (c->localCenterOffset.y - c->halfExtents.y) * t.Scale.y;
        return t.Position.y;
    }

    // Parte del corpo (blocco del ragdoll) a cui appartiene un osso; stringa vuota = come l'osso padre
    static const char* CorpseBlockOfBone(const std::string& name) {
        const size_t cut = name.find_last_of(":|");
        const std::string b = cut == std::string::npos ? name : name.substr(cut + 1);
        auto starts = [&](const char* p) { return b.rfind(p, 0) == 0; };
        if (b == "Hips" || starts("Spine") || b == "LeftShoulder" || b == "RightShoulder") return "Torso";
        if (b == "Neck" || starts("Head")) return "Head";
        if (starts("LeftArm") || starts("LeftForeArm") || starts("LeftHand")) return "Arm_L";
        if (starts("RightArm") || starts("RightForeArm") || starts("RightHand")) return "Arm_R";
        if (starts("LeftUpLeg") || starts("LeftLeg") || starts("LeftFoot") || starts("LeftToe")) return "Leg_L";
        if (starts("RightUpLeg") || starts("RightLeg") || starts("RightFoot") || starts("RightToe")) return "Leg_R";
        return "";
    }

    // Il corpo animato di un'unita' appena morta diventa un "morto con le ossa": si ferma sulla posa che aveva e
    // da li' ogni parte segue il suo blocco del ragdoll. visual passa di proprieta' (non va piu' distrutto dal chiamante).
    bool GameLayer::MakeSkinnedCorpse(entt::entity unit, entt::entity visual) {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(unit) || !reg.valid(visual) || m_SkinnedCorpses.count(unit)) return false;
        if (!reg.all_of<SkeletonComponent>(unit) || !reg.all_of<SkinnedModelInstanceComponent, TransformComponent>(visual)) return false;
        auto& sk = reg.get<SkinnedModelInstanceComponent>(visual);
        if (!sk.sourceModel || !sk.boneMatrices || sk.boneMatrices->empty()) return false;
        const auto& blocks = reg.get<SkeletonComponent>(unit).data.blocks;
        if (blocks.empty()) return false;

        auto corpse = std::make_unique<SkinnedCorpse>();
        corpse->visual = visual;
        corpse->bones = *sk.boneMatrices;
        const auto& vt = reg.get<TransformComponent>(visual);
        const glm::mat4 model = vt.GetTransform();
        corpse->basePos = vt.Position;
        corpse->frozenWorld.resize(corpse->bones.size());
        for (size_t i = 0; i < corpse->bones.size(); ++i) corpse->frozenWorld[i] = model * corpse->bones[i];

        auto blockIndex = [&](const std::string& n) { for (size_t i = 0; i < blocks.size(); ++i) if (blocks[i].name == n) return static_cast<int>(i); return -1; };
        corpse->torso = (std::max)(0, blockIndex("Torso"));
        corpse->boneBlock.assign(corpse->bones.size(), corpse->torso);
        // dall'alto dello scheletro in giu': le ossa senza una parte loro (dita, mascella, orecchie...) seguono il padre
        const auto& boneMap = sk.sourceModel->boneInfoMap;
        std::function<void(const AssimpNodeData&, int)> walk = [&](const AssimpNodeData& node, int current) {
            const char* part = CorpseBlockOfBone(node.name);
            if (part[0]) { const int idx = blockIndex(part); current = idx >= 0 ? idx : corpse->torso; }
            auto it = boneMap.find(node.name);
            if (it != boneMap.end() && it->second.id >= 0 && it->second.id < static_cast<int>(corpse->boneBlock.size())) corpse->boneBlock[it->second.id] = current;
            for (const auto& c : node.children) walk(c, current);
        };
        walk(sk.sourceModel->rootNode, corpse->torso);

        sk.boneMatrices = &corpse->bones;
        if (!reg.all_of<HideProceduralVisualTag>(unit)) reg.emplace<HideProceduralVisualTag>(unit);   // i cubi del ragdoll non si disegnano
        m_SkinnedCorpses[unit] = std::move(corpse);
        return true;
    }

    void GameLayer::ClearSkinnedCorpses() {
        auto& reg = m_Scene.GetRegistry();
        for (auto& [e, c] : m_SkinnedCorpses) {
            if (reg.valid(c->visual)) m_Scene.DestroyEntity(c->visual);
            if (reg.valid(e) && reg.all_of<HideProceduralVisualTag>(e)) reg.remove<HideProceduralVisualTag>(e);
        }
        m_SkinnedCorpses.clear();
    }

    void GameLayer::UpdateSkinnedCorpses() {
        auto& reg = m_Scene.GetRegistry();
        const bool enabled = m_UseAnimatedPlayer && m_GameState == GameState::Gameplay;
        for (auto it = m_SkinnedCorpses.begin(); it != m_SkinnedCorpses.end();) {
            const entt::entity e = it->first;
            SkinnedCorpse& c = *it->second;
            const bool alive = enabled && reg.valid(e) && reg.valid(c.visual) && reg.any_of<CorpseComponent, RagdollComponent>(e)
                && reg.all_of<SkinnedModelInstanceComponent, TransformComponent>(c.visual);
            if (!alive) {   // cadavere sparito, unita' rientrata in gioco, o modalita' a cubi
                if (reg.valid(c.visual)) m_Scene.DestroyEntity(c.visual);
                if (reg.valid(e) && reg.all_of<HideProceduralVisualTag>(e)) reg.remove<HideProceduralVisualTag>(e);
                it = m_SkinnedCorpses.erase(it);
                continue;
            }
            ++it;
            reg.get<SkinnedModelInstanceComponent>(c.visual).boneMatrices = &c.bones;
            const auto* rd = reg.try_get<RagdollComponent>(e);
            if (!rd || rd->worldTransforms.empty()) continue;   // ragdoll non ancora partito: resta la posa di morte
            const auto& W = rd->worldTransforms;
            bool valid = true;
            for (const auto& m : W) {
                const float det = glm::determinant(glm::mat3(m));
                if (!(std::abs(det) > 1e-12f) || std::isnan(m[3].x) || std::isnan(m[3].y) || std::isnan(m[3].z)) { valid = false; break; }
            }
            if (!valid) continue;
            if (!c.started || c.invStart.size() != W.size()) {
                c.invStart.resize(W.size());
                for (size_t i = 0; i < W.size(); ++i) c.invStart[i] = glm::inverse(W[i]);
                c.startTorso = glm::vec3(W[(std::min)(static_cast<size_t>(c.torso), W.size() - 1)][3]);
                c.started = true;
            }
            const size_t torso = (std::min)(static_cast<size_t>(c.torso), W.size() - 1);
            // l'entita' visiva segue il busto (serve al taglio di cio' che e' fuori vista e alle ombre)
            auto& vt = reg.get<TransformComponent>(c.visual);
            vt.Position = c.basePos + (glm::vec3(W[torso][3]) - c.startTorso);
            const glm::mat4 invModel = glm::inverse(vt.GetTransform());
            std::vector<glm::mat4> delta(W.size());
            for (size_t i = 0; i < W.size(); ++i) delta[i] = invModel * W[i] * c.invStart[i];
            for (size_t b = 0; b < c.bones.size(); ++b) {
                const size_t blk = (std::min)(static_cast<size_t>((std::max)(0, c.boneBlock[b])), W.size() - 1);
                c.bones[b] = delta[blk] * c.frozenWorld[b];
            }
        }
    }

    void GameLayer::ClearUnitAnimations() {
        auto& reg = m_Scene.GetRegistry();
        ClearSkinnedCorpses();
        for (auto& [e, inst] : m_UnitAnims) {
            if (reg.valid(inst.visual)) m_Scene.DestroyEntity(inst.visual);
            for (auto p : inst.skinParts) if (reg.valid(p)) m_Scene.DestroyEntity(p);
            if (reg.valid(e) && reg.all_of<HideProceduralVisualTag>(e)) reg.remove<HideProceduralVisualTag>(e);
        }
        m_UnitAnims.clear();
        m_RaceMotion.clear();
    }

    void GameLayer::UpdateUnitAnimations(float deltaTime) {
        auto& reg = m_Scene.GetRegistry();
        const bool enabled = m_UseAnimatedPlayer && m_GameState == GameState::Gameplay;
        // Via le istanze di unita' sparite, morte (ragdoll) o se si vogliono i cubi
        std::vector<entt::entity> drop;
        for (auto& [e, inst] : m_UnitAnims) {
            const bool ok = enabled && reg.valid(e) && reg.all_of<TransformComponent, SkeletonComponent>(e)
                && !reg.any_of<CorpseComponent, RagdollComponent>(e);
            if (!ok) drop.push_back(e);
        }
        for (auto e : drop) {
            auto& inst = m_UnitAnims[e];
            // Morto: il corpo con le ossa non sparisce, passa al ragdoll (prima tornavano i cubi)
            const bool died = enabled && reg.valid(e) && reg.any_of<CorpseComponent, RagdollComponent>(e);
            if (died && MakeSkinnedCorpse(e, inst.visual)) inst.visual = entt::null;
            else {
                if (reg.valid(inst.visual)) m_Scene.DestroyEntity(inst.visual);
                if (reg.valid(e) && reg.all_of<HideProceduralVisualTag>(e)) reg.remove<HideProceduralVisualTag>(e);
            }
            for (auto p : inst.skinParts) if (reg.valid(p)) m_Scene.DestroyEntity(p);
            m_UnitAnims.erase(e);
            m_RaceMotion.erase(e);
        }
        UpdateSkinnedCorpses();
        if (!enabled) return;

        static const std::vector<glm::mat4> s_BindPose(ANIMATOR_MAX_BONES, glm::mat4(1.0f));   // posa in T del file
        std::vector<entt::entity> units;
        for (auto e : reg.view<SkeletonComponent, TransformComponent>(entt::exclude<PlayerTag, CorpseComponent, RagdollComponent, SquigComponent>)) units.push_back(e);
        for (auto e : units) {
            const std::string unitId = AnimUnitIdOf(reg, e);
            if (unitId.empty()) continue;
            const AnimatedBodyKind kind = BodyKindForUnit(unitId);
            if (!AnimatedBodyReady(kind)) continue;
            auto it = m_UnitAnims.find(e);
            if (it != m_UnitAnims.end() && it->second.kind != kind) {   // cambio di corpo: si ricrea
                if (reg.valid(it->second.visual)) m_Scene.DestroyEntity(it->second.visual);
                for (auto p : it->second.skinParts) if (reg.valid(p)) m_Scene.DestroyEntity(p);
                m_UnitAnims.erase(it);
                it = m_UnitAnims.end();
            }
            if (it == m_UnitAnims.end()) {
                UnitAnimInstance inst;
                inst.kind = kind;
                inst.bindPose = reg.all_of<TrainingDummyComponent>(e);
                inst.ctrl = std::make_unique<RifleAnimationController>();
                inst.ctrl->ShareClipsFrom(AnimatedBodyController(kind));
                inst.poseTime = Random::Range(0.0f, 3.0f);
                inst.visual = m_Scene.CreateEntity();
                reg.emplace<TransformComponent>(inst.visual);
                reg.emplace<ColorComponent>(inst.visual);
                auto& sk = reg.emplace<SkinnedModelInstanceComponent>(inst.visual);
                sk.sourceModel = &AnimatedBodyModel(kind);
                sk.boneMatrices = inst.bindPose ? &s_BindPose : &inst.ctrl->GetFinalBoneMatrices();
                it = m_UnitAnims.emplace(e, std::move(inst)).first;
            }
            UnitAnimInstance& inst = it->second;
            if (!reg.all_of<HideProceduralVisualTag>(e)) reg.emplace<HideProceduralVisualTag>(e);
            const auto& t = reg.get<TransformComponent>(e);

            // Comandi dell'animazione dedotti da come si muove l'unita'
            if (!inst.bindPose && deltaTime > 0.0f) {
                RifleAnimInput in;
                glm::vec3 v(0.0f);
                if (inst.hasLast) v = (t.Position - inst.lastPos) / deltaTime;
                inst.lastPos = t.Position; inst.hasLast = true;
                v.y = 0.0f;
                float speed = glm::length(v);
                if (speed > 15.0f) speed = 0.0f, v = glm::vec3(0.0f);   // teletrasporto (rientro): niente corsa
                const float yr = glm::radians(t.Rotation.y);
                const glm::vec3 fwd(-std::sin(yr), 0.0f, -std::cos(yr)), right(std::cos(yr), 0.0f, -std::sin(yr));
                const float f = glm::dot(v, fwd), sd = glm::dot(v, right);
                in.isMoving = speed > 0.35f;
                if (in.isMoving) {
                    if (std::abs(f) >= std::abs(sd)) { in.forward = f > 0.0f; in.backward = f < 0.0f; }
                    else { in.right = sd > 0.0f; in.left = sd < 0.0f; }
                }
                if (const auto* st = reg.try_get<UnitStatsComponent>(e)) in.sprinting = st->sprinting || speed > std::max(1.0f, st->baseSpeed) * 1.25f;
                in.groundSpeed = speed;
                if (const auto* brain = reg.try_get<BotBrainComponent>(e)) in.aiming = brain->isAiming;
                if (const auto* ws = reg.try_get<WeaponStateComponent>(e)) {
                    in.shotFired = inst.lastAmmo >= 0 && ws->currentAmmo < inst.lastAmmo;
                    in.firing = in.shotFired;
                    inst.lastAmmo = ws->currentAmmo;
                }
                if (const auto* cr = reg.try_get<CrouchComponent>(e)) in.crouching = cr->amount > 0.5f;
                inst.poseTime += deltaTime;
                BuildUnitPose(e, inst.pose, *inst.ctrl, inst.poseTime, inst.climb, deltaTime);
                inst.ctrl->SetProceduralPose(&inst.pose);
                inst.ctrl->Update(deltaTime, in);
                inst.poseFresh = true;
            }

            // Entita' visiva: ai piedi dell'unita', girata come il corpo
            float unitScale = 1.0f;
            if (const auto* ubp = BlueprintManager::Get().GetUnit(unitId); ubp && ubp->model_scale > 0.05f) unitScale = ubp->model_scale;
            auto& vt = reg.get<TransformComponent>(inst.visual);
            vt.Position = glm::vec3(t.Position.x, FootYOf(reg, e), t.Position.z);
            vt.Rotation = glm::vec3(0.0f, t.Rotation.y + m_AnimatedYawOffset, 0.0f);
            vt.Scale = glm::vec3(m_AnimatedModelScale * unitScale * AnimatedBodyModel(inst.kind).unitScale);
            auto& sk = reg.get<SkinnedModelInstanceComponent>(inst.visual);
            if (const auto* uc = reg.try_get<ColorComponent>(e)) sk.albedo = uc->BaseColor;
            ApplySkinStyle(sk, static_cast<int>(inst.kind));
            // Vestiti e armature (stesse ossa del corpo)
            sk.garments.clear();
            const auto* skin = reg.try_get<SkinComponent>(e);
            if (skin && sk.sourceModel) {
                for (const auto& d : skin->garments) {
                    GarmentDraw draw;
                    if (m_Garments.Get(*sk.sourceModel, d, draw)) sk.garments.push_back(draw);
                }
            }
            // Pezzi della skin agganciati alle ossa (elmetti, zaini, mantelli...)
            std::vector<const SkinPart*> parts;
            const bool authoredBody = sk.sourceModel && sk.sourceModel->authored;   // modello d'autore: niente pezzi a cubi
            if (skin && !inst.bindPose && !authoredBody) for (const auto& p : skin->parts) if (!p.bone.empty()) parts.push_back(&p);
            while (inst.skinParts.size() > parts.size()) { if (reg.valid(inst.skinParts.back())) m_Scene.DestroyEntity(inst.skinParts.back()); inst.skinParts.pop_back(); }
            while (inst.skinParts.size() < parts.size()) {
                const entt::entity pe = m_Scene.CreateEntity();
                reg.emplace<TransformComponent>(pe);
                reg.emplace<MeshComponent>(pe, 0u);
                reg.emplace<ColorComponent>(pe);
                inst.skinParts.push_back(pe);
            }
            const glm::mat4 yawRot = glm::rotate(glm::mat4(1.0f), glm::radians(t.Rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
            for (size_t i = 0; i < parts.size(); ++i) {
                auto& pt = reg.get<TransformComponent>(inst.skinParts[i]);
                glm::mat4 bone(1.0f);
                if (!inst.ctrl->GetBoneModelTransform(parts[i]->bone, bone)) { pt.Scale = glm::vec3(0.0f); continue; }
                const glm::vec3 bonePos = glm::vec3((vt.GetTransform() * bone)[3]);
                pt.Position = bonePos + glm::vec3(yawRot * glm::vec4(parts[i]->boneOffset * unitScale, 0.0f));
                pt.Rotation = glm::vec3(0.0f, t.Rotation.y, 0.0f);
                pt.Scale = parts[i]->size * unitScale;
                auto& c = reg.get<ColorComponent>(inst.skinParts[i]);
                c.Color = parts[i]->color; c.BaseColor = c.Color;
            }
        }
    }

    // Arma del bot nella mano destra del suo corpo animato (a due mani: fra le due mani), puntata dove mira
    bool GameLayer::PlaceBotWeaponOnHand(entt::entity bot, TransformComponent& wt, const BlockModel* model) {
        auto it = m_UnitAnims.find(bot);
        auto& reg = m_Scene.GetRegistry();
        if (it == m_UnitAnims.end() || it->second.bindPose || !reg.valid(it->second.visual)) return false;
        auto& inst = it->second;
        glm::mat4 boneR(1.0f), boneL(1.0f);
        if (!inst.ctrl->GetBoneModelTransform("mixamorig1:RightHand", boneR)) return false;
        const auto& vt = reg.get<TransformComponent>(inst.visual);
        const auto& t = reg.get<TransformComponent>(bot);
        const glm::vec3 handR = glm::vec3((vt.GetTransform() * boneR)[3]);
        glm::mat4 aimRot = glm::rotate(glm::mat4(1.0f), glm::radians(t.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
        aimRot = glm::rotate(aimRot, glm::radians(t.AimPitch * m_WeaponPitchFactor), glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 aimDir = glm::normalize(glm::vec3(aimRot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        glm::vec3 anchorWorld = handR, anchorLocal(0.0f);
        if (model && model->hasGripRight) anchorLocal = model->gripRight;
        if (model && model->hasGripRight && model->hasGripLeft && inst.ctrl->GetBoneModelTransform("mixamorig1:LeftHand", boneL)) {
            anchorWorld = (handR + glm::vec3((vt.GetTransform() * boneL)[3])) * 0.5f;
            anchorLocal = (model->gripRight + model->gripLeft) * 0.5f;
        }
        const glm::vec3 zAxis = -aimDir;
        glm::vec3 xAxis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), zAxis);
        if (glm::length(xAxis) < 1e-3f) xAxis = glm::vec3(1.0f, 0.0f, 0.0f);
        xAxis = glm::normalize(xAxis);
        const glm::vec3 yAxis = glm::cross(zAxis, xAxis);
        glm::mat4 basis(1.0f);
        basis[0] = glm::vec4(xAxis, 0.0f); basis[1] = glm::vec4(yAxis, 0.0f); basis[2] = glm::vec4(zAxis, 0.0f);
        if (!model) anchorLocal = glm::vec3(0.0f, 0.0f, 0.30f);   // cubo: impugnato a un'estremita'
        wt.Position = anchorWorld - glm::vec3(basis * glm::vec4(anchorLocal, 0.0f));
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
        glm::extractEulerAngleYXZ(basis, yaw, pitch, roll);
        wt.Rotation = glm::degrees(glm::vec3(pitch, yaw, roll));
        // Mani sulle impugnature vere dell'arma (una volta per posa: in pausa la posa resta quella con l'IK)
        if (inst.poseFresh && model && model->hasGripRight) {
            const glm::vec3 gR = wt.Position + glm::vec3(basis * glm::vec4(model->gripRight, 0.0f));
            const glm::vec3 gL = wt.Position + glm::vec3(basis * glm::vec4(model->gripLeft, 0.0f));
            ApplyHandIK(*inst.ctrl, inst.pose, vt, &gR, model->hasGripLeft ? &gL : nullptr);
        }
        inst.poseFresh = false;
        return true;
    }

    // IK delle braccia. L'arma e' gia' al suo posto (messa fra le mani della clip); qui le braccia si piegano
    // perche' il palmo destro cada sull'impugnatura e il sinistro sul guardamano. Le rotazioni si sommano alla
    // posa del frame e le ossa si ricalcolano. Se una mano e' lontana dalla sua presa (ricarica, lancio di
    // granata, colpo corpo a corpo: la mano sta facendo altro) l'IK sfuma a zero per quella mano.
    void GameLayer::ApplyHandIK(RifleAnimationController& ctrl, ProceduralPose& pose, const TransformComponent& visualT,
        const glm::vec3* gripRightWorld, const glm::vec3* gripLeftWorld) {
        if (!gripRightWorld && !gripLeftWorld) return;
        const glm::mat4 toWorld = visualT.GetTransform();
        const glm::mat4 toMesh = glm::inverse(toWorld);
        bool changed = false;
        auto arm = [&](const std::string& side, const glm::vec3& gripWorld) {
            glm::mat4 tS(1.0f), tE(1.0f), tH(1.0f);
            if (!ctrl.GetBoneModelTransform("mixamorig1:" + side + "Arm", tS) ||
                !ctrl.GetBoneModelTransform("mixamorig1:" + side + "ForeArm", tE) ||
                !ctrl.GetBoneModelTransform("mixamorig1:" + side + "Hand", tH)) return;
            const glm::vec3 S(tS[3]), E(tE[3]), H(tH[3]);
            // Palmo stimato: un terzo di avambraccio oltre il polso (come in PoseLib::ArmIK)
            const glm::vec3 palmWorld = glm::vec3(toWorld * glm::vec4(H + (H - E) * 0.3f, 1.0f));
            const float d = glm::distance(palmWorld, gripWorld);
            const float w = 1.0f - std::clamp((d - 0.22f) / 0.15f, 0.0f, 1.0f);
            if (w <= 0.0f) return;
            PoseLib::ArmIK(pose, side, S, E, H, glm::vec3(toMesh * glm::vec4(gripWorld, 1.0f)), w);
            changed = true;
        };
        if (gripRightWorld) arm("Right", *gripRightWorld);
        if (gripLeftWorld) arm("Left", *gripLeftWorld);
        if (changed) ctrl.Reevaluate();
    }

    // Campo da tiro: le hitbox dei manichini (i cubi dello scheletro) disegnate trasparenti sopra il corpo 3D,
    // con gli spigoli, per vedere subito se sono troppo grandi o troppo piccole rispetto al modello.
    // Testa rossa, busto azzurro, braccia e gambe verdi. Si vedono attraverso tutto (disegno dopo la scena).
    void GameLayer::RenderHitboxOverlay() {
        auto& reg = m_Scene.GetRegistry();
        auto view = reg.view<TrainingDummyComponent, SkeletonComponent, TransformComponent>();
        if (view.begin() == view.end()) return;
        static const glm::vec3 corners[8] = {
            { -0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, -0.5f }, { -0.5f, 0.5f, -0.5f },
            { -0.5f, -0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, { -0.5f, 0.5f, 0.5f } };
        static const int edges[12][2] = { {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7} };
        for (auto e : view) {
            const auto& skel = view.get<SkeletonComponent>(e);
            const auto& t = view.get<TransformComponent>(e);
            const auto mats = skel.data.ComputeWorldTransforms(t.Position, t.Rotation, skel.scale);
            for (size_t i = 0; i < mats.size() && i < skel.data.blocks.size(); ++i) {
                const std::string& n = skel.data.blocks[i].name;
                glm::vec3 c = n == "Head" ? glm::vec3(1.0f, 0.35f, 0.3f) : (n == "Torso" ? glm::vec3(0.3f, 0.8f, 1.0f) : glm::vec3(0.4f, 1.0f, 0.45f));
                Renderer::DrawCube(mats[i], glm::vec4(c, 0.16f));
                for (const auto& ed : edges)
                    Renderer::DrawLine(glm::vec3(mats[i] * glm::vec4(corners[ed[0]], 1.0f)), glm::vec3(mats[i] * glm::vec4(corners[ed[1]], 1.0f)), glm::vec4(c, 0.95f));
            }
        }
    }

    // Mimetismo: l'unita' ha il CloakComponent "vero"; le entita' solo visive che la rappresentano (arma in mano,
    // modello animato del giocatore) ne ricevono una copia, cosi' anche loro diventano trasparenti.
    void GameLayer::SyncCloakVisuals() {
        auto& reg = m_Scene.GetRegistry();
        auto mirror = [&](entt::entity visual, float amount) {
            if (visual == entt::null || !reg.valid(visual)) return;
            if (amount > 0.001f) { auto& c = reg.get_or_emplace<CloakComponent>(visual); c.amount = amount; c.owner = false; }
            else if (auto* c = reg.try_get<CloakComponent>(visual); c && !c->owner) reg.remove<CloakComponent>(visual);
        };
        float playerAmount = 0.0f;
        if (reg.valid(m_PlayerEntity)) if (const auto* c = reg.try_get<CloakComponent>(m_PlayerEntity)) playerAmount = c->amount;
        mirror(m_AnimatedPlayerEntity, playerAmount);
        mirror(m_WeaponEntity, playerAmount);
        mirror(m_LeftWeaponEntity, playerAmount);
        for (auto e : reg.view<BotBrainComponent>()) {
            const auto* c = reg.try_get<CloakComponent>(e);
            mirror(reg.get<BotBrainComponent>(e).weaponEntity, c ? c->amount : 0.0f);
        }
        for (auto& [e, inst] : m_UnitAnims) {
            const auto* c = reg.valid(e) ? reg.try_get<CloakComponent>(e) : nullptr;
            mirror(inst.visual, c ? c->amount : 0.0f);
            for (auto p : inst.skinParts) mirror(p, c ? c->amount : 0.0f);
        }
    }

    // Auspex in mano: sonar in basso a sinistra, nemici entro la portata come pallini rossi (in alto = davanti)
    void GameLayer::RenderAuspex() {
        auto& reg = m_Scene.GetRegistry();
        const auto* inv = reg.try_get<InventoryComponent>(m_PlayerEntity);
        if (!inv) return;
        const auto* held = BlueprintManager::Get().GetWeapon(inv->GetActiveWeaponId());
        if (!held || held->equipment_type != "auspex") return;
        const float range = std::max(5.0f, held->equipment_range);
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float R = 110.0f;
        const ImVec2 c(30.0f + R, io.DisplaySize.y - 30.0f - R);
        const float t = static_cast<float>(ImGui::GetTime());
        dl->AddCircleFilled(c, R, IM_COL32(8, 30, 14, 200), 64);
        for (int i = 1; i <= 3; ++i) dl->AddCircle(c, R * i / 3.0f, IM_COL32(60, 200, 90, 110), 64, 1.0f);
        dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), IM_COL32(60, 200, 90, 80));
        dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), IM_COL32(60, 200, 90, 80));
        // Lancetta che gira (2 s per giro) con una scia
        const float sweep = std::fmod(t * 3.14159f, 6.28318f);
        for (int k = 0; k < 12; ++k) {
            const float a = sweep - k * 0.05f;
            dl->AddLine(c, ImVec2(c.x + std::sin(a) * R, c.y - std::cos(a) * R), IM_COL32(90, 255, 120, 150 - k * 12), 2.0f);
        }
        const auto& pt = reg.get<TransformComponent>(m_PlayerEntity);
        const float yr = glm::radians(pt.AimYaw);
        const glm::vec2 fwd(-std::sin(yr), -std::cos(yr)), right(std::cos(yr), -std::sin(yr));
        const auto* myTeam = reg.try_get<TeamComponent>(m_PlayerEntity);
        auto view = reg.view<TransformComponent, HealthComponent, UnitStatsComponent>(entt::exclude<CorpseComponent, PlayerTag>);
        for (auto o : view) {
            if (view.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
            bool enemy;
            if (const auto* ot = reg.try_get<TeamComponent>(o)) enemy = !myTeam || ot->team != myTeam->team;
            else enemy = reg.any_of<HordeAgentComponent, TrainingDummyComponent>(o);
            if (!enemy) continue;
            const glm::vec3 op = view.get<TransformComponent>(o).Position;
            const glm::vec2 rel(op.x - pt.Position.x, op.z - pt.Position.z);
            if (glm::length(rel) > range) continue;
            const float x = glm::dot(rel, right) / range * R, y = glm::dot(rel, fwd) / range * R;
            // il pallino si accende quando ci passa la lancetta e poi sfuma
            float ang = std::atan2(x, y); if (ang < 0.0f) ang += 6.28318f;
            float since = std::fmod(sweep - ang + 6.28318f, 6.28318f) / 6.28318f;   // 0 = appena passata
            const int alpha = static_cast<int>(255 * (1.0f - 0.7f * since));
            dl->AddCircleFilled(ImVec2(c.x + x, c.y - y), 5.0f, IM_COL32(255, 50, 40, alpha), 12);
        }
        dl->AddTriangleFilled(ImVec2(c.x, c.y - 7.0f), ImVec2(c.x - 5.0f, c.y + 5.0f), ImVec2(c.x + 5.0f, c.y + 5.0f), IM_COL32(120, 255, 140, 255));
        dl->AddCircle(c, R, IM_COL32(90, 255, 120, 220), 64, 2.0f);
        char txt[48]; std::snprintf(txt, sizeof(txt), "AUSPEX  %d m", static_cast<int>(range));
        dl->AddText(ImVec2(c.x - R, c.y - R - 18.0f), IM_COL32(120, 255, 140, 255), txt);
    }

    // Cerchio verde a terra sotto ogni rinforzo evocato (vox caster): fa parte della squadra di chi l'ha chiamato
    void GameLayer::RenderSummonRings(bool deferred) {
        auto& reg = m_Scene.GetRegistry();
        constexpr int kSegments = 28;
        constexpr float kRadius = 0.6f;
        const glm::vec4 color(0.2f, 0.95f, 0.3f, 1.0f);
        const float segLen = 2.0f * glm::pi<float>() * kRadius / kSegments * 1.1f;
        for (auto e : reg.view<SummonedComponent, TransformComponent>(entt::exclude<CorpseComponent>)) {
            const auto& t = reg.get<TransformComponent>(e);
            float footY = t.Position.y;
            if (const auto* c = reg.try_get<ColliderComponent>(e)) footY = t.Position.y + (c->localCenterOffset.y - c->halfExtents.y) * t.Scale.y;
            for (int i = 0; i < kSegments; ++i) {
                const float a = 2.0f * glm::pi<float>() * (i + 0.5f) / kSegments;
                const glm::vec3 p(t.Position.x + std::cos(a) * kRadius, footY + 0.03f, t.Position.z + std::sin(a) * kRadius);
                glm::mat4 m = glm::translate(glm::mat4(1.0f), p);
                m = glm::rotate(m, -a, glm::vec3(0.0f, 1.0f, 0.0f));
                m = glm::scale(m, glm::vec3(0.06f, 0.012f, segLen));
                if (deferred) m_DeferredRenderer.DrawCube(m, color, 0.4f, 0.0f, 0.8f);
                else Renderer::DrawCube(m, color);
            }
        }
    }

    bool GameLayer::HasSummons() {
        return m_Scene.GetRegistry().view<SummonedComponent>().size() > 0;
    }

    // Rinforzi chiamati col vox caster: il conto alla rovescia, poi l'unita' compare dietro chi l'ha chiamata.
    // Se chi ha chiamato cade prima, i rinforzi non arrivano.
    void GameLayer::UpdateSummons(float deltaTime) {
        auto& reg = m_Scene.GetRegistry();
        std::vector<std::pair<entt::entity, std::string>> arrivals;
        for (auto e : reg.view<SummonRequestComponent>()) {
            auto& req = reg.get<SummonRequestComponent>(e);
            const auto* hp = reg.try_get<HealthComponent>(e);
            if (!hp || hp->currentHealth <= 0.0f || reg.all_of<CorpseComponent>(e)) { req.pending.clear(); continue; }
            for (auto it = req.pending.begin(); it != req.pending.end();) {
                it->timer -= deltaTime;
                if (it->timer <= 0.0f) { arrivals.emplace_back(e, it->unitId); it = req.pending.erase(it); }
                else ++it;
            }
        }
        for (const auto& [owner, unitId] : arrivals) SpawnSummon(owner, unitId);
    }

    entt::entity GameLayer::SpawnSummon(entt::entity owner, const std::string& unitId) {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(owner) || !reg.all_of<TransformComponent>(owner) || !BlueprintManager::Get().GetUnit(unitId)) return entt::null;
        // Squadra del padrone (in modalita' orda il giocatore non ne ha: si usa la 0, quella degli alleati)
        int team = 0;
        if (const auto* tc = reg.try_get<TeamComponent>(owner)) team = tc->team;
        else reg.emplace<TeamComponent>(owner, TeamComponent{ 0 });

        int already = 0;
        for (auto s : reg.view<SummonedComponent>(entt::exclude<CorpseComponent>)) if (reg.get<SummonedComponent>(s).owner == owner) ++already;
        const float side = (already % 2 == 0) ? 1.0f : -1.0f;

        const auto& ot = reg.get<TransformComponent>(owner);
        float footY = ot.Position.y;
        if (const auto* c = reg.try_get<ColliderComponent>(owner)) footY = ot.Position.y + (c->localCenterOffset.y - c->halfExtents.y) * ot.Scale.y;
        const float yr = glm::radians(ot.AimYaw);
        const glm::vec3 fwd(-std::sin(yr), 0.0f, -std::cos(yr)), right(std::cos(yr), 0.0f, -std::sin(yr));
        glm::vec3 pos = glm::vec3(ot.Position.x, footY, ot.Position.z) - fwd * 2.0f + right * (side * 1.5f);
        const glm::vec3 nav = m_NavQuery.GetNearestValidPoint(pos);
        if (glm::length(glm::vec2(nav.x - pos.x, nav.z - pos.z)) < 4.0f) pos = nav;   // navmesh vicino: li' (a terra)

        TrainedUnit u = MakeBotUnit(unitId, 0);
        const entt::entity e = SpawnBot(u, team, pos, ot.AimYaw, -1);
        if (e == entt::null) return entt::null;
        m_VsBotUnits.erase(e);   // non e' un'unita' di una squadra del Vs Bot
        reg.emplace<SummonedComponent>(e, SummonedComponent{ owner, side });
        if (auto* brain = reg.try_get<BotBrainComponent>(e)) {
            std::string ownerName = "te";
            if (owner != m_PlayerEntity) if (const auto* ob = reg.try_get<BotBrainComponent>(owner)) ownerName = ob->displayName;
            brain->displayName = u.customName + " (rinforzo di " + ownerName + ")";
            brain->advanceSide = side;
        }
        if (auto* a = reg.try_get<AmmoComponent>(owner)) { a->feedback = "Rinforzo arrivato: " + u.customName; a->feedbackTimer = 2.5f; }
        SUN_CORE_INFO("Rinforzo dal vox caster: {0}", u.customName);
        return e;
    }

    // Fuori dalla partita: via i rinforzi e le chiamate in sospeso
    void GameLayer::ClearSummons() {
        auto& reg = m_Scene.GetRegistry();
        std::vector<entt::entity> gone;
        for (auto e : reg.view<SummonedComponent>()) gone.push_back(e);
        for (auto e : gone) {
            if (!reg.valid(e)) continue;
            m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
            m_Scene.DestroyEntity(e);
        }
        reg.clear<SummonRequestComponent>();
        // In modalita' orda il giocatore riceve la squadra 0 col primo rinforzo: fuori dal Vs Bot la si toglie
        if (!m_VsBotActive && reg.valid(m_PlayerEntity) && reg.all_of<TeamComponent>(m_PlayerEntity)) reg.remove<TeamComponent>(m_PlayerEntity);
    }

    // Voce di un combo dell'equipaggiamento nel garage. Gli oggetti con un nodo di ricerca vanno prima ricercati
    // (voce disattivata) e poi comprati per questa unita' con energia: il primo click compra, poi si equipaggia.
    // Ritorna true se lo slot e' cambiato.
    bool GameLayer::DrawItemOption(TrainedUnit& unit, std::string& slotRef, const std::string& wId, const WeaponBlueprint& wBP) {
        const ResearchNode* node = m_ResearchTree.FindNodeByItem(wId);
        const bool researched = !node || m_ResearchTree.IsResearched(m_Profile, *node);
        bool owned = !node || unit.OwnsItem(wId);
        std::string label = WeaponLabel(wBP.displayName, wId);
        if (!researched) label += "   [DA RICERCARE]";
        else if (!owned) label += "   [COMPRA: " + std::to_string(static_cast<long long>(node->purchaseCost)) + " ENERGIA]";
        bool changed = false;
        if (ImGui::Selectable(label.c_str(), slotRef == wId, researched ? 0 : ImGuiSelectableFlags_Disabled)) {
            if (!owned) {
                if (m_ResearchTree.Pay(m_Profile, *node)) {
                    unit.ownedItems.push_back(wId);
                    owned = true;
                    m_ResearchMessage = "Comprato per " + unit.customName + ": " + wBP.displayName;
                }
                else m_ResearchMessage = "Energia insufficiente per " + wBP.displayName;
                m_ResearchMessageTimer = 4.0f;
            }
            if (owned && slotRef != wId) { slotRef = wId; changed = true; }
        }
        return changed;
    }

    // ===================== ESPERIENZA DI PARTITA =====================
    void GameLayer::ResetMatchXp() {
        m_MatchXp.clear();
        m_MatchXpDamaged.clear();
        m_MatchSurvivalAccum = 0.0f;
        m_MatchXpApplied = false;
        m_MatchPsBefore.clear();
        m_MatchXpTotal = 0;
        m_MatchEnergy = 0;
        m_MatchRevealTimer = 0.0f;
        m_MatchPlayerDied = false;
        m_MatchRawXp = 0;
        m_MatchOutcomeMult = 1.0f;
        m_MatchOutcomeLabel.clear();
        m_MatchVip = false;
        m_MatchUnitGain.clear();
        XpEventLog().clear();
    }

    // Raccoglie gli eventi del giocatore e li assegna all'unita' che sta controllando adesso
    void GameLayer::CollectMatchXp(float deltaTime, bool countSurvival) {
        auto& reg = m_Scene.GetRegistry();
        std::string unitId;
        if (const auto* uid = reg.try_get<UnitBlueprintId>(m_PlayerEntity)) unitId = uid->id;
        auto& log = XpEventLog();
        // Aiuti ai compagni per gli ordini della Guardia (munizioni date, cure, strutture costruite e che assorbono)
        for (const auto& ev : log) {
            if (ev.source != m_PlayerEntity) continue;
            if (ev.category == XpCategory::AmmoAlly || ev.category == XpCategory::Heal || ev.category == XpCategory::Build) m_OrdersAssistAccum += ev.xp * 0.1f;
            else if (ev.category == XpCategory::Absorb) m_OrdersAssistAccum += ev.xp * 0.05f;
        }
        if (m_MatchXpApplied || unitId.empty()) { log.clear(); return; }
        auto& lines = m_MatchXp[unitId];
        for (const auto& ev : log) {
            if (ev.source != m_PlayerEntity) continue;   // solo quello che fa il giocatore
            if (ev.category == XpCategory::Damage) {
                const uint32_t key = static_cast<uint32_t>(entt::to_integral(ev.target));
                if (!m_MatchXpDamaged.insert(key).second) continue;   // questo nemico l'ha gia' danneggiato
            }
            auto& line = lines[static_cast<size_t>(ev.category)];
            line.count += (ev.category == XpCategory::Absorb) ? static_cast<int>(ev.xp) : 1;   // assorbito: conta i punti di danno
            line.xp += ev.xp;
        }
        log.clear();

        // Sopravvivenza: 50 punti per ogni secondo in vita
        const auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity);
        if (hp && hp->currentHealth <= 0.0f) m_MatchPlayerDied = true;
        if (countSurvival && !m_IsPaused && hp && hp->currentHealth > 0.0f) {
            m_MatchSurvivalAccum += deltaTime;
            while (m_MatchSurvivalAccum >= 1.0f) {
                m_MatchSurvivalAccum -= 1.0f;
                auto& line = lines[static_cast<size_t>(XpCategory::Survival)];
                line.count += 1;
                line.xp += XpValues::SurvivalPerSecond;
            }
        }
    }

    // Fine partita: punti studio a ogni tipo di unita' usata, energia = meta' del totale. Una volta sola.
    // outcomeMult = esito della 5v5 (vivo e vittoria 100%, morto e vittoria 75%, morto e sconfitta 50%);
    // il VIP moltiplica ancora per 1.25 (anche in sopravvivenza).
    void GameLayer::ApplyMatchXp(float outcomeMult, const std::string& outcomeLabel) {
        if (m_MatchXpApplied) return;
        CollectMatchXp(0.0f, false);
        m_MatchXpApplied = true;
        m_MatchOutcomeMult = outcomeMult;
        m_MatchOutcomeLabel = outcomeLabel;
        m_MatchVip = m_Profile.vip;
        const double mult = static_cast<double>(outcomeMult) * (m_MatchVip ? 1.25 : 1.0);
        m_MatchXpTotal = 0;
        m_MatchRawXp = 0;
        for (const auto& [unitId, lines] : m_MatchXp) {
            int64_t unitXp = 0;
            for (const auto& l : lines) unitXp += l.xp;
            const int64_t gain = static_cast<int64_t>(std::llround(static_cast<double>(unitXp) * mult));
            m_MatchPsBefore[unitId] = ResearchTree::GetStudyPoints(m_Profile, unitId);
            m_MatchUnitGain[unitId] = gain;
            m_Profile.studyPoints[unitId] += gain;
            m_MatchRawXp += unitXp;
            m_MatchXpTotal += gain;
        }
        m_MatchEnergy = m_MatchXpTotal / 2;
        m_Profile.energy += m_MatchEnergy;
        m_MatchRevealTimer = 0.0f;
        SUN_CORE_INFO("Esperienza di partita: +{0} punti studio, +{1} energia", m_MatchXpTotal, m_MatchEnergy);
    }

    // Riepilogo stile Battlefield: le voci compaiono una alla volta, poi le barre dei punti studio si riempiono
    // verso le prossime ricerche disponibili di ogni unita' usata.
    void GameLayer::RenderMatchRecap() {
        ImGuiIO& io = ImGui::GetIO();
        m_MatchRevealTimer += io.DeltaTime;
        static const char* kNames[static_cast<size_t>(XpCategory::Count)] = {
            "Sopravvivenza (secondi in vita)", "Munizioni a te stesso", "Munizioni ai compagni", "Nemici danneggiati",
            "Uccisioni", "Cure ai compagni", "Strutture costruite", "Danni assorbiti dalle strutture" };

        std::array<MatchXpLine, static_cast<size_t>(XpCategory::Count)> totals{};
        for (const auto& [unitId, lines] : m_MatchXp)
            for (size_t c = 0; c < lines.size(); ++c) { totals[c].count += lines[c].count; totals[c].xp += lines[c].xp; }

        Ui::Heading("RIEPILOGO DELLA PARTITA");
        int shown = 0;
        const int visibleRows = static_cast<int>(m_MatchRevealTimer / 0.3f);
        if (ImGui::BeginTable("##recap", 3, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("voce", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("quanti", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("punti", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            for (size_t c = 0; c < totals.size(); ++c) {
                if (totals[c].xp <= 0) continue;
                if (shown++ >= visibleRows) break;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", kNames[c]);
                ImGui::TableSetColumnIndex(1); ImGui::Text("x%d", totals[c].count);
                ImGui::TableSetColumnIndex(2); ImGui::TextColored(ImVec4(0.50f, 0.74f, 0.44f, 1.0f), "+%lld", static_cast<long long>(totals[c].xp));
            }
            ImGui::EndTable();
        }
        if (m_MatchRawXp <= 0 && m_MatchXpTotal <= 0) ImGui::TextDisabled("Nessuna esperienza in questa partita.");
        ImGui::Separator();
        if (m_MatchXpApplied && static_cast<int>(m_MatchRevealTimer / 0.3f) > shown) {
            ImGui::Text("Esperienza della partita: %lld", static_cast<long long>(m_MatchRawXp));
            if (!m_MatchOutcomeLabel.empty()) {
                const ImVec4 oc = m_MatchOutcomeMult >= 1.0f ? ImVec4(0.50f, 0.74f, 0.44f, 1.0f) : (m_MatchOutcomeMult >= 0.75f ? ImVec4(0.95f, 0.77f, 0.36f, 1.0f) : ImVec4(0.90f, 0.30f, 0.17f, 1.0f));
                ImGui::TextColored(oc, "%s: %d%%", m_MatchOutcomeLabel.c_str(), static_cast<int>(std::lround(m_MatchOutcomeMult * 100.0f)));
            }
            if (m_MatchVip) ImGui::TextColored(ImVec4(0.64f, 0.47f, 0.84f, 1.0f), "VIP: +25%%");
            else ImGui::TextDisabled("VIP: non attivo (+25%% nel negozio)");
        }
        ImGui::TextColored(ImVec4(0.36f, 0.80f, 0.74f, 1.0f), "Punti studio: +%lld", static_cast<long long>(m_MatchXpTotal));
        ImGui::SameLine(0.0f, 30.0f);
        ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "Energia: +%lld", static_cast<long long>(m_MatchEnergy));
        ImGui::Spacing();

        // Barre: punti studio di ogni unita' usata verso le sue prossime ricerche (si riempiono dopo il riepilogo)
        const float rowsTime = 0.3f * static_cast<float>(shown + 1);
        const float fill = std::clamp((m_MatchRevealTimer - rowsTime) / 2.0f, 0.0f, 1.0f);
        const float eased = 1.0f - (1.0f - fill) * (1.0f - fill);
        ImGui::BeginChild("##researchbars", ImVec2(0, 210), true);
        for (const auto& [unitId, lines] : m_MatchXp) {
            int64_t unitXp = 0;
            for (const auto& l : lines) unitXp += l.xp;
            if (m_MatchUnitGain.count(unitId)) unitXp = m_MatchUnitGain.at(unitId);   // dopo esito e VIP
            const int64_t before = m_MatchPsBefore.count(unitId) ? m_MatchPsBefore.at(unitId) : ResearchTree::GetStudyPoints(m_Profile, unitId) - unitXp;
            const int64_t now = before + static_cast<int64_t>(static_cast<double>(unitXp) * eased);
            const auto* ubp = BlueprintManager::Get().GetUnit(unitId);
            ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "%s: %lld PS  (+%lld)", ubp ? ubp->displayName.c_str() : unitId.c_str(),
                static_cast<long long>(now), static_cast<long long>(unitXp));
            bool any = false;
            if (m_ResearchTree.IsLoaded()) {
                for (const auto& faction : m_ResearchTree.GetFactions()) {
                    for (const auto& n : faction.nodes) {
                        if (m_ResearchTree.IsResearched(m_Profile, n)) continue;
                        bool paysHere = false;   // anche coi padri alternativi (es. Kommandos oppure Tankbustas)
                        for (const ResearchNode* src : m_ResearchTree.StudySources(n)) paysHere = paysHere || src->unitId == unitId;
                        if (!paysHere) continue;
                        const ResearchStatus rst = m_ResearchTree.GetStatus(m_Profile, n);
                        if (rst == ResearchStatus::LockedParent || rst == ResearchStatus::LockedMastery) continue;   // non ancora disponibile
                        any = true;
                        const float frac = n.researchCost > 0 ? std::clamp(static_cast<float>(now) / static_cast<float>(n.researchCost), 0.0f, 1.0f) : 1.0f;
                        char overlay[96];
                        if (now >= n.researchCost) std::snprintf(overlay, sizeof(overlay), "PRONTA DA RICERCARE");
                        else std::snprintf(overlay, sizeof(overlay), "mancano %lld PS", static_cast<long long>(n.researchCost - now));
                        ImGui::Text("  %s", n.name.c_str());
                        ImGui::SameLine(220.0f);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, now >= n.researchCost ? ImVec4(0.50f, 0.74f, 0.44f, 1.0f) : ImVec4(0.36f, 0.80f, 0.74f, 1.0f));
                        ImGui::ProgressBar(frac, ImVec2(-1, 16), overlay);
                        ImGui::PopStyleColor();
                    }
                }
            }
            if (!any) ImGui::TextDisabled("  Nessuna ricerca disponibile per questa unita'.");
            ImGui::Spacing();
        }
        ImGui::EndChild();
    }

    // HUD munizioni in basso a destra: nome dell'arma in mano, "caricatore / riserva" e stato.
    // Con la scatola di munizioni in mano: usi rimasti, comandi e barra del rifornimento in corso.
    // ===================== BUFF DI FAZIONE =====================
    // Si attiva col tasto V. Il contatore resta se rientri con un'altra unita' della stessa fazione e si azzera se
    // passi a un'altra fazione. Vale solo per l'unita' del giocatore.
    //
    // Orki: WAAAGH!. La scritta "Waaagh!" ha 7 caratteri da 50 punti (350). Si carica con i danni inflitti e
    // subiti; piena, si scatena con V: per 20 secondi +15% velocita', +40% velocita' di ricarica, +20% velocita'
    // dei colpi in mischia, +15% danni in mischia. Se non combatti per piu' di 5 secondi (sparare con un nemico in
    // vista o colpire in mischia con un nemico entro 5 m) la barra inizia a svuotarsi.
    //
    // Guardia imperiale: FOLLOW ORDERS (300 punti). Si carica con la coesione (compagni entro 12 m, fino a 4; non
    // nei primi 10 secondi dopo ogni schieramento), i danni inflitti e gli aiuti ai compagni. Scende se dai le
    // spalle a un nemico che ti vede ed e' a portata: la Guardia non arretra. Piena, V tenuto apre il selettore
    // radiale (mouse per scegliere, rilascia per dare l'ordine), 10 secondi:
    //   Take AIM!          +30% cadenza, -30% dispersione
    //   Take Cover         -15% danni subiti, immune agli stordimenti per i primi 3 secondi
    //   MOVE MOVE MOVE!    +20% velocita', immune ai rallentamenti
    //   Fix Bayonets!      +15% danni in mischia, +20% recupero della stamina
    static const char* kOrderNames[4] = { "Ordine: Mirare", "Take Cover", "MOVE MOVE MOVE!", "Ordine: Fissare le Baionette" };
    static const char* kOrderEffects[4] = { "+30% cadenza   -30% dispersione", "-15% danni subiti   immune agli stordimenti (3 s)",
                                            "+20% velocita'   immune ai rallentamenti", "+15% danni in mischia   +20% recupero stamina" };

    void GameLayer::ResetFactionBuff() {
        m_FactionBuffFaction.clear();
        m_WaaaghPoints = 0.0f;
        m_WaaaghTimer = 0.0f;
        m_WaaaghPrevHp = -1.0f;
        m_FactionFightIdle = 0.0f;
        m_OrdersAssistAccum = 0.0f;
        m_OrderActive = -1;
        m_OrderTimer = 0.0f;
        m_CohesionDelay = 10.0f;
        m_RadialOpen = false;
        m_RadialVec = glm::vec2(0.0f);
        PlayerDamageDealtAccum() = 0.0f;
        auto& reg = m_Scene.GetRegistry();
        if (reg.valid(m_PlayerEntity) && reg.all_of<FactionBuffComponent>(m_PlayerEntity)) reg.remove<FactionBuffComponent>(m_PlayerEntity);
        if (reg.valid(m_PlayerEntity)) if (auto* st = reg.try_get<UnitStatsComponent>(m_PlayerEntity)) st->damageTakenMult = 1.0f;
    }

    // Effetti dell'ordine in corso (ogni frame: dopo un rientro l'unita' nuova li riprende)
    void GameLayer::ApplyOrderBuff() {
        auto& reg = m_Scene.GetRegistry();
        auto* st = reg.try_get<UnitStatsComponent>(m_PlayerEntity);
        if (m_OrderActive < 0) {
            if (reg.all_of<FactionBuffComponent>(m_PlayerEntity)) reg.remove<FactionBuffComponent>(m_PlayerEntity);
            if (st) st->damageTakenMult = 1.0f;
            return;
        }
        auto& b = reg.get_or_emplace<FactionBuffComponent>(m_PlayerEntity);
        b = FactionBuffComponent{};
        if (st) st->damageTakenMult = 1.0f;
        switch (m_OrderActive) {
        case 0: b.fireRateMult = 1.30f; b.spreadMult = 0.70f; break;
        case 1: if (st) st->damageTakenMult = 0.85f; b.stunImmuneTimer = std::max(0.0f, m_OrderTimer - (kOrderDuration - 3.0f)); break;
        case 2: b.moveSpeedMult = 1.20f; b.ignoreSlows = true; break;
        case 3: b.meleeDamageMult = 1.15f; b.staminaRegenMult = 1.20f; break;
        default: break;
        }
    }

    // Nemici in vista dal giocatore (linea di vista dagli occhi), ogni 0,2 s: davanti entro la portata dell'arma
    // (orki: "sta combattendo") e alle spalle entro la portata (Guardia: "volta le spalle al nemico").
    void GameLayer::ScanFactionEnemies() {
        auto& reg = m_Scene.GetRegistry();
        m_FactionEnemyFront = m_FactionEnemyBehind = false;
        m_FactionNearestEnemy = 1e9f;
        if (!reg.valid(m_PlayerEntity)) return;
        const auto& pt = reg.get<TransformComponent>(m_PlayerEntity);
        float footY = pt.Position.y, height = 1.8f;
        if (const auto* c = reg.try_get<ColliderComponent>(m_PlayerEntity)) {
            height = std::max(0.6f, c->halfExtents.y * 2.0f * pt.Scale.y);
            footY = pt.Position.y + (c->localCenterOffset.y - c->halfExtents.y) * pt.Scale.y;
        }
        const glm::vec3 eye(pt.Position.x, footY + height * 0.88f, pt.Position.z);
        const float yr = glm::radians(pt.AimYaw);
        const glm::vec3 fwd(-std::sin(yr), 0.0f, -std::cos(yr));
        float range = 40.0f;
        if (const auto* inv = reg.try_get<InventoryComponent>(m_PlayerEntity)) {
            const WeaponBlueprint* w = Ammo::RangedAt(*inv, 0);
            if (!w) w = Ammo::RangedAt(*inv, 1);
            if (w) range = std::clamp(w->range_effective, 25.0f, 60.0f);
        }
        const auto* myTeam = reg.try_get<TeamComponent>(m_PlayerEntity);
        auto view = reg.view<TransformComponent, HealthComponent, UnitStatsComponent>(entt::exclude<CorpseComponent, PlayerTag>);
        for (auto o : view) {
            if (view.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
            if (const auto* sq = reg.try_get<SquigComponent>(o); sq && sq->owner == m_PlayerEntity) continue;
            bool enemy;
            if (const auto* ot = reg.try_get<TeamComponent>(o)) enemy = !myTeam || ot->team != myTeam->team;
            else enemy = reg.any_of<HordeAgentComponent, TrainingDummyComponent>(o);
            if (!enemy) continue;
            const glm::vec3 op = view.get<TransformComponent>(o).Position;
            glm::vec3 to = op - pt.Position; to.y = 0.0f;
            const float d = glm::length(to);
            m_FactionNearestEnemy = std::min(m_FactionNearestEnemy, d);
            if (d > range || d < 1e-3f) continue;
            const float dot = glm::dot(fwd, to / d);
            const bool front = dot > 0.5f, behind = dot < -0.34f;   // davanti: +-60 gradi; alle spalle: oltre 110 gradi
            if ((!front || m_FactionEnemyFront) && (!behind || m_FactionEnemyBehind)) continue;
            if (!HasLineOfSight(m_Scene, eye, op + glm::vec3(0.0f, 1.0f, 0.0f), m_PlayerEntity)) continue;
            if (front) m_FactionEnemyFront = true;
            if (behind) m_FactionEnemyBehind = true;
        }
    }

    void GameLayer::UpdateFactionBuff(float deltaTime) {
        auto& reg = m_Scene.GetRegistry();
        float dealt = PlayerDamageDealtAccum();
        PlayerDamageDealtAccum() = 0.0f;
        float assist = m_OrdersAssistAccum;
        m_OrdersAssistAccum = 0.0f;
        if (!reg.valid(m_PlayerEntity)) return;

        std::string faction;
        if (const auto* uid = reg.try_get<UnitBlueprintId>(m_PlayerEntity)) faction = VsBotFactionOf(uid->id);
        if (faction != m_FactionBuffFaction) {   // cambio di fazione: si riparte da zero
            ResetFactionBuff();
            m_FactionBuffFaction = faction;
            dealt = assist = 0.0f;
        }

        // Danni subiti: calo della salute del giocatore (il rientro con salute piena non conta)
        float taken = 0.0f;
        const auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity);
        if (hp) {
            if (m_WaaaghPrevHp >= 0.0f && hp->currentHealth < m_WaaaghPrevHp) taken = m_WaaaghPrevHp - std::max(0.0f, hp->currentHealth);
            m_WaaaghPrevHp = std::max(0.0f, hp->currentHealth);
        }
        const bool alive = hp && hp->currentHealth > 0.0f && !reg.all_of<CorpseComponent>(m_PlayerEntity);
        if (alive && !m_FactionWasAlive) m_CohesionDelay = 10.0f;   // appena schierato (inizio partita o rientro)
        m_FactionWasAlive = alive;
        if (alive) m_CohesionDelay = std::max(0.0f, m_CohesionDelay - deltaTime);

        // Tasto V (fronte di salita / rilascio)
        const bool canInput = alive && !m_IsPaused && m_GameState == GameState::Gameplay;
        const bool keyDown = canInput && Input::IsKeyPressed(GLFW_KEY_V);
        const bool keyPressed = keyDown && !m_FactionKeyWasDown;
        const bool keyReleased = !keyDown && m_FactionKeyWasDown;
        m_FactionKeyWasDown = keyDown;

        // Combattimento del giocatore: sparo con un nemico in vista davanti, o colpo in mischia con un nemico entro 5 m
        m_FactionScanTimer -= deltaTime;
        if (m_FactionScanTimer <= 0.0f) { m_FactionScanTimer = 0.2f; ScanFactionEnemies(); }
        bool fought = false;
        if (alive) {
            const auto* ws = reg.try_get<WeaponStateComponent>(m_PlayerEntity);
            const auto* inv = reg.try_get<InventoryComponent>(m_PlayerEntity);
            if (ws && inv) {
                if (inv->currentSlot == m_FactionPrevSlot && ws->currentAmmo < m_FactionPrevAmmo && m_FactionEnemyFront) fought = true;
                m_FactionPrevAmmo = ws->currentAmmo; m_FactionPrevSlot = inv->currentSlot;
            }
            if (const auto* melee = reg.try_get<MeleeStateComponent>(m_PlayerEntity)) {
                const bool windup = melee->currentPhase == MeleePhase::Windup;
                if (windup && !m_FactionPrevWindup && m_FactionNearestEnemy <= 5.0f) fought = true;
                m_FactionPrevWindup = windup;
            }
        }

        if (faction == "orks") {
            if (m_WaaaghTimer > 0.0f) {
                m_WaaaghTimer -= deltaTime;
                if (m_WaaaghTimer <= 0.0f) {
                    m_WaaaghTimer = 0.0f;
                    m_WaaaghPoints = 0.0f;
                    if (reg.all_of<FactionBuffComponent>(m_PlayerEntity)) reg.remove<FactionBuffComponent>(m_PlayerEntity);
                    SUN_CORE_INFO("WAAAGH! finita");
                }
                else {
                    // Anche dopo un rientro con un'altra unita' orka il buff resta per il tempo che manca
                    auto& b = reg.get_or_emplace<FactionBuffComponent>(m_PlayerEntity);
                    b.moveSpeedMult = 1.15f; b.reloadSpeedMult = 1.40f; b.meleeSpeedMult = 1.20f; b.meleeDamageMult = 1.15f;
                }
                return;
            }
            m_WaaaghPoints = std::min(kWaaaghMax, m_WaaaghPoints + dealt + taken);
            // Senza combattere per piu' di 5 secondi la barra si svuota (25 punti al secondo)
            m_FactionFightIdle = (fought || dealt > 0.0f) ? 0.0f : m_FactionFightIdle + deltaTime;
            if (m_FactionFightIdle > 5.0f) m_WaaaghPoints = std::max(0.0f, m_WaaaghPoints - 25.0f * deltaTime);
            if (m_WaaaghPoints >= kWaaaghMax && keyPressed) {
                m_WaaaghTimer = kWaaaghDuration;
                auto& b = reg.get_or_emplace<FactionBuffComponent>(m_PlayerEntity);
                b.moveSpeedMult = 1.15f; b.reloadSpeedMult = 1.40f; b.meleeSpeedMult = 1.20f; b.meleeDamageMult = 1.15f;
                SUN_CORE_INFO("WAAAGH!!! (20 s)");
            }
            return;
        }

        if (faction == "imperium") {
            if (m_OrderActive >= 0) {
                m_RadialOpen = false;
                m_OrderTimer -= deltaTime;
                if (m_OrderTimer <= 0.0f) { m_OrderTimer = 0.0f; m_OrderActive = -1; m_WaaaghPoints = 0.0f; }
                ApplyOrderBuff();
                return;
            }
            ApplyOrderBuff();   // nessun ordine: niente effetti
            // Carica: coesione (compagni entro 12 m, fino a 4: 2,5 punti al secondo ciascuno), danni, aiuti
            float gain = dealt * 0.5f + assist;
            if (alive && m_CohesionDelay <= 0.0f) {
                if (const auto* myTeam = reg.try_get<TeamComponent>(m_PlayerEntity)) {
                    const glm::vec3 me = reg.get<TransformComponent>(m_PlayerEntity).Position;
                    int allies = 0;
                    auto mates = reg.view<TeamComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent, PlayerTag, SquigComponent>);
                    for (auto o : mates) {
                        if (mates.get<TeamComponent>(o).team != myTeam->team || mates.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
                        const glm::vec3 op = mates.get<TransformComponent>(o).Position;
                        if (glm::length(glm::vec2(op.x - me.x, op.z - me.z)) <= 12.0f) ++allies;
                    }
                    gain += std::min(allies, 4) * 2.5f * deltaTime;
                }
            }
            m_WaaaghPoints = std::min(kOrdersMax, m_WaaaghPoints + gain);
            // La Guardia non arretra: spalle a un nemico che ti vede e a portata = 20 punti al secondo persi
            if (alive && m_FactionEnemyBehind) m_WaaaghPoints = std::max(0.0f, m_WaaaghPoints - 20.0f * deltaTime);

            // Selettore radiale: V tenuto con la barra piena, il mouse sceglie, il rilascio da' l'ordine
            if (keyPressed && m_WaaaghPoints >= kOrdersMax) { m_RadialOpen = true; m_RadialVec = glm::vec2(0.0f); }
            if (m_RadialOpen && (!alive || m_IsPaused)) m_RadialOpen = false;
            if (m_RadialOpen && keyReleased) {
                m_RadialOpen = false;
                if (glm::length(m_RadialVec) > 20.0f) {
                    // 0 su, 1 destra, 2 giu', 3 sinistra (il mouse in su ha delta y negativo)
                    const float a = glm::degrees(std::atan2(m_RadialVec.x, -m_RadialVec.y));
                    const int sector = ((static_cast<int>(std::lround(a / 90.0f)) % 4) + 4) % 4;
                    m_OrderActive = sector;
                    m_OrderTimer = kOrderDuration;
                    ApplyOrderBuff();
                    SUN_CORE_INFO("Ordine: {0}", kOrderNames[sector]);
                }
            }
            return;
        }
        // Altre fazioni: nessun buff
        m_RadialOpen = false;
    }

    // Selettore radiale degli ordini al centro dello schermo: 4 voci (su, destra, giu', sinistra)
    void GameLayer::RenderOrdersRadial() {
        if (!m_RadialOpen) return;
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        const float R = 150.0f;
        dl->AddCircleFilled(c, R + 40.0f, IM_COL32(10, 12, 10, 150), 48);
        dl->AddCircle(c, R + 40.0f, IM_COL32(200, 170, 80, 200), 48, 2.0f);
        int sel = -1;
        if (glm::length(m_RadialVec) > 20.0f) {
            const float a = glm::degrees(std::atan2(m_RadialVec.x, -m_RadialVec.y));
            sel = ((static_cast<int>(std::lround(a / 90.0f)) % 4) + 4) % 4;
        }
        const ImVec2 dirs[4] = { ImVec2(0, -1), ImVec2(1, 0), ImVec2(0, 1), ImVec2(-1, 0) };
        for (int i = 0; i < 4; ++i) {
            const ImVec2 p(c.x + dirs[i].x * R, c.y + dirs[i].y * R);
            const bool on = i == sel;
            const ImVec2 ts = ImGui::CalcTextSize(kOrderNames[i]);
            const ImVec2 es = ImGui::CalcTextSize(kOrderEffects[i]);
            const float w = std::max(ts.x, es.x) * 0.5f + 12.0f;
            dl->AddRectFilled(ImVec2(p.x - w, p.y - 24.0f), ImVec2(p.x + w, p.y + 24.0f), on ? IM_COL32(150, 115, 30, 235) : IM_COL32(40, 42, 36, 225), 6.0f);
            dl->AddRect(ImVec2(p.x - w, p.y - 24.0f), ImVec2(p.x + w, p.y + 24.0f), on ? IM_COL32(255, 220, 110, 255) : IM_COL32(150, 140, 100, 200), 6.0f, 0, on ? 2.5f : 1.0f);
            dl->AddText(ImVec2(p.x - ts.x * 0.5f, p.y - 18.0f), on ? IM_COL32(255, 245, 200, 255) : IM_COL32(230, 220, 190, 255), kOrderNames[i]);
            dl->AddText(ImVec2(p.x - es.x * 0.5f, p.y + 2.0f), IM_COL32(190, 190, 170, 230), kOrderEffects[i]);
        }
        // Puntatore: dove sta andando il mouse
        const glm::vec2 v = m_RadialVec * (R * 0.6f / 90.0f);
        dl->AddLine(c, ImVec2(c.x + v.x, c.y + v.y), IM_COL32(255, 220, 110, 230), 3.0f);
        dl->AddCircleFilled(c, 5.0f, IM_COL32(255, 220, 110, 255));
        const char* title = "FOLLOW ORDERS   (muovi il mouse, rilascia V)";
        const ImVec2 tt = ImGui::CalcTextSize(title);
        dl->AddText(ImVec2(c.x - tt.x * 0.5f, c.y + R + 52.0f), IM_COL32(230, 210, 150, 255), title);
    }

    // In alto a sinistra: salute, sotto la stamina, sotto ancora il buff di fazione
    // Danni subiti piu' "fisici": a ogni colpo i bordi dello schermo si arrossano (piu' forte se il colpo e' grosso),
    // dal bordo colano gocce di sangue e compaiono macchie che svaniscono in qualche secondo. Sotto il 30% di vita
    // i bordi pulsano come un battito. Disegnato sotto le finestre dell'HUD.
    void GameLayer::RenderDamageVignette() {
        auto& reg = m_Scene.GetRegistry();
        const auto* h = reg.valid(m_PlayerEntity) ? reg.try_get<HealthComponent>(m_PlayerEntity) : nullptr;
        if (!h) { m_LastPlayerHp = -1.0f; m_BloodSplats.clear(); m_DamageFlash = 0.0f; return; }
        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        const float W = ds.x, H = ds.y;
        const float maxHp = std::max(1.0f, h->maxHealth);

        // Colpo subito: lampo proporzionale al danno e nuove macchie sui bordi
        if (m_LastPlayerHp >= 0.0f && h->currentHealth < m_LastPlayerHp - 0.5f) {
            const float frac = (m_LastPlayerHp - h->currentHealth) / maxHp;
            m_DamageFlash = std::min(1.0f, m_DamageFlash + std::clamp(frac * 4.0f, 0.3f, 1.0f));
            const int n = 1 + static_cast<int>(std::min(4.0f, frac * 20.0f));
            for (int i = 0; i < n && m_BloodSplats.size() < 40; ++i) {
                BloodSplat b;
                b.seed = static_cast<uint32_t>(Random::Range(0.0f, 1.0e6f));
                // Posizione lungo un bordo a caso, un po' dentro lo schermo
                const int side = static_cast<int>(Random::Range(0.0f, 3.999f));
                const float u = Random::Range(0.05f, 0.95f), in = Random::Range(0.0f, 0.07f);
                if (side == 0) { b.x = u * W; b.y = in * H; }
                else if (side == 1) { b.x = u * W; b.y = H * (1.0f - in); }
                else if (side == 2) { b.x = in * W; b.y = u * H; }
                else { b.x = W * (1.0f - in); b.y = u * H; }
                b.radius = H * Random::Range(0.025f, 0.05f) * (0.8f + std::min(1.0f, frac * 6.0f));
                b.maxLife = b.life = Random::Range(2.2f, 3.5f);
                m_BloodSplats.push_back(b);
                // Goccia che cola dal bordo alto o dai lati
                if (side != 1) {
                    BloodSplat d = b;
                    d.drip = true; d.seed ^= 0x9e3779b9u;
                    d.radius = H * Random::Range(0.006f, 0.011f);
                    if (side == 0) d.y = 0.0f;
                    m_BloodSplats.push_back(d);
                }
            }
        }
        m_LastPlayerHp = h->currentHealth;
        m_DamageFlash = std::max(0.0f, m_DamageFlash - dt * 1.1f);
        m_DamagePulseTime += dt;

        // Vita bassa: battito (piu' veloce e forte piu' si e' vicini a zero)
        const float hpFrac = std::clamp(h->currentHealth / maxHp, 0.0f, 1.0f);
        float low = 0.0f;
        if (hpFrac < 0.3f && h->currentHealth > 0.0f) {
            const float k = (0.3f - hpFrac) / 0.3f;
            const float beat = std::pow(std::max(0.0f, std::sin(m_DamagePulseTime * (5.0f + 4.0f * k))), 6.0f);
            low = (0.25f + 0.35f * k) + 0.35f * beat;
        }
        const float a = std::clamp(std::max(m_DamageFlash, low * 0.75f), 0.0f, 1.0f);

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (a > 0.01f) {
            const float t = std::min(W, H) * (0.10f + 0.14f * a);   // spessore del bordo rosso
            const ImU32 c0 = IM_COL32(150, 0, 0, static_cast<int>(200 * a));
            const ImU32 c1 = IM_COL32(150, 0, 0, 0);
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, t), c0, c0, c1, c1);             // alto
            dl->AddRectFilledMultiColor(ImVec2(0, H - t), ImVec2(W, H), c1, c1, c0, c0);         // basso
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(t, H), c0, c1, c1, c0);             // sinistra
            dl->AddRectFilledMultiColor(ImVec2(W - t, 0), ImVec2(W, H), c1, c0, c0, c1);         // destra
            // Secondo strato piu' scuro e sottile: il bordo sembra "impregnato"
            const float t2 = t * 0.35f;
            const ImU32 d0 = IM_COL32(70, 0, 0, static_cast<int>(170 * a));
            const ImU32 d1 = IM_COL32(70, 0, 0, 0);
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, t2), d0, d0, d1, d1);
            dl->AddRectFilledMultiColor(ImVec2(0, H - t2), ImVec2(W, H), d1, d1, d0, d0);
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(t2, H), d0, d1, d1, d0);
            dl->AddRectFilledMultiColor(ImVec2(W - t2, 0), ImVec2(W, H), d1, d0, d0, d1);
        }

        // Macchie e gocce
        auto hash = [](uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return (x & 0xffffu) / 65535.0f; };
        for (auto& b : m_BloodSplats) {
            b.life -= dt;
            if (b.life <= 0.0f) continue;
            const float age = b.maxLife - b.life;
            const float fade = std::clamp(b.life / (b.maxLife * 0.6f), 0.0f, 1.0f) * std::clamp(age * 12.0f, 0.0f, 1.0f);
            const int alpha = static_cast<int>(185 * fade);
            if (b.drip) {
                // Colata: scende rallentando, larga in alto e con una goccia in fondo
                const float len = H * 0.16f * (1.0f - std::exp(-age * 1.4f)) * (0.6f + 0.8f * hash(b.seed));
                const ImVec2 top(b.x, b.y), bot(b.x, b.y + len);
                dl->AddRectFilledMultiColor(ImVec2(b.x - b.radius * 0.5f, top.y), ImVec2(b.x + b.radius * 0.5f, bot.y),
                    IM_COL32(120, 0, 0, alpha), IM_COL32(120, 0, 0, alpha), IM_COL32(95, 0, 0, alpha), IM_COL32(95, 0, 0, alpha));
                dl->AddCircleFilled(bot, b.radius * 0.8f, IM_COL32(100, 0, 0, alpha), 12);
                continue;
            }
            // Macchia irregolare: un disco centrale e schizzi intorno
            dl->AddCircleFilled(ImVec2(b.x, b.y), b.radius, IM_COL32(110, 0, 0, static_cast<int>(alpha * 0.85f)), 20);
            dl->AddCircleFilled(ImVec2(b.x, b.y), b.radius * 0.6f, IM_COL32(75, 0, 0, alpha), 16);
            for (int k = 0; k < 7; ++k) {
                const float ang = hash(b.seed + k * 31u) * 6.2832f;
                const float dist = b.radius * (0.8f + 0.9f * hash(b.seed + k * 57u));
                const float r = b.radius * (0.12f + 0.3f * hash(b.seed + k * 83u));
                dl->AddCircleFilled(ImVec2(b.x + std::cos(ang) * dist, b.y + std::sin(ang) * dist), r, IM_COL32(105, 0, 0, alpha), 10);
            }
        }
        m_BloodSplats.erase(std::remove_if(m_BloodSplats.begin(), m_BloodSplats.end(), [](const BloodSplat& b) { return b.life <= 0.0f; }), m_BloodSplats.end());
    }

    void GameLayer::RenderPlayerStatusHUD() {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(m_PlayerEntity)) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImFont* font = ImGui::GetFont();
        const float x = 20.0f;
        float y = 20.0f;
        const float w = 280.0f;

        if (const auto* h = reg.try_get<HealthComponent>(m_PlayerEntity)) {
            const float frac = std::clamp(h->currentHealth / std::max(1.0f, h->maxHealth), 0.0f, 1.0f);
            const float bh = 24.0f;
            // Salute: piastra smussata col bordo d'ottone, riempimento verde spento che vira al sangue sotto il 40%
            const ImVec2 a(x, y), b(x + w, y + bh);
            Ui::ChamferRect(dl, a, b, 7.0f, IM_COL32(14, 12, 12, 215), 0);
            if (frac > 0.0f) {
                const ImU32 top = frac > 0.4f ? IM_COL32(96, 150, 84, 255) : IM_COL32(190, 44, 34, 255);
                const ImU32 bot = frac > 0.4f ? IM_COL32(52, 88, 48, 255) : IM_COL32(110, 20, 16, 255);
                dl->PushClipRect(a, ImVec2(x + w * frac, y + bh), true);
                dl->AddRectFilledMultiColor(ImVec2(x + 1.0f, y + 1.0f), ImVec2(x + w - 1.0f, y + bh - 1.0f), top, top, bot, bot);
                dl->PopClipRect();
            }
            for (int k = 1; k < 4; ++k) dl->AddLine(ImVec2(x + w * 0.25f * k, y + bh - 6.0f), ImVec2(x + w * 0.25f * k, y + bh - 1.0f), IM_COL32(0, 0, 0, 150));   // tacche dei quarti
            Ui::ChamferRect(dl, a, b, 7.0f, 0, Ui::U32(Ui::Col::Brass, 0.95f), 1.5f);
            char txt[64];
            std::snprintf(txt, sizeof(txt), "%.0f / %.0f", std::max(0.0f, h->currentHealth), h->maxHealth);
            ImFont* hf = Ui::FontStrong();
            const float hfs = hf->FontSize * 0.86f;
            const ImVec2 ts = hf->CalcTextSizeA(hfs, FLT_MAX, 0.0f, txt);
            dl->AddText(hf, hfs, ImVec2(x + (w - ts.x) * 0.5f + 1.0f, y + (bh - ts.y) * 0.5f + 1.0f), IM_COL32(0, 0, 0, 230), txt);
            dl->AddText(hf, hfs, ImVec2(x + (w - ts.x) * 0.5f, y + (bh - ts.y) * 0.5f), Ui::U32(Ui::Col::Bone), txt);
            y += bh + 5.0f;
        }
        if (const auto* st = reg.try_get<UnitStatsComponent>(m_PlayerEntity)) {
            const float frac = std::clamp(st->stamina / std::max(1.0f, st->maxStamina), 0.0f, 1.0f);
            const float bh = 10.0f;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + bh), IM_COL32(14, 12, 12, 205));
            // Senza fiato: barra ambra finche' non si torna a poter correre
            const ImU32 stCol = st->staminaExhausted ? Ui::U32(Ui::Col::Rust) : Ui::U32(Ui::Col::Gold, 0.95f);
            if (frac > 0.0f) dl->AddRectFilled(ImVec2(x + 1.0f, y + 1.0f), ImVec2(x + (w - 1.0f) * frac, y + bh - 1.0f), stCol);
            dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + bh), Ui::U32(Ui::Col::Steel, 0.9f), 0.0f, 0, 1.0f);
            y += bh + 8.0f;
            if (st->staminaExhausted) { dl->AddText(ImVec2(x, y), IM_COL32(230, 130, 60, 255), "Senza fiato"); y += 16.0f; }
            // Rigenerazione passiva della vita (fuori dal combattimento)
            const auto* hpc = reg.try_get<HealthComponent>(m_PlayerEntity);
            if (hpc && hpc->currentHealth < hpc->maxHealth && hpc->currentHealth > 0.0f) {
                char t[64];
                if (HealthRegenSystem::TimeToRegen(*st) <= 0.0f) {
                    std::snprintf(t, sizeof(t), "Rigenerazione: +%.1f hp/s", HealthRegenSystem::RegenPerSecond(*st));
                    dl->AddText(ImVec2(x, y), IM_COL32(110, 230, 120, 255), t); y += 16.0f;
                }
                else if (st->outOfCombatTime > 1.0f) {
                    std::snprintf(t, sizeof(t), "Rigenerazione tra %.0f s", std::ceil(HealthRegenSystem::TimeToRegen(*st)));
                    dl->AddText(ImVec2(x, y), IM_COL32(150, 170, 150, 220), t); y += 16.0f;
                }
            }
        }
        // Effetti in corso della dotazione (zaino energetico, insegna imperiale vicina)
        if (const auto* boost = reg.try_get<SpeedBoostComponent>(m_PlayerEntity)) {
            char t[64]; std::snprintf(t, sizeof(t), "Zaino energetico: +%d%% velocita'  %.0f s", static_cast<int>(std::lround((boost->mult - 1.0f) * 100.0f)), std::ceil(boost->timeLeft));
            dl->AddText(ImVec2(x, y), IM_COL32(120, 200, 255, 255), t); y += 16.0f;
        }
        if (const auto* aura = reg.try_get<MoraleAuraComponent>(m_PlayerEntity)) {
            char t[64]; std::snprintf(t, sizeof(t), "Insegna imperiale: morale +%d%%", static_cast<int>(std::lround(aura->bonus * 100.0f)));
            dl->AddText(ImVec2(x, y), IM_COL32(255, 215, 90, 255), t); y += 16.0f;
        }
        if (reg.any_of<SpeedBoostComponent, MoraleAuraComponent>(m_PlayerEntity)) y += 4.0f;

        if (m_FactionBuffFaction == "imperium") {
            // "FOLLOW ORDERS": le lettere si accendono d'oro da sinistra a destra man mano che la barra si carica
            const char* word = "FOLLOW ORDERS";
            const int n = static_cast<int>(std::strlen(word));
            const float size = 28.0f;
            const bool active = m_OrderActive >= 0;
            const float frac = active ? m_OrderTimer / kOrderDuration : m_WaaaghPoints / kOrdersMax;
            const bool full = !active && m_WaaaghPoints >= kOrdersMax;
            const float pulse = (active || full) ? 0.75f + 0.25f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.0f) : 1.0f;
            const ImU32 off = IM_COL32(70, 70, 66, 230);
            const ImU32 on = IM_COL32(static_cast<int>(255 * pulse), static_cast<int>(205 * pulse), 70, 255);
            float lx = x;
            for (int i = 0; i < n; ++i) {
                const char ch[2] = { word[i], 0 };
                const ImVec2 ls = font->CalcTextSizeA(size, FLT_MAX, 0.0f, ch);
                const float f = std::clamp(frac * n - i, 0.0f, 1.0f);
                dl->AddText(font, size, ImVec2(lx + 2.0f, y + 2.0f), IM_COL32(0, 0, 0, 200), ch);
                dl->AddText(font, size, ImVec2(lx, y), off, ch);
                if (f > 0.0f) {
                    dl->PushClipRect(ImVec2(lx, y), ImVec2(lx + ls.x * f, y + ls.y), true);
                    dl->AddText(font, size, ImVec2(lx, y), on, ch);
                    dl->PopClipRect();
                }
                lx += ls.x + 1.0f;
            }
            char info[96];
            if (active) std::snprintf(info, sizeof(info), "%s  %.0f s", kOrderNames[m_OrderActive], std::ceil(m_OrderTimer));
            else if (full) std::snprintf(info, sizeof(info), "PRONTO: tieni [V]");
            else std::snprintf(info, sizeof(info), "%.0f / %.0f", m_WaaaghPoints, kOrdersMax);
            dl->AddText(ImVec2(lx + 10.0f, y + size * 0.5f - 7.0f), (active || full) ? on : IM_COL32(200, 200, 200, 220), info);
            float ly = y + size + 2.0f;
            if (active) { dl->AddText(ImVec2(x, ly), IM_COL32(240, 220, 160, 230), kOrderEffects[m_OrderActive]); ly += 16.0f; }
            else {
                if (m_CohesionDelay > 0.0f) {
                    char d[64]; std::snprintf(d, sizeof(d), "Coesione attiva tra %.0f s", std::ceil(m_CohesionDelay));
                    dl->AddText(ImVec2(x, ly), IM_COL32(170, 170, 160, 220), d); ly += 16.0f;
                }
                if (m_FactionEnemyBehind) dl->AddText(ImVec2(x, ly), IM_COL32(255, 90, 70, 255), "Non voltare le spalle al nemico!");
            }
            return;
        }
        if (m_FactionBuffFaction != "orks") return;
        // "HUNGRY!" (ex "Waaagh!"): ogni carattere vale 50 punti e si accende di verde da sinistra a destra (anche a meta')
        static const char* kLetters[7] = { "H", "U", "N", "G", "R", "Y", "!" };
        const float size = 34.0f;
        const bool active = m_WaaaghTimer > 0.0f;
        const float lit = active ? (m_WaaaghTimer / kWaaaghDuration) * kWaaaghMax : m_WaaaghPoints;   // attiva: si svuota
        const float pulse = active ? 0.75f + 0.25f * std::sin(static_cast<float>(ImGui::GetTime()) * 10.0f) : 1.0f;
        const ImU32 off = IM_COL32(70, 75, 70, 230);
        const ImU32 on = active ? IM_COL32(static_cast<int>(120 * pulse), 255, static_cast<int>(60 * pulse), 255) : IM_COL32(60, 220, 70, 255);
        float lx = x;
        for (int i = 0; i < 7; ++i) {
            const ImVec2 ls = font->CalcTextSizeA(size, FLT_MAX, 0.0f, kLetters[i]);
            const float f = std::clamp((lit - i * 50.0f) / 50.0f, 0.0f, 1.0f);
            dl->AddText(font, size, ImVec2(lx + 2.0f, y + 2.0f), IM_COL32(0, 0, 0, 200), kLetters[i]);
            dl->AddText(font, size, ImVec2(lx, y), off, kLetters[i]);
            if (f > 0.0f) {   // parte accesa della lettera
                dl->PushClipRect(ImVec2(lx, y), ImVec2(lx + ls.x * f, y + ls.y), true);
                dl->AddText(font, size, ImVec2(lx, y), on, kLetters[i]);
                dl->PopClipRect();
            }
            lx += ls.x + 1.0f;
        }
        char info[96];
        const bool ready = !active && m_WaaaghPoints >= kWaaaghMax;
        if (active) std::snprintf(info, sizeof(info), "HUNGRY!  %.0f s", std::ceil(m_WaaaghTimer));
        else if (ready) std::snprintf(info, sizeof(info), "PRONTA: premi [V]");
        else std::snprintf(info, sizeof(info), "%.0f / %.0f", m_WaaaghPoints, kWaaaghMax);
        dl->AddText(ImVec2(lx + 10.0f, y + size * 0.5f - 7.0f), (active || ready) ? on : IM_COL32(200, 200, 200, 220), info);
        if (active) dl->AddText(ImVec2(x, y + size + 2.0f), IM_COL32(170, 240, 170, 230),
            "+15% velocita'  +40% ricarica  +20% colpi in mischia  +15% danni in mischia");
        else if (m_FactionFightIdle > 5.0f && m_WaaaghPoints > 0.0f)
            dl->AddText(ImVec2(x, y + size + 2.0f), IM_COL32(255, 120, 80, 240), "Niente rissa: la fame HUNGRY! si sta spegnendo!");
    }

    // Animazioni procedurali del modello animato (ProceduralPose.h): accovacciato (se mancano le clip),
    // arrampicata lenta, colpi in mischia e parata, lancio delle granate, oggetti posati a terra (cibo, mine),
    // scatola di munizioni, chiave inglese e costruzione di strutture. Si sommano alla clip in corso.
    void GameLayer::BuildPlayerPose(float deltaTime, RifleAnimationController& controller) {
        m_PoseTime += deltaTime;
        BuildUnitPose(m_PlayerEntity, m_PlayerPose, controller, m_PoseTime, m_PoseClimb, deltaTime);
    }

    RaceMotionKind GameLayer::RaceMotionOf(AnimatedBodyKind k) {
        switch (k) {
        case AnimatedBodyKind::Ork: return RaceMotionKind::Ork;
        case AnimatedBodyKind::Gretchin: return RaceMotionKind::Gretchin;
        case AnimatedBodyKind::Astartes: return RaceMotionKind::Astartes;
        case AnimatedBodyKind::Neophyte: return RaceMotionKind::Neophyte;
        case AnimatedBodyKind::Ogryn: return RaceMotionKind::Ogryn;
        default: return RaceMotionKind::Human;
        }
    }

    // Stile di movimento della razza (RaceMotion.h) per un'unita' animata: velocita' dal suo spostamento, mira,
    // corsa, accovacciato e "urlo" (carica, WAAAGH!, colpo pesante). Imposta anche il ritmo delle clip.
    void GameLayer::ApplyRaceStyle(entt::entity e, ProceduralPose& pose, RifleAnimationController& controller, float legLen, float deltaTime) {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(e) || !reg.all_of<TransformComponent>(e)) return;
        const std::string unitId = AnimUnitIdOf(reg, e);
        const AnimatedBodyKind kind = e == m_PlayerEntity ? CurrentAnimatedBodyKind() : BodyKindForUnit(unitId);
        auto& slot = m_RaceMotion[e];
        const auto& t = reg.get<TransformComponent>(e);
        RaceMotionInput in;
        in.dt = deltaTime;
        if (slot.has && deltaTime > 0.0f) {
            glm::vec3 d = t.Position - slot.lastPos; d.y = 0.0f;
            in.speed = glm::length(d) / deltaTime;
            if (in.speed > 15.0f) in.speed = 0.0f;   // teletrasporto / rientro
        }
        slot.lastPos = t.Position; slot.has = true;
        in.legLen = legLen;
        float unitScale = 1.0f;
        if (const auto* ubp = BlueprintManager::Get().GetUnit(unitId); ubp && ubp->model_scale > 0.05f) unitScale = ubp->model_scale;
        in.modelUnitsPerMeter = 1.0f / std::max(1e-4f, m_AnimatedModelScale * unitScale * AnimatedBodyModel(kind).unitScale);
        in.yaw = t.Rotation.y;
        const auto* st = reg.try_get<UnitStatsComponent>(e);
        in.sprinting = st && (st->sprinting || in.speed > std::max(1.0f, st->baseSpeed) * 1.25f);
        if (const auto* cr = reg.try_get<CrouchComponent>(e)) in.crouching = cr->amount > 0.5f;
        if (e == m_PlayerEntity) in.aiming = m_PlayerAimingNow;
        else if (const auto* brain = reg.try_get<BotBrainComponent>(e)) in.aiming = brain->isAiming;
        // Urlo: chi corre all'assalto (non gli astartes, che hanno l'elmo), la WAAAGH! attiva, il colpo pesante
        const auto* buff = reg.try_get<FactionBuffComponent>(e);
        const auto* melee = reg.try_get<MeleeStateComponent>(e);
        const bool heavy = melee && melee->currentAttack == MeleeAttackType::Heavy &&
            (melee->currentPhase == MeleePhase::Windup || melee->currentPhase == MeleePhase::Active);
        in.roar = heavy || (buff && buff->moveSpeedMult > 1.01f) || (in.sprinting && kind != AnimatedBodyKind::Human);
        const float rate = ApplyRaceMotion(pose, GetRaceMotionProfile(RaceMotionOf(kind)), slot.state, in);
        controller.SetStyleRate(rate);
    }

    // Pose procedurali di un'unita' (giocatore, bot, orda) sopra la clip in corso: accovacciato, arrampicata,
    // mischia e parata, dotazione (lancio, oggetti posati, scatola, chiave inglese, costruzione, medikit, vox, torretta).
    void GameLayer::BuildUnitPose(entt::entity e, ProceduralPose& pose, RifleAnimationController& controller, float poseTime, float& climb, float deltaTime) {
        pose.Clear();
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(e)) return;

        // Lunghezze di coscia e stinco (unita' del modello) dalla posa del frame precedente
        float thigh = 0.0f, shin = 0.0f;
        {
            auto bonePos = [&](const char* b, glm::vec3& out) {
                glm::mat4 t;
                if (!controller.GetBoneModelTransform(std::string("mixamorig1:") + b, t)) return false;
                out = glm::vec3(t[3]);
                return true;
            };
            glm::vec3 a, k, f;
            if (bonePos("LeftUpLeg", a) && bonePos("LeftLeg", k) && bonePos("LeftFoot", f)) { thigh = glm::distance(a, k); shin = glm::distance(k, f); }
        }
        auto approach = [&](float& v, bool on, float rate) { v = std::clamp(v + (on ? 1.0f : -1.0f) * rate * deltaTime, 0.0f, 1.0f); };

        // Modo di muoversi della razza: postura, passo, testa, respiro, ossa in piu' (sotto le azioni)
        ApplyRaceStyle(e, pose, controller, thigh + shin, deltaTime);

        // Accovacciato (solo se non ci sono le clip "crouch idle/walking")
        if (const auto* crouch = reg.try_get<CrouchComponent>(e); crouch && !controller.HasCrouchClips())
            PoseLib::Crouch(pose, crouch->amount, thigh, shin);

        // Arrampicata lenta (scavalcamento piu' lungo di 1,2 s)
        const auto* vault = reg.try_get<VaultComponent>(e);
        approach(climb, vault && vault->isVaulting && vault->vaultDuration > 1.2f, 6.0f);
        PoseLib::Climb(pose, poseTime, climb);

        // Mischia: fasi dell'attacco dalla macchina a stati della mischia
        if (const auto* melee = reg.try_get<MeleeStateComponent>(e)) {
            if (melee->currentPhase == MeleePhase::Stunned) PoseLib::Stunned(pose, 1.0f);
            else if (melee->currentAttack == MeleeAttackType::Parry) {
                const float t = melee->stateTimer;
                const float end = DamageResolver::ParryEnd(*melee);
                PoseLib::Parry(pose, t < 0.08f ? t / 0.08f : (t < 0.28f ? 1.0f : std::max(0.0f, (end - t) / (end - 0.28f))));
            }
            else if (melee->currentPhase == MeleePhase::Windup || melee->currentPhase == MeleePhase::Active || melee->currentPhase == MeleePhase::Recovery) {
                const auto* wbp = DamageResolver::ResolveMeleeWeapon(reg, e);
                MeleeTiming tm;
                if (wbp && wbp->hasMoveset) {
                    auto it = wbp->moveset.nodes.find(melee->currentAttackNode);
                    if (it != wbp->moveset.nodes.end()) tm = DamageResolver::EffectiveTiming(it->second, melee->currentAttack, melee->currentAttackNode);
                }
                const int phase = melee->currentPhase == MeleePhase::Windup ? 0 : (melee->currentPhase == MeleePhase::Active ? 1 : 2);
                const float dur = phase == 0 ? tm.windup : (phase == 1 ? tm.active : tm.recovery + melee->whiffPenalty);
                const int type = melee->currentAttack == MeleeAttackType::Light ? 0 : (melee->currentAttack == MeleeAttackType::Heavy ? 2 : 1);
                PoseLib::Melee(pose, type, phase, melee->stateTimer / std::max(0.01f, dur),
                               wbp && wbp->hand_type == HandType::TwoHanded);
            }
        }

        // Dotazione: stato rilevato da ActionPose (lo stesso usato per gli scheletri a blocchi dei bot)
        const auto* ap = reg.try_get<ActionPoseComponent>(e);
        if (ap && m_GameState == GameState::Gameplay) {   // nel garage lo stato delle azioni non si aggiorna
            PoseLib::Give(pose, poseTime, ap->give);
            PoseLib::Build(pose, poseTime, ap->build, thigh, shin);
            PoseLib::Repair(pose, poseTime, ap->repair);
            PoseLib::SelfHeal(pose, poseTime, ap->medkit);
            PoseLib::VoxCall(pose, ap->vox);
            PoseLib::Gunner(pose, ap->gunner, thigh, shin);
            if (ap->action == 1) PoseLib::Throw(pose, ap->time / ap->duration);
            else if (ap->action == 2) PoseLib::UseItem(pose, ap->time / ap->duration, thigh, shin);
        }
        // Sussulto di chi viene colpito (sopra tutto il resto)
        m_Feel.ApplyFlinch(e, pose);
    }

    // HUD della torretta: dentro (carica del colpo, colpi, salute dell'arma, uscita) o vicino (entra / smonta)
    void GameLayer::RenderTurretHUD() {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(m_PlayerEntity)) return;
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float cx = io.DisplaySize.x * 0.5f, by = io.DisplaySize.y * 0.70f;
        auto centered = [&](float y, const char* txt, ImU32 col) {
            const ImVec2 ts = ImGui::CalcTextSize(txt);
            dl->AddText(ImVec2(cx - ts.x * 0.5f + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, 200), txt);
            dl->AddText(ImVec2(cx - ts.x * 0.5f, y), col, txt);
        };
        auto bar = [&](float y, float w, float frac, ImU32 col) {
            dl->AddRectFilled(ImVec2(cx - w * 0.5f, y), ImVec2(cx + w * 0.5f, y + 10.0f), IM_COL32(20, 20, 20, 200), 2.0f);
            dl->AddRectFilled(ImVec2(cx - w * 0.5f, y), ImVec2(cx - w * 0.5f + w * std::clamp(frac, 0.0f, 1.0f), y + 10.0f), col, 2.0f);
            dl->AddRect(ImVec2(cx - w * 0.5f, y), ImVec2(cx + w * 0.5f, y + 10.0f), IM_COL32(0, 0, 0, 220), 2.0f);
        };
        if (const auto* m = reg.try_get<TurretMountComponent>(m_PlayerEntity); m && reg.valid(m->turret) && reg.all_of<TurretComponent>(m->turret)) {
            const auto& t = reg.get<TurretComponent>(m->turret);
            const auto* bp = BlueprintManager::Get().GetWeapon(t.itemId);
            if (!bp) return;
            char txt[160];
            std::snprintf(txt, sizeof(txt), "%s   colpo: %s   riserva: %d", bp->displayName.c_str(),
                t.loaded > 0 ? "PRONTO" : (t.reserve > 0 ? "ricarica..." : "finito"), t.reserve);
            centered(by, txt, IM_COL32(230, 230, 230, 255));
            // Carica: verde fino al colpo, poi rosso quando surriscalda
            const float full = t.charge / std::max(0.01f, bp->charge_time);
            const bool over = t.charge > bp->overcharge_time;
            bar(by + 22.0f, 300.0f, full, over ? IM_COL32(230, 50, 40, 255) : (full >= 1.0f ? IM_COL32(80, 230, 90, 255) : IM_COL32(230, 200, 60, 255)));
            if (t.loaded > 0 && t.charge <= 0.0f) centered(by + 38.0f, "Tieni premuto il sinistro per caricare, rilascia per sparare", IM_COL32(190, 190, 190, 230));
            else if (over) centered(by + 38.0f, "SURRISCALDAMENTO! Rilascia o l'arma si danneggia", IM_COL32(255, 90, 70, 255));
            else if (full >= 1.0f) centered(by + 38.0f, "CARICO: rilascia per sparare", IM_COL32(120, 255, 130, 255));
            if (const auto* sh = reg.try_get<StructureHealthComponent>(m->turret)) {
                std::snprintf(txt, sizeof(txt), "Arma: %.0f / %.0f", std::max(0.0f, sh->hp), sh->maxHp);
                centered(by + 58.0f, txt, IM_COL32(200, 200, 200, 230));
                bar(by + 76.0f, 160.0f, sh->hp / std::max(1.0f, sh->maxHp), IM_COL32(120, 170, 220, 255));
            }
            if (m_TurretSystem.exitProgress > 0.0f) { centered(by + 94.0f, "Uscita...", IM_COL32(230, 230, 230, 255)); bar(by + 112.0f, 160.0f, m_TurretSystem.exitProgress, IM_COL32(230, 230, 230, 255)); }
            else centered(by + 94.0f, "Tieni premuto E per 1,5 s per uscire", IM_COL32(160, 160, 160, 220));
            return;
        }
        // Cassa di munizioni a portata
        if (m_EquipmentSystem.nearCrate != entt::null && reg.valid(m_EquipmentSystem.nearCrate)) {
            if (const auto* c = reg.try_get<AmmoCrateComponent>(m_EquipmentSystem.nearCrate)) {
                char txt[128];
                std::snprintf(txt, sizeof(txt), "E: prendi un caricatore dell'arma in mano   (cassa %d/%d)", c->charges, c->maxCharges);
                centered(by - 26.0f, txt, IM_COL32(255, 210, 90, 255));
            }
        }
        if (m_TurretSystem.nearTurret != entt::null && reg.valid(m_TurretSystem.nearTurret)) {
            const auto* t = reg.try_get<TurretComponent>(m_TurretSystem.nearTurret);
            const auto* bp = t ? BlueprintManager::Get().GetWeapon(t->itemId) : nullptr;
            char txt[160];
            std::snprintf(txt, sizeof(txt), "E: usa %s%s", bp ? bp->displayName.c_str() : "l'arma",
                m_TurretSystem.canDismantle ? "    |    tieni E 5 s: smonta e riprendi" : "");
            centered(by, txt, IM_COL32(230, 230, 230, 255));
            if (m_TurretSystem.dismantleProgress > 0.0f) {
                centered(by + 20.0f, "Smontaggio...", IM_COL32(230, 200, 60, 255));
                bar(by + 38.0f, 200.0f, m_TurretSystem.dismantleProgress, IM_COL32(230, 200, 60, 255));
            }
        }
    }

    void GameLayer::RenderAmmoHUD() {
        auto& reg = m_Scene.GetRegistry();
        const auto* inv = reg.try_get<InventoryComponent>(m_PlayerEntity);
        const auto* ws = reg.try_get<WeaponStateComponent>(m_PlayerEntity);
        const auto* ammo = reg.try_get<AmmoComponent>(m_PlayerEntity);
        if (!inv || !ws) return;

        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImFont* font = ImGui::GetFont();
        const float baseSize = ImGui::GetFontSize();
        const float right = io.DisplaySize.x - 30.0f;
        float y = io.DisplaySize.y - 30.0f;

        // Testo allineato a destra, con ombra per leggerlo su ogni sfondo
        auto textRight = [&](const std::string& t, float size, ImU32 col) {
            ImFont* f = size > baseSize * 1.5f ? Ui::FontStrong() : font;   // i numeri grandi col carattere pieno
            const ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str());
            y -= sz.y;
            const ImVec2 p(right - sz.x, y);
            dl->AddText(f, size, ImVec2(p.x + 2.0f, p.y + 2.0f), IM_COL32(0, 0, 0, 190), t.c_str());
            dl->AddText(f, size, p, col, t.c_str());
            y -= 4.0f;
        };

        const std::string heldId = inv->GetActiveWeaponId();
        const auto* held = BlueprintManager::Get().GetWeapon(heldId);
        const int rangedIdx = Ammo::HeldRangedIndex(*inv);
        const ImU32 white = Ui::U32(Ui::Col::Bone), grey = Ui::U32(Ui::Col::Ash);
        const ImU32 red = Ui::U32(Ui::Col::Ember), yellow = Ui::U32(Ui::Col::Gold);
        // filo d'ottone a destra del blocco delle munizioni
        dl->AddRectFilled(ImVec2(right + 8.0f, io.DisplaySize.y - 110.0f), ImVec2(right + 10.0f, io.DisplaySize.y - 30.0f), Ui::U32(Ui::Col::Brass, 0.85f));

        if (held && held->is_equipment && ammo) {
            const std::string& type = held->equipment_type;
            const int slotIdx = inv->currentSlot - 4;
            const int uses = (slotIdx >= 0 && slotIdx < 3) ? ammo->equipmentUses[slotIdx] : 0;
            if (ammo->useTimer > 0.0f && ammo->useDuration > 0.0f) {
                // Barra del rifornimento in corso
                const float w = 220.0f, h = 8.0f;
                const float prog = std::clamp(1.0f - ammo->useTimer / ammo->useDuration, 0.0f, 1.0f);
                y -= h;
                dl->AddRectFilled(ImVec2(right - w, y), ImVec2(right, y + h), IM_COL32(20, 20, 20, 190));
                dl->AddRectFilled(ImVec2(right - w, y), ImVec2(right - w + w * prog, y + h), IM_COL32(255, 200, 60, 255));
                y -= 6.0f;
                if (type == "scrap_barricade" || type == "slit_wall") textRight(type == "slit_wall" ? "Costruzione muro... (resta fermo)" : "Costruzione barricata... (resta fermo)", baseSize, yellow);
                else if (type == "turret") textRight("Montaggio... (resta fermo)", baseSize, yellow);
                else if (type == "medkit") textRight("Medicazione...", baseSize, yellow);
                else if (type == "vox_caster") textRight("Chiamata col Communication Device...", baseSize, yellow);
                else textRight(ammo->useTarget == m_PlayerEntity ? "Rifornimento..." : "Rifornimento al compagno...", baseSize, yellow);
            }
            if (type == "ammo_box")
                textRight("Sinistro: rifornisci te   |   Destro: compagno entro " + std::to_string(static_cast<int>(held->equipment_range)) + " m", baseSize, grey);
            else if (type == "scrap_barricade" || type == "slit_wall")
                textRight("Tieni premuto sinistro da fermo per " + std::to_string(static_cast<int>(std::lround(held->equipment_use_time))) + " s: " +
                          (type == "slit_wall" ? "muro con feritoia" : "barricata"), baseSize, grey);
            else if (type == "medkit")
                textRight("Sinistro: curati di " + std::to_string(static_cast<int>(held->heal_amount)) + " salute in " +
                          std::to_string(static_cast<int>(std::lround(held->equipment_use_time))) + " s", baseSize, grey);
            else if (type == "power_pack") {
                const float cd = (slotIdx >= 0 && slotIdx < 3) ? ammo->equipmentCooldown[slotIdx] : 0.0f;
                textRight("Sinistro: +" + std::to_string(static_cast<int>(std::lround(held->move_speed_bonus * 100.0f))) + "% velocita' per " +
                          std::to_string(static_cast<int>(std::lround(held->effect_duration))) + " s (ricarica " + std::to_string(static_cast<int>(held->equipment_cooldown)) + " s)", baseSize, grey);
                textRight(cd > 0.0f ? "Ricarica: " + std::to_string(static_cast<int>(std::ceil(cd))) + " s" : std::string("PRONTO"), baseSize * 2.0f, cd > 0.0f ? red : white);
            }
            else if (type == "insignia")
                textRight("Passiva: morale +" + std::to_string(static_cast<int>(std::lround(held->morale_bonus * 100.0f))) + "% a te e ai compagni entro " +
                          std::to_string(static_cast<int>(held->equipment_range)) + " m", baseSize, grey);
            else if (type == "camo_cloak")
                textRight("Sinistro: mimetizzati per " + std::to_string(static_cast<int>(std::lround(held->effect_duration))) +
                          " s (sparare o colpire lo interrompe)", baseSize, grey);
            else if (type == "auspex")
                textRight("Sonar attivo: nemici entro " + std::to_string(static_cast<int>(held->equipment_range)) + " m", baseSize, grey);
            else if (type == "ammo_crate")
                textRight("Sinistro: posa la cassa a terra (" + std::to_string(held->crate_charges) + " caricatori, E vicino per prenderne uno)", baseSize, grey);
            else if (type == "vox_caster")
                textRight("Sinistro: chiama un rinforzo (" + std::to_string(static_cast<int>(std::lround(held->equipment_use_time))) +
                          " s di chiamata, arriva dopo " + std::to_string(static_cast<int>(std::lround(held->summon_delay))) + " s)", baseSize, grey);
            else if (type == "turret")
                textRight("Tieni premuto sinistro da fermo per " + std::to_string(static_cast<int>(std::lround(held->equipment_use_time))) +
                          " s: piazza " + held->displayName, baseSize, grey);
            else if (type == "grenade")
                textRight("Sinistro: lancia verso il mirino (scoppia dopo " + std::to_string(static_cast<int>(std::lround(held->fuse_time))) + " s)", baseSize, grey);
            else if (type == "landmine")
                textRight("Sinistro: piazza la mina davanti a te (si arma in " + std::to_string(static_cast<int>(std::lround(held->fuse_time))) + " s)", baseSize, grey);
            else if (type == "food_pack")
                textRight("Sinistro: posa lo zaino (cura " + std::to_string(static_cast<int>(held->heal_per_second)) + " HP/s entro " +
                          std::to_string(static_cast<int>(held->equipment_range)) + " m per " + std::to_string(static_cast<int>(held->effect_duration)) + " s)", baseSize, grey);
            if (type != "auspex" && type != "power_pack" && type != "insignia")   // passivi o a ricarica: niente usi
                textRight("Usi: " + std::to_string(uses) + " / " + std::to_string(held->equipment_uses), baseSize * 2.0f, uses > 0 ? white : red);
            textRight(held->displayName, baseSize * 1.15f, grey);
        }
        else if (rangedIdx >= 0 && held) {
            const int mag = ws->currentAmmo;
            const bool infinite = ammo && ammo->infinite;
            const int reserve = ammo ? ammo->reserve[rangedIdx] : 0;
            if (ws->isReloading) textRight("RICARICA...", baseSize, yellow);
            else if (mag == 0 && reserve == 0 && !infinite) textRight("MUNIZIONI ESAURITE - passa a un'altra arma", baseSize, red);
            else if (mag == 0) textRight("CARICATORE VUOTO - premi R", baseSize, red);
            const std::string count = std::to_string(mag) + " / " + (infinite ? std::string("INF") : std::to_string(reserve));
            const ImU32 col = (mag == 0) ? red : (mag <= std::max(1, held->magazine_size / 4) ? yellow : white);
            textRight(count, baseSize * 2.2f, col);
            textRight(held->displayName, baseSize * 1.15f, grey);
        }
        else if (held) {
            textRight(held->displayName, baseSize * 1.15f, grey);   // arma da mischia: solo il nome
        }

        // Messaggi brevi (scatola usata, munizioni ricevute, esaurite...)
        if (ammo && ammo->feedbackTimer > 0.0f && !ammo->feedback.empty()) {
            const int alpha = static_cast<int>(std::clamp(ammo->feedbackTimer / 0.5f, 0.0f, 1.0f) * 255.0f);
            textRight(ammo->feedback, baseSize, IM_COL32(255, 220, 120, alpha));
        }
    }

    void GameLayer::RenderTrainingDummyLabels() {
        auto& reg = m_Scene.GetRegistry();
        auto view = reg.view<TrainingDummyComponent, HealthComponent, TransformComponent>();
        if (view.begin() == view.end()) return;

        const glm::mat4 vp = m_Camera.GetViewProjection();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        glm::vec3 camRef(0.0f);
        if (const auto* pt = reg.try_get<TransformComponent>(m_PlayerEntity)) camRef = pt->Position;

        for (auto e : view) {
            const auto& d = view.get<TrainingDummyComponent>(e);
            const auto& h = view.get<HealthComponent>(e);
            const auto& t = view.get<TransformComponent>(e);
            if (glm::distance(camRef, t.Position) > 70.0f) continue;

            float topY = 2.0f;
            if (const auto* col = reg.try_get<ColliderComponent>(e)) topY = col->localCenterOffset.y + col->halfExtents.y;
            const glm::vec3 anchor = t.Position + glm::vec3(0.0f, topY + 0.35f, 0.0f);
            const glm::vec4 clip = vp * glm::vec4(anchor, 1.0f);
            if (clip.w <= 0.05f) continue;
            const glm::vec2 sp = WorldToScreen(anchor, vp, m_WindowWidth, m_WindowHeight);

            char line1[96], line2[96], line3[96];
            std::snprintf(line1, sizeof(line1), "%s", d.displayName.c_str());
            if (d.hits > 0) {
                std::snprintf(line2, sizeof(line2), "Ultimo: %.1f  (%s)", d.lastDamage, HitLocationName(d.lastLocation));
                std::snprintf(line3, sizeof(line3), "Totale: %.1f  /  %d colpi", d.totalDamage, d.hits);
            }
            else {
                std::snprintf(line2, sizeof(line2), "Salute %.0f", h.maxHealth);
                line3[0] = '\0';
            }

            const float barW = 130.0f, barH = 8.0f;
            const float w = (std::max)({ barW, ImGui::CalcTextSize(line1).x, ImGui::CalcTextSize(line2).x, ImGui::CalcTextSize(line3).x }) + 12.0f;
            const float lineH = ImGui::GetTextLineHeight();
            const float hBox = 6.0f + lineH + 4.0f + barH + 4.0f + lineH + (line3[0] ? lineH : 0.0f) + 6.0f;
            ImVec2 p0(sp.x - w * 0.5f, sp.y - hBox);
            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + hBox), IM_COL32(10, 12, 16, 170), 4.0f);

            float y = p0.y + 6.0f;
            dl->AddText(ImVec2(p0.x + 6.0f, y), IM_COL32(235, 235, 240, 255), line1);
            y += lineH + 4.0f;

            const float frac = std::clamp(h.currentHealth / (h.maxHealth > 0.0f ? h.maxHealth : 1.0f), 0.0f, 1.0f);
            const glm::vec3 hc = HitEffectivenessColor(h.maxHealth - h.currentHealth, h.maxHealth * 4.0f, d.lethal);
            dl->AddRectFilled(ImVec2(p0.x + 6.0f, y), ImVec2(p0.x + 6.0f + barW, y + barH), IM_COL32(50, 50, 55, 255), 2.0f);
            dl->AddRectFilled(ImVec2(p0.x + 6.0f, y), ImVec2(p0.x + 6.0f + barW * frac, y + barH),
                ImGui::GetColorU32(ImVec4(hc.r, hc.g, hc.b, 1.0f)), 2.0f);
            if (d.lethal) dl->AddText(ImVec2(p0.x + 12.0f + barW, y - 3.0f), IM_COL32(255, 60, 50, 255), "LETALE");
            y += barH + 4.0f;

            dl->AddText(ImVec2(p0.x + 6.0f, y), IM_COL32(200, 205, 215, 255), line2);
            y += lineH;
            if (line3[0]) dl->AddText(ImVec2(p0.x + 6.0f, y), IM_COL32(170, 175, 185, 255), line3);
        }
    }

    // ===================== FEEDBACK DI COLPO =====================
    // Invecchia i marcatori di colpo (e li distrugge a fine vita) e spegne il flash dei blocchi colpiti.
    void GameLayer::UpdateHitFeedback(float deltaTime) {
        auto& reg = m_Scene.GetRegistry();

        std::vector<entt::entity> expired;
        auto markers = reg.view<HitMarkerComponent>();
        for (auto e : markers) {
            auto& m = markers.get<HitMarkerComponent>(e);
            m.age += deltaTime;
            if (reg.valid(m.target)) {
                if (const auto* t = reg.try_get<TransformComponent>(m.target)) m.worldPoint = t->Position + m.localOffset;
            }
            if (m.age >= m_HitMarkerLife) expired.push_back(e);
        }
        for (auto e : expired) if (reg.valid(e)) m_Scene.DestroyEntity(e);

        auto skels = reg.view<SkeletonComponent>();
        for (auto e : skels) {
            for (float& timer : skels.get<SkeletonComponent>(e).blockFlashTimers) {
                if (timer > 0.0f) timer = (std::max)(0.0f, timer - deltaTime);
            }
        }
    }

    // Cerchio sul punto colpito, grande quanto m_HitMarkerWorldRadius sul corpo del bersaglio.
    // Colore = efficacia del colpo (verde poco danno -> rosso colpo pesante); uccisione = croce rossa.
    // Scritte della mischia che salgono (PARATA!, GUARDIA ROTTA!, INTERROTTO!, PUNITO!), solo vicino al giocatore
    void GameLayer::RenderVfxTexts() {
        const auto& texts = m_ParticleSystem.Texts();
        if (texts.empty()) return;
        const glm::mat4 vp = m_Camera.GetViewProjection();
        glm::vec3 me(0.0f);
        if (const auto* pt = m_Scene.GetRegistry().try_get<TransformComponent>(m_PlayerEntity)) me = pt->Position;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (const auto& t : texts) {
            if (glm::distance(t.pos, me) > 30.0f) continue;
            const glm::vec4 clip = vp * glm::vec4(t.pos, 1.0f);
            if (clip.w <= 0.05f) continue;
            const glm::vec2 sp = WorldToScreen(t.pos, vp, m_WindowWidth, m_WindowHeight);
            const float k = std::clamp(t.age / t.life, 0.0f, 1.0f);
            const float pop = k < 0.12f ? 0.7f + 2.5f * k : 1.0f;   // entra con un piccolo scatto
            const int alpha = static_cast<int>(255 * (1.0f - k * k));
            ImFont* font = ImGui::GetFont();
            const float size = ImGui::GetFontSize() * 1.35f * pop;
            const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.text.c_str());
            const ImVec2 pos(sp.x - ts.x * 0.5f, sp.y - ts.y * 0.5f);
            dl->AddText(font, size, ImVec2(pos.x + 2.0f, pos.y + 2.0f), IM_COL32(0, 0, 0, alpha), t.text.c_str());
            dl->AddText(font, size, pos, IM_COL32(static_cast<int>(t.color.r * 255), static_cast<int>(t.color.g * 255), static_cast<int>(t.color.b * 255), alpha), t.text.c_str());
        }
    }

    void GameLayer::RenderHitMarkers() {
        auto& reg = m_Scene.GetRegistry();
        auto markers = reg.view<HitMarkerComponent>();
        if (markers.begin() == markers.end()) return;

        const glm::mat4 vp = m_Camera.GetViewProjection();
        const float screenW = m_WindowWidth, screenH = m_WindowHeight;
        const float tanHalfFov = std::tan(glm::radians(m_Camera.GetFovDegrees()) * 0.5f);
        ImDrawList* dl = ImGui::GetForegroundDrawList();

        for (auto e : markers) {
            const auto& m = markers.get<HitMarkerComponent>(e);
            const glm::vec4 clip = vp * glm::vec4(m.worldPoint, 1.0f);
            if (clip.w <= 0.05f) continue;   // dietro la camera
            const glm::vec2 sp = WorldToScreen(m.worldPoint, vp, screenW, screenH);

            // Raggio in pixel di un cerchio di m_HitMarkerWorldRadius metri alla distanza del colpo
            float radius = m_HitMarkerWorldRadius * screenH / (2.0f * tanHalfFov * clip.w);
            radius = std::clamp(radius, 6.0f, 48.0f);

            const float life = std::clamp(m.age / m_HitMarkerLife, 0.0f, 1.0f);
            const float alpha = 1.0f - life * life;               // svanisce verso la fine
            const float grow = 1.0f + 0.35f * life;               // si allarga leggermente
            const glm::vec3 c = HitEffectivenessColor(m.damage, m.targetMaxHealth, m.killed);
            const ImU32 col = ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, alpha));
            const ImU32 fill = ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, 0.25f * alpha));
            const ImVec2 center(sp.x, sp.y);

            dl->AddCircleFilled(center, radius * grow, fill, 24);
            dl->AddCircle(center, radius * grow, col, 24, 2.5f);
            dl->AddCircleFilled(center, 2.5f, col, 8);
            if (m.killed) {
                const float s = radius * grow * 0.7f;
                dl->AddLine(ImVec2(center.x - s, center.y - s), ImVec2(center.x + s, center.y + s), col, 2.5f);
                dl->AddLine(ImVec2(center.x - s, center.y + s), ImVec2(center.x + s, center.y - s), col, 2.5f);
            }
            else if (m.graze) {   // preso di striscio: danno ridotto
                const ImU32 gc = ImGui::GetColorU32(ImVec4(0.85f, 0.85f, 0.85f, alpha));
                dl->AddText(ImVec2(center.x + radius * grow + 4.0f, center.y - 7.0f), gc, "di striscio");
            }
        }
    }

    // ===================== PERSONAGGIO ANIMATO =====================
    // Stato dei comandi che pilota le animazioni (stessi tasti usati dal movimento).
    RifleAnimInput GameLayer::BuildPlayerAnimInput(bool isAiming, float deltaTime) {
        RifleAnimInput in;
        in.forward = Input::IsKeyPressed(GLFW_KEY_W);
        in.backward = Input::IsKeyPressed(GLFW_KEY_S);
        in.left = Input::IsKeyPressed(GLFW_KEY_A);
        in.right = Input::IsKeyPressed(GLFW_KEY_D);
        in.isMoving = in.forward || in.backward || in.left || in.right;
        // Corsa vera (MovementSystem: SHIFT + avanti con fiato), non il solo tasto premuto
        if (const auto* st = m_Scene.GetRegistry().try_get<UnitStatsComponent>(m_PlayerEntity)) in.sprinting = st->sprinting;
        in.aiming = isAiming;
        m_PlayerAimingNow = isAiming;

        auto& reg = m_Scene.GetRegistry();
        if (reg.valid(m_PlayerEntity)) {
            // Velocita' orizzontale reale (m/s), leggermente filtrata: adatta il passo delle animazioni
            if (auto* pt = reg.try_get<TransformComponent>(m_PlayerEntity)) {
                if (m_HasPrevAnimPlayerPos && deltaTime > 0.0001f) {
                    glm::vec2 d(pt->Position.x - m_PrevAnimPlayerPos.x, pt->Position.z - m_PrevAnimPlayerPos.z);
                    float raw = glm::length(d) / deltaTime;
                    if (raw > 30.0f) raw = 0.0f;   // teletrasporto/respawn: non e' una velocita'
                    m_SmoothedGroundSpeed += (raw - m_SmoothedGroundSpeed) * (1.0f - std::exp(-12.0f * deltaTime));
                }
                m_PrevAnimPlayerPos = pt->Position;
                m_HasPrevAnimPlayerPos = true;
                in.groundSpeed = m_SmoothedGroundSpeed;
            }
            if (auto* vault = reg.try_get<VaultComponent>(m_PlayerEntity)) {
                in.vaulting = vault->isVaulting;
                in.vaultDuration = vault->vaultDuration;
                in.climbing = vault->isVaulting && vault->vaultDuration > 1.2f;   // arrampicata lenta: posa procedurale
            }
            if (auto* crouch = reg.try_get<CrouchComponent>(m_PlayerEntity)) {
                in.crouching = crouch->amount > 0.5f;
                if (in.crouching) in.sprinting = false;
            }
            // Fuoco: solo armi a distanza, grilletto premuto, non in ricarica e con munizioni
            if (auto* wState = reg.try_get<WeaponStateComponent>(m_PlayerEntity)) {
                bool isMelee = false;
                if (auto* wbp = reg.try_get<WeaponBlueprintId>(m_PlayerEntity)) {
                    if (const auto* bp = BlueprintManager::Get().GetWeapon(wbp->id)) isMelee = bp->is_melee || bp->is_equipment;
                }
                in.firing = !isMelee && Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)
                    && !wState->isReloading && wState->currentAmmo > 0;
                // Colpo partito = il cooldown di fuoco e' appena risalito: lancia il rinculo una volta per colpo
                in.shotFired = !isMelee && wState->fireCooldown > m_PrevFireCooldown + 0.0001f;
                m_PrevFireCooldown = wState->fireCooldown;
            }
        }
        return in;
    }

    // Carica un corpo animato alternativo (Characters/<fbxName>) e le sue clip: Animations/<animFolder>/ se
    // la cartella esiste, altrimenti quelle Human (il retarget le adatta alle proporzioni del rig).
    void GameLayer::LoadAnimatedBody(AnimatedBody& body, const std::string& modelsDir, const char* fbxName,
                                     const char* animFolder, const char* label) {
        std::error_code ec;
        // 1. Modello d'autore in Characters/<corpo>/ (AuthoredModel.h): texture dell'artista, niente corpo procedurale
        {
            const std::string folder = modelsDir + "Characters/" + AuthoredFolderOf(label);
            AuthoredCharacterInfo info;
            if (FindAuthoredCharacter(folder, info)) {
                if (LoadAuthoredCharacter(info, body.model, DefaultBodyHeightCm(label))) {
                    body.modelLoaded = true;
                    // clip: quelle della cartella del modello, poi quelle della razza, poi quelle umane
                    const std::string animsDir = FindValidDir("Animations");
                    std::string gameClips;   // clip del gioco per questa razza (o umane)
                    if (!animsDir.empty())
                        gameClips = std::filesystem::is_directory(animsDir + animFolder + "/", ec) ? animsDir + animFolder + "/" : animsDir + "Human/";
                    const std::string clips = !info.animsDir.empty() ? info.animsDir : gameClips;
                    // le clip che mancano nella cartella dell'artista si prendono da quelle del gioco
                    if (!clips.empty()) body.animLoaded = body.controller.LoadClips(clips, info.modelPath, &body.model, info.animsDir.empty() ? std::string() : gameClips);
                    SUN_CORE_INFO("Corpo {0}: modello d'autore {1} (animazioni: {2})", label, info.modelPath, clips);
                    return;
                }
                SUN_CORE_WARN("Corpo {0}: modello d'autore in {1} non caricato, uso quello del gioco", label, folder);
                body.model = SkinnedModel();
            }
        }
        // 2. Corpo del gioco (procedurale sopra il modello base della razza)
        const std::string path = modelsDir + "Characters/" + fbxName;
        if (!std::filesystem::exists(path, ec)) {
            SUN_CORE_WARN("Modello {0} non trovato ({1}): usera' il modello umano", label, path);
            return;
        }
        body.modelLoaded = body.model.LoadModel(path);
        // Corpo piu' preciso: anatomia, mani e piedi del corpo umano base adattati allo scheletro e alla sagoma
        // della razza (la testa resta quella della razza). Vedi BodyFit.h. Solo per ork, aspirante e astartes:
        // ogryn e gretchin restano col loro modello (pancia dell'ogryn e torace del gretchin vengono meglio cosi').
        const std::string bodyLabel = label;
        const bool fitThisBody = bodyLabel == "ork" || bodyLabel == "neofita" || bodyLabel == "astartes";
        if (body.modelLoaded && m_SkinnedModelLoaded && m_FitBodiesFromHuman && fitThisBody && !m_SkinnedTestModel.authored) {
            const BodyFitSkeleton hs = BodyFitSkeletonOf(m_SkinnedTestModel), rs = BodyFitSkeletonOf(body.model);
            std::vector<BodyFitMesh> hm, rm;
            for (const auto& m : m_SkinnedTestModel.meshes) hm.push_back({ &m.vertices, &m.indices });
            for (const auto& m : body.model.meshes) rm.push_back({ &m.vertices, &m.indices });
            std::vector<SkinnedVertex> verts; std::vector<unsigned int> idx;
            BodyFitOptions opt;
            opt.conformSmooth = 20;
            BodyFit(hs, hm, rs, rm, opt, verts, idx);
            if (!idx.empty()) {
                body.model.meshes.clear();
                body.model.meshes.emplace_back(std::move(verts), std::move(idx));
                SUN_CORE_INFO("Corpo {0}: adattato dal corpo umano base", label);
            }
        }
        // Ossa in piu' della razza (orecchie e mascella del gretchin, mascella dell'ork, mascella e pancia
        // dell'ogryn, spallacci dell'astartes): prima delle clip, che le ereditano dalla gerarchia del modello
        if (body.modelLoaded) {
            const int extra = AddExtraBones(body.model, RaceExtraBones(bodyLabel));
            if (extra > 0) SUN_CORE_INFO("Corpo {0}: {1} ossa in piu' (vedi ExtraBones.h)", label, extra);
        }
        // Tono e spessore dei muscoli (MuscleTone.h)
        if (body.modelLoaded) {
            ApplyMuscleTone(body.model, RaceMuscleTone(bodyLabel));
        }
        const std::string animsDir = FindValidDir("Animations");
        if (!body.modelLoaded || animsDir.empty()) return;
        const std::string ownDir = animsDir + animFolder + "/";
        const bool hasOwnClips = std::filesystem::is_directory(ownDir, ec);
        body.animLoaded = body.controller.LoadClips(hasOwnClips ? ownDir : animsDir + "Human/", path, &body.model);
        SUN_CORE_INFO("Modello {0} animato caricato (animazioni: {1})", label, hasOwnClips ? animFolder : "Human");
    }

    // Quale corpo animato usa l'unita' del giocatore:
    //   orks_*                              -> ork (boy e nob)
    //   imperium_ogryn*                     -> ogryn
    //   spacemarines_neophyte / _scout      -> neofita (gli scout sono neofiti)
    //   altri spacemarines_*                -> astartes (battle brother, phobos, gravis)
    //   tutto il resto                      -> guardsman
    // Se il modello dedicato non e' caricato si ricade sul guardsman.
    GameLayer::AnimatedBodyKind GameLayer::CurrentAnimatedBodyKind() {
        const auto* uid = m_Scene.GetRegistry().try_get<UnitBlueprintId>(m_PlayerEntity);
        return BodyKindForUnit(uid ? uid->id : std::string());
    }

    GameLayer::AnimatedBodyKind GameLayer::BodyKindForUnit(const std::string& id) const {
        AnimatedBodyKind kind = AnimatedBodyKind::Human;
        const auto* bp = id.empty() ? nullptr : BlueprintManager::Get().GetUnit(id);
        if (bp && !bp->animated_body.empty()) {
            // Indicato nel blueprint dell'unita' ("animated_body")
            const std::string& b = bp->animated_body;
            if (b == "ork") kind = AnimatedBodyKind::Ork;
            else if (b == "ogryn") kind = AnimatedBodyKind::Ogryn;
            else if (b == "gretchin") kind = AnimatedBodyKind::Gretchin;
            else if (b == "neophyte") kind = AnimatedBodyKind::Neophyte;
            else if (b == "astartes") kind = AnimatedBodyKind::Astartes;
        }
        else if (!id.empty()) {
            // Senza indicazione: dedotto dall'id
            if (id == "orks_gretchin") kind = AnimatedBodyKind::Gretchin;
            else if (id.rfind("orks_", 0) == 0) kind = AnimatedBodyKind::Ork;
            else if (id.rfind("imperium_ogryn", 0) == 0) kind = AnimatedBodyKind::Ogryn;
            else if (id == "spacemarines_neophyte" || id == "spacemarines_scout") kind = AnimatedBodyKind::Neophyte;
            else if (id.rfind("spacemarines_", 0) == 0) kind = AnimatedBodyKind::Astartes;
        }
        return AnimatedBodyReady(kind) ? kind : AnimatedBodyKind::Human;
    }

    bool GameLayer::AnimatedBodyReady(AnimatedBodyKind kind) const {
        if (kind == AnimatedBodyKind::Human || kind == AnimatedBodyKind::Count) return m_SkinnedModelLoaded && m_RifleAnimLoaded;
        const AnimatedBody& b = Body(kind);
        return b.modelLoaded && b.animLoaded;
    }

    SkinnedModel& GameLayer::AnimatedBodyModel(AnimatedBodyKind kind) {
        if (kind == AnimatedBodyKind::Human || kind == AnimatedBodyKind::Count) return m_SkinnedTestModel;
        return Body(kind).model;
    }

    RifleAnimationController& GameLayer::AnimatedBodyController(AnimatedBodyKind kind) {
        if (kind == AnimatedBodyKind::Human || kind == AnimatedBodyKind::Count) return m_RifleAnimController;
        return Body(kind).controller;
    }

    RifleAnimationController& GameLayer::ActiveAnimController() {
        return AnimatedBodyController(CurrentAnimatedBodyKind());
    }

    bool GameLayer::IsAnimatedPlayerActive() {
        auto& reg = m_Scene.GetRegistry();
        return m_UseAnimatedPlayer && AnimatedBodyReady(CurrentAnimatedBodyKind())
            && reg.valid(m_AnimatedPlayerEntity) && reg.valid(m_PlayerEntity)
            && !reg.all_of<CorpseComponent>(m_PlayerEntity);
    }

    // Aggiorna il controller delle animazioni e, se scelto il modello animato, la sua entity visiva:
    // segue il player (posizione + direzione del corpo), nasconde i cubi (che restano hitbox).
    void GameLayer::UpdateAnimatedPlayer(float deltaTime, const RifleAnimInput& input) {
        auto& reg = m_Scene.GetRegistry();

        // Corpo dell'unita' corrente: guardsman, ork (boy/nob) oppure ogryn
        const AnimatedBodyKind kind = CurrentAnimatedBodyKind();
        const bool modelReady = AnimatedBodyReady(kind);
        RifleAnimationController& controller = AnimatedBodyController(kind);
        if (modelReady) {
            BuildPlayerPose(deltaTime, controller);   // azioni senza clip, sopra la clip in corso
            controller.SetProceduralPose(&m_PlayerPose);
            controller.Update(deltaTime, input);
            if (deltaTime > 0.0f) m_PlayerPoseFresh = true;
        }

        const bool playerOk = reg.valid(m_PlayerEntity) && reg.all_of<TransformComponent>(m_PlayerEntity);
        const bool wantAnimated = m_UseAnimatedPlayer && modelReady
            && playerOk && !reg.all_of<CorpseComponent>(m_PlayerEntity);

        if (!wantAnimated) {
            // Giocatore morto: il suo corpo con le ossa resta a terra e segue il ragdoll (prima tornavano i cubi)
            if (m_UseAnimatedPlayer && playerOk && reg.all_of<CorpseComponent>(m_PlayerEntity)) {
                if (reg.valid(m_AnimatedPlayerEntity) && MakeSkinnedCorpse(m_PlayerEntity, m_AnimatedPlayerEntity)) {
                    m_AnimatedPlayerEntity = entt::null;
                    m_AnimatedBodyAssigned = false;
                }
                if (m_SkinnedCorpses.count(m_PlayerEntity)) return;
            }
            // Modalita' cuboni: niente modello animato
            if (reg.valid(m_AnimatedPlayerEntity)) {
                m_Scene.DestroyEntity(m_AnimatedPlayerEntity);
                m_AnimatedPlayerEntity = entt::null;
            }
            if (playerOk && reg.all_of<HideProceduralVisualTag>(m_PlayerEntity)) {
                reg.remove<HideProceduralVisualTag>(m_PlayerEntity);
            }
            return;
        }

        if (!reg.valid(m_AnimatedPlayerEntity)) {
            m_AnimatedPlayerEntity = m_Scene.CreateEntity();
            reg.emplace<TransformComponent>(m_AnimatedPlayerEntity);
            reg.emplace<ColorComponent>(m_AnimatedPlayerEntity);   // richiesto dal render deferred
            reg.emplace<SkinnedModelInstanceComponent>(m_AnimatedPlayerEntity);
            m_AnimatedBodyAssigned = false;   // forza l'assegnazione del modello qui sotto
        }
        if (!m_AnimatedBodyAssigned || m_AnimatedBodyInUse != kind) {
            // Cambio di unita' (es. guardsman -> ogryn nel garage): stessa entity, altro modello
            auto& inst = reg.get<SkinnedModelInstanceComponent>(m_AnimatedPlayerEntity);
            inst.sourceModel = &AnimatedBodyModel(kind);
            inst.boneMatrices = &controller.GetFinalBoneMatrices();
            m_AnimatedBodyInUse = kind;
            m_AnimatedBodyAssigned = true;
        }
        if (!reg.all_of<HideProceduralVisualTag>(m_PlayerEntity)) {
            reg.emplace<HideProceduralVisualTag>(m_PlayerEntity);
        }

        const auto& pt = reg.get<TransformComponent>(m_PlayerEntity);
        auto& vt = reg.get<TransformComponent>(m_AnimatedPlayerEntity);
        vt.Position = pt.Position;   // Position del player = piedi, come l'origine del modello
        vt.Rotation = glm::vec3(0.0f, pt.Rotation.y + m_AnimatedYawOffset, 0.0f);
        // Scala del modello dal blueprint ("model_scale"): il nob e' il modello dell'ork boy a 1.2, lo yoof a 0.85.
        // Gli altri corpi hanno gia' la loro altezza nel file: gretchin ~0,9 m, neofita ~1,95 m, astartes ~2,2 m,
        // ogryn ~2,75 m.
        float unitScale = 1.0f;
        if (const auto* uid = reg.try_get<UnitBlueprintId>(m_PlayerEntity)) {
            if (const auto* ubp = BlueprintManager::Get().GetUnit(uid->id); ubp && ubp->model_scale > 0.05f) unitScale = ubp->model_scale;
        }
        vt.Scale = glm::vec3(m_AnimatedModelScale * unitScale * AnimatedBodyModel(kind).unitScale);   // unitScale: modelli d'autore in m/mm

        // Colore armatura scelto nel garage come albedo del modello
        if (auto* pc = reg.try_get<ColorComponent>(m_PlayerEntity)) {
            reg.get<SkinnedModelInstanceComponent>(m_AnimatedPlayerEntity).albedo = pc->BaseColor;
        }
    }

    // Pezzi della skin (elmetto, vesti) sul modello animato del giocatore: un cubo per pezzo, sull'osso
    // indicato ("bone") + offset negli assi del corpo. Segue la posizione dell'osso e la direzione del corpo
    // (non ruota con la testa o le gambe: va bene finche' le animazioni sono semplici).
    void GameLayer::UpdateAnimatedSkin() {
        auto& reg = m_Scene.GetRegistry();
        std::vector<const SkinPart*> parts;
        // In partita e nel garage (dove il personaggio animato gira su se' stesso)
        if ((m_GameState == GameState::Gameplay || m_GameState == GameState::MainMenu) && IsAnimatedPlayerActive() &&
            !AnimatedBodyModel(CurrentAnimatedBodyKind()).authored) {   // modello d'autore: i pezzi sono gia' nel modello
            if (const auto* skin = reg.try_get<SkinComponent>(m_PlayerEntity))
                for (const auto& p : skin->parts) if (!p.bone.empty()) parts.push_back(&p);
        }
        // Strati (vestiti, armature) disegnati sopra il corpo animato con le sue stesse ossa
        if (reg.valid(m_AnimatedPlayerEntity) && reg.all_of<SkinnedModelInstanceComponent>(m_AnimatedPlayerEntity)) {
            auto& inst = reg.get<SkinnedModelInstanceComponent>(m_AnimatedPlayerEntity);
            ApplySkinStyle(inst, static_cast<int>(m_AnimatedBodyInUse));
            inst.garments.clear();
            const auto* skin = reg.try_get<SkinComponent>(m_PlayerEntity);
            if (skin && inst.sourceModel && (m_GameState == GameState::Gameplay || m_GameState == GameState::MainMenu)) {
                for (const auto& d : skin->garments) {
                    GarmentDraw draw;
                    if (m_Garments.Get(*inst.sourceModel, d, draw)) inst.garments.push_back(draw);
                }
            }
        }
        while (m_SkinPartEntities.size() > parts.size()) {
            if (reg.valid(m_SkinPartEntities.back())) m_Scene.DestroyEntity(m_SkinPartEntities.back());
            m_SkinPartEntities.pop_back();
        }
        while (m_SkinPartEntities.size() < parts.size()) {
            const entt::entity e = m_Scene.CreateEntity();
            reg.emplace<TransformComponent>(e);
            reg.emplace<MeshComponent>(e, 0u);
            reg.emplace<ColorComponent>(e);
            m_SkinPartEntities.push_back(e);
        }
        if (parts.empty()) return;

        const auto* vt = reg.try_get<TransformComponent>(m_AnimatedPlayerEntity);
        const auto* pt = reg.try_get<TransformComponent>(m_PlayerEntity);
        if (!vt || !pt) return;
        float unitScale = 1.0f;
        if (const auto* uid = reg.try_get<UnitBlueprintId>(m_PlayerEntity))
            if (const auto* ubp = BlueprintManager::Get().GetUnit(uid->id); ubp && ubp->model_scale > 0.05f) unitScale = ubp->model_scale;
        const glm::mat4 yawRot = glm::rotate(glm::mat4(1.0f), glm::radians(pt->Rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        for (size_t i = 0; i < parts.size(); ++i) {
            const entt::entity e = m_SkinPartEntities[i];
            auto& t = reg.get<TransformComponent>(e);
            glm::mat4 bone(1.0f);
            if (!ActiveAnimController().GetBoneModelTransform(parts[i]->bone, bone)) { t.Scale = glm::vec3(0.0f); continue; }
            const glm::vec3 bonePos = glm::vec3((vt->GetTransform() * bone)[3]);
            t.Position = bonePos + glm::vec3(yawRot * glm::vec4(parts[i]->boneOffset * unitScale, 0.0f));
            t.Rotation = glm::vec3(0.0f, pt->Rotation.y, 0.0f);
            t.Scale = parts[i]->size * unitScale;
            auto& c = reg.get<ColorComponent>(e);
            c.Color = parts[i]->color; c.BaseColor = c.Color;
        }
    }

    // Posiziona un'arma (cubo) sulla mano destra/sinistra del modello animato. Posizione = osso della
    // mano + offset (destra/su/avanti rispetto al corpo); orientamento = direzione del corpo con
    // inclinazione di mira, oppure quello della mano (m_WeaponFollowsHand). Tutto tarabile dalle Opzioni.
    bool GameLayer::PlaceWeaponOnAnimatedHand(bool leftHand, TransformComponent& weaponTransform, const BlockModel* model) {
        auto& reg = m_Scene.GetRegistry();
        glm::mat4 bone(1.0f);
        const char* boneName = leftHand ? "mixamorig1:LeftHand" : "mixamorig1:RightHand";
        if (!ActiveAnimController().GetBoneModelTransform(boneName, bone)) return false;

        auto* vt = reg.try_get<TransformComponent>(m_AnimatedPlayerEntity);
        auto* pt = reg.try_get<TransformComponent>(m_PlayerEntity);
        if (!vt || !pt) return false;

        const glm::mat4 handWorld = vt->GetTransform() * bone;
        const glm::vec3 handPos = glm::vec3(handWorld[3]);

        const float yawRad = glm::radians(pt->Rotation.y);
        const glm::vec3 right(std::cos(yawRad), 0.0f, -std::sin(yawRad));
        const glm::vec3 fwd(-std::sin(yawRad), 0.0f, -std::cos(yawRad));
        // Modello a blocchi con impugnatura: l'arma si orienta lungo la linea mano destra -> mano sinistra
        // (impugnatura e guardamano), cosi' "sta in braccio" al soldato com'e' nell'animazione. Se il
        // modello non ha l'appoggio sinistro, o l'arma e' a una mano, segue il corpo (direzione + mira).
        if (model && model->hasGripRight) {
            // Direzione di mira = la stessa che WeaponSystem usa per il colpo (AimYaw / AimPitch)
            glm::mat4 aimRot = glm::rotate(glm::mat4(1.0f), glm::radians(pt->AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
            aimRot = glm::rotate(aimRot, glm::radians(pt->AimPitch * m_WeaponPitchFactor), glm::vec3(1.0f, 0.0f, 0.0f));
            const glm::vec3 aimDir = glm::normalize(glm::vec3(aimRot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

            // L'arma e' DRITTA lungo la mira (prima seguiva la linea mano dx -> mano sx, che nelle clip
            // Mixamo e' inclinata di ~25 gradi verso il centro: per questo il fucile risultava storto).
            // Per le armi a due mani il punto medio delle due prese cade tra le due mani.
            glm::vec3 anchorWorld = handPos;
            glm::vec3 anchorLocal = model->gripRight;
            glm::vec3 barrelDir = aimDir;
            glm::mat4 boneL(1.0f);
            if (!leftHand && model->hasGripLeft && ActiveAnimController().GetBoneModelTransform("mixamorig1:LeftHand", boneL)) {
                const glm::vec3 leftPos = glm::vec3((vt->GetTransform() * boneL)[3]);
                const glm::vec3 handLine = leftPos - handPos;
                if (glm::length(handLine) > 1e-4f && m_WeaponHandAlign > 0.0f) {
                    barrelDir = glm::normalize(glm::mix(aimDir, glm::normalize(handLine), m_WeaponHandAlign));
                }
                anchorWorld = (handPos + leftPos) * 0.5f;
                anchorLocal = (model->gripRight + model->gripLeft) * 0.5f;
            }

            // Base ortonormale: il modello ha la canna verso -Z, +Y alto, +X destra
            const glm::vec3 zAxis = -barrelDir;
            glm::vec3 xAxis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), zAxis);
            if (glm::length(xAxis) < 1e-3f) xAxis = right;
            xAxis = glm::normalize(xAxis);
            const glm::vec3 yAxis = glm::cross(zAxis, xAxis);

            glm::mat4 basis(1.0f);
            basis[0] = glm::vec4(xAxis, 0.0f);
            basis[1] = glm::vec4(yAxis, 0.0f);
            basis[2] = glm::vec4(zAxis, 0.0f);
            basis = basis * glm::eulerAngleYXZ(glm::radians(m_WeaponModelRotation.y),
                glm::radians(m_WeaponModelRotation.x), glm::radians(m_WeaponModelRotation.z));

            weaponTransform.Position = anchorWorld - glm::vec3(basis * glm::vec4(anchorLocal + m_WeaponModelGripOffset, 0.0f));
            float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
            glm::extractEulerAngleYXZ(basis, yaw, pitch, roll);   // stesso ordine di TransformComponent::GetTransform
            weaponTransform.Rotation = glm::degrees(glm::vec3(pitch, yaw, roll));
            return true;
        }

        glm::vec3 off = m_HandWeaponOffset;
        if (leftHand) off.x = -off.x;   // specchio per la mano sinistra
        weaponTransform.Position = handPos + right * off.x + glm::vec3(0.0f, 1.0f, 0.0f) * off.y + fwd * off.z;

        glm::vec3 rot;
        if (m_WeaponFollowsHand) {
            glm::mat4 R(1.0f);
            R[0] = glm::vec4(glm::normalize(glm::vec3(handWorld[0])), 0.0f);
            R[1] = glm::vec4(glm::normalize(glm::vec3(handWorld[1])), 0.0f);
            R[2] = glm::vec4(glm::normalize(glm::vec3(handWorld[2])), 0.0f);
            float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
            glm::extractEulerAngleYXZ(R, yaw, pitch, roll);   // stesso ordine di TransformComponent::GetTransform
            rot = glm::degrees(glm::vec3(pitch, yaw, roll));
        }
        else {
            rot = glm::vec3(pt->AimPitch * m_WeaponPitchFactor, pt->Rotation.y, 0.0f);
        }
        weaponTransform.Rotation = rot + m_HandWeaponRotation;
        return true;
    }

    // FIX #8: logica di aggancio arma->scheletro estratta da OnUpdate. Chiamata sia nel ramo Gameplay
    // sia nel MainMenu (garage): prima in garage l'arma restava all'origine perche' la sync stava
    // DOPO i return di MapEditor/MainMenu.
    // FIX #10: se il player e' cadavere (ragdoll) l'arma viene nascosta (Scale 0): lo scheletro e'
    // congelato mentre i cubi del ragdoll si muovono, quindi l'arma altrimenti resterebbe a mezz'aria.
    void GameLayer::SyncWeaponAttachment() {
        auto* playerTransform = m_Scene.GetRegistry().try_get<TransformComponent>(m_PlayerEntity);
        if (!playerTransform || !m_Scene.GetRegistry().all_of<InventoryComponent>(m_PlayerEntity)) return;
        auto& transform = *playerTransform;
        // Bocca della canna reale (la riempiono i rami sotto): WeaponSystem la usa come origine del colpo
        if (!m_Scene.GetRegistry().all_of<MuzzleComponent>(m_PlayerEntity)) {
            m_Scene.GetRegistry().emplace<MuzzleComponent>(m_PlayerEntity);
        }
        auto& muzzleComp = m_Scene.GetRegistry().get<MuzzleComponent>(m_PlayerEntity);
        muzzleComp = MuzzleComponent{};

        if (m_Scene.GetRegistry().valid(m_PlayerEntity)) {
            if (!m_Scene.GetRegistry().valid(m_LeftWeaponEntity)) {
                m_LeftWeaponEntity = m_Scene.CreateEntity();
                m_Scene.GetRegistry().emplace<TransformComponent>(m_LeftWeaponEntity);
                m_Scene.GetRegistry().emplace<MeshComponent>(m_LeftWeaponEntity, 0u);
                auto& c = m_Scene.GetRegistry().emplace<ColorComponent>(m_LeftWeaponEntity);
                c.Color = glm::vec4(1.0f);
                c.BaseColor = glm::vec4(1.0f);
            }

            // FIX #10: durante il ragdoll niente arma agganciata allo scheletro congelato
            if (m_Scene.GetRegistry().all_of<CorpseComponent>(m_PlayerEntity)) {
                if (m_Scene.GetRegistry().valid(m_WeaponEntity)) {
                    if (auto* wt = m_Scene.GetRegistry().try_get<TransformComponent>(m_WeaponEntity)) wt->Scale = glm::vec3(0.0f);
                }
                if (auto* wtl = m_Scene.GetRegistry().try_get<TransformComponent>(m_LeftWeaponEntity)) wtl->Scale = glm::vec3(0.0f);
                return;
            }

            auto& inv = m_Scene.GetRegistry().get<InventoryComponent>(m_PlayerEntity);
            EquipStance currentStance = inv.GetEquipStance();

            std::string rightWeaponId = "";
            std::string leftWeaponId = "";
            // Prese dell'arma nel mondo per l'IK delle mani (riempite quando l'arma e' sul corpo animato)
            glm::vec3 ikGripR(0.0f), ikGripL(0.0f);
            bool ikR = false, ikL = false;
            auto weaponPoint = [](const TransformComponent& w, const glm::vec3& local) {
                glm::mat4 r = glm::rotate(glm::mat4(1.0f), glm::radians(w.Rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
                r = glm::rotate(r, glm::radians(w.Rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
                r = glm::rotate(r, glm::radians(w.Rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
                return w.Position + glm::vec3(r * glm::vec4(local, 0.0f));
            };

            if (currentStance == EquipStance::HybridRangedMelee) {
                rightWeaponId = inv.meleeWeaponId;
                leftWeaponId = inv.secondaryWeaponId;
            }
            else if (currentStance == EquipStance::DualRangedOneHanded) {
                rightWeaponId = inv.primaryWeaponId;
                leftWeaponId = inv.secondaryWeaponId;
            }
            else {
                rightWeaponId = inv.GetActiveWeaponId();
                leftWeaponId = "";
            }

            if (m_Scene.GetRegistry().valid(m_WeaponEntity) && !rightWeaponId.empty()) {
                auto& wTransR = m_Scene.GetRegistry().get<TransformComponent>(m_WeaponEntity);
                const auto* wBPR = BlueprintManager::Get().GetWeapon(rightWeaponId);

                if (wBPR) {
                    // Modello animato: arma sull'osso della mano destra (i cubi del corpo restano hitbox)
                    const BlockModel* rightModel = m_BlockModels.Find(rightWeaponId);
                    const bool placedOnHand = IsAnimatedPlayerActive() && PlaceWeaponOnAnimatedHand(false, wTransR, rightModel);
                    if (placedOnHand && rightModel && rightModel->hasGripRight) {
                        ikGripR = weaponPoint(wTransR, rightModel->gripRight); ikR = true;
                        if (rightModel->hasGripLeft && leftWeaponId.empty()) { ikGripL = weaponPoint(wTransR, rightModel->gripLeft); ikL = true; }
                    }
                    if (placedOnHand) {
                        glm::vec3 scale = wBPR->weapon_scale;
                        if (scale.x == scale.y && scale.y == scale.z && scale.x > 0.3f) {
                            scale = wBPR->is_melee ? glm::vec3(0.14f, 0.32f, 1.10f) : glm::vec3(0.08f, 0.18f, 0.80f);
                        }
                        wTransR.Scale = scale;
                    }
                    else if (m_Scene.GetRegistry().all_of<SkeletonComponent>(m_PlayerEntity)) {
                        auto& skel = m_Scene.GetRegistry().get<SkeletonComponent>(m_PlayerEntity);
                        auto worldTransforms = skel.data.ComputeWorldTransforms(transform.Position, transform.Rotation, skel.scale);

                        int armRIndex = -1;
                        for (size_t i = 0; i < skel.data.blocks.size(); ++i) {
                            if (skel.data.blocks[i].name == "Arm_R") { armRIndex = static_cast<int>(i); break; }
                        }

                        if (armRIndex >= 0 && armRIndex < static_cast<int>(worldTransforms.size())) {
                            glm::mat4 armWorldMat = worldTransforms[armRIndex];
                            glm::mat4 weaponWorldMat = glm::translate(armWorldMat, wBPR->hold_offset);

                            wTransR.Position = glm::vec3(weaponWorldMat[3]);
                            glm::quat armQuat = glm::quat_cast(weaponWorldMat);
                            wTransR.Rotation = glm::degrees(glm::eulerAngles(armQuat));

                            glm::vec3 scale = wBPR->weapon_scale;
                            if (scale.x == scale.y && scale.y == scale.z && scale.x > 0.3f) {
                                scale = wBPR->is_melee ? glm::vec3(0.14f, 0.32f, 1.10f) : glm::vec3(0.08f, 0.18f, 0.80f);
                            }
                            wTransR.Scale = scale;
                        }
                    }

                    // Modello a blocchi dell'arma (se esiste): sostituisce il cubo, scala 1 = metri reali
                    SetWeaponBlockModel(m_WeaponEntity, rightWeaponId);
                    if (const BlockModel* mm = m_BlockModels.Find(rightWeaponId); mm && mm->hasMuzzle) {
                        muzzleComp.right = glm::vec3(wTransR.GetTransform() * glm::vec4(mm->muzzle, 1.0f));
                        muzzleComp.hasRight = true;
                    }

                    if (m_Scene.GetRegistry().all_of<ColorComponent>(m_WeaponEntity)) {
                        auto& wColor = m_Scene.GetRegistry().get<ColorComponent>(m_WeaponEntity);
                        glm::vec4 col = wBPR->primary_color;
                        if (glm::length(glm::vec3(col)) < 0.05f) col = wBPR->is_melee ? glm::vec4(0.85f, 0.75f, 0.2f, 1.0f) : glm::vec4(0.8f, 0.2f, 0.2f, 1.0f);
                        wColor.Color = col;
                        wColor.BaseColor = col;
                    }
                }
            }

            auto& wTransL = m_Scene.GetRegistry().get<TransformComponent>(m_LeftWeaponEntity);
            if (!leftWeaponId.empty()) {
                const auto* wBPL = BlueprintManager::Get().GetWeapon(leftWeaponId);
                if (wBPL) {
                    const BlockModel* leftModel = m_BlockModels.Find(leftWeaponId);
                    const bool placedOnHandL = IsAnimatedPlayerActive() && PlaceWeaponOnAnimatedHand(true, wTransL, leftModel);
                    if (placedOnHandL && leftModel && leftModel->hasGripRight) { ikGripL = weaponPoint(wTransL, leftModel->gripRight); ikL = true; }
                    if (placedOnHandL) {
                        glm::vec3 scale = wBPL->weapon_scale;
                        if (scale.x == scale.y && scale.y == scale.z && scale.x > 0.3f) {
                            scale = glm::vec3(0.06f, 0.14f, 0.30f);
                        }
                        wTransL.Scale = scale;
                    }
                    else if (m_Scene.GetRegistry().all_of<SkeletonComponent>(m_PlayerEntity)) {
                        auto& skel = m_Scene.GetRegistry().get<SkeletonComponent>(m_PlayerEntity);
                        auto worldTransforms = skel.data.ComputeWorldTransforms(transform.Position, transform.Rotation, skel.scale);

                        int armLIndex = -1;
                        for (size_t i = 0; i < skel.data.blocks.size(); ++i) {
                            if (skel.data.blocks[i].name == "Arm_L") { armLIndex = static_cast<int>(i); break; }
                        }

                        if (armLIndex >= 0 && armLIndex < static_cast<int>(worldTransforms.size())) {
                            glm::mat4 armWorldMat = worldTransforms[armLIndex];

                            glm::vec3 localOffset = wBPL->hold_offset;
                            localOffset.x = -localOffset.x;
                            glm::mat4 weaponWorldMat = glm::translate(armWorldMat, localOffset);

                            wTransL.Position = glm::vec3(weaponWorldMat[3]);
                            glm::quat armQuat = glm::quat_cast(weaponWorldMat);
                            wTransL.Rotation = glm::degrees(glm::eulerAngles(armQuat));

                            glm::vec3 scale = wBPL->weapon_scale;
                            if (scale.x == scale.y && scale.y == scale.z && scale.x > 0.3f) {
                                scale = glm::vec3(0.06f, 0.14f, 0.30f);
                            }
                            wTransL.Scale = scale;
                        }
                    }

                    SetWeaponBlockModel(m_LeftWeaponEntity, leftWeaponId);
                    if (const BlockModel* mm = m_BlockModels.Find(leftWeaponId); mm && mm->hasMuzzle) {
                        muzzleComp.left = glm::vec3(wTransL.GetTransform() * glm::vec4(mm->muzzle, 1.0f));
                        muzzleComp.hasLeft = true;
                    }

                    if (m_Scene.GetRegistry().all_of<ColorComponent>(m_LeftWeaponEntity)) {
                        auto& wColorL = m_Scene.GetRegistry().get<ColorComponent>(m_LeftWeaponEntity);
                        glm::vec4 col = wBPL->primary_color;
                        if (glm::length(glm::vec3(col)) < 0.05f) col = glm::vec4(0.8f, 0.2f, 0.2f, 1.0f);
                        wColorL.Color = col;
                        wColorL.BaseColor = col;
                    }
                }
            }
            else {
                wTransL.Scale = glm::vec3(0.0f);
            }

            // Mani sulle impugnature (una volta per posa nuova)
            if (m_PlayerPoseFresh && (ikR || ikL)) {
                if (auto* vt = m_Scene.GetRegistry().try_get<TransformComponent>(m_AnimatedPlayerEntity))
                    ApplyHandIK(ActiveAnimController(), m_PlayerPose, *vt, ikR ? &ikGripR : nullptr, ikL ? &ikGripL : nullptr);
            }
            m_PlayerPoseFresh = false;
        }

    }

    void GameLayer::OnUpdate(float deltaTime)
    {
        // Tempi per il pannello delle prestazioni (F3): fotogramma intero e logica di gioco
        {
            const double now = glfwGetTime();
            if (m_PerfFrameStart > 0.0) m_PerfFrameMs = m_PerfFrameMs * 0.9f + static_cast<float>((now - m_PerfFrameStart) * 1000.0) * 0.1f;
            m_PerfFrameStart = now;
            m_PerfUpdateStart = now;
            const bool f3 = Input::IsKeyPressed(GLFW_KEY_F3);
            if (f3 && !m_F3WasDown) m_ShowPerf = !m_ShowPerf;
            m_F3WasDown = f3;
            m_DeferredRenderer.SetGpuProfiling(m_ShowPerf);
            // sincronia verticale (opzioni grafiche)
            const int vs = m_DeferredRenderer.GetQuality().vsync ? 1 : 0;
            if (vs != m_AppliedVSync && glfwGetCurrentContext()) { glfwSwapInterval(vs); m_AppliedVSync = vs; }
        }
        m_DeferredRenderer.SetFrameDelta(deltaTime);
        GLFWwindow* window = glfwGetCurrentContext();
        UpdateWindowSize();
        // Entrata o uscita da una partita: si riparte puliti
        if (m_GameState != m_PrevGameState) {
            if (m_PrevGameState == GameState::Gameplay && m_Net.active) NetLeaveMatch();   // uscito dalla partita in rete (qualunque strada)
            if (m_PrevGameState == GameState::Gameplay || m_GameState == GameState::Gameplay) ResetMatchState(m_PrevGameState == GameState::Gameplay);
            m_PrevGameState = m_GameState;
        }

        // Autosave del profilo ogni 2 secondi (scrive solo se qualcosa e' cambiato)
        UpdateMusic(deltaTime);
        m_ProfileSaveTimer += deltaTime;
        if (m_ProfileSaveTimer >= 2.0f) {
            m_ProfileSaveTimer = 0.0f;
            SaveProfileIfChanged();
        }
        static bool escPressedLastFrame = false;
        bool escPressed = Input::IsKeyPressed(GLFW_KEY_ESCAPE);
        if (escPressed && !escPressedLastFrame) {
            if (m_GameState == GameState::Gameplay && !(m_VsBotActive && m_VsBotResult != 0)) {
                m_IsPaused = !m_IsPaused;
                if (window) {
                    glfwSetInputMode(window, GLFW_CURSOR, m_IsPaused ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                }
            }
        }
        escPressedLastFrame = escPressed;

        // Sensazione di gioco: consuma gli eventi di spari, impatti, esplosioni, mischia e passi del fotogramma
        // precedente (suoni, scossoni, lampi, bossoli, segni). Usa il tempo vero; poi il fermo-immagine dei colpi
        // di mischia rallenta il resto dell'aggiornamento per qualche centesimo di secondo.
        {
            const bool inMatch = m_GameState == GameState::Gameplay;
            const glm::vec3 camPos = glm::vec3(glm::inverse(m_Camera.GetView())[3]);
            m_Feel.Update(deltaTime, m_IsPaused || !inMatch, m_Scene, inMatch ? m_PlayerEntity : entt::entity(entt::null), camPos,
                [this](entt::entity e) {
                    if (e == m_PlayerEntity) return static_cast<int>(CurrentAnimatedBodyKind());
                    const auto* uid = m_Scene.GetRegistry().try_get<UnitBlueprintId>(e);
                    return static_cast<int>(BodyKindForUnit(uid ? uid->id : std::string()));
                });
            UpdateParty(deltaTime);   // squadra e lobby: col tempo vero, anche nei menu
            if (inMatch && !m_IsPaused) deltaTime *= m_Feel.TimeScale();
        }

        // Fuori dalla partita niente squig in giro (garage, editor)
        if (m_GameState != GameState::Gameplay) {
            XpEventLog().clear();   // eventi rimasti fuori partita: non contano
            m_Feel.Reset();
            m_SquigSystem.Clear(m_Scene);
            m_ExplosiveSystem.Clear(m_Scene);
            const bool showcase = m_GameState == GameState::MainMenu && m_CurrentTab == MenuTab::Showcase;
            if (!showcase) m_ParticleSystem.Clear();   // la vetrina usa gli effetti (lampi, scie dei fendenti)
            m_TurretSystem.Clear(m_Scene);
            ClearSummons();
            if (!m_UnitAnims.empty()) ClearUnitAnimations();
            if (m_GameState != GameState::MainMenu) UpdateAnimatedSkin();   // editor: via i pezzi della skin
            // Barricate e zaini di cibo appartengono alla partita
            std::vector<entt::entity> leftovers;
            for (auto e : m_Scene.GetRegistry().view<BarricadeTag>()) leftovers.push_back(e);
            for (auto e : m_Scene.GetRegistry().view<HealZoneComponent>()) leftovers.push_back(e);
            for (auto e : m_Scene.GetRegistry().view<AmmoCrateComponent>()) leftovers.push_back(e);
            for (auto e : leftovers) if (m_Scene.GetRegistry().valid(e)) m_Scene.DestroyEntity(e);
        }

        if (m_GameState == GameState::MapEditor) {
            if (Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) {
                glm::vec2 mouseDelta = Input::GetMouseDelta();
                m_EditorCameraYaw -= mouseDelta.x * 0.3f;
                m_EditorCameraPitch -= mouseDelta.y * 0.3f;
                m_EditorCameraPitch = std::clamp(m_EditorCameraPitch, -89.0f, 89.0f);
            }

            float moveSpeed = 20.0f * deltaTime;
            if (Input::IsKeyPressed(GLFW_KEY_LEFT_SHIFT)) moveSpeed *= 3.0f;

            glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(m_EditorCameraYaw), glm::vec3(0.0f, 1.0f, 0.0f));
            glm::vec3 forward = glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
            glm::vec3 right = glm::vec3(rot * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));

            if (Input::IsKeyPressed(GLFW_KEY_W) && Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_EditorCameraTarget += forward * moveSpeed;
            if (Input::IsKeyPressed(GLFW_KEY_S) && Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_EditorCameraTarget -= forward * moveSpeed;
            if (Input::IsKeyPressed(GLFW_KEY_A) && Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_EditorCameraTarget -= right * moveSpeed;
            if (Input::IsKeyPressed(GLFW_KEY_D) && Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_EditorCameraTarget += right * moveSpeed;

            m_Camera.SetViewportSize(m_WindowWidth, m_WindowHeight);
            m_Camera.SetCameraOffsets(0.0f, 0.0f, m_EditorCameraDistance);
            m_Camera.SetTarget(m_EditorCameraTarget, m_EditorCameraYaw);
            m_Camera.SetPitch(m_EditorCameraPitch);
            m_Camera.Update(glm::vec3(0.0f), 1.0f);

            Renderer::SetViewProjection(m_Camera.GetViewProjection());

            HandleEditor3DInteraction(deltaTime);
            return;
        }

        auto& transform = m_Scene.GetRegistry().get<TransformComponent>(m_PlayerEntity);

        // Vetrina: via gli attori quando si esce
        if (!(m_GameState == GameState::MainMenu && m_CurrentTab == MenuTab::Showcase) && !m_ShowcaseActors.empty()) ClearShowcase();
        if (m_GameState == GameState::MainMenu && m_CurrentTab == MenuTab::Showcase) {
            UpdateShowcase(deltaTime);
            return;
        }
        if (m_GameState == GameState::MainMenu && m_ShowCustomizeModal && m_CurrentTab == MenuTab::StasisVault) {
            // Personalizzazione: camera dedicata sul personaggio (a destra del pannello), da trascinare col mouse
            ImGuiIO& io = ImGui::GetIO();
            const bool overUi = io.WantCaptureMouse;
            if (!overUi && ImGui::IsMouseDown(ImGuiMouseButton_Left)) { m_CustomizeYaw -= io.MouseDelta.x * 0.4f; m_CustomizeAutoRotate = false; }
            if (!overUi && io.MouseWheel != 0.0f) m_CustomizeZoom = std::clamp(m_CustomizeZoom * (1.0f - io.MouseWheel * 0.1f), 0.35f, 1.8f);
            if (m_CustomizeAutoRotate) m_CustomizeYaw += deltaTime * 18.0f;
            float h = 1.8f;
            if (const auto* col = m_Scene.GetRegistry().try_get<ColliderComponent>(m_PlayerEntity)) h = std::max(0.6f, col->halfExtents.y * 2.0f * transform.Scale.y);
            const bool faceFocus = false;   // (inquadratura del viso: verra' con i volti)
            const glm::vec3 wantTarget = transform.Position + glm::vec3(0.0f, faceFocus ? h * 0.9f : h * 0.52f, 0.0f);
            const float wantDist = (faceFocus ? std::max(0.55f, h * 0.42f) : h * 1.55f) * m_CustomizeZoom;
            const float k = 1.0f - std::exp(-8.0f * deltaTime);
            if (!m_CustomizeCamInit) { m_CustomizeCamTarget = wantTarget; m_CustomizeCamDist = wantDist; m_CustomizeCamInit = true; }
            m_CustomizeCamTarget = glm::mix(m_CustomizeCamTarget, wantTarget, k);
            m_CustomizeCamDist = glm::mix(m_CustomizeCamDist, wantDist, k);
            m_Camera.SetViewportSize(m_WindowWidth, m_WindowHeight);
            m_Camera.SetCameraOffsets(-m_CustomizeCamDist * 0.3f, 0.0f, m_CustomizeCamDist);   // personaggio a destra del pannello
            m_Camera.SetTarget(m_CustomizeCamTarget, m_CustomizeYaw);
            m_Camera.SetPitch(faceFocus ? -3.0f : -8.0f);
            m_Camera.SetYawOffset(0.0f);
            m_Camera.Update(glm::vec3(0.0f), 1.0f);
            Renderer::SetViewProjection(m_Camera.GetViewProjection());

            RifleAnimInput in;
            if (m_CustomizePreviewAnim == 1) in.aiming = true;
            else if (m_CustomizePreviewAnim >= 2) { in.isMoving = true; in.forward = true; in.sprinting = m_CustomizePreviewAnim == 3; in.groundSpeed = m_CustomizePreviewAnim == 3 ? 5.5f : 2.0f; }
            UpdateAnimatedPlayer(deltaTime, in);
            UpdateAnimatedSkin();
            SyncWeaponAttachment();
            return;
        }
        m_CustomizeCamInit = false;
        if (m_GameState == GameState::MainMenu) {
            m_GarageCameraAngle += deltaTime * 25.0f;
            if (m_GarageCameraAngle >= 360.0f) m_GarageCameraAngle -= 360.0f;

            glm::vec3 garageTargetPos = transform.Position + glm::vec3(0.0f, 0.9f, 0.0f);
            // Compagni di squadra accanto: la telecamera si allarga per inquadrarli tutti
            float garageDist = m_GarageCameraDistance;
            if (!m_PartyActors.empty()) {
                float spread = 0.0f;
                for (const auto& pa : m_PartyActors) spread = std::max(spread, glm::length(glm::vec2(pa.actor.pos.x - transform.Position.x, pa.actor.pos.z - transform.Position.z)));
                garageDist = std::max(m_GarageCameraDistance, 2.6f + spread * 1.35f);
            }
            m_GarageCamDistSmooth += (garageDist - m_GarageCamDistSmooth) * std::min(1.0f, deltaTime * 3.0f);

            m_Camera.SetViewportSize(m_WindowWidth, m_WindowHeight);
            m_Camera.SetCameraOffsets(0.0f, 0.2f, m_GarageCamDistSmooth);
            m_Camera.SetTarget(garageTargetPos, m_GarageCameraAngle);
            m_Camera.SetPitch(-5.0f);
            m_Camera.Update(glm::vec3(0.0f), 1.0f);

            Renderer::SetViewProjection(m_Camera.GetViewProjection());

            // Modello animato anche nel garage (idle) con vestiti e armatura, poi arma sulla mano
            UpdateAnimatedPlayer(deltaTime, RifleAnimInput{});
            UpdateAnimatedSkin();

            // FIX #8: arma visibile nel garage (stessa logica del ramo Gameplay)
            SyncWeaponAttachment();
            return;
        }
        auto* hp = m_Scene.GetRegistry().try_get<HealthComponent>(m_PlayerEntity);
        bool isPlayerAlive = hp && (hp->currentHealth > 0.0f);

        if (hp && !isPlayerAlive && !m_PlayerDeathHandled) {
            if (m_CurrentSquadMemberIdx >= 0 &&
                m_CurrentSquadMemberIdx < static_cast<int>(m_ActiveSquad.size())) {
                m_ActiveSquad[m_CurrentSquadMemberIdx].isAlive = false;
                m_ActiveSquad[m_CurrentSquadMemberIdx].currentHealth = 0.0f;
            }

            // Attiva ragdoll anche per il player (feedback visivo).
            // KillUnit gestisce sia la creazione dei body Jolt sia
            // l'aggiunta di CorpseComponent per escluderlo dai sistemi AI.
            glm::vec3 deathImpulse(0.0f, 1.5f, -3.5f);
            m_PhysicsSystem.KillUnit(m_Scene, m_PlayerEntity, deathImpulse);

            m_PlayerDeathHandled = true;
            m_VsBotDeadTime = 0.0f;
        }

        bool isAiming = false;

        EquipStance stance = EquipStance::RangedTwoHanded;
        bool hasPrimary1H = false;
        bool hasSecondary1H = false;

        if (m_Scene.GetRegistry().all_of<InventoryComponent>(m_PlayerEntity)) {
            const auto& inv = m_Scene.GetRegistry().get<InventoryComponent>(m_PlayerEntity);
            stance = inv.GetEquipStance();

            const auto* primBP = BlueprintManager::Get().GetWeapon(inv.primaryWeaponId);
            const auto* secBP = BlueprintManager::Get().GetWeapon(inv.secondaryWeaponId);

            if (primBP && primBP->hand_type == HandType::OneHanded && !primBP->is_melee) hasPrimary1H = true;
            if (secBP && secBP->hand_type == HandType::OneHanded && !secBP->is_melee) hasSecondary1H = true;
        }

        bool isSinglePistolStance = (stance == EquipStance::DualRangedOneHanded) &&
            ((hasPrimary1H && !hasSecondary1H) || (!hasPrimary1H && hasSecondary1H));

        bool isDualPistolStance = (stance == EquipStance::DualRangedOneHanded) &&
            (hasPrimary1H && hasSecondary1H);

        if (!m_IsPaused && isPlayerAlive) {
            // FIX #11: la salute del membro attivo segue l'HealthComponent (prima restava al valore
            // iniziale fino alla morte, quando veniva forzata a 0).
            if (hp && m_CurrentSquadMemberIdx >= 0 &&
                m_CurrentSquadMemberIdx < static_cast<int>(m_ActiveSquad.size())) {
                m_ActiveSquad[m_CurrentSquadMemberIdx].currentHealth = hp->currentHealth;
            }

            glm::vec2 mouseDelta = Input::GetMouseDelta();
            float mouseSensitivity = 0.15f;

            if (m_RadialOpen) {   // selettore degli ordini aperto: il mouse sceglie, la visuale resta ferma
                m_RadialVec += mouseDelta;
                if (glm::length(m_RadialVec) > 90.0f) m_RadialVec = glm::normalize(m_RadialVec) * 90.0f;
            }
            else {
                transform.AimYaw -= mouseDelta.x * mouseSensitivity;
                transform.AimPitch -= mouseDelta.y * mouseSensitivity;
                transform.AimPitch = std::clamp(transform.AimPitch, -85.0f, 85.0f);
            }

            if (!Input::IsKeyPressed(GLFW_KEY_W) &&
                !Input::IsKeyPressed(GLFW_KEY_S) &&
                !Input::IsKeyPressed(GLFW_KEY_A) &&
                !Input::IsKeyPressed(GLFW_KEY_D)) {

                float diff = std::fmod(transform.AimYaw - transform.Rotation.y + 180.0f, 360.0f) - 180.0f;
                if (diff < -180.0f) diff += 360.0f;

                if (std::abs(diff) > 45.0f) {
                    transform.Rotation.y = LerpAngle(transform.Rotation.y, transform.AimYaw, 5.0f * deltaTime);
                }
            }

            // FIX #4/#19: il conteggio kill e' ora in CheckUnitDeaths (ProcessedCorpseTag rimosso).

            if (m_Scene.GetRegistry().all_of<
                InventoryComponent,
                UnitStatsComponent,
                WeaponStateComponent,
                WeaponBlueprintId
            >(m_PlayerEntity) && !m_Scene.GetRegistry().all_of<TurretMountComponent>(m_PlayerEntity))   // in torretta: comandi dell'arma pesante
            {
                auto& inv = m_Scene.GetRegistry().get<InventoryComponent>(m_PlayerEntity);
                auto& stats = m_Scene.GetRegistry().get<UnitStatsComponent>(m_PlayerEntity);
                auto& melee = m_Scene.GetRegistry().get_or_emplace<MeleeStateComponent>(m_PlayerEntity);
                auto& wState = m_Scene.GetRegistry().get<WeaponStateComponent>(m_PlayerEntity);
                auto& wBlueprintId = m_Scene.GetRegistry().get<WeaponBlueprintId>(m_PlayerEntity);

                // 1 primaria, 2 secondaria, 3 mischia, 4-5-6 dotazione tattica (slot equipaggiamento 1-3)
                int requestedSlot = -1;
                if (Input::IsKeyPressed(GLFW_KEY_1)) requestedSlot = 0;
                else if (Input::IsKeyPressed(GLFW_KEY_2)) requestedSlot = 1;
                else if (Input::IsKeyPressed(GLFW_KEY_3)) requestedSlot = 2;
                else if (Input::IsKeyPressed(GLFW_KEY_4)) requestedSlot = 4;
                else if (Input::IsKeyPressed(GLFW_KEY_5)) requestedSlot = 5;
                else if (Input::IsKeyPressed(GLFW_KEY_6)) requestedSlot = 6;
                if (requestedSlot != -1 && !inv.IsSlotSelectable(requestedSlot)) requestedSlot = -1;   // slot vuoto o squig

                if (requestedSlot != -1 && requestedSlot != inv.currentSlot) {
                    inv.currentSlot = requestedSlot;
                    std::string targetWeaponId = inv.GetActiveWeaponId();

                    melee.currentPhase = MeleePhase::None;
                    melee.currentAttackNode = "";
                    melee.stateTimer = 0.0f;

                    if (inv.GetEquipStance() == EquipStance::HybridRangedMelee) {
                        targetWeaponId = inv.secondaryWeaponId;
                    }

                    if (!targetWeaponId.empty()) {
                        wBlueprintId.id = targetWeaponId;

                        wState.isReloading = false;
                        wState.reloadTimer = 0.0f;
                        wState.fireCooldown = 0.0f;
                        wState.currentHeat = 0.0f;
                        wState.isOverheating = false;
                        // Il caricatore NON si riempie piu' cambiando arma: lo scambia Ammo::SyncHeld (WeaponSystem)
                    }
                }

                if (stance == EquipStance::RangedTwoHanded ||
                    stance == EquipStance::HybridRangedMelee ||
                    isSinglePistolStance) {
                    if (Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT) && melee.currentPhase == MeleePhase::None) {
                        isAiming = true;
                    }
                }

                static bool mmbPressedLastFrame = false;
                bool mmbPressed = Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_MIDDLE);
                bool mmbClicked = mmbPressed && !mmbPressedLastFrame;
                mmbPressedLastFrame = mmbPressed;

                if (isAiming) {
                    stats.speedModifier = 0.60f;

                    const auto* currentBP = BlueprintManager::Get().GetWeapon(wBlueprintId.id);
                    if (mmbClicked && currentBP) {
                        if (currentBP->has_scope) {
                            wState.isScopeActive = !wState.isScopeActive;
                            SUN_CORE_INFO("Mirino Ottico: {0}", wState.isScopeActive ? "ATTIVATO" : "DISATTIVATO");
                        }
                        else if (currentBP->has_underbarrel) {
                            wState.currentFireMode = (wState.currentFireMode == "standard") ? currentBP->underbarrel_type : "standard";
                            SUN_CORE_INFO("Modalita' di Fuoco Sotto-Canna: {0}", wState.currentFireMode);
                        }
                    }
                }
                else {
                    stats.speedModifier = 1.00f;
                    wState.isScopeActive = false;
                }

                // Stessa arma da mischia usata da MeleeCombatSystem e dal calcolo del danno
                const auto* meleeBP = DamageResolver::ResolveMeleeWeapon(m_Scene.GetRegistry(), m_PlayerEntity);

                auto triggerMeleeNode = [&](const std::string& inputKey, const std::string& defaultNode, MeleeAttackType atkType) {
                    melee.bufferedInput = inputKey;
                    melee.inputBufferTimer = 0.30f;

                    if (melee.currentPhase == MeleePhase::None) {
                        std::string nodeToStart = defaultNode;
                        if (meleeBP && meleeBP->hasMoveset && !meleeBP->moveset.entryNode.empty()) {
                            if (defaultNode == "heavy_1" || defaultNode == "low_1") {
                                nodeToStart = defaultNode;
                            }
                            else {
                                nodeToStart = meleeBP->moveset.entryNode;
                            }
                        }
                        melee.currentAttackNode = nodeToStart;
                        melee.currentPhase = MeleePhase::Windup;
                        melee.currentAttack = atkType;
                        melee.stateTimer = 0.0f;
                        melee.hasHitInThisAttack = false;
                    }
                };

                bool cIsDown = Input::IsKeyPressed(GLFW_KEY_C);
                static bool cWasDown = false;
                if (cIsDown && !cWasDown) {
                    triggerMeleeNode("C", "low_1", MeleeAttackType::Light);
                }
                cWasDown = cIsDown;

                bool fIsDown = Input::IsKeyPressed(GLFW_KEY_F);
                static bool fWasDown = false;
                static float fHoldTimer = 0.0f;

                if (fIsDown) {
                    fHoldTimer += deltaTime;
                    if (fHoldTimer >= 0.22f && !melee.fKeyWasPressed) {
                        triggerMeleeNode("F_HOLD", "heavy_1", MeleeAttackType::Heavy);
                        melee.fKeyWasPressed = true;
                    }
                }
                else if (fWasDown) {
                    if (fHoldTimer < 0.22f && !melee.fKeyWasPressed) {
                        triggerMeleeNode("F_CLICK", "normal_1", MeleeAttackType::Normal);
                    }
                    fHoldTimer = 0.0f;
                    melee.fKeyWasPressed = false;
                }
                fWasDown = fIsDown;

                if (stance == EquipStance::MeleePure && !inv.IsHoldingEquipment()) {
                    static bool lmbWasDown = false;
                    bool lmbIsDown = Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
                    if (lmbIsDown && !lmbWasDown) {
                        triggerMeleeNode("LMB", "normal_1", MeleeAttackType::Normal);
                    }
                    lmbWasDown = lmbIsDown;

                    static bool rmbWasDown = false;
                    bool rmbIsDown = Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT);
                    if (rmbIsDown && !rmbWasDown && !RepairSystem::HeldRepairTool(inv)) {   // chiave inglese: destro = ripara
                        triggerMeleeNode("RMB", "heavy_1", MeleeAttackType::Heavy);
                    }
                    rmbWasDown = rmbIsDown;
                }

                if (Input::IsKeyPressed(GLFW_KEY_Q) && melee.currentPhase == MeleePhase::None) {
                    float parryCost = 10.0f;
                    if (stats.stamina >= parryCost) {
                        stats.stamina -= parryCost;
                        melee.currentPhase = MeleePhase::Windup;
                        melee.currentAttack = MeleeAttackType::Parry;
                        melee.stateTimer = 0.0f;
                    }
                }
            }

            m_MovementSystem.Update(
                m_Scene,
                m_PhysicsSystem,
                m_AudioEngineInitialized ? &m_AudioEngine : nullptr,
                m_SFXGroupInitialized ? &m_SFXGroup : nullptr,
                deltaTime,
                m_SFXGroupInitialized ? 1.0f : m_VolumeSFX   // il volume SFX lo applica il gruppo (prima era applicato due volte)
            );
            // Campo da tiro (manichini presenti, fuori dal Vs Bot): riserva infinita per provare le armi
            if (auto* ammo = m_Scene.GetRegistry().try_get<AmmoComponent>(m_PlayerEntity)) {
                const auto dummies = m_Scene.GetRegistry().view<TrainingDummyComponent>();
                ammo->infinite = !m_VsBotActive && dummies.begin() != dummies.end();
            }
            m_WeaponSystem.Update(m_Scene, m_PhysicsSystem, deltaTime);
            m_EquipmentSystem.Update(m_Scene, deltaTime, &m_BlockModels);
            m_ExplosiveSystem.Update(m_Scene, deltaTime);
            m_TurretSystem.Update(m_Scene, m_WeaponSystem, m_PhysicsSystem, m_PlayerEntity, true, deltaTime);
            m_PhysicsSystem.Update(m_Scene, deltaTime);
            // FIX #2: despawn dei cadaveri dopo maxLifetime (ragdoll Jolt + entity)
            CorpseSystem::Update(m_Scene, m_PhysicsSystem, deltaTime);
            UpdateHitFeedback(deltaTime);
            UpdateTrainingDummies(deltaTime);
            m_WeightSystem.Update(m_Scene, deltaTime);
            m_HealthRegen.Update(m_Scene, deltaTime);

            m_HordeAI.Update(m_Scene, m_NavQuery, deltaTime);
            UpdateSummons(deltaTime);
            if ((m_VsBotActive || HasSummons()) && !(m_VsBotActive && m_VsBotWarmup > 0.0f)) m_BotAI.Update(m_Scene, m_NavQuery, deltaTime);   // nel riscaldamento i bot aspettano
            m_MeleeCombat.Update(m_Scene, deltaTime);
            // Compagni squig (seguono il padrone e attaccano quello a cui spara) e riparazione con la chiave inglese
            if (!m_SquigSkeletonLoaded) {   // un solo tentativo: se manca il file lo squig resta un cubo
                m_SquigSkeleton = SkeletonLoader::LoadFromFile(FindValidDir("Skeletons") + "squig.json");
                m_SquigSkeletonLoaded = true;
            }
            m_SquigSystem.Update(m_Scene, m_NavQuery, deltaTime, m_SquigSkeleton ? &*m_SquigSkeleton : nullptr);
            UpdateSquigVisuals();
            m_RepairSystem.Update(m_Scene, m_PlayerEntity, deltaTime);
            if (auto* ap = m_Scene.GetRegistry().try_get<ActionPoseComponent>(m_PlayerEntity)) ap->repairing = m_RepairSystem.repairing;
            ActionPose::Update(m_Scene, deltaTime);   // azioni di tutte le unita' (granate, oggetti, costruzione...)

            // F2: cambia al volo tra personaggio a cuboni e personaggio animato
            {
                const bool f2 = Input::IsKeyPressed(GLFW_KEY_F2);
                if (f2 && !m_F2WasDown) m_UseAnimatedPlayer = !m_UseAnimatedPlayer;
                m_F2WasDown = f2;
            }

            // Comandi reali del giocatore -> animazioni del personaggio animato
            UpdateAnimatedPlayer(deltaTime, BuildPlayerAnimInput(isAiming, deltaTime));
            UpdateAnimatedSkin();   // elmetto / vesti sulle ossa del modello animato

            CheckUnitDeaths();
            CollectMatchXp(deltaTime, !(m_VsBotActive && m_VsBotResult != 0));

            if (m_Scene.GetRegistry().all_of<SkeletonComponent, WeaponBlueprintId>(m_PlayerEntity)) {
                auto& skel = m_Scene.GetRegistry().get<SkeletonComponent>(m_PlayerEntity);
                auto& melee = m_Scene.GetRegistry().get_or_emplace<MeleeStateComponent>(m_PlayerEntity);
                auto& inv = m_Scene.GetRegistry().get<InventoryComponent>(m_PlayerEntity);

                const std::string& currentWeaponId = m_Scene.GetRegistry().get<WeaponBlueprintId>(m_PlayerEntity).id;

                std::string activeMeleeWeaponId = currentWeaponId;
                if (stance == EquipStance::HybridRangedMelee && !inv.meleeWeaponId.empty()) {
                    activeMeleeWeaponId = inv.meleeWeaponId;
                }
                const auto* meleeBP = BlueprintManager::Get().GetWeapon(activeMeleeWeaponId);
                const auto* weaponBP = BlueprintManager::Get().GetWeapon(currentWeaponId);

                bool isTwoHanded = weaponBP ? (weaponBP->hand_type == HandType::TwoHanded) : true;

                float spineTwist = std::fmod(transform.AimYaw - transform.Rotation.y + 180.0f, 360.0f) - 180.0f;
                if (spineTwist < -180.0f) spineTwist += 360.0f;
                spineTwist = std::clamp(spineTwist, -75.0f, 75.0f);

                float torsoPitchOffset = transform.AimPitch * 0.35f;
                float armPitchOffset = transform.AimPitch * 0.65f;

                bool isParrying = (melee.currentAttack == MeleeAttackType::Parry) && (melee.currentPhase != MeleePhase::None);

                float lerpSpeed = (melee.currentPhase == MeleePhase::Active) ? 32.0f : (isParrying ? 22.0f : (isAiming ? 18.0f : 12.0f));

                for (auto& block : skel.data.blocks) {

                    if (isParrying) {
                        if (stance == EquipStance::RangedTwoHanded) {
                            if (block.name == "Torso") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(12.0f, spineTwist - 20.0f, 5.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Head") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(-8.0f, 20.0f, -5.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Arm_R") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(65.0f + armPitchOffset, -35.0f, -20.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Arm_L") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(72.0f + armPitchOffset, 40.0f, 30.0f), lerpSpeed * deltaTime);
                        }
                        else if (isSinglePistolStance) {
                            if (block.name == "Torso") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(8.0f, spineTwist - 25.0f, 0.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Head") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(-10.0f, 25.0f, -5.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Arm_R") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(50.0f + armPitchOffset, -45.0f, -15.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Arm_L") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(82.0f + armPitchOffset, 55.0f, 40.0f), lerpSpeed * deltaTime);
                        }
                        else if (isDualPistolStance) {
                            if (block.name == "Torso") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(10.0f, spineTwist, 0.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Head") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(-10.0f, 0.0f, 0.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Arm_R") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(72.0f + armPitchOffset, -58.0f, -35.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Arm_L") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(72.0f + armPitchOffset, 58.0f, 35.0f), lerpSpeed * deltaTime);
                        }
                        else if (stance == EquipStance::HybridRangedMelee) {
                            if (block.name == "Torso") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(5.0f, spineTwist - 30.0f, -5.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Head") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(-5.0f, 30.0f, -5.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Arm_R") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(80.0f + armPitchOffset, -20.0f, -50.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Arm_L") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(18.0f + armPitchOffset * 0.3f, 15.0f, 0.0f), lerpSpeed * deltaTime);
                        }
                        else {
                            if (block.name == "Torso") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(15.0f, spineTwist - 15.0f, 0.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Head") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(-10.0f, 15.0f, 0.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Arm_R") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(82.0f + armPitchOffset, -30.0f, -25.0f), lerpSpeed * deltaTime);
                            else if (block.name == "Arm_L") block.jointRotation = glm::mix(block.jointRotation, glm::vec3(88.0f + armPitchOffset, 35.0f, 30.0f), lerpSpeed * deltaTime);
                        }
                        continue;
                    }

                    if (meleeBP && meleeBP->hasMoveset && !melee.currentAttackNode.empty() && melee.currentPhase != MeleePhase::None) {
                        std::string nodeKey = melee.currentAttackNode;

                        if (nodeKey.find("heavy") != std::string::npos || melee.currentAttack == MeleeAttackType::Heavy) {
                            if (block.name == "Torso") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(-20.0f, spineTwist + 60.0f, -15.0f)
                                    : glm::vec3(30.0f, spineTwist - 50.0f, 15.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Head") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(10.0f, -40.0f, 0.0f)
                                    : glm::vec3(-10.0f, 25.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_R") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(120.0f + armPitchOffset, -45.0f, 30.0f)
                                    : glm::vec3(-40.0f + armPitchOffset, 60.0f, -40.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_L") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(45.0f, 20.0f, 10.0f)
                                    : glm::vec3(20.0f, -30.0f, -10.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            continue;
                        }
                        else if (nodeKey == "normal_1") {
                            if (block.name == "Torso") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(-5.0f, spineTwist + 50.0f, -5.0f)
                                    : glm::vec3(10.0f, spineTwist - 55.0f, 8.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Head") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(5.0f, -25.0f, 0.0f)
                                    : glm::vec3(-5.0f, 30.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_R") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(45.0f + armPitchOffset, -75.0f, 20.0f)
                                    : glm::vec3(25.0f + armPitchOffset, 75.0f, -25.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_L") {
                                glm::vec3 targetRot(20.0f, 10.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            continue;
                        }
                        else if (nodeKey == "normal_2") {
                            if (block.name == "Torso") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(-5.0f, spineTwist - 50.0f, 5.0f)
                                    : glm::vec3(10.0f, spineTwist + 50.0f, -8.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Head") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(0.0f, 25.0f, 0.0f)
                                    : glm::vec3(0.0f, -25.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_R") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(35.0f + armPitchOffset, 70.0f, -20.0f)
                                    : glm::vec3(30.0f + armPitchOffset, -75.0f, 25.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_L") {
                                glm::vec3 targetRot(15.0f, -10.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            continue;
                        }
                        else if (nodeKey.find("low") != std::string::npos || melee.currentAttack == MeleeAttackType::Light) {
                            if (block.name == "Torso") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(25.0f, spineTwist - 20.0f, 0.0f)
                                    : glm::vec3(35.0f, spineTwist + 25.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Head") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(-10.0f, 15.0f, 0.0f)
                                    : glm::vec3(-15.0f, -15.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_R") {
                                glm::vec3 targetRot = (melee.currentPhase == MeleePhase::Windup)
                                    ? glm::vec3(-15.0f + armPitchOffset, -30.0f, -10.0f)
                                    : glm::vec3(75.0f + armPitchOffset, 20.0f, -15.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_L") {
                                glm::vec3 targetRot(20.0f, -10.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            continue;
                        }
                    }

                    if (block.name == "Torso") {
                        float targetTorsoYaw = spineTwist;
                        float targetTorsoPitch = 0.0f;
                        float targetTorsoRoll = 0.0f;

                        if (isAiming) {
                            targetTorsoPitch = -torsoPitchOffset;

                            if (stance == EquipStance::RangedTwoHanded || (isTwoHanded && stance != EquipStance::HybridRangedMelee)) {
                                targetTorsoYaw += -38.0f;
                                targetTorsoRoll = 4.0f;
                            }
                            else if (isSinglePistolStance) {
                                targetTorsoYaw += -25.0f;
                                targetTorsoRoll = 2.0f;
                            }
                            else if (stance == EquipStance::HybridRangedMelee) {
                                targetTorsoYaw += 52.0f;
                                targetTorsoRoll = -4.0f;
                            }
                            else if (isDualPistolStance) {
                                targetTorsoYaw += -10.0f;
                            }
                        }

                        glm::vec3 targetRot(targetTorsoPitch, targetTorsoYaw, targetTorsoRoll);
                        block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                    }
                    else if (block.name == "Head") {
                        float targetHeadPitch = -armPitchOffset;
                        float targetHeadYaw = 0.0f;
                        float targetHeadRoll = 0.0f;

                        if (isAiming) {
                            if (stance == EquipStance::RangedTwoHanded || (isTwoHanded && stance != EquipStance::HybridRangedMelee)) {
                                targetHeadYaw = 38.0f;
                                targetHeadRoll = -8.0f;
                            }
                            else if (isSinglePistolStance) {
                                targetHeadYaw = 25.0f;
                                targetHeadRoll = -4.0f;
                            }
                            else if (stance == EquipStance::HybridRangedMelee) {
                                targetHeadYaw = -52.0f;
                                targetHeadRoll = 6.0f;
                            }
                        }

                        glm::vec3 targetRot(targetHeadPitch, targetHeadYaw, targetHeadRoll);
                        block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                    }
                    else if (melee.currentPhase == MeleePhase::None) {
                        if (stance == EquipStance::RangedTwoHanded || (isTwoHanded && stance != EquipStance::HybridRangedMelee)) {
                            if (block.name == "Arm_R") {
                                glm::vec3 targetRot = isAiming
                                    ? glm::vec3(58.0f + armPitchOffset, -42.0f, 12.0f)
                                    : glm::vec3(38.0f + armPitchOffset * 0.5f, -20.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_L") {
                                glm::vec3 targetRot = isAiming
                                    ? glm::vec3(64.0f + armPitchOffset, 28.0f, 18.0f)
                                    : glm::vec3(42.0f + armPitchOffset * 0.5f, 40.0f, 22.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                        }
                        else if (isSinglePistolStance) {
                            if (block.name == "Arm_R") {
                                glm::vec3 targetRot = isAiming
                                    ? glm::vec3(54.0f + armPitchOffset * 1.05f, -20.0f, 5.0f)
                                    : glm::vec3(35.0f + armPitchOffset * 0.5f, -10.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_L") {
                                glm::vec3 targetRot = isAiming
                                    ? glm::vec3(58.0f + armPitchOffset * 1.05f, 32.0f, 16.0f)
                                    : glm::vec3(20.0f, 10.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                        }
                        else if (stance == EquipStance::HybridRangedMelee) {
                            if (block.name == "Arm_R") {
                                glm::vec3 targetRot(15.0f, -40.0f, -20.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_L") {
                                glm::vec3 targetRot = isAiming
                                    ? glm::vec3(58.0f + armPitchOffset * 1.1f, -8.0f, 0.0f)
                                    : glm::vec3(35.0f + armPitchOffset * 0.5f, 15.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                        }
                        else if (isDualPistolStance) {
                            if (block.name == "Arm_R" || block.name == "Arm_L") {
                                float sideSign = (block.name == "Arm_R") ? -1.0f : 1.0f;
                                glm::vec3 targetRot = isAiming
                                    ? glm::vec3(52.0f + armPitchOffset, sideSign * 10.0f, 0.0f)
                                    : glm::vec3(35.0f + armPitchOffset * 0.5f, sideSign * 20.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                        }
                        else {
                            if (block.name == "Arm_R") {
                                glm::vec3 targetRot(30.0f + armPitchOffset * 0.5f, -10.0f, 0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                            else if (block.name == "Arm_L") {
                                glm::vec3 targetRot(0.0f);
                                block.jointRotation = glm::mix(block.jointRotation, targetRot, lerpSpeed * deltaTime);
                            }
                        }
                    }
                }
            }

            float unitHeight = 1.8f;
            float torsoWidth = 0.5f;

            if (m_Scene.GetRegistry().all_of<SkeletonComponent>(m_PlayerEntity)) {
                auto& skel = m_Scene.GetRegistry().get<SkeletonComponent>(m_PlayerEntity);
                auto worldTransforms = skel.data.ComputeWorldTransforms(transform.Position, transform.Rotation, skel.scale);

                float minY = 9999.0f, maxY = -9999.0f;

                for (size_t i = 0; i < skel.data.blocks.size(); ++i) {
                    const auto& block = skel.data.blocks[i];
                    glm::vec3 blockPos = glm::vec3(worldTransforms[i][3]);

                    minY = std::min(minY, blockPos.y - (block.size.y * 0.5f));
                    maxY = std::max(maxY, blockPos.y + (block.size.y * 0.5f));

                    if (block.name == "Torso") torsoWidth = block.size.x;
                }

                if (maxY > minY) unitHeight = maxY - minY;
            }

            float dynamicDistance = std::clamp(unitHeight * 2.20f, 3.5f, 6.0f);
            float dynamicShoulderOffset = (torsoWidth * 0.5f) + 0.60f;
            float dynamicHeightOffset = unitHeight * 0.15f;

            float recoilPitch = 0.0f;
            float recoilYaw = 0.0f;
            glm::vec3 shakeOffset = glm::vec3(0.0f);
            float extraFov = 0.0f;

            if (m_Scene.GetRegistry().all_of<CameraRecoilComponent>(m_PlayerEntity)) {
                auto& recoil = m_Scene.GetRegistry().get<CameraRecoilComponent>(m_PlayerEntity);

                recoilPitch = recoil.recoilPitch;
                recoilYaw = recoil.recoilYaw;
                extraFov = recoil.fovOffset;
                if (recoil.shakeIntensity > 0.001f) {
                    float shakeX = (Random::Float01() - 0.5f) * recoil.shakeIntensity;
                    float shakeY = (Random::Float01() - 0.5f) * recoil.shakeIntensity;
                    shakeOffset = glm::vec3(shakeX, shakeY, 0.0f);
                }

                recoil.recoilPitch = glm::mix(recoil.recoilPitch, 0.0f, recoil.recoverySpeed * deltaTime);
                recoil.recoilYaw = glm::mix(recoil.recoilYaw, 0.0f, recoil.recoverySpeed * deltaTime);
                recoil.shakeIntensity = glm::mix(recoil.shakeIntensity, 0.0f, 15.0f * deltaTime);
                recoil.fovOffset = glm::mix(recoil.fovOffset, 0.0f, 12.0f * deltaTime);
            }

            const auto* wBpId = m_Scene.GetRegistry().try_get<WeaponBlueprintId>(m_PlayerEntity);
            const auto* wSt = m_Scene.GetRegistry().try_get<WeaponStateComponent>(m_PlayerEntity);
            const auto* currentBP = wBpId ? BlueprintManager::Get().GetWeapon(wBpId->id) : nullptr;

            if (isAiming) {
                if (stance == EquipStance::HybridRangedMelee) {
                    dynamicDistance = 2.0f;
                    dynamicShoulderOffset = 0.62f;
                    dynamicHeightOffset = unitHeight * 0.18f;
                    extraFov -= 20.0f;
                }
                else if (stance == EquipStance::RangedTwoHanded || isSinglePistolStance) {
                    dynamicDistance = 1.95f;
                    dynamicShoulderOffset = 0.50f;
                    dynamicHeightOffset = unitHeight * 0.18f;
                    extraFov -= 22.0f;
                }
                else {
                    dynamicDistance = 2.05f;
                    dynamicShoulderOffset = 0.55f;
                    dynamicHeightOffset = unitHeight * 0.18f;
                    extraFov -= 18.0f;
                }

                if (wSt && wSt->isScopeActive && currentBP && currentBP->has_scope) {
                    dynamicDistance = 0.75f;
                    dynamicShoulderOffset = 0.12f;
                    extraFov -= currentBP->scope_zoom_fov;
                }
            }

            // Scossoni (trauma), calci dei colpi presi e pugno di FOV: sopra il rinculo dell'arma
            const GameFeel::CameraKick feelKick = m_Feel.GetCameraKick();
            extraFov += feelKick.fov;
            shakeOffset += feelKick.offset;

            glm::vec3 lookTarget = transform.Position + glm::vec3(0.0f, unitHeight * 0.65f, 0.0f);
            float totalPitch = transform.AimPitch + recoilPitch + feelKick.pitch;
            float totalYaw = transform.AimYaw + recoilYaw + feelKick.yaw;

            float pitchRad = glm::radians(totalPitch);
            float yawRad = glm::radians(totalYaw);
            glm::vec3 camDir(
                std::sin(yawRad) * std::cos(pitchRad),
                std::sin(pitchRad),
                std::cos(yawRad) * std::cos(pitchRad)
            );

            glm::vec3 idealCamPos = lookTarget + camDir * dynamicDistance;

            auto colliderView = m_Scene.GetRegistry().view<ColliderComponent, TransformComponent>(
                entt::exclude<PlayerTag, HealthComponent, CorpseComponent>
            );

            float minHitT = dynamicDistance;
            glm::vec3 rayDir = glm::normalize(idealCamPos - lookTarget);

            for (auto entity : colliderView) {
                const auto& col = colliderView.get<ColliderComponent>(entity);
                const auto& trans = colliderView.get<TransformComponent>(entity);

                // FIX #1: centro = Position + localCenterOffset
                glm::vec3 half = col.halfExtents * trans.Scale;
                glm::vec3 center = GetColliderWorldCenter(trans, col);
                glm::vec3 minBox = center - half;
                glm::vec3 maxBox = center + half;

                float tMin = 0.0f, tMax = dynamicDistance;
                bool hit = true;

                for (int a = 0; a < 3; ++a) {
                    if (std::abs(rayDir[a]) < 0.0001f) {
                        if (lookTarget[a] < minBox[a] || lookTarget[a] > maxBox[a]) { hit = false; break; }
                    }
                    else {
                        float invD = 1.0f / rayDir[a];
                        float t0 = (minBox[a] - lookTarget[a]) * invD;
                        float t1 = (maxBox[a] - lookTarget[a]) * invD;
                        if (invD < 0.0f) std::swap(t0, t1);
                        tMin = std::max(tMin, t0);
                        tMax = std::min(tMax, t1);
                        if (tMax < tMin) { hit = false; break; }
                    }
                }

                if (hit && tMin < minHitT && tMin > 0.2f) {
                    minHitT = tMin - 0.20f;
                }
            }

            dynamicDistance = std::clamp(minHitT, 0.6f, dynamicDistance);

            glm::vec3 projectedCamPos = lookTarget + camDir * dynamicDistance;
            // FIX #21: senza terreno sotto la camera non applico il clamp (prima: terreno fittizio a y=0)
            float groundY = GetGroundHeightAt(m_Scene, projectedCamPos, projectedCamPos.y).value_or(-1.0e6f);
            float minCamY = groundY + 0.35f;

            if (projectedCamPos.y < minCamY) {
                if (std::abs(camDir.y) > 0.05f) {
                    float requiredDist = (lookTarget.y - minCamY) / std::abs(camDir.y);
                    dynamicDistance = std::clamp(requiredDist, 0.5f, dynamicDistance);
                }
            }

            m_Camera.SetCameraOffsets(dynamicShoulderOffset, dynamicHeightOffset, dynamicDistance);
            m_Camera.SetTarget(lookTarget, transform.AimYaw);

            m_Camera.SetPitch(totalPitch);
            m_Camera.SetYawOffset(recoilYaw + feelKick.yaw);
            m_Camera.SetViewportSize(m_WindowWidth, m_WindowHeight, extraFov);
            m_Camera.Update(shakeOffset, 1.0f);

            // Raggio del mirino (centro dello schermo) per far convergere i colpi del giocatore
            {
                auto& aim = m_Scene.GetRegistry().get_or_emplace<AimRayComponent>(m_PlayerEntity);
                const glm::mat4 invView = glm::inverse(m_Camera.GetView());
                aim.origin = glm::vec3(invView[3]);
                aim.dir = -glm::normalize(glm::vec3(invView[2]));
                aim.recoilPitch = recoilPitch;
                aim.valid = true;
            }

            Renderer::SetViewProjection(m_Camera.GetViewProjection());

            if (g_WaaaghActive) {
                UpdateWaaaghWaves(deltaTime, transform.Position);
                UpdateOrkVoices(m_Scene, m_PlayerEntity, deltaTime, m_VoiceGroupInitialized ? 1.0f : m_VolumeVoice);
            }

            AnimateUnits(deltaTime, transform.Position, true);
            ActionPose::ApplyCubes(m_Scene, m_PlayerEntity);   // pose delle azioni sugli scheletri a blocchi (bot e hitbox)
        }
        else if (m_Net.active && (m_IsPaused || !isPlayerAlive)) {
            // Partita in rete: il mondo non si ferma mentre sei a terra o in pausa (gli altri continuano a giocare)
            if (m_IsPaused) if (auto* ws = m_Scene.GetRegistry().try_get<WeaponStateComponent>(m_PlayerEntity)) ws->fireCooldown = std::max(ws->fireCooldown, 0.15f);   // i clic sul menu non sparano
            if (m_VsBotActive) {
                UpdateVsBotWorld(deltaTime);
                if (!isPlayerAlive && !m_IsPaused) UpdateVsBotSpectator();
            }
            else UpdateNetWorld(deltaTime);
        }
        else if (!m_IsPaused && !isPlayerAlive && m_VsBotActive) {
            // 5v5: la partita va avanti mentre scegli con chi rientrare (o osservi gli alleati)
            UpdateVsBotWorld(deltaTime);
            UpdateVsBotSpectator();
        }
        else if (!m_IsPaused && !isPlayerAlive) {
            // Player morto: la fisica deve continuare a girare per simulare il ragdoll.
            // Niente input, camera, AI: solo lo step di Jolt.
            m_PhysicsSystem.Update(m_Scene, deltaTime);
            // FIX #2: i cadaveri nemici devono scadere anche mentre il player e' morto
            CorpseSystem::Update(m_Scene, m_PhysicsSystem, deltaTime);
            UpdateHitFeedback(deltaTime);
            UpdateTrainingDummies(deltaTime);
        }
        // FIX #16: le particelle non avanzano in pausa (in rete si': la partita continua)
        if (!m_IsPaused || m_Net.active) {
            m_ParticleSystem.Update(m_Scene, deltaTime);
        }

        // FIX #7: il listener audio segue il player (posizione) e la camera (orientamento).
        // Senza questo il listener restava a (0,0,0) fisso e panning/direzione di voci, passi e
        // spari erano relativi all'origine della mappa.
        if (m_AudioEngineInitialized && m_Scene.GetRegistry().valid(m_PlayerEntity)) {
            const glm::vec3 listenerPos = transform.Position + glm::vec3(0.0f, 1.6f, 0.0f);
            const glm::mat4 viewMat = m_Camera.GetView();
            // righe della view = (right, up, -forward) in coordinate mondo
            const glm::vec3 listenerFwd = -glm::vec3(viewMat[0][2], viewMat[1][2], viewMat[2][2]);
            ma_engine_listener_set_position(&m_AudioEngine, 0, listenerPos.x, listenerPos.y, listenerPos.z);
            ma_engine_listener_set_direction(&m_AudioEngine, 0, listenerFwd.x, listenerFwd.y, listenerFwd.z);
            ma_engine_listener_set_world_up(&m_AudioEngine, 0, 0.0f, 1.0f, 0.0f);
        }

        SyncWeaponAttachment();
        UpdateUnitAnimations(m_IsPaused && !m_Net.active ? 0.0f : deltaTime);   // corpi animati di bot, orda e manichini
        SyncCloakVisuals();
        if (!m_IsPaused) UpdateFactionBuff(deltaTime);   // WAAAGH! (anche mentre sei a terra: il contatore resta)
        if (m_VsBotActive) {
            UpdateBotVisuals();
            UpdateVsBot(deltaTime);
            VsBotApplySpawnZones();   // muri invisibili delle basi
            if (m_VsBotWarmup > 0.0f) {
                // riscaldamento: niente colpi; T = passa all'altra squadra al posto di un bot
                if (auto* ws = m_Scene.GetRegistry().try_get<WeaponStateComponent>(m_PlayerEntity)) ws->fireCooldown = std::max(ws->fireCooldown, 0.15f);
                const bool tDown = Input::IsKeyPressed(GLFW_KEY_T);
                if (tDown && !m_VsBotTWasDown && !m_IsPaused) NetRequestTeamSwitch();
                m_VsBotTWasDown = tDown;
            }
        }
        else if (m_GameState == GameState::Gameplay && (HasSummons() || m_Net.active)) UpdateBotVisuals();   // rinforzi del vox in modalita' orda, compagni in rete

        auto orkWeaponView = m_Scene.GetRegistry().view<HordeAgentComponent, TransformComponent, SkeletonComponent>();
        for (auto orkEnt : orkWeaponView) {
            auto& horde = orkWeaponView.get<HordeAgentComponent>(orkEnt);
            auto& orkTrans = orkWeaponView.get<TransformComponent>(orkEnt);
            auto& skel = orkWeaponView.get<SkeletonComponent>(orkEnt);

            if (m_Scene.GetRegistry().valid(horde.weaponEntity)) {
                auto& wTrans = m_Scene.GetRegistry().get<TransformComponent>(horde.weaponEntity);
                const auto* wBP = BlueprintManager::Get().GetWeapon("ork_choppa_1h");
                {   // Corpo animato: la choppa va in mano al modello 3D
                    std::string id = "ork_choppa_1h";
                    if (const auto* wid = m_Scene.GetRegistry().try_get<WeaponBlueprintId>(orkEnt); wid && !wid->id.empty()) id = wid->id;
                    const BlockModel* m = m_BlockModels.Find(id);
                    if (m && PlaceBotWeaponOnHand(orkEnt, wTrans, m)) { SetWeaponBlockModel(horde.weaponEntity, id); continue; }
                }

                if (wBP) {
                    auto worldTransforms = skel.data.ComputeWorldTransforms(orkTrans.Position, orkTrans.Rotation, skel.scale);

                    int armRIndex = -1;
                    for (size_t i = 0; i < skel.data.blocks.size(); ++i) {
                        if (skel.data.blocks[i].name == "Arm_R") { armRIndex = static_cast<int>(i); break; }
                    }

                    if (armRIndex >= 0 && armRIndex < static_cast<int>(worldTransforms.size())) {
                        glm::mat4 armWorldMat = worldTransforms[armRIndex];

                        // Modello a blocchi dell'arma dell'ork (es. la choppa): sostituisce il cubo
                        std::string orkWeaponId = "ork_choppa_1h";
                        if (const auto* wid = m_Scene.GetRegistry().try_get<WeaponBlueprintId>(orkEnt)) {
                            if (!wid->id.empty()) orkWeaponId = wid->id;
                        }
                        if (m_BlockModels.Find(orkWeaponId)) {
                            // La matrice del braccio include la scala del blocco: separo rotazione e dimensioni,
                            // cosi' il modello (in metri reali) non viene deformato.
                            glm::mat4 armRot(1.0f);
                            for (int i = 0; i < 3; ++i) armRot[i] = glm::vec4(glm::normalize(glm::vec3(armWorldMat[i])), 0.0f);
                            const glm::vec3 armSize = skel.data.blocks[armRIndex].size * skel.scale;
                            const glm::mat4 corr = glm::eulerAngleYXZ(glm::radians(m_OrkWeaponModelRotation.y),
                                glm::radians(m_OrkWeaponModelRotation.x), glm::radians(m_OrkWeaponModelRotation.z));
                            const glm::mat4 weaponRot = armRot * corr;

                            // Il pugno e' il fondo del blocco braccio: l'impugnatura della choppa (ancora del
                            // modello) cade li', e la lama esce dal pugno in avanti (rotazione tarabile).
                            const BlockModel* orkModel = m_BlockModels.Find(orkWeaponId);
                            const glm::vec3 handLocal = glm::vec3(0.0f, -armSize.y * 0.5f, 0.0f) + m_OrkWeaponHoldOffset;
                            const glm::vec3 handWorld = glm::vec3(armWorldMat[3]) + glm::vec3(armRot * glm::vec4(handLocal, 0.0f));
                            const glm::vec3 gripLocal = orkModel->hasGripRight ? orkModel->gripRight : glm::vec3(0.0f);

                            wTrans.Position = handWorld - glm::vec3(weaponRot * glm::vec4(gripLocal, 0.0f));
                            float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
                            glm::extractEulerAngleYXZ(weaponRot, yaw, pitch, roll);
                            wTrans.Rotation = glm::degrees(glm::vec3(pitch, yaw, roll));
                            SetWeaponBlockModel(horde.weaponEntity, orkWeaponId);   // scala 1 = metri reali
                        }
                        else {
                            glm::mat4 weaponWorldMat = glm::translate(armWorldMat, glm::vec3(0.0f, -0.15f, 0.45f));

                            wTrans.Position = glm::vec3(weaponWorldMat[3]);
                            glm::quat armQuat = glm::quat_cast(weaponWorldMat);
                            wTrans.Rotation = glm::degrees(glm::eulerAngles(armQuat));
                            wTrans.Scale = glm::vec3(0.10f, 0.55f, 0.35f);
                        }
                    }
                }
            }
        }
    }

    void GameLayer::OnRender()
    {
        {
            const double now = glfwGetTime();
            m_PerfUpdateMs = m_PerfUpdateMs * 0.9f + static_cast<float>((now - m_PerfUpdateStart) * 1000.0) * 0.1f;
            m_PerfRenderStart = now;
        }
        // FIX #12: rimosso il ramo m_UseDeferredTest (scena hardcoded mai eseguita: il flag non veniva
        // mai messo a true). La pipeline si valida direttamente sulla scena di gioco.
        // ================================================================
// PIPELINE PBR DEFERRED — disegna la scena di gioco
// ================================================================
        if (m_DeferredRenderer.IsInitialized()) {
            // Luci
            m_DeferredRenderer.ClearLights();

            // Sole obliquo, luce calda grimdark
            m_DeferredRenderer.SetDirectionalLight(
                glm::vec3(-0.4f, -1.0f, 0.3f),
                glm::vec3(1.0f, 0.90f, 0.75f),
                1.3f);

            // Point light "faro" sopra il player per dare profondità local
            if (m_Scene.GetRegistry().valid(m_PlayerEntity) &&
                m_Scene.GetRegistry().all_of<TransformComponent>(m_PlayerEntity)) {
                glm::vec3 playerPos = m_Scene.GetRegistry()
                    .get<TransformComponent>(m_PlayerEntity).Position;
                m_DeferredRenderer.AddLight(
                    Light::Point(playerPos + glm::vec3(0.0f, 6.0f, 0.0f),
                        25.0f,
                        glm::vec3(0.70f, 0.80f, 1.0f),
                        0.45f));
            }
            m_Feel.SubmitLights(m_DeferredRenderer);   // lampi di spari ed esplosioni
            m_DeferredRenderer.SetSunDirection(glm::vec3(-0.4f, -1.0f, 0.3f));
            // ===== SHADOW PASS =====
            const glm::vec3 sunDir = glm::normalize(glm::vec3(-0.4f, -1.0f, 0.3f));
            m_DeferredRenderer.SetSunDirection(sunDir);

            // Opzioni grafiche: ombre spente = niente passaggio (si risparmia anche il tempo della GPU)
            const RenderQuality& gq = m_DeferredRenderer.GetQuality();
            auto& shadowMap = m_DeferredRenderer.GetShadowMap();
            shadowMap.UpdateCascades(m_Camera, sunDir, std::clamp(gq.shadowDistance, 20.0f, 150.0f));

            m_DeferredRenderer.GpuTimerBegin(GpuT_Shadows);
            for (int c = 0; gq.sunShadows && c < ShadowMap::CASCADE_COUNT; ++c) {
                shadowMap.BeginCascade(c);

                // Disegna TUTTI i cubi della scena (per ora solo i cubi, in futuro le mesh)
                auto view = m_Scene.GetRegistry().view<TransformComponent, MeshComponent, ColorComponent>();
                for (auto entity : view) {
                    // Il giocatore col modello animato non proietta l'ombra del suo cubo hitbox
                    if (m_Scene.GetRegistry().all_of<HideProceduralVisualTag>(entity)) continue;
                    if (!gq.mapDecor && m_Scene.GetRegistry().all_of<MapDecorTag>(entity)) continue;   // detriti nascosti
                    auto& t = view.get<TransformComponent>(entity);
                    const auto* blockModel = m_Scene.GetRegistry().try_get<BlockModelComponent>(entity);
                    if (blockModel && blockModel->model) {
                        const glm::mat4 entityMat = t.GetTransform();
                        for (const auto& part : blockModel->model->parts) {
                            m_DeferredRenderer.RenderShadowPassCube(BlockPartMatrix(entityMat, part), c);
                        }
                        continue;
                    }
                    m_DeferredRenderer.RenderShadowPassCube(t.GetTransform(), c);
                }
                m_DeferredRenderer.FlushShadowCubes(c);   // tutti i cubi della cascata in una chiamata

                // Personaggi animati (corpo, vestiti, armature, squig): l'ombra segue l'animazione
                auto skinned = m_Scene.GetRegistry().view<TransformComponent, SkinnedModelInstanceComponent>();
                for (auto entity : skinned) {
                    if (!gq.characterShadows) break;
                    if (c == ShadowMap::CASCADE_COUNT - 1) break;   // ultima cascata (lontano): personaggi troppo piccoli per l'ombra
                    const auto& t = skinned.get<TransformComponent>(entity);
                    const auto& sk = skinned.get<SkinnedModelInstanceComponent>(entity);
                    if (!sk.sourceModel || !sk.boneMatrices || t.Scale.x < 1e-4f) continue;   // nascosto (scala 0)
                    if (!m_DeferredRenderer.IsSphereInShadowCascade(c, t.Position + glm::vec3(0.0f, 1.2f, 0.0f), 2.2f)) continue;
                    if (const auto* cloak = m_Scene.GetRegistry().try_get<CloakComponent>(entity); cloak && cloak->amount > 0.5f) continue;
                    if (!m_DeferredRenderer.BeginShadowSkinned(c, t.GetTransform(), *sk.boneMatrices)) break;
                    for (const auto& m : sk.sourceModel->meshes) m_DeferredRenderer.DrawShadowSkinned(m);
                    for (const auto& g : sk.garments) if (g.mesh) m_DeferredRenderer.DrawShadowSkinned(*g.mesh, true);
                    if (sk.extraParts) for (const auto& g : sk.extraParts->parts) if (g.mesh) m_DeferredRenderer.DrawShadowSkinned(*g.mesh);
                    m_DeferredRenderer.EndShadowSkinned();
                }

                shadowMap.EndCascade();
            }

            shadowMap.UploadMatrices();
            m_DeferredRenderer.GpuTimerEnd();
            // ===== FINE SHADOW PASS =====
            m_DeferredRenderer.BeginGeometryPass(m_Camera);
            m_RenderSystem.RenderDeferred(m_Scene, m_DeferredRenderer);
            m_Feel.Draw(m_DeferredRenderer);   // bossoli a terra e segni dei colpi
            RenderParryRings(true);
            RenderHealZones(true);
            RenderSummonRings(true);
            m_DeferredRenderer.EndGeometryPassAndPresent(m_Camera);
        }
        else {
            // Fallback al vecchio renderer (solo se DeferredRenderer non è init)
            m_RenderSystem.Render(m_Scene);
            RenderParryRings(false);
            RenderHealZones(false);
            RenderSummonRings(false);
        }
        m_ParticleSystem.Render(m_Scene, m_Camera.GetView(), m_Camera.GetViewProjection());
        if (m_GameState == GameState::Gameplay && m_UseAnimatedPlayer) RenderHitboxOverlay();   // campo da tiro
        // (rimossi i due modelli di prova disegnati accanto al giocatore)

        if (m_GameState == GameState::Gameplay) {
            glm::vec2 screen = glm::vec2(m_WindowWidth, m_WindowHeight);
            glm::vec2 center = screen * 0.5f;

            if (m_Scene.GetRegistry().all_of<InventoryComponent>(m_PlayerEntity)) {
                auto& inv = m_Scene.GetRegistry().get<InventoryComponent>(m_PlayerEntity);

                // Slot in alto: 1 primaria, 2 secondaria, 3 mischia, 4-5-6 dotazione (solo quelli pieni)
                std::vector<int> shownSlots = { 0, 1, 2 };
                for (int s = 4; s <= 6; ++s) if (inv.HasSlotItem(s)) shownSlots.push_back(s);
                float slotSize = 50.0f;
                float slotSpacing = 10.0f;
                const float n = static_cast<float>(shownSlots.size());
                float startX = (screen.x - (n * slotSize + (n - 1.0f) * slotSpacing)) * 0.5f;
                float startY = 15.0f;

                for (int k = 0; k < static_cast<int>(shownSlots.size()); ++k) {
                    const int i = shownSlots[k];
                    glm::vec2 pos = glm::vec2(startX + k * (slotSize + slotSpacing), startY);
                    bool isActive = (i == inv.currentSlot);
                    bool hasWeapon = inv.HasSlotItem(i);

                    glm::vec4 boxColor = isActive ? glm::vec4(0.55f, 0.42f, 0.20f, 0.92f) :
                        hasWeapon ? glm::vec4(0.13f, 0.12f, 0.115f, 0.85f) :
                        glm::vec4(0.06f, 0.06f, 0.065f, 0.5f);

                    Renderer::DrawQuadUI(pos, glm::vec2(slotSize, slotSize), boxColor, screen);

                    if (isActive) {
                        Renderer::DrawQuadUI(pos + glm::vec2(3.0f, 3.0f), glm::vec2(slotSize - 6.0f, 3.0f), glm::vec4(0.95f, 0.77f, 0.36f, 1.0f), screen);
                    }
                }
            }

            float dotSize = 4.0f;
            glm::vec2 dotPos = center - glm::vec2(dotSize * 0.5f);
            // Preparazione del tiro: quattro tacche che si stringono attorno al punto man mano che la mira si
            // assesta; a mira assestata il punto diventa verde (il prossimo colpo va esattamente li').
            float steady = 0.0f;
            bool rangedInHand = false;
            if (const auto* ws = m_Scene.GetRegistry().try_get<WeaponStateComponent>(m_PlayerEntity)) steady = ws->aimSteady;
            if (const auto* inv = m_Scene.GetRegistry().try_get<InventoryComponent>(m_PlayerEntity)) {
                const auto* bp = BlueprintManager::Get().GetWeapon(inv->GetActiveWeaponId());
                rangedInHand = bp && !bp->is_melee && !bp->is_equipment;
            }
            const bool settled = rangedInHand && steady >= 0.98f;
            if (rangedInHand) {
                const float gap = 4.0f + (1.0f - steady) * 14.0f, len = 5.0f, th = 2.0f;
                const glm::vec4 tc = settled ? glm::vec4(0.4f, 1.0f, 0.5f, 0.9f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.55f + 0.35f * steady);
                Renderer::DrawQuadUI(center + glm::vec2(-th * 0.5f, -gap - len), glm::vec2(th, len), tc, screen);   // su
                Renderer::DrawQuadUI(center + glm::vec2(-th * 0.5f, gap), glm::vec2(th, len), tc, screen);          // giu'
                Renderer::DrawQuadUI(center + glm::vec2(-gap - len, -th * 0.5f), glm::vec2(len, th), tc, screen);   // sinistra
                Renderer::DrawQuadUI(center + glm::vec2(gap, -th * 0.5f), glm::vec2(len, th), tc, screen);          // destra
            }
            Renderer::DrawQuadUI(dotPos, glm::vec2(dotSize, dotSize), settled ? glm::vec4(0.4f, 1.0f, 0.5f, 1.0f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.9f), screen);

            if (m_Scene.GetRegistry().all_of<MeleeStateComponent>(m_PlayerEntity)) {
                auto& melee = m_Scene.GetRegistry().get<MeleeStateComponent>(m_PlayerEntity);
                bool isParrying = (melee.currentAttack == MeleeAttackType::Parry) &&
                    (melee.currentPhase != MeleePhase::None);

                if (isParrying) {
                    bool isPerfectWindow = (melee.currentPhase == MeleePhase::Active);

                    glm::vec4 shieldColor = isPerfectWindow
                        ? glm::vec4(0.0f, 0.9f, 1.0f, 0.95f)
                        : glm::vec4(0.9f, 0.65f, 0.1f, 0.80f);

                    glm::vec4 coreColor = isPerfectWindow
                        ? glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)
                        : glm::vec4(1.0f, 0.85f, 0.3f, 0.90f);

                    Renderer::DrawQuadUI(center + glm::vec2(-16.0f, -18.0f), glm::vec2(32.0f, 4.0f), shieldColor, screen);
                    Renderer::DrawQuadUI(center + glm::vec2(-14.0f, -14.0f), glm::vec2(28.0f, 16.0f), shieldColor, screen);
                    Renderer::DrawQuadUI(center + glm::vec2(-10.0f, 2.0f), glm::vec2(20.0f, 10.0f), shieldColor, screen);
                    Renderer::DrawQuadUI(center + glm::vec2(-5.0f, 12.0f), glm::vec2(10.0f, 8.0f), shieldColor, screen);

                    Renderer::DrawQuadUI(center + glm::vec2(-3.0f, -10.0f), glm::vec2(6.0f, 20.0f), coreColor, screen);
                    Renderer::DrawQuadUI(center + glm::vec2(-10.0f, -4.0f), glm::vec2(20.0f, 6.0f), coreColor, screen);
                }
            }

            if (m_Scene.GetRegistry().all_of<WeaponStateComponent, WeaponBlueprintId>(m_PlayerEntity)) {
                auto& weaponState = m_Scene.GetRegistry().get<WeaponStateComponent>(m_PlayerEntity);
                const auto* weaponBP = BlueprintManager::Get().GetWeapon(
                    m_Scene.GetRegistry().get<WeaponBlueprintId>(m_PlayerEntity).id
                );

                if (weaponBP && weaponState.isReloading && weaponBP->reload_time_sec > 0.0f) {
                    float progress = std::clamp(1.0f - (weaponState.reloadTimer / weaponBP->reload_time_sec), 0.0f, 1.0f);
                    int totalSegments = 16;
                    int activeSegments = static_cast<int>(progress * totalSegments);
                    float radius = 18.0f;

                    for (int i = 0; i < totalSegments; ++i) {
                        float angle = glm::radians(i * (360.0f / totalSegments) - 90.0f);
                        glm::vec2 segPos = center + glm::vec2(std::cos(angle), std::sin(angle)) * radius - glm::vec2(2.5f);

                        glm::vec4 segColor = (i <= activeSegments)
                            ? glm::vec4(0.0f, 0.9f, 1.0f, 0.95f)
                            : glm::vec4(0.2f, 0.2f, 0.2f, 0.5f);

                        Renderer::DrawQuadUI(segPos, glm::vec2(5.0f, 5.0f), segColor, screen);
                    }
                }
            }

            if (m_Scene.GetRegistry().all_of<WeaponStateComponent, WeaponBlueprintId>(m_PlayerEntity)) {
                auto& weaponState = m_Scene.GetRegistry().get<WeaponStateComponent>(m_PlayerEntity);
                const auto* weaponBP = BlueprintManager::Get().GetWeapon(
                    m_Scene.GetRegistry().get<WeaponBlueprintId>(m_PlayerEntity).id
                );

                if (weaponBP && weaponBP->heat_capacity > 0.0f) {
                    float heatRatio = std::clamp(weaponState.currentHeat / weaponBP->heat_capacity, 0.0f, 1.0f);

                    if (heatRatio > 0.01f || weaponState.isOverheating) {
                        float barWidth = 40.0f;
                        float barHeight = 4.0f;
                        glm::vec2 barPos = glm::vec2(center.x - (barWidth * 0.5f), center.y + 14.0f);

                        Renderer::DrawQuadUI(barPos, glm::vec2(barWidth, barHeight), glm::vec4(0.1f, 0.1f, 0.1f, 0.6f), screen);

                        glm::vec4 heatColor = weaponState.isOverheating ? glm::vec4(1.0f, 0.1f, 0.1f, 1.0f) :
                            glm::vec4(1.0f, 1.0f - (heatRatio * 0.8f), 0.0f, 0.9f);

                        Renderer::DrawQuadUI(barPos, glm::vec2(barWidth * heatRatio, barHeight), heatColor, screen);
                    }
                }
            }

            // La barra della salute ora e' in alto a sinistra (RenderPlayerStatusHUD), con stamina e buff di fazione
        }

        m_PerfRenderMs = m_PerfRenderMs * 0.9f + static_cast<float>((glfwGetTime() - m_PerfRenderStart) * 1000.0) * 0.1f;
        OnImGuiRender();
    }

    void GameLayer::OnImGuiRender()
    {
        GLFWwindow* window = glfwGetCurrentContext();
        if (!window) return;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (m_GameState == GameState::MainMenu && m_ShowCustomizeModal && m_CurrentTab == MenuTab::StasisVault) {
            // personalizzazione: solo il suo pannello, il resto dello schermo e' l'anteprima del personaggio
            RenderCustomizeModal();
            RenderOptionsMenu();
        }
        else if (m_GameState == GameState::MainMenu) {
            RenderTopNavBar();

            if (m_CurrentTab == MenuTab::StasisVault) {
                RenderStasisVaultUI();
            }
            else if (m_CurrentTab == MenuTab::Deployment) {
                RenderDeploymentUI();
            }
            else if (m_CurrentTab == MenuTab::Research) {
                RenderResearchUI();
            }
            else if (m_CurrentTab == MenuTab::Shop) {
                RenderShopUI();
            }
            else if (m_CurrentTab == MenuTab::Servers) {
                RenderServerBrowserUI();
            }
            else if (m_CurrentTab == MenuTab::Showcase) {
                RenderShowcaseUI();
            }

            if (m_CurrentTab == MenuTab::StasisVault || m_CurrentTab == MenuTab::Deployment) RenderPartyPanel();
            RenderPartyOverlay();
            RenderRecruitModal();
            RenderOptionsMenu();
            RenderCustomizeModal();
        }
        else if (m_GameState == GameState::MapEditor) {
            RenderTopNavBar();
            RenderMapEditorUI();
            RenderEditorGizmos3D();
            RenderOptionsMenu();
        }
        else if (m_GameState == GameState::Gameplay) {
            auto* playerHealth = m_Scene.GetRegistry().try_get<HealthComponent>(m_PlayerEntity);
            bool isPlayerDead = playerHealth && (playerHealth->currentHealth <= 0.0f);

            if (!isPlayerDead && !m_IsPaused) {
                RenderDamageVignette();
                RenderVfxTexts();
                RenderTrainingDummyLabels();
                RenderHitMarkers();
                RenderAmmoHUD();
                RenderPlayerStatusHUD();
                RenderOrdersRadial();
                RenderAuspex();
                RenderTurretHUD();
                RenderNetMatchOverlay();
                RenderPartyOverlay();   // avvisi della squadra anche in partita (cambio di squadra, nuovo host...)
                if (m_VsBotActive) RenderVsBotHUD();
                else {
                    // Uccisioni dell'orda in alto a destra (in alto a sinistra ci sono salute, stamina e buff)
                    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 20.0f, 20.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
                    ImGui::Begin("KillCounterOverlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
                    ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "VREPIGHS ELIMINATI: %d", m_KillCount);
                    ImGui::End();
                }
            }

            if (m_VsBotActive && m_VsBotResult == 0 && Input::IsKeyPressed(GLFW_KEY_TAB)) RenderVsBotScoreboard();

            if (m_VsBotActive && m_VsBotResult != 0) {
                RenderVsBotResults();
            }
            else if (isPlayerDead && m_VsBotActive) {
                RenderVsBotDeathUI();
            }
            else if (isPlayerDead) {
                RenderRespawnUI();
            }
            else if (m_IsPaused) {
                ImGuiIO& io = ImGui::GetIO();
                ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(380, 470));

                ImGui::Begin("MENU DI PAUSA", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
                {   // sole in eclissi e nome, centrati
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const float cw = ImGui::GetContentRegionAvail().x;
                    Ui::EclipseSigil(ImGui::GetWindowDrawList(), ImVec2(p.x + cw * 0.5f, p.y + 26.0f), 22.0f, Ui::U32(Ui::Col::Gold));
                    ImGui::Dummy(ImVec2(cw, 54.0f));
                    ImFont* ft = Ui::FontTitle();
                    const float tw = ft->CalcTextSizeA(ft->FontSize, FLT_MAX, 0.0f, "SUN ENGINE").x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cw - tw) * 0.5f);
                    Ui::Title("SUN ENGINE");
                    Ui::Divider();
                }

                if (Ui::PlainButton("Riprendi Gioco", ImVec2(-1, 40))) {
                    m_IsPaused = false;
                    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                }

                ImGui::Spacing();

                if (Ui::PlainButton("Opzioni", ImVec2(-1, 40))) {
                    m_ShowOptionsMenu = true;
                }

                ImGui::Spacing();

                bool backToMenu = Ui::PlainButton("Torna al Menu Principale", ImVec2(-1, 40));
                if (backToMenu && m_VsBotActive) {
                    LeaveVsBotMatch();   // abbandono della partita Vs Bot: nessuna ricompensa
                    backToMenu = false;
                }
                if (backToMenu) {
                    ApplyMatchXp();   // sopravvivenza: quello che hai fatto finora conta
                    m_GameState = GameState::MainMenu;
                    m_IsPaused = false;
                    m_PlayerDeathHandled = false;
                    g_WaaaghActive = false;
                    g_WaaaghTimer = 0.0f;
                    g_WaaaghSpawnCount = 1;

                    auto enemyView = m_Scene.GetRegistry().view<HealthComponent>(entt::exclude<PlayerTag>);
                    for (auto e : enemyView) {
                        m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
                        m_Scene.DestroyEntity(e);
                    }

                    if (auto* hp = m_Scene.GetRegistry().try_get<HealthComponent>(m_PlayerEntity)) {
                        hp->currentHealth = hp->maxHealth;
                    }

                    if (window) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                }

                if (Ui::PlainButton("Esci dal Gioco", ImVec2(-1, 40))) {
                    // FIX #15: uscita pulita. Application::Run controlla glfwWindowShouldClose, poi
                    // vengono eseguiti i distruttori (ImGui/audio/GL). std::exit(0) li saltava tutti.
                    if (window) glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
                ImGui::End();

                RenderOptionsMenu();
            }
        }

        // Primo avvio: avviso della qualita' grafica scelta in automatico (una volta sola)
        if (!m_GfxAutoNotice.empty() && m_GameState == GameState::MainMenu) {
            const ImVec2 ds = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y - 24.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(std::min(620.0f, ds.x - 40.0f), 0.0f));
            ImGui::SetNextWindowBgAlpha(0.9f);
            if (ImGui::Begin("##GfxAuto", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "QUALITA' GRAFICA");
                ImGui::TextWrapped("%s", m_GfxAutoNotice.c_str());
                if (Ui::PlainButton("Ok", ImVec2(90, 0))) m_GfxAutoNotice.clear();
                ImGui::SameLine();
                if (Ui::PlainButton("Apri le opzioni", ImVec2(140, 0))) { m_GfxAutoNotice.clear(); m_ShowOptionsMenu = true; }
            }
            ImGui::End();
        }
        if (m_ShowPerf) RenderPerfOverlay();
        Ui::DecorateWindows();   // angolari d'ottone e testate su tutte le finestre
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    // Pannello delle prestazioni (F3, oppure Opzioni -> GRAFICA): dice dove va il tempo di ogni fotogramma
    void GameLayer::RenderPerfOverlay() {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.72f);
        const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##Perf", nullptr, fl)) {
            const float fps = m_PerfFrameMs > 0.01f ? 1000.0f / m_PerfFrameMs : 0.0f;
            const ImVec4 col = fps >= 55.0f ? ImVec4(0.50f, 0.74f, 0.44f, 1.0f) : (fps >= 30.0f ? ImVec4(0.95f, 0.77f, 0.36f, 1.0f) : ImVec4(0.90f, 0.30f, 0.17f, 1.0f));
            ImGui::TextColored(col, "%.0f FPS  (%.1f ms)", fps, m_PerfFrameMs);
#ifndef NDEBUG
            ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "VERSIONE DEBUG: molto piu' lenta.");
            ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "Compila in x64-Release per giocare.");
#endif
            const RenderQuality& q = m_DeferredRenderer.GetQuality();
            ImGui::TextDisabled("%dx%d interna (%.0f%%)%s", m_DeferredRenderer.GetRenderWidth(), m_DeferredRenderer.GetRenderHeight(),
                                q.renderScale * 100.0f, q.vsync ? "  VSync" : "");
            ImGui::Separator();
            ImGui::Text("CPU logica      %5.1f ms", m_PerfUpdateMs);
            ImGui::Text("CPU disegno     %5.1f ms", m_PerfRenderMs);
            static const char* names[GpuT_Count] = { "GPU ombre", "GPU geometria", "GPU occlusione", "GPU illuminaz.", "GPU post" };
            float gpuTotal = 0.0f;
            for (int i = 0; i < GpuT_Count; ++i) {
                const float ms = m_DeferredRenderer.GetGpuMs(i);
                gpuTotal += ms;
                ImGui::Text("%-15s %5.1f ms", names[i], ms);
            }
            ImGui::Text("GPU totale      %5.1f ms", gpuTotal);
            ImGui::TextDisabled("Oggetti scartati: %d", m_DeferredRenderer.GetCulledCount());
            // chi e' il collo di bottiglia
            const float cpu = m_PerfUpdateMs + m_PerfRenderMs;
            ImGui::TextDisabled(gpuTotal > cpu ? "Limite: scheda video" : "Limite: processore");
        }
        ImGui::End();
    }

    void GameLayer::RenderTopNavBar() {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, 60));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("TopBar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar(2);

        // Fascia: ferro scuro che sfuma, filo d'ottone sotto, sole in eclissi accanto al nome
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 barA(0.0f, 0.0f), barB(io.DisplaySize.x, 60.0f);
        dl->AddRectFilledMultiColor(barA, barB, IM_COL32(26, 23, 20, 255), IM_COL32(26, 23, 20, 255), IM_COL32(10, 10, 12, 255), IM_COL32(10, 10, 12, 255));
        dl->AddRectFilled(ImVec2(0.0f, 58.0f), barB, Ui::U32(Ui::Col::Brass, 0.9f));
        dl->AddRectFilled(ImVec2(0.0f, 55.0f), ImVec2(io.DisplaySize.x, 56.0f), Ui::U32(Ui::Col::Brass, 0.25f));
        Ui::EclipseSigil(dl, ImVec2(34.0f, 29.0f), 19.0f, Ui::U32(Ui::Col::Gold));
        {
            ImFont* ft = Ui::FontTitle(); ImFont* fs = Ui::FontSmall();
            dl->AddText(ft, ft->FontSize * 0.80f, ImVec2(64.0f, 8.0f), Ui::U32(Ui::Col::Bone), "PROJECT SUNCOLLECTOR");
            dl->AddText(fs, fs->FontSize, ImVec2(66.0f, 35.0f), Ui::U32(Ui::Col::Ash), "S U N   E N G I N E");
        }

        const bool inMenu = (m_GameState == GameState::MainMenu);
        // Schede di navigazione, centrate
        struct Nav { const char* label; bool active; ImVec4 accent; };
        const Nav navs[] = {
            { "GARAGE", inMenu && m_CurrentTab == MenuTab::StasisVault, Ui::Col::Gold },
            { "SCHIERAMENTO", inMenu && m_CurrentTab == MenuTab::Deployment, Ui::Col::Gold },
            { "SERVER", inMenu && m_CurrentTab == MenuTab::Servers, Ui::Col::Phosphor },
            { "RICERCA", inMenu && m_CurrentTab == MenuTab::Research, Ui::Col::Gold },
            { "NEGOZIO", inMenu && m_CurrentTab == MenuTab::Shop, Ui::Col::Amethyst },
            { "VETRINA", inMenu && m_CurrentTab == MenuTab::Showcase, Ui::Col::Gold },
            { "EDITOR", m_GameState == GameState::MapEditor, Ui::Col::Rust },
            { "OPZIONI", false, Ui::Col::Phosphor },
        };
        constexpr int kNavCount = static_cast<int>(sizeof(navs) / sizeof(navs[0]));
        float navW[kNavCount], totalNav = 0.0f;
        {
            ImFont* fh = Ui::FontHeading();
            for (int i = 0; i < kNavCount; ++i) { navW[i] = fh->CalcTextSizeA(fh->FontSize * 0.92f, FLT_MAX, 0.0f, navs[i].label).x + 34.0f; totalNav += navW[i]; }
        }
        ImGui::SetCursorPos(ImVec2(std::max(300.0f, io.DisplaySize.x * 0.5f - totalNav * 0.5f), 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        int clickedNav = -1;
        for (int i = 0; i < kNavCount; ++i) {
            if (Ui::NavTab(navs[i].label, navW[i], 58.0f, navs[i].active, navs[i].accent)) clickedNav = i;
            if (i < kNavCount - 1) ImGui::SameLine();
        }
        ImGui::PopStyleVar();
        // dalla scheda cliccata all'azione (per nome: l'ordine delle schede si puo' cambiare liberamente)
        const std::string nav = clickedNav >= 0 ? navs[clickedNav].label : "";
        if (nav == "SERVER") {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::Servers;
            m_Party.RefreshServerList();
        }
        clickedNav = nav == "GARAGE" ? 0 : nav == "SCHIERAMENTO" ? 1 : nav == "RICERCA" ? 2 : nav == "NEGOZIO" ? 3 : nav == "VETRINA" ? 4 : nav == "EDITOR" ? 5 : nav == "OPZIONI" ? 6 : -1;

        if (clickedNav == 0) {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::StasisVault;
            if (!m_PlayerRoster.empty() && m_SelectedVaultIndex < static_cast<int>(m_PlayerRoster.size())) {
                ApplyUnitVisuals(m_PlayerRoster[m_SelectedVaultIndex]);
            }
        }
        else if (clickedNav == 1) {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::Deployment;
            if (!m_SelectedSquadIndices.empty() && m_SelectedSquadIndices[0] < static_cast<int>(m_PlayerRoster.size())) {
                ApplyUnitVisuals(m_PlayerRoster[m_SelectedSquadIndices[0]]);
            }
        }
        else if (clickedNav == 2) {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::Research;
        }
        else if (clickedNav == 3) {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::Shop;
        }
        else if (clickedNav == 4) {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::Showcase;
            m_ShowcaseFocusUnit.clear();
            m_ShowcaseFromResearch = false;
        }
        else if (clickedNav == 5) {
            m_GameState = GameState::MapEditor;
            if (m_EditorMap.structures.empty()) {
                m_EditorMap.playerSpawns.push_back(glm::vec3(0.0f, 0.5f, 10.0f));
                m_EditorMap.enemySpawns.push_back(glm::vec3(0.0f, 0.5f, -10.0f));
            }
            SyncEditorEntitiesWithScene();
        }
        else if (clickedNav == 6) {
            m_ShowOptionsMenu = true;
        }

        // Valute del profilo, allineate a destra
        {
            ImFont* fs = Ui::FontSmall(); ImFont* fv = Ui::FontStrong();
            char eb[32], sb[32];
            std::snprintf(eb, sizeof(eb), "%lld", static_cast<long long>(m_Profile.energy));
            std::snprintf(sb, sizeof(sb), "%lld", static_cast<long long>(m_Profile.blackStone));
            const float wE = 26.0f + fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.0f, "ENERGIA").x + fv->CalcTextSizeA(fv->FontSize, FLT_MAX, 0.0f, eb).x;
            const float wS = 26.0f + fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.0f, "PIETRA NERA").x + fv->CalcTextSizeA(fv->FontSize, FLT_MAX, 0.0f, sb).x;
            ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - wE - wS - 48.0f, 18.0f));
            Ui::Currency("ENERGIA", static_cast<long long>(m_Profile.energy), Ui::Col::Gold);
            ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - wS - 22.0f, 18.0f));
            Ui::Currency("PIETRA NERA", static_cast<long long>(m_Profile.blackStone), Ui::Col::Amethyst);
        }

        ImGui::End();
    }

    // ===================== MODELLI A BLOCCHI DELLE ARME =====================
    void GameLayer::LoadBlockModels() {
        const std::string dir = FindValidDir("BlockModels");
        std::vector<std::string> errors;
        const int n = m_BlockModels.LoadDirectory(dir, &errors);
        for (const auto& e : errors) SUN_CORE_WARN("BlockModels: {0}", e);
        SUN_CORE_INFO("Modelli a blocchi caricati: {0} (cartella: {1})", n, dir);
    }

    // Associa all'entity dell'arma il modello a blocchi corrispondente (se esiste), altrimenti torna al cubo.
    void GameLayer::SetWeaponBlockModel(entt::entity weaponEntity, const std::string& weaponId) {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(weaponEntity)) return;
        const BlockModel* model = m_BlockModels.Find(weaponId);
        if (model) {
            if (auto* c = reg.try_get<BlockModelComponent>(weaponEntity)) c->model = model;
            else reg.emplace<BlockModelComponent>(weaponEntity, BlockModelComponent{ model });
            reg.get<TransformComponent>(weaponEntity).Scale = glm::vec3(1.0f);   // il modello e' gia' in metri
        }
        else if (reg.all_of<BlockModelComponent>(weaponEntity)) {
            reg.remove<BlockModelComponent>(weaponEntity);
        }
    }

    // ===================== RICERCA UNITA' =====================
    void GameLayer::LoadResearchTree() {
        const std::string dir = FindValidDir("Research");
        const std::filesystem::path file = std::filesystem::path(dir) / "research_tree.json";
        std::string error;
        if (!m_ResearchTree.Load(file, &error)) {
            SUN_CORE_WARN("Albero di ricerca non caricato ({0}): {1}", file.string(), error);
            return;
        }
        m_ResearchTree.EnsureRootsResearched(m_Profile);
        m_ResearchTree.MigrateProfile(m_Profile);   // id rinominati e rami riordinati
        if (m_PlayerRoster.empty()) BuildStarterRoster();
        std::vector<std::string> owned;
        for (const auto& u : m_PlayerRoster) owned.push_back(u.blueprintId);
        m_ResearchTree.GrandfatherUnits(m_Profile, owned);   // chi ha gia' un garage non perde nulla
        SUN_CORE_INFO("Albero di ricerca caricato: {0} fazioni", m_ResearchTree.GetFactions().size());
    }

    // Garage di un profilo nuovo: solo l'unita' iniziale (nodo radice) di ciascun ramo dell'albero di ricerca,
    // col suo nome e colore predefiniti: oggi coscritto, neofita e gretchin. Se l'albero non e' leggibile si
    // usano direttamente quelle tre unita'.
    void GameLayer::BuildStarterRoster() {
        m_PlayerRoster.clear();
        if (m_ResearchTree.IsLoaded()) {
            for (const auto& faction : m_ResearchTree.GetFactions()) {
                for (const auto& n : faction.nodes) {
                    if (!n.parentId.empty() || !BlueprintManager::Get().GetUnit(n.unitId)) continue;
                    if (static_cast<int>(m_PlayerRoster.size()) >= MAX_ROSTER_SIZE) break;
                    m_PlayerRoster.push_back(TrainedUnit{ n.defaultName, n.unitId,
                        glm::vec4(n.color[0], n.color[1], n.color[2], n.color[3]) });
                    m_PlayerRoster.back().ApplyDefaultLoadout();
                }
            }
        }
        if (m_PlayerRoster.empty()) {
            m_PlayerRoster.push_back(TrainedUnit{ "Coscritto", "imperium_conscript", glm::vec4(0.55f, 0.45f, 0.30f, 1.0f) });
            m_PlayerRoster.push_back(TrainedUnit{ "Neofita Corvo", "spacemarines_neophyte", glm::vec4(0.92f, 0.72f, 0.68f, 1.0f) });
            m_PlayerRoster.push_back(TrainedUnit{ "Snik", "orks_gretchin", glm::vec4(0.34f, 0.40f, 0.21f, 1.0f) });
            for (auto& u : m_PlayerRoster) u.ApplyDefaultLoadout();
        }
        m_SelectedVaultIndex = 0;
        m_SelectedSquadIndices.clear();
        m_SelectedSquadIndices.push_back(0);
        SUN_CORE_INFO("Profilo nuovo: garage iniziale con {0} unita' (una per ramo)", m_PlayerRoster.size());
    }

    // Addebita l'energia e aggiunge una nuova unita' al garage.
    bool GameLayer::PurchaseResearchedUnit(const ResearchNode& node) {
        if (static_cast<int>(m_PlayerRoster.size()) >= MAX_ROSTER_SIZE) return false;
        if (!BlueprintManager::Get().GetUnit(node.unitId)) return false;
        if (!m_ResearchTree.Pay(m_Profile, node)) return false;

        int sameCount = 0;
        for (const auto& u : m_PlayerRoster) if (u.blueprintId == node.unitId) ++sameCount;
        std::string name = node.defaultName;
        if (sameCount > 0) name += " " + std::to_string(sameCount + 1);

        TrainedUnit newUnit{ name, node.unitId, glm::vec4(node.color[0], node.color[1], node.color[2], node.color[3]) };
        newUnit.ApplyDefaultLoadout();
        m_PlayerRoster.push_back(newUnit);
        m_SelectedVaultIndex = static_cast<int>(m_PlayerRoster.size()) - 1;
        return true;
    }

    void GameLayer::RenderResearchUI() {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 60.0f));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - 60.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f, 16.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##RICERCA", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar(2);
        Ui::Backdrop(ImGui::GetWindowDrawList(), ImVec2(0.0f, 60.0f), io.DisplaySize);

        if (!m_ResearchTree.IsLoaded()) {
            Ui::Title("ARCHIVIO DI RICERCA");
            ImGui::TextColored(Ui::Col::Ember, "Albero di ricerca non caricato.");
            ImGui::TextDisabled("Manca GameData/Research/research_tree.json (controlla anche la copia nella cartella di build).");
            ImGui::End();
            return;
        }

        if (m_ResearchMessageTimer > 0.0f) m_ResearchMessageTimer -= io.DeltaTime;

        // Testata: titolo a sinistra, come funziona a destra
        Ui::Title("ARCHIVIO DI RICERCA");
        {
            const char* hint = "I punti studio si guadagnano giocando un'unita' e aprono le sue ricerche e le unita' che seguono.";
            ImFont* fs = Ui::FontSmall();
            const float w = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.0f, hint).x;
            if (io.DisplaySize.x > w + 520.0f) {
                ImGui::SameLine(ImGui::GetWindowWidth() - w - 26.0f);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.0f);
                Ui::TextIn(fs, Ui::Col::Ash, hint);
            }
        }

        // Fazioni: schede col colore della fazione
        const auto& factions = m_ResearchTree.GetFactions();
        static std::string s_Faction;
        const ResearchFaction* current = nullptr;
        for (const auto& f : factions) if (f.id == s_Faction) current = &f;
        if (!current && !factions.empty()) { current = &factions.front(); s_Faction = current->id; }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
        for (const auto& f : factions) {
            std::string label = f.name;
            for (auto& ch : label) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            label += "##fac_" + f.id;
            if (Ui::NavTab(label.c_str(), 0.0f, 38.0f, current == &f, ImVec4(f.accent[0], f.accent[1], f.accent[2], 1.0f))) { s_Faction = f.id; current = &f; }
            ImGui::SameLine();
        }
        ImGui::PopStyleVar();
        ImGui::NewLine();
        {   // filo sotto le schede
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y - 5.0f), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 4.0f), Ui::U32(Ui::Col::Brass, 0.45f));
        }
        if (current) DrawResearchFactionTree(*current);
        ImGui::End();
    }

    void GameLayer::DrawResearchFactionTree(const ResearchFaction& faction) {
        ImGuiIO& io = ImGui::GetIO();
        const float nodeW = 240.0f, nodeH = 92.0f, gapX = 84.0f, gapY = 30.0f, pad = 30.0f;
        const ImVec4 accent(faction.accent[0], faction.accent[1], faction.accent[2], 1.0f);

        // Nell'albero solo i personaggi; gli oggetti di ciascuno si vedono selezionandolo (pannello sotto)
        float maxCol = 0.0f, maxRow = 0.0f;
        for (const auto& n : faction.nodes) { if (n.unitId.empty()) continue; maxCol = std::max(maxCol, n.col); maxRow = std::max(maxRow, n.row); }
        const float totalW = pad * 2.0f + (maxCol + 1.0f) * nodeW + maxCol * gapX;
        const float totalH = pad * 2.0f + (maxRow + 1.0f) * nodeH + maxRow * gapY;

        // Selezione valida solo se appartiene a questa fazione
        const ResearchNode* selected = nullptr;
        for (const auto& n : faction.nodes) if (n.id == m_ResearchSelectedNode) selected = &n;
        if (!selected && !faction.nodes.empty()) selected = &faction.nodes.front();
        // Personaggio evidenziato nell'albero: quello selezionato, o il padrone dell'oggetto selezionato
        const ResearchNode* selectedUnit = selected;
        if (selected && selected->unitId.empty()) selectedUnit = m_ResearchTree.FindStudySource(*selected);

        // Ricerche che si pagano coi punti studio di un personaggio (oggetti e personaggi successivi)
        auto researchesOf = [&](const ResearchNode& unitNode) { return m_ResearchTree.ResearchesOf(unitNode); };
        // Maestria: tutte le ricerche legate al personaggio completate (sole in eclissi dorato)
        auto isMastered = [&](const ResearchNode& unitNode) { return m_ResearchTree.IsMastered(m_Profile, unitNode); };

        // ---------- Albero (da sinistra a destra) ----------
        const float treeH = std::max(220.0f, (io.DisplaySize.y - 60.0f) * 0.50f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.35f));
        ImGui::BeginChild(("##tree_" + faction.id).c_str(), ImVec2(0, treeH), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // si puo' trascinare l'albero col tasto destro o centrale
        if (ImGui::IsWindowHovered() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2.0f))) {
            ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
        }
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        auto nodePos = [&](const ResearchNode& n) {
            return ImVec2(std::floor(origin.x + pad + n.col * (nodeW + gapX)), std::floor(origin.y + pad + n.row * (nodeH + gapY)));
        };

        // Rami padre -> figlio (a gomito), anche dai padri alternativi (ne basta uno)
        for (const auto& n : faction.nodes) {
            if (n.unitId.empty()) continue;   // oggetti: non nell'albero
            const bool unlocked = m_ResearchTree.IsResearched(m_Profile, n);
            const ImVec2 cp = nodePos(n);
            const ImVec2 b(cp.x, cp.y + nodeH * 0.5f);
            if (const ResearchNode* parent = m_ResearchTree.FindParent(n)) {
                const ImVec2 pp = nodePos(*parent);
                Ui::DrawBranch(dl, ImVec2(pp.x + nodeW, pp.y + nodeH * 0.5f), b, unlocked, accent);
            }
            if (!n.anyParents.empty()) {
                for (const ResearchNode* par : m_ResearchTree.Parents(n)) {
                    if (par->id == n.parentId) continue;   // il primo e' gia' disegnato sopra
                    const ImVec2 pp = nodePos(*par);
                    Ui::DrawBranch(dl, ImVec2(pp.x + nodeW, pp.y + nodeH * 0.5f), b, unlocked, accent);
                }
            }
        }
        // Rami che si riuniscono: dall'unita' da padroneggiare scende (o sale) un tratteggio fino alla riga del
        // nodo e poi entra da sinistra. Oro pieno = maestria ottenuta.
        for (const auto& n : faction.nodes) {
            if (n.unitId.empty()) continue;
            for (const auto& reqId : n.requiresMastered) {
                if (reqId == n.parentId) continue;   // gia' collegato dal ramo normale
                const ResearchNode* req = m_ResearchTree.FindNode(reqId);
                if (!req) continue;
                const ImVec2 rp = nodePos(*req), cp = nodePos(n);
                const bool down = cp.y > rp.y;
                Ui::DrawMasteryBranch(dl, ImVec2(rp.x + nodeW * 0.5f, down ? rp.y + nodeH : rp.y), ImVec2(cp.x, cp.y + nodeH * 0.5f), isMastered(*req));
            }
        }

        // Nodi (solo personaggi)
        for (const auto& n : faction.nodes) {
            if (n.unitId.empty()) continue;
            const ImVec2 p = nodePos(n);
            const ResearchStatus st = m_ResearchTree.GetStatus(m_Profile, n);
            const bool hasBp = BlueprintManager::Get().GetUnit(n.unitId) != nullptr;
            int owned = 0;
            for (const auto& u : m_PlayerRoster) owned += (u.blueprintId == n.unitId ? 1 : 0);

            ImGui::SetCursorScreenPos(p);
            ImGui::InvisibleButton(("##node_" + n.id).c_str(), ImVec2(nodeW, nodeH));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) m_ResearchSelectedNode = n.id;

            const long long ps = static_cast<long long>(ResearchTree::GetStudyPoints(m_Profile, n.unitId));
            char info[96];
            int itemsTotal = 0, itemsDone = 0;
            for (const auto* r : researchesOf(n)) if (!r->itemId.empty()) { ++itemsTotal; itemsDone += m_ResearchTree.IsResearched(m_Profile, *r) ? 1 : 0; }
            if (itemsTotal > 0) std::snprintf(info, sizeof(info), "PUNTI STUDIO %lld   -   OGGETTI %d/%d", ps, itemsDone, itemsTotal);
            else std::snprintf(info, sizeof(info), "PUNTI STUDIO %lld", ps);
            char status[96];
            if (!hasBp && st == ResearchStatus::Researched) std::snprintf(status, sizeof(status), "IN ARRIVO (sbloccata)");
            else if (!hasBp && st == ResearchStatus::Researchable) std::snprintf(status, sizeof(status), "IN ARRIVO - %lld PS", static_cast<long long>(n.researchCost));
            else if (st == ResearchStatus::LockedMastery) std::snprintf(status, sizeof(status), "SERVE LA MAESTRIA");
            else if (!hasBp && st == ResearchStatus::LockedPoints) std::snprintf(status, sizeof(status), "IN ARRIVO - servono %lld PS", static_cast<long long>(n.researchCost));
            else if (!hasBp) std::snprintf(status, sizeof(status), "IN ARRIVO");
            else if (st == ResearchStatus::Researched) std::snprintf(status, sizeof(status), "SBLOCCATA - in garage %d", owned);
            else if (st == ResearchStatus::Researchable) std::snprintf(status, sizeof(status), "RICERCA - %lld PS", static_cast<long long>(n.researchCost));
            else if (st == ResearchStatus::LockedPoints) std::snprintf(status, sizeof(status), "SERVONO %lld PS", static_cast<long long>(n.researchCost));
            else std::snprintf(status, sizeof(status), "BLOCCATA");

            Ui::NodeCard card;
            card.name = n.name.c_str(); card.status = status; card.info = info; card.tier = n.tier;
            card.unitColor = ImVec4(n.color[0], n.color[1], n.color[2], 1.0f);
            card.state = st == ResearchStatus::Researched ? Ui::NodeState::Done : (st == ResearchStatus::Researchable ? Ui::NodeState::Available : Ui::NodeState::Locked);
            card.selected = (selectedUnit && selectedUnit->id == n.id);
            card.hovered = hovered;
            card.mastered = isMastered(n);
            card.missing = !hasBp;
            // barra: quanto manca al costo, coi punti studio di chi paga la ricerca
            if (st != ResearchStatus::Researched && n.researchCost > 0) {
                if (const ResearchNode* payer = m_ResearchTree.PaySource(m_Profile, n))
                    card.progress = static_cast<float>(ResearchTree::GetStudyPoints(m_Profile, payer->unitId)) / static_cast<float>(n.researchCost);
            }
            Ui::DrawNodeCard(dl, p, ImVec2(nodeW, nodeH), card);
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(totalW, totalH));   // estende l'area scorrevole fino ai nodi piu' lontani
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // ---------- Dettaglio del nodo selezionato ----------
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.045f, 0.043f, 0.042f, 0.92f));
        ImGui::BeginChild(("##detail_" + faction.id).c_str(), ImVec2(0, 0), true);
        if (selected) {
            const ResearchNode& n = *selected;
            const ResearchNode* parent = m_ResearchTree.FindParent(n);
            const ResearchStatus st = m_ResearchTree.GetStatus(m_Profile, n);
            const auto* bp = BlueprintManager::Get().GetUnit(n.unitId);
            const auto* itemBp = BlueprintManager::Get().GetWeapon(n.itemId);
            const bool isItem = !n.itemId.empty();
            const ResearchNode* studySource = m_ResearchTree.FindStudySource(n);
            auto tile = [](const char* label, float width, const char* fmt, ...) {
                char buf[64]; va_list args; va_start(args, fmt); std::vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
                Ui::StatTile(label, buf, width); ImGui::SameLine(0.0f, 6.0f);
            };

            // Testata: nome, grado, stato
            if (isItem && studySource) {
                if (Ui::Button(("< " + studySource->name + "##back").c_str(), ImVec2(0.0f, 26.0f), Ui::Btn::Ghost)) m_ResearchSelectedNode = studySource->id;
                ImGui::SameLine();
            }
            {
                std::string upper = n.name;
                for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                const float titleY = ImGui::GetCursorPosY();
                Ui::Title(upper.c_str(), accent);
                // targhette sulla stessa riga del titolo, a meta' altezza
                auto chip = [&](const char* text, const ImVec4& col) { ImGui::SameLine(0.0f, 10.0f); ImGui::SetCursorPosY(titleY + 8.0f); Ui::Chip(text, col); };
                char tierBuf[32]; std::snprintf(tierBuf, sizeof(tierBuf), "GRADO %s", Ui::Roman(n.tier));
                chip(tierBuf, Ui::Col::Brass);
                if (st == ResearchStatus::Researched) chip(isItem ? "RICERCATO" : "SBLOCCATA", Ui::Col::Verdigris);
                else if (st == ResearchStatus::Researchable) chip("RICERCA DISPONIBILE", Ui::Col::Gold);
                else chip("BLOCCATA", Ui::Col::Ash);
                if (!isItem && isMastered(n)) chip("MAESTRIA COMPLETA", Ui::Col::Gold);
                ImGui::SameLine(0.0f, 14.0f);
                ImGui::SetCursorPosY(titleY + 11.0f);
                Ui::TextIn(Ui::FontSmall(), Ui::Col::Ash, isItem ? n.itemId.c_str() : n.unitId.c_str());
                ImGui::SetCursorPosY(titleY + Ui::FontTitle()->FontSize + 6.0f);
            }
            Ui::Divider();

            const float actionW = 380.0f;
            const float leftW = std::max(300.0f, ImGui::GetContentRegionAvail().x - actionW - 20.0f);
            ImGui::BeginGroup();   // ----- sinistra: scheda -----
            const auto* armorBp = BlueprintManager::Get().GetArmorPiece(n.itemId);
            if (isItem && armorBp) {
                tile("ARMATURA", 100.0f, "%d", armorBp->armorRating);
                tile("DURABILITA'", 110.0f, "%.0f", armorBp->maxDurability);
                tile("COPERTURA", 110.0f, "%.0f%%", armorBp->coverage * 100.0f);
                tile("PESO", 100.0f, "%.1f kg", armorBp->weight);
                ImGui::NewLine();
                if (armorBp->blocks_headshot_bonus) ImGui::TextColored(Ui::Col::Ash, "Finche' regge annulla il bonus del colpo in testa.");
            }
            else if (isItem) {
                if (itemBp) {
                    if (itemBp->is_equipment) {
                        tile("DOTAZIONE", 130.0f, "%d usi", itemBp->equipment_uses);
                        tile("PESO", 100.0f, "%.1f kg", itemBp->weight_kg);
                    }
                    else if (itemBp->is_melee) {
                        tile("DANNO IN MISCHIA", 150.0f, "%.0f", itemBp->base_damage);
                        tile("PESO", 100.0f, "%.1f kg", itemBp->weight_kg);
                    }
                    else {
                        if (itemBp->pellet_count > 1) tile("DANNO", 110.0f, "%.0f x%d", itemBp->base_damage, itemBp->pellet_count);
                        else tile("DANNO", 100.0f, "%.0f", itemBp->base_damage);
                        tile("CARICATORE", 110.0f, "%d", itemBp->magazine_size);
                        tile("DI RISERVA", 110.0f, "%d", itemBp->spare_magazines);
                        tile("RICARICA", 100.0f, "%.1f s", itemBp->reload_time_sec);
                    }
                    ImGui::NewLine();
                    if (!itemBp->allowed_units.empty()) {
                        std::string who;
                        for (const auto& u : itemBp->allowed_units) {
                            const auto* ub = BlueprintManager::Get().GetUnit(u);
                            who += (who.empty() ? "" : ", ") + (ub ? ub->displayName : u);
                        }
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + leftW);
                        ImGui::TextColored(Ui::Col::Ash, "Per: %s", who.c_str());
                        ImGui::PopTextWrapPos();
                    }
                }
                else ImGui::TextColored(Ui::Col::Ember, "Blueprint '%s' non trovato in GameData/Weapons.", n.itemId.c_str());
            }
            else if (bp) {
                tile("SALUTE", 92.0f, "%.0f", bp->health);
                tile("VELOCITA'", 92.0f, "%.1f", bp->base_speed);
                tile("ARMATURA", 92.0f, "%d", bp->armor_rating);
                tile("FORZA", 92.0f, "%.0f", bp->strength);
                tile("MISCHIA", 92.0f, "%.0f", bp->melee_skill);
                tile("MIRA", 92.0f, "%.0f", bp->accuracy_skill);
                tile("PUNTI SQUADRA", 120.0f, "%d", bp->point_cost);
                ImGui::NewLine();
                ImGui::Spacing();
                // Anteprima 3D: l'unita' nella vetrina con la sua dotazione base e il suo modo di muoversi
                if (Ui::Button("ANTEPRIMA 3D", ImVec2(180, 30))) OpenShowcasePreview(n.unitId);
                ImGui::SameLine(0.0f, 16.0f);
                const float psY = ImGui::GetCursorPosY() + 6.0f;
                ImGui::SetCursorPosY(psY);
                ImGui::TextColored(Ui::Col::Ash, "Punti studio di questa unita':");
                ImGui::SameLine();
                ImGui::SetCursorPosY(psY);
                ImGui::TextColored(Ui::Col::Phosphor, "%lld", static_cast<long long>(ResearchTree::GetStudyPoints(m_Profile, n.unitId)));
            }
            else {
                ImGui::TextColored(Ui::Col::Rust, "UNITA' IN ARRIVO");
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + leftW);
                ImGui::TextColored(Ui::Col::Ash, "Non ancora giocabile ('%s' non e' in GameData/Units): si puo' gia' ricercare per proseguire "
                                   "nel ramo, ma non ancora acquistare.", n.unitId.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndGroup();

            // ----- destra: requisiti e azione -----
            ImGui::SameLine(leftW + 20.0f);
            ImGui::BeginGroup();
            auto setMessage = [&](const std::string& msg) { m_ResearchMessage = msg; m_ResearchMessageTimer = 4.0f; };
            // riga di requisito: rombo pieno = soddisfatto
            auto requirement = [&](bool ok, const std::string& text) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImDrawList* d = ImGui::GetWindowDrawList();
                const ImU32 c = Ui::U32(ok ? Ui::Col::Gold : Ui::Col::Rust);
                const ImVec2 ctr(p.x + 7.0f, p.y + ImGui::GetTextLineHeight() * 0.5f + 1.0f);
                const ImVec2 q[4] = { ImVec2(ctr.x, ctr.y - 5.0f), ImVec2(ctr.x + 5.0f, ctr.y), ImVec2(ctr.x, ctr.y + 5.0f), ImVec2(ctr.x - 5.0f, ctr.y) };
                if (ok) d->AddConvexPolyFilled(q, 4, c); else d->AddPolyline(q, 4, c, ImDrawFlags_Closed, 1.5f);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20.0f);
                ImGui::TextColored(ok ? Ui::Col::Bone : Ui::Col::Rust, "%s", text.c_str());
            };

            if (st != ResearchStatus::Researched) {
                // Fase 1: ricerca con i punti studio del PADRE
                if (const ResearchNode* payer = m_ResearchTree.PaySource(m_Profile, n)) {
                    const long long have = static_cast<long long>(ResearchTree::GetStudyPoints(m_Profile, payer->unitId));
                    ImGui::TextColored(Ui::Col::Ash, "Punti studio di %s", payer->name.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(have >= n.researchCost ? Ui::Col::Gold : Ui::Col::Bone, "%lld / %lld", have, static_cast<long long>(n.researchCost));
                    Ui::Meter(n.researchCost > 0 ? static_cast<float>(have) / static_cast<float>(n.researchCost) : 1.0f, ImVec2(actionW - 40.0f, 6.0f),
                        have >= n.researchCost ? Ui::Col::Gold : Ui::Col::Brass);
                }
                // Padri alternativi: ne basta uno (sbloccato e, per un personaggio, con la maestria)
                std::string alternatives;
                for (const ResearchNode* par : m_ResearchTree.Parents(n)) alternatives += (alternatives.empty() ? "" : " oppure ") + par->name;
                if (st == ResearchStatus::LockedParent && parent) requirement(false, "Sblocca prima: " + (n.anyParents.empty() ? parent->name : alternatives));
                if (!n.anyParents.empty() && !isItem) requirement(m_ResearchTree.AnyParentReady(m_Profile, n), "Maestria di " + alternatives);
                // Maestria richiesta: il personaggio padre (e i rami che si riuniscono) completati
                for (const ResearchNode* req : m_ResearchTree.MasteryRequirements(n)) {
                    const std::string& reqId = req->id;
                    const bool ok = isMastered(*req);
                    requirement(ok, "Maestria di " + req->name);
                    if (!ok) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(("vai##req_" + reqId).c_str())) m_ResearchSelectedNode = reqId;
                    }
                }
                ImGui::Spacing();
                char label[96];
                std::snprintf(label, sizeof(label), "RICERCA   -%lld PS", static_cast<long long>(n.researchCost));
                if (Ui::Button(label, ImVec2(actionW - 40.0f, 40.0f), Ui::Btn::Primary, st == ResearchStatus::Researchable)) {
                    if (m_ResearchTree.Research(m_Profile, n)) setMessage("Ricerca completata: " + n.name);
                }
            }
            else if (isItem) {
                // Oggetto: si compra nel garage, unita' per unita' (Personalizza -> Equipaggiamento)
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + actionW - 40.0f);
                ImGui::TextColored(Ui::Col::Ash, "Si compra nel garage per ogni unita' che lo deve usare: Personalizza -> Equipaggiamento, "
                                   "scegli l'oggetto e conferma l'acquisto.");
                ImGui::PopTextWrapPos();
                Ui::Currency("COSTO IN ENERGIA", static_cast<long long>(n.purchaseCost), Ui::Col::Gold);
            }
            else {
                // Fase 2: acquisto con energia -> nuova unita' nel garage
                const bool roomLeft = static_cast<int>(m_PlayerRoster.size()) < MAX_ROSTER_SIZE;
                const bool canPay = m_Profile.energy >= n.purchaseCost;
                ImGui::TextColored(Ui::Col::Ash, "Unita' in garage");
                ImGui::SameLine();
                ImGui::TextColored(roomLeft ? Ui::Col::Bone : Ui::Col::Ember, "%d / %d", static_cast<int>(m_PlayerRoster.size()), MAX_ROSTER_SIZE);
                ImGui::Spacing();
                char label[96];
                std::snprintf(label, sizeof(label), "ACQUISTA UNITA'   %lld ENERGIA", static_cast<long long>(n.purchaseCost));
                if (Ui::Button(label, ImVec2(actionW - 40.0f, 40.0f), Ui::Btn::Primary, bp != nullptr && roomLeft && canPay)) {
                    if (PurchaseResearchedUnit(n)) setMessage("Aggiunta al garage: " + n.name);
                }
                if (!roomLeft) ImGui::TextColored(Ui::Col::Ember, "Garage pieno.");
                else if (!canPay) ImGui::TextColored(Ui::Col::Ember, "Energia insufficiente.");
            }
            if (m_ResearchMessageTimer > 0.0f && !m_ResearchMessage.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(Ui::Col::Verdigris, "%s", m_ResearchMessage.c_str());
            }
            ImGui::EndGroup();

            // Oggetti del personaggio: si ricercano coi suoi punti studio. Click = dettaglio e ricerca.
            if (!isItem) {
                std::vector<const ResearchNode*> items;
                for (const auto* r : researchesOf(n)) if (!r->itemId.empty()) items.push_back(r);
                std::sort(items.begin(), items.end(), [](const ResearchNode* a, const ResearchNode* b) {
                    return a->row != b->row ? a->row < b->row : a->col < b->col; });
                if (!items.empty()) {
                    int done = 0;
                    for (const auto* r : items) done += m_ResearchTree.IsResearched(m_Profile, *r) ? 1 : 0;
                    ImGui::Spacing(); ImGui::Spacing();
                    char head[96]; std::snprintf(head, sizeof(head), "EQUIPAGGIAMENTO DA RICERCARE   %d / %d", done, static_cast<int>(items.size()));
                    Ui::Heading(head);
                    // a due colonne, ogni riga: nome (rientrato se dipende da un altro oggetto) e stato
                    if (ImGui::BeginTable("##items", 2, ImGuiTableFlags_SizingStretchSame)) {
                        for (const auto* r : items) {
                            const ResearchStatus rs = m_ResearchTree.GetStatus(m_Profile, *r);
                            int depth = 0;   // oggetti a catena (es. busto -> spallacci -> gambali): rientro
                            for (const ResearchNode* q = m_ResearchTree.FindParent(*r); q && q->unitId.empty(); q = m_ResearchTree.FindParent(*q)) ++depth;
                            char status[96];
                            ImVec4 sc = Ui::Col::Ash;
                            if (rs == ResearchStatus::Researched) { std::snprintf(status, sizeof(status), "RICERCATO"); sc = Ui::Col::Verdigris; }
                            else if (rs == ResearchStatus::Researchable) { std::snprintf(status, sizeof(status), "RICERCA - %lld PS", static_cast<long long>(r->researchCost)); sc = Ui::Col::Gold; }
                            else if (rs == ResearchStatus::LockedPoints) std::snprintf(status, sizeof(status), "servono %lld PS", static_cast<long long>(r->researchCost));
                            else { const ResearchNode* q = m_ResearchTree.FindParent(*r); std::snprintf(status, sizeof(status), "prima: %s", q ? q->name.c_str() : "-"); }
                            ImGui::TableNextColumn();
                            ImGui::PushID(r->id.c_str());
                            const float indent = 4.0f + depth * 18.0f;
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                            const float rowW = ImGui::GetContentRegionAvail().x - 8.0f;
                            const ImVec2 rp = ImGui::GetCursorScreenPos();
                            if (ImGui::Selectable("##row", false, 0, ImVec2(rowW, 0.0f))) m_ResearchSelectedNode = r->id;
                            ImDrawList* d = ImGui::GetWindowDrawList();
                            d->AddRectFilled(ImVec2(rp.x, rp.y + 2.0f), ImVec2(rp.x + 2.0f, rp.y + ImGui::GetTextLineHeight()), Ui::U32(sc, 0.9f));
                            d->AddText(ImVec2(rp.x + 10.0f, rp.y), Ui::U32(rs == ResearchStatus::Researched || rs == ResearchStatus::Researchable ? Ui::Col::Bone : Ui::Col::Ash), r->name.c_str());
                            const float sw = ImGui::CalcTextSize(status).x;
                            d->AddText(ImVec2(rp.x + rowW - sw - 6.0f, rp.y), Ui::U32(sc), status);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // ===================== NEGOZIO =====================
    void GameLayer::RenderShopUI() {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 60.0f));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - 60.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f, 16.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##NEGOZIO", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar(2);
        Ui::Backdrop(ImGui::GetWindowDrawList(), ImVec2(0.0f, 60.0f), io.DisplaySize);

        Ui::Title("EMPORIO DELLA PIETRA NERA", Ui::Col::Amethyst);
        ImGui::TextColored(Ui::Col::Ash, "Qui si usa solo la valuta premium.");
        ImGui::SameLine(0.0f, 24.0f);
        Ui::Currency("PIETRA NERA", static_cast<long long>(m_Profile.blackStone), Ui::Col::Amethyst);
        Ui::Divider(0.0f, Ui::Col::Amethyst);
        ImGui::Spacing();

        // ---- VIP: +25% di punti studio ed energia a fine partita, per sempre ----
        constexpr int64_t kVipPrice = 1000;
        {
            const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + 440.0f, a.y + 210.0f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            Ui::ChamferRect(dl, a, b, 16.0f, IM_COL32(16, 14, 18, 240), Ui::U32(m_Profile.vip ? Ui::Col::Gold : Ui::Col::Amethyst, 0.9f), 1.5f);
            Ui::EclipseSigil(dl, ImVec2(b.x - 46.0f, a.y + 46.0f), 26.0f, Ui::U32(m_Profile.vip ? Ui::Col::Gold : Ui::Col::Amethyst), 0.9f);
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::BeginChild("##vip", ImVec2(440, 210), false);
        ImGui::SetCursorPos(ImVec2(18.0f, 14.0f));
        ImGui::BeginGroup();
        Ui::Title("VIP", Ui::Col::Bone);
        ImGui::PushTextWrapPos(330.0f);
        ImGui::TextColored(Ui::Col::Ash, "+25%% di punti studio ed energia alla fine di ogni partita, in tutti i casi "
                           "(anche quando la ricompensa e' ridotta perche' sei caduto o hai perso).");
        ImGui::PopTextWrapPos();
        ImGui::Spacing(); ImGui::Spacing();
        if (m_Profile.vip) {
            Ui::Chip("VIP ATTIVO", Ui::Col::Gold);
        }
        else {
            const bool canBuy = m_Profile.blackStone >= kVipPrice;
            char label[64];
            std::snprintf(label, sizeof(label), "ACQUISTA   %lld PIETRA NERA", static_cast<long long>(kVipPrice));
            if (Ui::Button(label, ImVec2(400.0f, 40.0f), Ui::Btn::Primary, canBuy)) {
                m_Profile.blackStone -= kVipPrice;
                m_Profile.vip = true;
                SaveProfileIfChanged();
                SUN_CORE_INFO("Negozio: VIP acquistato");
            }
            if (!canBuy) ImGui::TextColored(Ui::Col::Rust, "Pietra Nera insufficiente.");
        }
        ImGui::EndGroup();
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::End();
    }

    void GameLayer::RenderStasisVaultUI() {
        ImGuiIO& io = ImGui::GetIO();

        ImGui::SetNextWindowPos(ImVec2(20.0f, 80.0f));
        ImGui::SetNextWindowSize(ImVec2(340, io.DisplaySize.y - 100.0f));
        ImGui::Begin("IL MIO ESERCITO", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

        ImGui::Text("Unita Addestrate: %d / %d", (int)m_PlayerRoster.size(), MAX_ROSTER_SIZE);
        ImGui::Separator(); ImGui::Spacing();

        for (int i = 0; i < static_cast<int>(m_PlayerRoster.size()); ++i) {
            bool isSelected = (m_SelectedVaultIndex == i);
            if (isSelected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.31f, 0.13f, 1.0f));
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));

            std::string btnText = m_PlayerRoster[i].customName + "\n(" + m_PlayerRoster[i].blueprintId + ")  EXP " + std::to_string(m_PlayerRoster[i].exp);
            if (Ui::PlainButton(btnText.c_str(), ImVec2(-1, 50))) {
                m_SelectedVaultIndex = i;
                ApplyUnitVisuals(m_PlayerRoster[i]);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s [%s]", m_PlayerRoster[i].customName.c_str(), m_PlayerRoster[i].blueprintId.c_str());
            }
            ImGui::PopStyleColor();
        }

        if (static_cast<int>(m_PlayerRoster.size()) < MAX_ROSTER_SIZE) {
            ImGui::Spacing();
            // Le nuove unita' non si reclutano piu' liberamente: si sbloccano e si acquistano nella Ricerca
            if (Ui::PlainButton("+ NUOVA UNITA (RICERCA)", ImVec2(-1, 40))) {
                m_CurrentTab = MenuTab::Research;
            }
        }
        ImGui::End();

        if (!m_PlayerRoster.empty() && m_SelectedVaultIndex < static_cast<int>(m_PlayerRoster.size())) {
            TrainedUnit& activeUnit = m_PlayerRoster[m_SelectedVaultIndex];
            const auto* currentBp = BlueprintManager::Get().GetUnit(activeUnit.blueprintId);

            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 380.0f, 80.0f));
            ImGui::SetNextWindowSize(ImVec2(360, io.DisplaySize.y - 100.0f));
            ImGui::Begin("RESOCONTO UNITA'", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

            ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "%s", activeUnit.customName.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", activeUnit.customName.c_str());
            }

            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Classe: %s", currentBp ? currentBp->displayName.c_str() : "Sconosciuta");
            if (currentBp && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("ID Blueprint: %s", currentBp->displayName.c_str());
            }

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "STATISTICHE COMBATTIMENTO");
            if (currentBp) {
                ImGui::Text("Salute MAX: %.0f HP", currentBp->health);
                if (m_Scene.GetRegistry().all_of<UnitStatsComponent>(m_PlayerEntity)) {
                    int avgArmor = m_Scene.GetRegistry().get<UnitStatsComponent>(m_PlayerEntity).armorSet.GetAverageRating();
                    ImGui::Text("Valore Armatura Medio: %d", avgArmor);
                }
                ImGui::Text("Velocita' Base: %.1f m/s", currentBp->base_speed);
                ImGui::Text("Forza Fisica: %.0f", currentBp->strength);

                float toughnessVal = static_cast<float>(currentBp->toughness);
                ImGui::Spacing();
                ImGui::Text("Toughness (Resistenza): %.0f/100", toughnessVal);
                ImGui::ProgressBar(toughnessVal / 100.0f, ImVec2(-1, 10), "");
            }
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "DOTAZIONE ATTUALE");

            const auto* primBP = BlueprintManager::Get().GetWeapon(activeUnit.primaryWeaponId);
            ImGui::Text("Primaria: %s", primBP ? primBP->displayName.c_str() : "[Vuoto]");
            if (primBP && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s (Danno: %.0f | AP: %d | Caricatore: %d)",
                    primBP->displayName.c_str(), primBP->base_damage, primBP->armor_penetration, primBP->magazine_size);
            }

            const auto* secBP = BlueprintManager::Get().GetWeapon(activeUnit.secondaryWeaponId);
            ImGui::Text("Secondaria: %s", secBP ? secBP->displayName.c_str() : "[Vuoto]");
            if (secBP && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s (Danno: %.0f | AP: %d | Caricatore: %d)",
                    secBP->displayName.c_str(), secBP->base_damage, secBP->armor_penetration, secBP->magazine_size);
            }

            const auto* meleeBP = BlueprintManager::Get().GetWeapon(activeUnit.meleeWeaponId);
            ImGui::Text("Corpo a Corpo: %s", meleeBP ? meleeBP->displayName.c_str() : "[Nessuna / Disarmato]");
            if (meleeBP && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s (Danno Base: %.0f | Tipo: %s)",
                    meleeBP->displayName.c_str(), meleeBP->base_damage, meleeBP->default_melee_type.c_str());
            }

            ImGui::Text("Granate: [Slot Vuoto]");
            ImGui::Text("Oggetto Supporto: [Slot Vuoto]");

            ImGui::SetCursorPosY(io.DisplaySize.y - 180.0f);
            ImGui::Separator(); ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.34f, 0.17f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.34f, 0.17f, 1.0f));
            if (Ui::PlainButton("MODIFICA ED EQUIPAGGIA", ImVec2(-1, 60))) {
                m_ShowCustomizeModal = true;
                m_CustomizeMainTab = 0;
                m_CustomizeHasBackup = false;   // la copia per ANNULLA si prende alla prima schermata
                m_CustomizeZoom = 1.0f;
            }
            ImGui::PopStyleColor(2);

            ImGui::End();
        }
    }

    void GameLayer::RenderRecruitModal() {
        if (!m_ShowRecruitModal) return;

        ImGui::OpenPopup("Reclutamento Nuova Unita");
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("Reclutamento Nuova Unita", &m_ShowRecruitModal, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (m_RecruitStep == 0) {
                Ui::Heading("FASE 1: SELEZIONA LA FAZIONE");

                if (Ui::PlainButton("KINGSHIP", ImVec2(280, 45))) {
                    m_SelectedFaction = "Imperium";
                    m_RecruitStep = 1;
                }
                if (Ui::PlainButton("VANGUARD ARMORS", ImVec2(280, 45))) {
                    m_SelectedFaction = "SpaceMarines";
                    m_RecruitStep = 1;
                }
                if (Ui::PlainButton("VREPIGHS", ImVec2(280, 45))) {
                    m_SelectedFaction = "Orks";
                    m_RecruitStep = 1;
                }
            }
            else if (m_RecruitStep == 1) {
                // nome mostrato della fazione (l'id interno resta Imperium / SpaceMarines / Orks)
                const char* factionLabel = m_SelectedFaction == "Imperium" ? "Kingship" : m_SelectedFaction == "SpaceMarines" ? "Vanguard Armors" : "Vrepighs";
                ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "FASE 2: SELEZIONA TRUPPA (%s)", factionLabel);
                ImGui::Separator(); ImGui::Spacing();

                auto recruitAction = [&](const std::string& bpId, const std::string& defaultName, const glm::vec4& defaultColor) {
                    TrainedUnit newUnit{ defaultName, bpId, defaultColor };
                    newUnit.ApplyDefaultLoadout();
                    m_PlayerRoster.push_back(newUnit);
                    m_SelectedVaultIndex = static_cast<int>(m_PlayerRoster.size()) - 1;
                    ApplyUnitVisuals(m_PlayerRoster.back());
                    m_ShowRecruitModal = false;
                    ImGui::CloseCurrentPopup();
                };

                if (m_SelectedFaction == "Imperium") {
                    if (Ui::PlainButton("Fante del Regno (10 Pti)\n(Recluta)", ImVec2(280, 50))) {
                        recruitAction("imperium_guardsman_recruit", "Recluta Vance", glm::vec4(0.65f, 0.50f, 0.35f, 1.0f));
                    }
                    if (Ui::PlainButton("Kingsman Scelto (25 Pti)\n(Soldato Scelto)", ImVec2(280, 50))) {
                        recruitAction("imperium_kasrkin", "Kingsman Thorne", glm::vec4(0.35f, 0.25f, 0.18f, 1.0f));
                    }
                }
                else if (m_SelectedFaction == "SpaceMarines") {
                    if (Ui::PlainButton("Candidato Umano (40 Pti)\n(Candidato)", ImVec2(280, 50))) {
                        recruitAction("spacemarines_neophyte", "Neofita Corvo", glm::vec4(0.92f, 0.72f, 0.68f, 1.0f));
                    }
                    if (Ui::PlainButton("Difensore Vanguardio (75 Pti)\n(Difensore)", ImVec2(280, 50))) {
                        recruitAction("spacemarines_battle_brother", "Fratello Titus", glm::vec4(0.74f, 0.58f, 0.48f, 1.0f));
                    }
                }
                else if (m_SelectedFaction == "Orks") {
                    if (Ui::PlainButton("Vrepigh (15 Pti)\n(Fante Vrepigh)", ImVec2(280, 50))) {
                        recruitAction("orks_boy", "Gor-Gath", glm::vec4(0.24f, 0.33f, 0.15f, 1.0f));
                    }
                    if (Ui::PlainButton("Vrepigh Capozanna (50 Pti)\n(Capoclan Vrepigh)", ImVec2(280, 50))) {
                        recruitAction("orks_nob", "Uzgrak", glm::vec4(0.17f, 0.25f, 0.11f, 1.0f));
                    }
                }

                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                if (Ui::PlainButton("< Indietro alla Fazione", ImVec2(150, 30))) {
                    m_RecruitStep = 0;
                }
            }

            ImGui::EndPopup();
        }
    }

    // Schermata di personalizzazione: pannello a sinistra, personaggio in anteprima nel resto dello schermo (la
    // scena del garage con la camera dedicata, vedi OnUpdate). Ogni modifica si vede subito sul modello.
    void GameLayer::RenderCustomizeModal() {
        if (!m_ShowCustomizeModal) return;

        ImGuiIO& io = ImGui::GetIO();
        const float panelW = std::min(600.0f, io.DisplaySize.x * 0.42f);
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelW, io.DisplaySize.y), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.88f);

        if (ImGui::Begin("##PersonalizzaUnita", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {

            if (m_PlayerRoster.empty() || m_SelectedVaultIndex >= m_PlayerRoster.size()) {
                m_ShowCustomizeModal = false;
                ImGui::End();
                return;
            }

            TrainedUnit& activeUnit = m_PlayerRoster[m_SelectedVaultIndex];
            if (!m_CustomizeHasBackup) { m_CustomizeBackup = activeUnit; m_CustomizeHasBackup = true; }
            const auto* unitBpHead = BlueprintManager::Get().GetUnit(activeUnit.blueprintId);
            const int bodyKind = static_cast<int>(BodyKindForUnit(activeUnit.blueprintId));
            const char* bodyLabel = BodyLabelOf(bodyKind);

            ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "PERSONALIZZA");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "%s  -  %s", activeUnit.customName.c_str(), unitBpHead ? unitBpHead->displayName.c_str() : activeUnit.blueprintId.c_str());
            ImGui::Separator();

            const char* tabNames[3] = { "ASPETTO", "EQUIPAGGIAMENTO", "ARMATURA" };
            const int tabOrder[3] = { 0, 1, 2 };
            const float tabW = (panelW - 24.0f) / 3.0f;
            for (int i = 0; i < 3; ++i) {
                if (i > 0) ImGui::SameLine();
                const bool on = m_CustomizeMainTab == tabOrder[i];
                ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.42f, 0.31f, 0.13f, 1.0f) : ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
                if (Ui::PlainButton(tabNames[i], ImVec2(tabW, 38))) m_CustomizeMainTab = tabOrder[i];
                ImGui::PopStyleColor();
            }

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::BeginChild("##contenuto", ImVec2(0, io.DisplaySize.y - 190.0f), false);

            if (m_CustomizeMainTab == 0) {
                ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "ANAGRAFICA E IDENTITA'");
                char nameBuf[64];
                std::snprintf(nameBuf, sizeof(nameBuf), "%s", activeUnit.customName.c_str());
                if (ImGui::InputText("Nome Unita'", nameBuf, IM_ARRAYSIZE(nameBuf))) {
                    activeUnit.customName = nameBuf;
                }

                ImGui::Spacing(); ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "CARNAGIONE");
                // tinte tipiche della razza (orki e grot verdi, ogryn e umani dal pallido allo scuro)
                std::vector<glm::vec3> tones;
                const std::string lb = bodyLabel;
                if (lb == "ork" || lb == "gretchin") tones = { { 0.24f, 0.33f, 0.15f }, { 0.17f, 0.25f, 0.11f }, { 0.30f, 0.38f, 0.19f }, { 0.20f, 0.24f, 0.13f }, { 0.34f, 0.40f, 0.21f }, { 0.26f, 0.30f, 0.20f } };   // oliva e verdi spenti
                else tones = { { 0.86f, 0.68f, 0.58f }, { 0.80f, 0.62f, 0.52f }, { 0.70f, 0.52f, 0.40f }, { 0.56f, 0.40f, 0.30f }, { 0.42f, 0.29f, 0.21f }, { 0.30f, 0.20f, 0.15f } };
                for (size_t i = 0; i < tones.size(); ++i) {
                    if (i > 0) ImGui::SameLine();
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::ColorButton("##tono", ImVec4(tones[i].r, tones[i].g, tones[i].b, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(48, 48))) {
                        activeUnit.armorColor = glm::vec4(tones[i], 1.0f);
                        ApplyUnitVisuals(activeUnit);
                    }
                    ImGui::PopID();
                }
                if (ImGui::ColorEdit3("Tinta libera", glm::value_ptr(activeUnit.armorColor))) {
                    ApplyUnitVisuals(activeUnit);
                }
                ImGui::TextDisabled("La pelle ha gia' i muscoli, le vene e le chiazze della razza: la tinta li colora.");

                ImGui::Spacing(); ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "ANTEPRIMA");
                ImGui::TextWrapped("Trascina col tasto sinistro sul personaggio per girarlo, rotellina per avvicinarti.");
            }
            else if (m_CustomizeMainTab == 1) {

                if (ImGui::RadioButton("Armi da Fuoco / Corpo a Corpo", m_EquipSubTab == 0)) m_EquipSubTab = 0;
                ImGui::SameLine();
                if (ImGui::RadioButton("Dotazione Tattica", m_EquipSubTab == 1)) m_EquipSubTab = 1;
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                if (m_EquipSubTab == 0) {
                    ImGui::Text("Filtro Categoria Armi:");
                    if (Ui::PlainButton("Armi Laser", ImVec2(100, 25))) m_WeaponCategoryTab = 0; ImGui::SameLine();
                    if (Ui::PlainButton("Proiettili", ImVec2(100, 25))) m_WeaponCategoryTab = 1; ImGui::SameLine();
                    if (Ui::PlainButton("Plasma (WIP)", ImVec2(100, 25))) m_WeaponCategoryTab = 2;
                    ImGui::Spacing();

                    if (m_WeaponCategoryTab == 2) {
                        ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "Le armi al Plasma sono attualmente in sviluppo nella Forgia.");
                    }
                    else {
                        const auto* primBP = BlueprintManager::Get().GetWeapon(activeUnit.primaryWeaponId);
                        std::string primName = primBP ? primBP->displayName : "Nessuna";

                        if (ImGui::BeginCombo("Seleziona Primaria", primName.c_str())) {
                            if (ImGui::Selectable("[Nessuna]", activeUnit.primaryWeaponId.empty())) {
                                activeUnit.primaryWeaponId = "";
                                ApplyUnitVisuals(activeUnit);
                            }
                            for (const auto& [wId, wBP] : BlueprintManager::Get().GetWeapons()) {
                                if (wBP.slot_type == WeaponSlotType::Primary && BlueprintManager::Get().CanEquip(wBP, activeUnit.blueprintId)) {
                                    if (DrawItemOption(activeUnit, activeUnit.primaryWeaponId, wId, wBP)) ApplyUnitVisuals(activeUnit);   // con acquisto degli oggetti ricercati
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip("%s (Fazione: %s | Danno: %.0f | Colpi: %d / %d)", wBP.displayName.c_str(), wBP.faction.c_str(),
                                            wBP.base_damage, wBP.magazine_size, Ammo::StartingReserve(wBP, activeUnit.blueprintId));
                                    }
                                }
                            }
                            ImGui::EndCombo();
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", primName.c_str());
                        }

                        const auto* secBP = BlueprintManager::Get().GetWeapon(activeUnit.secondaryWeaponId);
                        std::string secName = secBP ? secBP->displayName : "Nessuna";

                        if (ImGui::BeginCombo("Seleziona Secondaria", secName.c_str())) {
                            if (ImGui::Selectable("[Nessuna]", activeUnit.secondaryWeaponId.empty())) {
                                activeUnit.secondaryWeaponId = "";
                                ApplyUnitVisuals(activeUnit);
                            }
                            for (const auto& [wId, wBP] : BlueprintManager::Get().GetWeapons()) {
                                if (wBP.slot_type == WeaponSlotType::Secondary && BlueprintManager::Get().CanEquip(wBP, activeUnit.blueprintId)) {
                                    if (DrawItemOption(activeUnit, activeUnit.secondaryWeaponId, wId, wBP)) ApplyUnitVisuals(activeUnit);   // con acquisto degli oggetti ricercati
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip("%s (Fazione: %s | Danno: %.0f | Colpi: %d / %d)", wBP.displayName.c_str(), wBP.faction.c_str(),
                                            wBP.base_damage, wBP.magazine_size, Ammo::StartingReserve(wBP, activeUnit.blueprintId));
                                    }
                                }
                            }
                            ImGui::EndCombo();
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", secName.c_str());
                        }

                        const auto* meleeBP = BlueprintManager::Get().GetWeapon(activeUnit.meleeWeaponId);
                        std::string meleeName = meleeBP ? meleeBP->displayName : "[Nessuna / Disarmato]";
                        const auto* unitForMelee = BlueprintManager::Get().GetUnit(activeUnit.blueprintId);
                        const bool noMelee = unitForMelee && unitForMelee->no_melee;
                        if (noMelee) {
                            activeUnit.meleeWeaponId = "";
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Arma Melee: non utilizzabile da questa unita' (in mischia usa l'arma che ha addosso)");
                        }

                        if (!noMelee && ImGui::BeginCombo("Seleziona Arma Melee", meleeName.c_str())) {

                            if (ImGui::Selectable("[Nessuna / Disarmato]", activeUnit.meleeWeaponId.empty() || activeUnit.meleeWeaponId == "none")) {
                                activeUnit.meleeWeaponId = "";
                                ApplyUnitVisuals(activeUnit);
                            }

                            for (const auto& [wId, wBP] : BlueprintManager::Get().GetWeapons()) {
                                if ((wBP.slot_type == WeaponSlotType::Melee || wBP.is_melee) && BlueprintManager::Get().CanEquip(wBP, activeUnit.blueprintId)) {
                                    if (DrawItemOption(activeUnit, activeUnit.meleeWeaponId, wId, wBP)) ApplyUnitVisuals(activeUnit);   // con acquisto degli oggetti ricercati
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip("%s (Danno Base: %.0f | Tipo: %s)", wBP.displayName.c_str(), wBP.base_damage, wBP.default_melee_type.c_str());
                                    }
                                }
                            }
                            ImGui::EndCombo();
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", meleeName.c_str());
                        }
                    }
                }
                else if (m_EquipSubTab == 1) {
                    // Slot equipaggiamento 1-3 (in partita: tasti 4, 5, 6). Solo la dotazione permessa all'unita'.
                    ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "SLOT EQUIPAGGIAMENTO (tasti 4 - 5 - 6 in partita)");
                    ImGui::Spacing();
                    for (int slot = 0; slot < 3; ++slot) {
                        const auto* eqBP = BlueprintManager::Get().GetWeapon(activeUnit.equipmentSlots[slot]);
                        const std::string eqName = eqBP ? eqBP->displayName : "[Vuoto]";
                        const std::string label = "Slot " + std::to_string(slot + 1) + " (tasto " + std::to_string(slot + 4) + ")";
                        if (ImGui::BeginCombo(label.c_str(), eqName.c_str())) {
                            if (ImGui::Selectable("[Vuoto]", activeUnit.equipmentSlots[slot].empty())) {
                                activeUnit.equipmentSlots[slot] = "";
                                ApplyUnitVisuals(activeUnit);
                            }
                            for (const auto& [wId, wBP] : BlueprintManager::Get().GetWeapons()) {
                                if (!wBP.is_equipment || !BlueprintManager::Get().CanEquip(wBP, activeUnit.blueprintId)) continue;
                                if (DrawItemOption(activeUnit, activeUnit.equipmentSlots[slot], wId, wBP)) ApplyUnitVisuals(activeUnit);
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("%s | Usi: %d | Portata: %.0f m | %.1f s per rifornimento", wBP.displayName.c_str(),
                                        wBP.equipment_uses, wBP.equipment_range, wBP.equipment_use_time);
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                    ImGui::Spacing();
                    if (Ui::PlainButton("Ripristina dotazione base", ImVec2(220, 28))) {
                        activeUnit.ApplyDefaultLoadout();
                        ApplyUnitVisuals(activeUnit);
                    }
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Scatola di munizioni in mano: click sinistro rifornisce te,");
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "click destro un compagno entro la portata.");
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Grugn: basta averlo in uno slot, ti segue da solo.");
                }
                // Esito dell'ultimo acquisto di un oggetto ricercato
                if (m_ResearchMessageTimer > 0.0f && !m_ResearchMessage.empty()) {
                    m_ResearchMessageTimer -= ImGui::GetIO().DeltaTime;
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.50f, 0.74f, 0.44f, 1.0f), "%s", m_ResearchMessage.c_str());
                }
            }
            else if (m_CustomizeMainTab == 2) {
                const auto* unitBp = BlueprintManager::Get().GetUnit(activeUnit.blueprintId);
                float maxWeight = unitBp ? unitBp->weight_capacity : 60.0f;
                float currentWeight = activeUnit.CalculateTotalWeight();
                int unitPoints = activeUnit.CalculateBattlePoints();

                ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "ISPEZIONE HITBOX E PIASTRE CORAZZATE");
                ImGui::Text("Punti Unita': %d Pti  |  Carico: %.1f / %.1f kg", unitPoints, currentWeight, maxWeight);

                float weightRatio = std::clamp(currentWeight / maxWeight, 0.0f, 1.0f);
                ImVec4 weightColor = (weightRatio > 1.0f) ? ImVec4(0.90f, 0.30f, 0.17f, 1.0f) : ImVec4(0.42f, 0.31f, 0.13f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, weightColor);
                ImGui::ProgressBar(weightRatio, ImVec2(-1, 12), "");
                ImGui::PopStyleColor();

                if (currentWeight > maxWeight) {
                    ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "ATTENZIONE: Sovraccarico! L'unita' subira' penalita' di movimento.");
                }

                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                auto drawArmorSlotCombo = [&](const char* slotLabel, HitboxLocation location, std::string& currentId) {
                    const auto* currentBp = BlueprintManager::Get().GetArmorPiece(currentId);

                    ImGui::PushID(slotLabel);
                    ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "[%s]", slotLabel);

                    std::string comboName = currentBp ? currentBp->displayName + " (" + std::to_string(currentBp->pointCost) + " Pti)" : "[Scoperto / Nessuna Protezione]";

                    if (ImGui::BeginCombo("##combo", comboName.c_str())) {

                        if (ImGui::Selectable("[Scoperto / Nessuna Protezione]", currentId.empty() || currentId == "none")) {
                            currentId = "none";
                            ApplyUnitVisuals(activeUnit);
                        }

                        for (const auto& [aId, aBp] : BlueprintManager::Get().GetArmorPieces()) {

                            bool isCorrectLocation = (aBp.location == location);
                            bool isAllowedForThisUnit = BlueprintManager::Get().CanEquip(aBp, activeUnit.blueprintId);

                            if (isCorrectLocation && isAllowedForThisUnit) {
                                bool isSelected = (currentId == aId);
                                std::string itemLabel = aBp.displayName + " | AR: " + std::to_string(aBp.armorRating) +
                                    " | HP: " + std::to_string((int)aBp.maxDurability) +
                                    " | " + std::to_string((int)aBp.pointCost) + " Pti";

                                // Pezzi ricercabili (es. elmetto di ferraglia): prima la ricerca, poi l'acquisto per unita'
                                const ResearchNode* node = m_ResearchTree.FindNodeByItem(aId);
                                const bool researched = !node || m_ResearchTree.IsResearched(m_Profile, *node);
                                bool owned = !node || activeUnit.OwnsItem(aId);
                                if (!researched) itemLabel += "   [DA RICERCARE]";
                                else if (!owned) itemLabel += "   [COMPRA: " + std::to_string(static_cast<long long>(node->purchaseCost)) + " ENERGIA]";

                                if (ImGui::Selectable(itemLabel.c_str(), isSelected, researched ? 0 : ImGuiSelectableFlags_Disabled)) {
                                    if (!owned) {
                                        if (m_ResearchTree.Pay(m_Profile, *node)) {
                                            activeUnit.ownedItems.push_back(aId);
                                            owned = true;
                                            m_ResearchMessage = "Comprato per " + activeUnit.customName + ": " + aBp.displayName;
                                        }
                                        else m_ResearchMessage = "Energia insufficiente per " + aBp.displayName;
                                        m_ResearchMessageTimer = 4.0f;
                                    }
                                    if (owned) {
                                        currentId = aId;
                                        ApplyUnitVisuals(activeUnit);
                                    }
                                }

                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("%s\nPotere d'Arresto (AR): %d | Durabilita': %.0f HP | Peso: %.1f kg | Costo: %d Pti",
                                        aBp.displayName.c_str(), aBp.armorRating, aBp.maxDurability, aBp.weight, aBp.pointCost);
                                }

                                if (isSelected) ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", comboName.c_str());
                    }

                    if (currentBp) {
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  Potere d'Arresto (AR): %d  |  Copertura: %.0f%%",
                            currentBp->armorRating, currentBp->coverage * 100.0f);
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  Integrita' (HP): %.0f  |  Peso: %.1f kg",
                            currentBp->maxDurability, currentBp->weight);
                    }
                    else {
                        ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "  ! ATTENZIONE: Questo arto subira' danni letali completi !");
                    }
                    ImGui::PopID();
                    ImGui::Spacing();
                };

                ImGui::Columns(2, "ArmorMixColumns", false);

                drawArmorSlotCombo("TESTA", HitboxLocation::Head, activeUnit.armorHeadId);
                drawArmorSlotCombo("TORSO", HitboxLocation::Torso, activeUnit.armorTorsoId);
                drawArmorSlotCombo("BRACCIO SX", HitboxLocation::LeftArm, activeUnit.armorLeftArmId);

                ImGui::NextColumn();

                drawArmorSlotCombo("BRACCIO DX", HitboxLocation::RightArm, activeUnit.armorRightArmId);
                drawArmorSlotCombo("GAMBA SX", HitboxLocation::LeftLeg, activeUnit.armorLeftLegId);
                drawArmorSlotCombo("GAMBA DX", HitboxLocation::RightLeg, activeUnit.armorRightLegId);

                ImGui::Columns(1);
                if (Ui::PlainButton("Ripristina armatura base", ImVec2(220, 26))) {
                    activeUnit.ApplyDefaultArmor();
                    ApplyUnitVisuals(activeUnit);
                }
                if (m_ResearchMessageTimer > 0.0f && !m_ResearchMessage.empty()) {
                    m_ResearchMessageTimer -= ImGui::GetIO().DeltaTime;
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.50f, 0.74f, 0.44f, 1.0f), "%s", m_ResearchMessage.c_str());
                }
            }

            ImGui::EndChild();

            ImGui::Separator(); ImGui::Spacing();
            const float half = (panelW - 24.0f) * 0.5f;
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.34f, 0.17f, 1.0f));
            if (Ui::PlainButton("SALVA E CHIUDI", ImVec2(half, 44))) {
                m_ShowCustomizeModal = false;
                m_CustomizeHasBackup = false;
                SaveProfileIfChanged();
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (Ui::PlainButton("ANNULLA MODIFICHE", ImVec2(half, 44))) {
                if (m_CustomizeHasBackup) { activeUnit = m_CustomizeBackup; ApplyUnitVisuals(activeUnit); }
                m_ShowCustomizeModal = false;
                m_CustomizeHasBackup = false;
            }
        }
        ImGui::End();

        // Barra dell'anteprima (in basso, sopra la scena)
        if (m_ShowCustomizeModal) {
            const float barW = 520.0f;
            ImGui::SetNextWindowPos(ImVec2(panelW + (io.DisplaySize.x - panelW - barW) * 0.5f, io.DisplaySize.y - 70.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(barW, 56.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.7f);
            ImGui::Begin("##AnteprimaBarra", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
            static const char* anims[4] = { "Fermo", "Mira", "Cammina", "Corre" };
            for (int i = 0; i < 4; ++i) {
                if (i > 0) ImGui::SameLine();
                if (ImGui::RadioButton(anims[i], m_CustomizePreviewAnim == i)) m_CustomizePreviewAnim = i;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Ruota", &m_CustomizeAutoRotate);
            ImGui::SameLine();
            if (Ui::PlainButton("Centra")) { m_CustomizeYaw = 200.0f; m_CustomizeZoom = 1.0f; }
            ImGui::End();
        }
    }

    void GameLayer::RenderMapEditorUI() {
        if (m_GameState != GameState::MapEditor) return;

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 410.0f, 65.0f));
        ImGui::SetNextWindowSize(ImVec2(400, io.DisplaySize.y - 80.0f));

        ImGui::Begin("MAP BUILDER (Solemnace Forge)", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.22f, 0.08f, 1.0f));
        if (Ui::PlainButton("+ CREA NUOVA MAPPA DA ZERO", ImVec2(-1, 35))) {
            CreateNewMap();
        }
        ImGui::PopStyleColor();

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::Text("Modalita' Manipolazione (Tasti Rapidi):");
        if (m_GizmoMode == EditorGizmoMode::Translate) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.77f, 0.36f, 1.0f));
        else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (Ui::PlainButton("[W] Sposta", ImVec2(110, 28))) m_GizmoMode = EditorGizmoMode::Translate;
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (m_GizmoMode == EditorGizmoMode::Rotate) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.77f, 0.36f, 1.0f));
        else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (Ui::PlainButton("[E] Ruota", ImVec2(110, 28))) m_GizmoMode = EditorGizmoMode::Rotate;
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (m_GizmoMode == EditorGizmoMode::Scale) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.77f, 0.36f, 1.0f));
        else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (Ui::PlainButton("[R] Scala", ImVec2(110, 28))) m_GizmoMode = EditorGizmoMode::Scale;
        ImGui::PopStyleColor();

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        if (ImGui::CollapsingHeader("1. Impostazioni Mappa")) {
            char idBuf[64];
            std::snprintf(idBuf, sizeof(idBuf), "%s", m_EditorMap.id.c_str());
            if (ImGui::InputText("ID File (.json)", idBuf, IM_ARRAYSIZE(idBuf))) m_EditorMap.id = idBuf;

            char nameBuf[128];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", m_EditorMap.name.c_str());
            if (ImGui::InputText("Nome Visualizzato", nameBuf, IM_ARRAYSIZE(nameBuf))) m_EditorMap.name = nameBuf;

            if (ImGui::DragFloat3("Dimensione Terreno", glm::value_ptr(m_EditorMap.terrainSize), 5.0f, 10.0f, 1000.0f)) SyncEditorEntitiesWithScene();
            if (ImGui::ColorEdit4("Colore Terreno", glm::value_ptr(m_EditorMap.terrainColor))) SyncEditorEntitiesWithScene();
        }

        if (ImGui::CollapsingHeader("2. Punti di Spawn")) {
            if (Ui::PlainButton("Aggiungi Spawn Giocatore")) m_EditorMap.playerSpawns.push_back(m_EditorCameraTarget);
            for (size_t i = 0; i < m_EditorMap.playerSpawns.size(); ++i) {
                ImGui::PushID(static_cast<int>(i) + 1000);
                ImGui::DragFloat3("Spawn P.", glm::value_ptr(m_EditorMap.playerSpawns[i]), 0.5f);
                ImGui::SameLine();
                if (Ui::PlainButton("X")) { m_EditorMap.playerSpawns.erase(m_EditorMap.playerSpawns.begin() + i); }
                ImGui::PopID();
            }

            ImGui::Separator();

            if (Ui::PlainButton("Aggiungi Spawn Nemici")) m_EditorMap.enemySpawns.push_back(m_EditorCameraTarget);
            for (size_t i = 0; i < m_EditorMap.enemySpawns.size(); ++i) {
                ImGui::PushID(static_cast<int>(i) + 2000);
                ImGui::DragFloat3("Spawn E.", glm::value_ptr(m_EditorMap.enemySpawns[i]), 0.5f);
                ImGui::SameLine();
                if (Ui::PlainButton("X")) { m_EditorMap.enemySpawns.erase(m_EditorMap.enemySpawns.begin() + i); }
                ImGui::PopID();
            }
        }

        if (ImGui::CollapsingHeader("3. Inserisci Blocchi Primitivi", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto addBlockShape = [&](const std::string& shapeType, const std::string& label, const glm::vec3& defaultScale) {
                if (Ui::PlainButton(label.c_str(), ImVec2(115, 28))) {
                    EditorStructure st;
                    st.type = "block";
                    st.shape = shapeType;
                    st.position = m_EditorCameraTarget;
                    st.scale = defaultScale;
                    m_EditorMap.structures.push_back(st);
                    m_SelectedStructureIdx = static_cast<int>(m_EditorMap.structures.size()) - 1;
                    SyncEditorEntitiesWithScene();
                }
            };

            addBlockShape("cube", "Cubo", glm::vec3(2.0f)); ImGui::SameLine();
            addBlockShape("rectangle", "Rettangolo", glm::vec3(6.0f, 2.0f, 2.0f)); ImGui::SameLine();
            addBlockShape("column", "Colonna", glm::vec3(1.2f, 6.0f, 1.2f));

            addBlockShape("sphere", "Sfera", glm::vec3(3.0f)); ImGui::SameLine();
            addBlockShape("pyramid", "Piramide", glm::vec3(4.0f, 4.0f, 4.0f)); ImGui::SameLine();
            addBlockShape("ramp", "Rampa", glm::vec3(4.0f, 3.0f, 8.0f));

            addBlockShape("doorway", "Vano Porta", glm::vec3(3.5f, 3.5f, 0.6f)); ImGui::SameLine();
            addBlockShape("arch", "Arco", glm::vec3(4.0f, 4.0f, 0.6f)); ImGui::SameLine();
            addBlockShape("stairs", "Scalinata", glm::vec3(3.0f, 3.0f, 6.0f));

            addBlockShape("barricade", "Barricata", glm::vec3(4.0f, 1.2f, 0.8f));
        }

        if (ImGui::CollapsingHeader("4. Inserisci Edifici Cavi", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto addBuildingStyle = [&](const std::string& roofStyle, const std::string& label) {
                if (Ui::PlainButton(label.c_str(), ImVec2(115, 32))) {
                    EditorStructure st;
                    st.type = "building";
                    st.shape = roofStyle;
                    st.position = m_EditorCameraTarget;
                    st.width = 12.0f;
                    st.depth = 12.0f;
                    st.floorHeight = 4.0f;
                    m_EditorMap.structures.push_back(st);
                    m_SelectedStructureIdx = static_cast<int>(m_EditorMap.structures.size()) - 1;
                    SyncEditorEntitiesWithScene();
                }
            };

            addBuildingStyle("flat_roof", "Tetto Piatto"); ImGui::SameLine();
            addBuildingStyle("gabled_roof", "Tetto Capanna"); ImGui::SameLine();
            addBuildingStyle("open_roof", "Senza Tetto");
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::Text("Strutture in Mappa (%d):", (int)m_EditorMap.structures.size());
        if (ImGui::BeginListBox("##StructList", ImVec2(-1, 100))) {
            for (int i = 0; i < (int)m_EditorMap.structures.size(); ++i) {
                std::string label = std::to_string(i) + ". " + m_EditorMap.structures[i].type + " (" + m_EditorMap.structures[i].shape + ")";
                if (ImGui::Selectable(label.c_str(), m_SelectedStructureIdx == i)) {
                    m_SelectedStructureIdx = i;
                    m_EditorCameraTarget = m_EditorMap.structures[i].position;
                    SyncEditorEntitiesWithScene();
                }
            }
            ImGui::EndListBox();
        }

        if (m_SelectedStructureIdx >= 0 && m_SelectedStructureIdx < (int)m_EditorMap.structures.size()) {
            auto& st = m_EditorMap.structures[m_SelectedStructureIdx];
            ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "Modifica [%s - %s]:", st.type.c_str(), st.shape.c_str());

            bool changed = false;
            if (ImGui::DragFloat3("Posizione", glm::value_ptr(st.position), 0.5f)) changed = true;
            if (ImGui::DragFloat3("Rotazione", glm::value_ptr(st.rotation), 1.0f)) changed = true;
            if (ImGui::ColorEdit4("Colore", glm::value_ptr(st.color))) changed = true;

            if (st.type == "block") {
                if (ImGui::DragFloat3("Scala (W,H,D)", glm::value_ptr(st.scale), 0.5f, 0.1f, 200.0f)) changed = true;
            }
            else if (st.type == "building") {
                if (ImGui::DragFloat("Larghezza", &st.width, 0.5f, 2.0f, 100.0f)) changed = true;
                if (ImGui::DragFloat("Profondita'", &st.depth, 0.5f, 2.0f, 100.0f)) changed = true;
                if (ImGui::DragFloat("Altezza Piano", &st.floorHeight, 0.5f, 1.0f, 50.0f)) changed = true;
                if (ImGui::DragFloat("Spessore Muri", &st.wallThickness, 0.1f, 0.1f, 2.0f)) changed = true;
                if (ImGui::DragFloat("Larghezza Porta", &st.doorWidth, 0.2f, 1.0f, 10.0f)) changed = true;
                if (ImGui::DragFloat("Altezza Porta", &st.doorHeight, 0.2f, 1.0f, 10.0f)) changed = true;
            }

            if (changed) SyncEditorEntitiesWithScene();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.90f, 0.30f, 0.17f, 1.0f));
            if (Ui::PlainButton("Elimina Oggetto", ImVec2(-1, 26))) {
                m_EditorMap.structures.erase(m_EditorMap.structures.begin() + m_SelectedStructureIdx);
                m_SelectedStructureIdx = -1;
                SyncEditorEntitiesWithScene();
            }
            ImGui::PopStyleColor();
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.34f, 0.17f, 1.0f));
        if (Ui::PlainButton("SALVA E GIOCA IN-GAME", ImVec2(-1, 38))) {
            SaveMapToJSON();
            LoadMapIntoPreview();
        }
        ImGui::PopStyleColor();

        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.31f, 0.13f, 1.0f));
        if (Ui::PlainButton("ESPORTA PER CONDIVISIONE (External)", ImVec2(-1, 38))) {
            ExportMapToExternal();
        }
        ImGui::PopStyleColor();

        if (!m_LastExportPathStatus.empty()) {
            ImGui::TextWrapped("Esportato in:\n%s", m_LastExportPathStatus.c_str());
        }

        ImGui::End();
    }

    void GameLayer::RenderDeploymentUI() {
        GLFWwindow* window = glfwGetCurrentContext();
        ImGuiIO& io = ImGui::GetIO();

        ImGui::SetNextWindowPos(ImVec2(20.0f, 80.0f));
        ImGui::SetNextWindowSize(ImVec2(320, io.DisplaySize.y - 100.0f));
        ImGui::Begin("TERMINALE MISSIONI", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

        ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "1. SELEZIONA MAPPA TARGET");

        // id del file mappa (non cambiarli: il profilo salva l'id) e nome mostrato
        const char* defaultMaps[] = { "warzone_sector4", "vaulting_test" };
        const char* defaultMapLabels[] = { "Settore 4 - Complesso Militare", "Testing Lobby (vault + campo da tiro)" };
        static int selectedMapComboIdx = -1;
        if (selectedMapComboIdx < 0) {   // prima apertura: allinea alla mappa salvata nel profilo
            selectedMapComboIdx = 0;
            for (int i = 0; i < 2; ++i) if (m_SelectedMapId == defaultMaps[i]) selectedMapComboIdx = i;
        }

        if (ImGui::BeginCombo("Mappa Predefinita", defaultMapLabels[selectedMapComboIdx])) {
            for (int i = 0; i < 2; ++i) {
                bool isSelected = (selectedMapComboIdx == i);
                if (ImGui::Selectable(defaultMapLabels[i], isSelected)) {
                    selectedMapComboIdx = i;
                    m_SelectedMapId = defaultMaps[i];
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Mappa: %s", defaultMaps[selectedMapComboIdx]);
        }

        char customMapBuf[64];
        std::snprintf(customMapBuf, sizeof(customMapBuf), "%s", m_SelectedMapId.c_str());
        if (ImGui::InputText("ID Mappa Custom", customMapBuf, IM_ARRAYSIZE(customMapBuf))) {
            m_SelectedMapId = customMapBuf;
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "2. MODALITA' DI GIOCO");

        const char* events[] = {
            "Difesa della pietra (ondata Vrepighs)",
            "Recupero Dati (Tyranidi)",   // nascosta per ora: non compare nella lista
            "Incursione Profonda",
            "SANDBOX (Esplorazione Libera)",
            "VS BOT (5 contro 5)"
        };

        for (int i = 0; i < 5; ++i) {
            if (i == 1) continue;   // Recupero Dati: tolta dalla lista per ora
            bool isSelected = (m_SelectedEventIndex == i);
            if (isSelected) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.88f, 0.52f, 0.24f, 1.0f));

            if (ImGui::Selectable(events[i], isSelected)) {
                m_SelectedEventIndex = i;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", events[i]);
            }

            if (isSelected) ImGui::PopStyleColor();
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        if (m_SelectedEventIndex == 4) {
            ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "[VS BOT - 5 CONTRO 5]");
            ImGui::TextWrapped("La tua squadra (fino a 5 unita', completata da alleati IA) contro 5 bot su 'Trincee di "
                "Solemnace'. Vince chi elimina la squadra avversaria; dopo 10 minuti vince chi ha piu' punti in campo. "
                "Se cadi prendi il controllo di un alleato vivo. Scegli la coda nel pannello di destra.");
        }
        else if (m_SelectedEventIndex == 3) {
            ImGui::TextColored(ImVec4(0.50f, 0.74f, 0.44f, 1.0f), "[MODALITA' SANDBOX]");
            ImGui::TextWrapped("Entrerai nella mappa '%s' senza nemici o eventi temporizzati. Ideale per testare il movimento, il vaulting, le armi e l'architettura.", m_SelectedMapId.c_str());
        }
        else {
            ImGui::TextWrapped("Seleziona una missione per schierare la tua squadra nella simulazione fisica di Solemnace.");
        }

        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(360.0f, 80.0f));
        ImGui::SetNextWindowSize(ImVec2(380, io.DisplaySize.y - 100.0f));
        ImGui::Begin("SELEZIONE COMPONENTI SQUADRA", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

        int squadPoints = CalculateSquadPoints();
        // Nessun limite di unita': ogni unita' e' un rientro in partita. Limiti: i punti e il massimo per tipo.
        ImGui::Text("Punti Battaglia: %d / %d   (unita': %d)", squadPoints, MAX_SQUAD_POINTS,
            static_cast<int>(m_SelectedSquadIndices.size()));
        ImGui::TextDisabled("Ogni unita' in squadra e' un rientro in partita.");
        ImGui::ProgressBar(static_cast<float>(squadPoints) / static_cast<float>(MAX_SQUAD_POINTS), ImVec2(-1, 15), "");
        ImGui::Separator(); ImGui::Spacing();

        for (int i = 0; i < static_cast<int>(m_PlayerRoster.size()); ++i) {
            auto it = std::find(m_SelectedSquadIndices.begin(), m_SelectedSquadIndices.end(), i);
            bool isInSquad = (it != m_SelectedSquadIndices.end());

            const auto* unitBpForCost = BlueprintManager::Get().GetUnit(m_PlayerRoster[i].blueprintId);
            const int cost = unitBpForCost ? m_PlayerRoster[i].CalculateBattlePoints() : 0;   // punti battaglia (unita' + equipaggiamento)

            std::string label = m_PlayerRoster[i].customName + " [" + std::to_string(cost) + " Pti]";

            if (isInSquad) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.34f, 0.17f, 0.9f));
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.20f, 0.6f));

            std::string btnText = isInSquad ? "[IN SQUADRA] " + label : "[RISERVA] " + label;
            if (Ui::PlainButton(btnText.c_str(), ImVec2(-1, 40))) {
                if (isInSquad) {
                    if (m_SelectedSquadIndices.size() > 1) {
                        m_SelectedSquadIndices.erase(it);
                    }
                }
                else {
                    int sameType = 0;
                    for (int idx : m_SelectedSquadIndices)
                        if (idx >= 0 && idx < static_cast<int>(m_PlayerRoster.size()) && m_PlayerRoster[idx].blueprintId == m_PlayerRoster[i].blueprintId) ++sameType;
                    const int maxType = unitBpForCost ? unitBpForCost->max_per_squad : 5;
                    if (sameType >= maxType) {
                        SUN_CORE_WARN("Massimo {0} unita' di tipo {1} per squadra", maxType, unitBpForCost ? unitBpForCost->displayName : m_PlayerRoster[i].blueprintId);
                    }
                    else if (squadPoints + cost <= MAX_SQUAD_POINTS) {
                        m_SelectedSquadIndices.push_back(i);
                    }
                    else {
                        SUN_CORE_WARN("Punti Squadra insufficienti! Limite: {0}", MAX_SQUAD_POINTS);
                    }
                }

                if (!m_SelectedSquadIndices.empty() && m_SelectedSquadIndices[0] < static_cast<int>(m_PlayerRoster.size())) {
                    ApplyUnitVisuals(m_PlayerRoster[m_SelectedSquadIndices[0]]);
                }
            }
            if (ImGui::IsItemHovered()) {
                const int withGear = m_PlayerRoster[i].CalculateTotalPoints();
                ImGui::SetTooltip("%s\nBlueprint: %s\nPunti battaglia: %d (unita' + equipaggiamento gia' rivisto)\nValore totale col vecchio equipaggiamento: %d\nMassimo per squadra: %d",
                    m_PlayerRoster[i].customName.c_str(), m_PlayerRoster[i].blueprintId.c_str(), cost, withGear,
                    unitBpForCost ? unitBpForCost->max_per_squad : 5);
            }
            ImGui::PopStyleColor();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 380.0f, 80.0f));
        ImGui::SetNextWindowSize(ImVec2(360, io.DisplaySize.y - 100.0f));
        ImGui::Begin("SCHIERAMENTO FINALE", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

        ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "COMPOSIZIONE SQUADRA (%d Unita):", static_cast<int>(m_SelectedSquadIndices.size()));
        ImGui::Separator(); ImGui::Spacing();

        for (size_t squadIdx = 0; squadIdx < m_SelectedSquadIndices.size(); ++squadIdx) {
            int rosterIdx = m_SelectedSquadIndices[squadIdx];
            if (rosterIdx >= 0 && rosterIdx < static_cast<int>(m_PlayerRoster.size())) {
                const auto& unit = m_PlayerRoster[rosterIdx];
                if (squadIdx == 0) {
                    ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "LEADER APRIPISTA: %s", unit.customName.c_str());
                }
                else {
                    ImGui::Text("  - Rincalzo %d: %s", static_cast<int>(squadIdx), unit.customName.c_str());
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s (Classe: %s)", unit.customName.c_str(), unit.blueprintId.c_str());
                }
            }
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        if (m_SelectedEventIndex == 4) {
            RenderVsBotQueues();
            ImGui::End();
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.90f, 0.30f, 0.17f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.30f, 0.17f, 1.0f));
        // In squadra (con almeno un compagno) la partita e' per tutti: la avvia il capo, gli altri entrano con lui.
        // Il Vs Bot in squadra non c'e' ancora (si gioca da soli: vedi il pannello delle code).
        const bool squadMatch = NetSquadMatchAvailable();
        if (squadMatch && m_Party.Playing()) {   // partita gia' in corso (anche per il capo): si entra in quella
            if (Ui::PlainButton("ENTRA IN PARTITA CON LA SQUADRA", ImVec2(-1, 65))) NetJoinRunningMatch();
        }
        else if (squadMatch && !m_Party.IsLeader()) {   // la avvia il capo: si entra con lui
            ImGui::PopStyleColor(2);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.20f, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.20f, 0.20f, 0.6f));
            Ui::PlainButton("LA AVVIA IL CAPO SQUADRA", ImVec2(-1, 65));
            ImGui::TextWrapped("Scegli la tua squadra qui sopra: quando il capo avvia, entri in partita con lui con queste unita'.");
        }
        else if (Ui::PlainButton(squadMatch ? "AVVIA PER TUTTA LA SQUADRA" : "AVVIA SIMULAZIONE", ImVec2(-1, 65))) {
            if (!m_SelectedSquadIndices.empty()) {
                if (squadMatch) NetLeaderStartMatch();
                else StartDeployment(0);
            }
        }
        if (squadMatch) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.36f, 0.80f, 0.74f, 1.0f), "PARTITA IN SQUADRA");
            ImGui::TextWrapped("Mappa e modalita' le sceglie il capo. Ognuno porta la sua squadra e i suoi rientri; "
                "i nemici li muove l'host (chi ha la connessione migliore) e passano a un altro se esce.");
        }
        ImGui::PopStyleColor(2);

        ImGui::End();
    }

    // Ingresso in partita con la squadra scelta, sulla mappa e nella modalita' selezionate (da solo o in squadra:
    // spawnSlot = mio posto fra i compagni, per non comparire uno sopra l'altro)
    void GameLayer::StartDeployment(int spawnSlot) {
        if (m_SelectedSquadIndices.empty()) return;

        auto view = m_Scene.GetRegistry().view<ColliderComponent>(entt::exclude<PlayerTag>);
        for (auto e : view) {
            m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
            m_Scene.DestroyEntity(e);
        }

        ReloadMap(m_SelectedMapId);

        m_ActiveSquad.clear();
        for (int rosterIdx : m_SelectedSquadIndices) {
            ActiveSquadMember member;
            member.rosterIndex = rosterIdx;
            member.isAlive = true;
            m_ActiveSquad.push_back(member);
        }

        m_CurrentSquadMemberIdx = 0;
        m_KillCount = 0;
        ResetMatchXp();
        m_PlayerDeathHandled = false;
        ClearPlayerDeathState();   // FIX #4: rete di sicurezza, il player parte sempre "vivo" pulito
        ApplyUnitVisuals(m_PlayerRoster[m_ActiveSquad[0].rosterIndex]);

        if (m_Scene.GetRegistry().all_of<TransformComponent>(m_PlayerEntity)) {
            auto& playerTrans = m_Scene.GetRegistry().get<TransformComponent>(m_PlayerEntity);
            if (!m_CurrentMapData.playerSpawnPoints.empty()) {
                // in squadra ognuno ha il suo punto (se i punti finiscono, un passo di lato)
                const int n = static_cast<int>(m_CurrentMapData.playerSpawnPoints.size());
                const int k = std::max(0, spawnSlot);
                playerTrans.Position = m_CurrentMapData.playerSpawnPoints[k % n] + glm::vec3(1.6f * static_cast<float>(k / n), 0.0f, 0.0f);
                m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, playerTrans.Position);
            }
        }

        if (auto* hp = m_Scene.GetRegistry().try_get<HealthComponent>(m_PlayerEntity)) {
            hp->currentHealth = hp->maxHealth;
        }
        m_IsPaused = false;

        if (m_SelectedEventIndex == 0) {
            g_WaaaghActive = true;
            g_WaaaghTimer = 9.0f;
            g_WaaaghSpawnCount = 1;
            PlayStartWaaagh(m_VoiceGroupInitialized ? 1.0f : m_VolumeVoice);
        }
        else {
            g_WaaaghActive = false;
        }

        ResetFactionBuff();
        m_GameState = GameState::Gameplay;
        if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }

    void GameLayer::RenderRespawnUI() {
        GLFWwindow* window = glfwGetCurrentContext();
        ImGuiIO& io = ImGui::GetIO();

        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420, 420));

        bool anyAlive = false;
        for (const auto& member : m_ActiveSquad) {
            if (member.isAlive) { anyAlive = true; break; }
        }

        if (!anyAlive) {
            ApplyMatchXp();   // una volta sola: fine della sopravvivenza
            ImGui::SetNextWindowSize(ImVec2(620, 0));
            ImGui::Begin("SQUADRA DISTRUTTA", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
            Ui::Heading("--- SQUADRONE ELIMINATO ---");
            ImGui::TextWrapped("Tutti i membri della squadra schierati sono stati travolti.");

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "PUNTEGGIO FINALE:");
            ImGui::Text("Vrepighs Eliminati: %d", m_KillCount);
            ImGui::Spacing();
            RenderMatchRecap();

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            if (Ui::PlainButton("Torna al Menu Principale (Garage)", ImVec2(-1, 50))) {
                m_GameState = GameState::MainMenu;
                m_PlayerDeathHandled = false;
                g_WaaaghActive = false;
                g_WaaaghTimer = 0.0f;
                g_WaaaghSpawnCount = 1;

                auto enemyView = m_Scene.GetRegistry().view<HealthComponent>(entt::exclude<PlayerTag>);
                for (auto e : enemyView) {
                    m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
                    m_Scene.DestroyEntity(e);
                }

                ClearPlayerDeathState();   // FIX #4: niente ragdoll/Corpse residui nel garage

                if (auto* hp = m_Scene.GetRegistry().try_get<HealthComponent>(m_PlayerEntity)) {
                    hp->currentHealth = hp->maxHealth;
                }

                if (window) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
            ImGui::End();
            return;
        }

        ImGui::Begin("CADUTO IN BATTAGLIA", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        ImGui::TextColored(ImVec4(0.88f, 0.52f, 0.24f, 1.0f), "UNITA' SOPRAVVISSUTE:");
        ImGui::Text("Vrepighs eliminati finora: %d", m_KillCount);
        ImGui::Text("Seleziona quale membro far scendere sul campo:");
        ImGui::Separator(); ImGui::Spacing();

        for (size_t i = 0; i < m_ActiveSquad.size(); ++i) {
            auto& member = m_ActiveSquad[i];
            if (member.rosterIndex >= 0 && member.rosterIndex < static_cast<int>(m_PlayerRoster.size())) {
                const auto& unit = m_PlayerRoster[member.rosterIndex];

                if (member.isAlive) {
                    std::string label = "SCHIERA: " + unit.customName + " [" + unit.blueprintId + "]";
                    if (Ui::PlainButton(label.c_str(), ImVec2(-1, 45))) {
                        // Cleanup stato morte: rimuove ragdoll Jolt e CorpseComponent.
                        // DestroyJoltBody rimuove sia JoltBodyComponent sia RagdollComponent,
                        // e fa scattare gli observer di cleanup dei body Jolt.
                        ClearPlayerDeathState();

                        m_CurrentSquadMemberIdx = static_cast<int>(i);
                        ApplyUnitVisuals(unit);

                        if (m_Scene.GetRegistry().all_of<TransformComponent>(m_PlayerEntity)) {
                            auto& playerTrans = m_Scene.GetRegistry().get<TransformComponent>(m_PlayerEntity);
                            if (!m_CurrentMapData.playerSpawnPoints.empty()) {
                                int randomSpawnIdx = Random::RangeInt(0, static_cast<int>(m_CurrentMapData.playerSpawnPoints.size()) - 1);
                                playerTrans.Position = m_CurrentMapData.playerSpawnPoints[randomSpawnIdx];
                                m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, playerTrans.Position);
                            }
                        }

                        if (auto* hp = m_Scene.GetRegistry().try_get<HealthComponent>(m_PlayerEntity)) {
                            hp->currentHealth = hp->maxHealth;
                        }

                        if (auto* melee = m_Scene.GetRegistry().try_get<MeleeStateComponent>(m_PlayerEntity)) {
                            melee->currentPhase = MeleePhase::None;
                            melee->currentAttack = MeleeAttackType::None;
                            melee->currentAttackNode = "";
                            melee->stateTimer = 0.0f;
                            melee->bufferedInput = "";
                            melee->inputBufferTimer = 0.0f;
                            melee->hasHitInThisAttack = false;
                        }
                        if (auto* wState = m_Scene.GetRegistry().try_get<WeaponStateComponent>(m_PlayerEntity)) {
                            wState->isReloading = false;
                            wState->reloadTimer = 0.0f;
                            wState->fireCooldown = 0.0f;
                            wState->currentHeat = 0.0f;
                            wState->isOverheating = false;
                            // FIX #5: caricatore pieno esplicito, indipendente dall'ordine di ApplyUnitVisuals
                            if (auto* wbp = m_Scene.GetRegistry().try_get<WeaponBlueprintId>(m_PlayerEntity)) {
                                if (const auto* bp = BlueprintManager::Get().GetWeapon(wbp->id)) {
                                    wState->currentAmmo = bp->magazine_size;
                                }
                            }
                        }

                        m_PlayerDeathHandled = false;

                        if (window) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                    }
                }
                else {
                    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "[CADUTO] %s", unit.customName.c_str());
                }
            }
        }
        ImGui::End();
    }

    // Scheda GRAFICA delle opzioni: tutto si applica subito e resta nel profilo (salvataggio automatico)
    void GameLayer::RenderGraphicsOptions() {
        DeferredRenderer& r = m_DeferredRenderer;
        RenderQuality& q = r.GetQuality();
        Atmosphere& a = r.GetAtmosphere();
        const ImVec4 head(0.0f, 0.85f, 1.0f, 1.0f);
        auto hint = [](const char* text) {
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(320.0f); ImGui::TextUnformatted(text); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
        };

        // il resto della finestra meno il pulsante "Chiudi": la scheda scorre
        ImGui::BeginChild("##GfxScroll", ImVec2(0, std::max(120.0f, ImGui::GetContentRegionAvail().y - 46.0f)), false);
        ImGui::PushItemWidth(-230.0f);

        ImGui::Spacing();
        ImGui::TextColored(head, "PRESET DI QUALITA'");
        ImGui::Separator();
        const int curPreset = CurrentGraphicsPreset(r);
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            const bool active = curPreset == i;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.36f, 0.16f, 1.0f));   // ottone: il preset in uso
            if (Ui::PlainButton(k_GraphicsPresets[i].name, ImVec2(90, 28))) ApplyGraphicsPreset(r, i);
            if (active) ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        if (Ui::PlainButton("Rileva automaticamente", ImVec2(-1, 28))) {
            std::string gpu, why;
            const int level = DetectGraphicsLevel(gpu, why);
            ApplyGraphicsPreset(r, level);
            m_GfxDetectInfo = std::string(k_GraphicsPresets[level].name) + " consigliato per " + gpu + " (" + why + ")";
        }
        ImGui::Text("In uso: %s", curPreset >= 0 ? k_GraphicsPresets[curPreset].name : "Personalizzata");
        if (!m_GfxDetectInfo.empty()) ImGui::TextDisabled("%s", m_GfxDetectInfo.c_str());
        ImGui::TextDisabled("I preset cambiano solo cio' che pesa sulle prestazioni; lo stile resta il tuo.");
        if (ImGui::SmallButton("Ripristina tutto ai valori di base (Alto, stile originale)")) {
            q = RenderQuality{};
            a = Atmosphere{};
            r.SetSSAOEnabled(true);
            r.SetFXAAEnabled(true);
            ApplyGraphicsPreset(r, 2);
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("PRESTAZIONI", ImGuiTreeNodeFlags_DefaultOpen)) {
            float pct = q.renderScale * 100.0f;
            if (ImGui::SliderFloat("Risoluzione di rendering", &pct, 50.0f, 100.0f, "%.0f%%")) q.renderScale = std::clamp(pct / 100.0f, 0.5f, 1.0f);
            hint("La scena si disegna a una risoluzione piu' bassa e viene ingrandita: e' la regolazione che fa guadagnare piu' FPS sulle schede video meno potenti. Interfaccia e scritte restano nitide.");
            ImGui::TextDisabled("Immagine interna: %d x %d", r.GetRenderWidth(), r.GetRenderHeight());
            ImGui::Checkbox("Sincronia verticale (VSync)", &q.vsync);
            hint("Accesa: niente immagine spezzata, ma se il gioco non tiene 60 FPS scende di colpo a 30. Spenta: FPS liberi.");
            ImGui::Checkbox("Mostra prestazioni (F3)", &m_ShowPerf);
            hint("FPS e millisecondi di processore e scheda video per ogni passaggio: dicono cosa abbassare.");
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("IMMAGINE", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool ssao = r.IsSSAOEnabled(), fxaa = r.IsFXAAEnabled();
            if (ImGui::Checkbox("Occlusione ambientale (SSAO)", &ssao)) r.SetSSAOEnabled(ssao);
            hint("Angoli, pieghe e punti di contatto piu' scuri: gli oggetti 'poggiano' davvero sul terreno.");
            ImGui::BeginDisabled(!ssao);
            ImGui::Checkbox("Occlusione a meta' risoluzione", &q.ssaoHalfRes);
            hint("Quasi identica a vederla e costa un quarto.");
            ImGui::SliderFloat("Raggio occlusione (m)", &q.ssaoRadius, 0.1f, 1.5f, "%.2f");
            ImGui::SliderFloat("Intensita' occlusione", &q.ssaoStrength, 0.0f, 3.0f, "%.2f");
            ImGui::EndDisabled();
            if (ImGui::Checkbox("Antialiasing (FXAA)", &fxaa)) r.SetFXAAEnabled(fxaa);
            hint("Ammorbidisce i bordi seghettati.");
            ImGui::Checkbox("Bagliore delle luci (bloom)", &q.bloom);
            ImGui::BeginDisabled(!q.bloom);
            ImGui::SliderFloat("Intensita' bagliore", &q.bloomIntensity, 0.0f, 3.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::SliderFloat("Luminosita'", &q.brightness, 0.4f, 2.0f, "%.2f");
            ImGui::SliderFloat("Gamma", &q.gamma, 1.8f, 2.6f, "%.2f");
        }

        if (ImGui::CollapsingHeader("OMBRE", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Ombre del sole", &q.sunShadows);
            hint("Spente: niente ombre per nessuno (il gioco va piu' veloce).");
            ImGui::BeginDisabled(!q.sunShadows);
            ImGui::Checkbox("Ombre dei personaggi", &q.characterShadows);
            hint("Corpi, vestiti, armature e grugn proiettano l'ombra che segue l'animazione.");
            const char* filters[] = { "Netto", "Morbido", "Molto morbido" };
            ImGui::Combo("Bordo delle ombre", &q.shadowFilter, filters, 3);
            {
                const char* res[] = { "1024 (veloce)", "2048", "4096 (nitide)" };
                int ri = q.shadowResolution <= 1024 ? 0 : (q.shadowResolution <= 2048 ? 1 : 2);
                if (ImGui::Combo("Risoluzione delle ombre", &ri, res, 3)) q.shadowResolution = ri == 0 ? 1024 : (ri == 1 ? 2048 : 4096);
            }
            ImGui::SliderFloat("Distanza delle ombre (m)", &q.shadowDistance, 20.0f, 150.0f, "%.0f");
            hint("Piu' lontano = ombre anche in fondo alla mappa, ma meno definite vicino.");
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("PERSONAGGI", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Luce sotto la pelle", &q.skinSSS, 0.0f, 1.5f, "%.2f");
            hint("Diffusione sottocutanea: passaggio luce/ombra morbido e rossastro, orecchie e dita che si illuminano controluce. 0 = pelle opaca.");
            ImGui::SliderFloat("Muscoli, chiazze e vene", &q.skinDetail, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Sporco su vestiti e armature", &q.gearGrime, 0.0f, 2.0f, "%.2f");
            hint("Fango che sale dagli stivali, polvere sulle spalle, sangue rappreso, ruggine, bruciature, colature d'olio.");
            ImGui::SliderFloat("Tinte spente (grimdark)", &q.desaturation, 0.0f, 1.0f, "%.2f");
            hint("0 = colori originali di pelle, vestiti e armature; 1 = spenti e consumati.");
        }

        if (ImGui::CollapsingHeader("MAPPE", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Materiali delle mappe", &q.mapMaterials);
            hint("Fango, rockcrete, lamiere arrugginite, sacchi di sabbia, legno, terrapieni. Spento = tinta unita.");
            ImGui::BeginDisabled(!q.mapMaterials);
            ImGui::SliderFloat("Rilievo dei materiali", &q.materialRelief, 0.0f, 2.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::Checkbox("Detriti sparsi", &q.mapDecor);
            hint("Macerie, pietre, assi e rottami sparsi sul campo (solo grafica, non bloccano).");
        }

        if (ImGui::CollapsingHeader("ATMOSFERA E COLORE", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Atmosfera attiva", &a.enabled);
            hint("Cielo con nuvole, nebbia in altezza con polvere, correzione colore. Spenta = resa neutra.");
            ImGui::BeginDisabled(!a.enabled);
            ImGui::TextDisabled("Cielo");
            ImGui::SliderFloat("Nuvolosita'", &a.cloudCover, 0.0f, 1.0f, "%.2f");
            ImGui::ColorEdit3("Cielo in alto", &a.zenith.x, ImGuiColorEditFlags_Float);
            ImGui::ColorEdit3("Orizzonte (smog)", &a.horizon.x, ImGuiColorEditFlags_Float);
            ImGui::ColorEdit3("Sotto l'orizzonte", &a.ground.x, ImGuiColorEditFlags_Float);
            ImGui::TextDisabled("Nebbia e polvere");
            ImGui::SliderFloat("Densita' nebbia", &a.fogDensity, 0.0f, 0.04f, "%.4f");
            ImGui::SliderFloat("Diradamento in altezza", &a.fogHeightFalloff, 0.01f, 0.4f, "%.3f");
            ImGui::ColorEdit3("Colore nebbia", &a.fogColor.x, ImGuiColorEditFlags_Float);
            ImGui::SliderFloat("Polvere nel vento", &a.dust, 0.0f, 1.5f, "%.2f");
            ImGui::TextDisabled("Correzione colore");
            ImGui::SliderFloat("Saturazione", &a.saturation, 0.0f, 1.5f, "%.2f");
            ImGui::SliderFloat("Contrasto", &a.contrast, 0.6f, 1.6f, "%.2f");
            ImGui::SliderFloat("Vignetta", &a.vignette, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Grana della pellicola", &a.grain, 0.0f, 0.08f, "%.3f");
            ImGui::ColorEdit3("Tinta delle ombre", &a.shadowTint.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            ImGui::ColorEdit3("Tinta delle luci", &a.highlightTint.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            ImGui::EndDisabled();
        }

        ImGui::PopItemWidth();
        ImGui::EndChild();
    }

    void GameLayer::RenderOptionsMenu() {
        if (!m_ShowOptionsMenu) return;

        ImGui::OpenPopup("Opzioni di Gioco");
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        // piu' grande di prima: la scheda GRAFICA ha molte regolazioni (scorre se lo schermo e' piccolo)
        ImGui::SetNextWindowSize(ImVec2(std::min(720.0f, io.DisplaySize.x * 0.92f), std::min(640.0f, io.DisplaySize.y * 0.9f)));

        if (ImGui::BeginPopupModal("Opzioni di Gioco", &m_ShowOptionsMenu, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {

            if (ImGui::BeginTabBar("OptionsTabBar")) {

                if (ImGui::BeginTabItem("VIDEO")) {
                    ImGui::Spacing();
                    Ui::Heading("IMPOSTAZIONI DISPLAY");

                    const char* currentResLabel = g_Resolutions[m_SelectedResolutionIdx].name;
                    if (ImGui::BeginCombo("Risoluzione", currentResLabel)) {
                        for (int i = 0; i < 4; ++i) {
                            bool isSelected = (m_SelectedResolutionIdx == i);
                            if (ImGui::Selectable(g_Resolutions[i].name, isSelected)) {
                                m_SelectedResolutionIdx = i;
                            }
                            if (isSelected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::Spacing();
                    ImGui::Checkbox("Schermo Intero (Fullscreen)", &m_IsFullscreen);

                    {
                        const char* levels[] = { "Basso", "Medio", "Alto", "Max" };
                        const int cur = CurrentGraphicsPreset(m_DeferredRenderer);
                        if (ImGui::BeginCombo("Qualita' grafica", cur >= 0 ? levels[cur] : "Personalizzata")) {
                            for (int i = 0; i < 4; ++i)
                                if (ImGui::Selectable(levels[i], cur == i)) ApplyGraphicsPreset(m_DeferredRenderer, i);
                            ImGui::EndCombo();
                        }
                        ImGui::TextDisabled("Ogni effetto si regola nella scheda GRAFICA.");
                    }

                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                    if (Ui::PlainButton("Applica Video", ImVec2(140, 35))) {
                        ApplyDisplaySettings(g_Resolutions[m_SelectedResolutionIdx].width, g_Resolutions[m_SelectedResolutionIdx].height, m_IsFullscreen);
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("AUDIO")) {
                    ImGui::Spacing();
                    Ui::Heading("CONTROLLO VOLUMI CANALI");

                    // Ogni barra agisce subito (e resta nel profilo): generale = tutto il motore audio, poi un gruppo per canale
                    auto volumeBar = [this](const char* label, const char* id, float& v) {
                        ImGui::Text("%s", label);
                        float pct = v * 100.0f;
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::SliderFloat(id, &pct, 0.0f, 100.0f, "%.0f%%")) {
                            v = std::clamp(pct / 100.0f, 0.0f, 1.0f);
                            ApplyAudioVolumes();
                        }
                        ImGui::Spacing();
                    };
                    volumeBar("Volume generale:", "##VolumeMaster", m_VolumeMaster);
                    volumeBar("Ambienza & Effetti Gioco (armi, passi, impatti):", "##VolumeSFX", m_VolumeSFX);
                    volumeBar("Voci, Schiamazzi & Versi:", "##VolumeVoice", m_VolumeVoice);
                    volumeBar("Musica di Sottofondo:", "##VolumeMusic", m_VolumeMusic);

                    Ui::Heading("SENSAZIONE DEI COLPI");
                    {
                        ImGui::Text("Scossoni della telecamera (spari, esplosioni, colpi presi):");
                        float pct = m_Feel.shakeScale * 100.0f;
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::SliderFloat("##CameraShake", &pct, 0.0f, 150.0f, "%.0f%%")) m_Feel.shakeScale = std::clamp(pct / 100.0f, 0.0f, 1.5f);
                        ImGui::Checkbox("Fermo-immagine sui colpi di mischia", &m_Feel.hitStopEnabled);
                        ImGui::Spacing();
                    }

                    if (!m_AudioEngineInitialized) {
                        ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "Audio non disponibile (nessun dispositivo di uscita).");
                    }
                    else if (m_MusicTracks.empty()) {
                        ImGui::TextDisabled("Nessun brano: metti file .mp3 / .wav / .flac in GameData/Audio/Music.");
                    }
                    else {
                        const std::string track = std::filesystem::path(m_MusicTracks[m_MusicTrackIdx]).stem().string();
                        ImGui::TextDisabled("In riproduzione: %s", track.c_str());
                        if (m_MusicTracks.size() > 1) {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Brano successivo")) PlayMusicTrack(m_MusicTrackIdx + 1);
                        }
                    }
                    ImGui::TextDisabled("In partita la musica si abbassa da sola sotto spari e voci.");

                    ImGui::Spacing();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("GRAFICA")) {
                    RenderGraphicsOptions();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Personaggio")) {
                    ImGui::Spacing();
                    ImGui::Text("Modello del giocatore:");
                    if (ImGui::RadioButton("Cuboni (hitbox)", !m_UseAnimatedPlayer)) m_UseAnimatedPlayer = false;
                    ImGui::SameLine();
                    bool animAvailable = false;
                    for (int k = 0; k < static_cast<int>(AnimatedBodyKind::Count); ++k)
                        animAvailable |= AnimatedBodyReady(static_cast<AnimatedBodyKind>(k));
                    if (ImGui::RadioButton("Animato (FBX)", m_UseAnimatedPlayer) && animAvailable) m_UseAnimatedPlayer = true;
                    ImGui::TextDisabled("In partita: F2 per passare dall'uno all'altro.");
                    if (!animAvailable) ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "Modello/animazioni non caricati.");

                    ImGui::Spacing();
                    ImGui::Text("Taratura armi sul modello animato:");
                    ImGui::BeginChild("##WeaponTune", ImVec2(0, 275), true);
                    ImGui::DragFloat3("Offset (dx,su,avanti) m", &m_HandWeaponOffset.x, 0.005f, -1.0f, 1.0f, "%.3f");
                    ImGui::DragFloat3("Rotazione (gradi)", &m_HandWeaponRotation.x, 0.5f, -180.0f, 180.0f, "%.1f");
                    ImGui::DragFloat("Fattore inclinazione mira", &m_WeaponPitchFactor, 0.01f, -2.0f, 2.0f, "%.2f");
                    ImGui::Checkbox("Segui rotazione della mano", &m_WeaponFollowsHand);
                    ImGui::Separator();
                    ImGui::TextDisabled("Armi con modello a blocchi (es. lasgun):");
                    ImGui::SliderFloat("Allineamento alle mani (0 = dritta)", &m_WeaponHandAlign, 0.0f, 1.0f, "%.2f");
                    ImGui::DragFloat3("Spostamento (dx,su,indietro) m", &m_WeaponModelGripOffset.x, 0.002f, -0.5f, 0.5f, "%.3f");
                    ImGui::DragFloat3("Correzione rotazione (gradi)", &m_WeaponModelRotation.x, 0.5f, -180.0f, 180.0f, "%.1f");
                    ImGui::Separator();
                    ImGui::TextDisabled("Arma dei Vrepighs (mannaia):");
                    ImGui::DragFloat3("Pugno sul braccio (m, dal fondo)", &m_OrkWeaponHoldOffset.x, 0.01f, -1.0f, 1.0f, "%.2f");
                    ImGui::DragFloat3("Rotazione lama (gradi)", &m_OrkWeaponModelRotation.x, 1.0f, -180.0f, 180.0f, "%.0f");
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::SetCursorPosY(310.0f);
            ImGui::Separator(); ImGui::Spacing();
            if (Ui::PlainButton("Chiudi Menu", ImVec2(-1, 32))) {
                m_ShowOptionsMenu = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    // ===================== MODALITA' VS BOT (5 contro 5) =====================

    // I tre bottoni delle code: cap, idoneita' della squadra attuale e motivo se non idonea.
    void GameLayer::RenderVsBotQueues() {
        const int pts = CalculateSquadPoints();
        const int n = static_cast<int>(m_SelectedSquadIndices.size());
        ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "VS BOT - SCEGLI LA CODA");
        ImGui::Text("La tua squadra: %d punti battaglia, %d unita'", pts, n);
        ImGui::TextWrapped("5 contro 5: tu e 4 alleati IA contro 5 avversari IA. Ogni giocatore porta una squadra entro il cap "
            "della coda (unita' + equipaggiamento) e rientra con un'altra unita' quando quella in campo cade.");
        ImGui::Spacing();
        // In squadra: la coda la sceglie il capo; i compagni prendono il posto di altrettanti bot
        const bool squad = NetSquadMatchAvailable();
        if (squad) {
            ImGui::TextColored(ImVec4(0.36f, 0.80f, 0.74f, 1.0f), "IN SQUADRA");
            if (m_Party.Playing()) {
                ImGui::TextWrapped("La squadra e' gia' in partita.");
                if (Ui::PlainButton("ENTRA IN PARTITA CON LA SQUADRA", ImVec2(-1, 50))) NetJoinRunningMatch();
                return;
            }
            if (!m_Party.IsLeader()) {
                ImGui::TextWrapped("La coda la sceglie il capo squadra: entri in partita con lui, con le unita' scelte qui "
                    "(quelle che stanno nel cap della coda). Nei 20 secondi prima dell'inizio puoi passare all'altra squadra con [T].");
                return;
            }
            ImGui::TextWrapped("Avvii per tutta la squadra: i compagni prendono il posto di altrettanti bot dalla tua parte. "
                "Nei 20 secondi prima dell'inizio ognuno puo' passare all'altra squadra con [T] (al posto di un bot): cosi' si prova il PvP.");
            ImGui::Spacing();
        }

        const std::string capProblem = SquadTypeCapProblem();
        if (!capProblem.empty()) ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "%s", capProblem.c_str());
        for (int i = 0; i < 3; ++i) {
            const auto& tier = kVsBotTiers[i];
            const bool ok = VsBotEligible(pts, n, tier) && capProblem.empty();
            char label[96];
            if (tier.minExclusive <= 0) std::snprintf(label, sizeof(label), "%s\nfino a %d punti", tier.name, tier.cap);
            else std::snprintf(label, sizeof(label), "%s\nda %d a %d punti", tier.name, tier.minExclusive + 1, tier.cap);

            char status[128];
            if (ok) std::snprintf(status, sizeof(status), "IDONEA: la tua squadra vale %d / %d", pts, tier.cap);
            else if (n < 1) std::snprintf(status, sizeof(status), "Seleziona almeno un'unita'");
            else if (!capProblem.empty()) std::snprintf(status, sizeof(status), "NON IDONEA: troppe unita' dello stesso tipo");
            else if (pts > tier.cap) std::snprintf(status, sizeof(status), "NON IDONEA: squadra troppo forte (%d > %d)", pts, tier.cap);
            else std::snprintf(status, sizeof(status), "NON IDONEA: servono piu' di %d punti (hai %d)", tier.minExclusive, pts);

            ImGui::PushStyleColor(ImGuiCol_Button, ok ? ImVec4(0.20f, 0.34f, 0.17f, 1.0f) : ImVec4(0.34f, 0.08f, 0.07f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ok ? ImVec4(0.20f, 0.34f, 0.17f, 1.0f) : ImVec4(0.34f, 0.08f, 0.07f, 1.0f));
            ImGui::BeginDisabled(!ok);
            if (Ui::PlainButton(label, ImVec2(-1, 58))) {
                if (squad) NetLeaderStartVsBot(i);
                else StartVsBotMatch(i);
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(2);
            ImGui::TextColored(ok ? ImVec4(0.50f, 0.74f, 0.44f, 1.0f) : ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "%s", status);
            ImGui::Spacing();
        }
        if (!m_VsBotMessage.empty()) ImGui::TextColored(ImVec4(0.88f, 0.52f, 0.24f, 1.0f), "%s", m_VsBotMessage.c_str());
    }

    // Unita' generata per un bot: nome e colore dall'albero di ricerca, armi in base alla fazione.
    TrainedUnit GameLayer::MakeBotUnit(const std::string& unitId, int number) const {
        const auto* bp = BlueprintManager::Get().GetUnit(unitId);
        std::string name = bp ? bp->displayName : unitId;
        glm::vec4 color = TrainingDummyColor(unitId);
        if (const ResearchNode* node = m_ResearchTree.FindNodeByUnit(unitId)) {
            name = node->defaultName;
            color = glm::vec4(node->color[0], node->color[1], node->color[2], node->color[3]);
        }
        if (number > 1) name += " " + std::to_string(number);
        TrainedUnit u{ name, unitId, color };
        const std::string faction = VsBotFactionOf(unitId);
        u.primaryWeaponId = "lasgun_standard";
        u.secondaryWeaponId = "laspistol_standard";
        if (faction == "orks") u.meleeWeaponId = "ork_choppa_1h";
        else if (faction == "spacemarines") u.meleeWeaponId = "standard_chainsword_1h";
        else if (unitId == "imperium_ogryn") u.meleeWeaponId = "entrenching_shovel_1h";
        else u.meleeWeaponId = "combat_knife_1h";
        u.ApplyDefaultLoadout();   // unita' con dotazione base definita (es. gretchin, yoof): quella
        return u;
    }

    // Varieta': i bot a volte hanno gli oggetti ricercabili, ma solo se il loro costo sta nel budget rimasto
    // della squadra (cosi' la squadra resta sotto il cap della coda). budget viene scalato.
    void GameLayer::AddBotExtras(TrainedUnit& u, int& budget) const {
        auto tryUpgrade = [&](float chance, std::string& slot, const std::string& itemId) {
            if (!Random::Chance(chance) || slot == itemId) return;
            const std::string old = slot;
            const int before = u.CalculateBattlePoints();
            slot = itemId;
            const int delta = u.CalculateBattlePoints() - before;
            if (delta > budget) slot = old;
            else budget -= delta;
        };
        if (u.blueprintId == "orks_gretchin") {
            tryUpgrade(0.25f, u.primaryWeaponId, "grot_trombone");
            const float r = Random::Float01();
            if (r < 0.35f) tryUpgrade(1.0f, u.equipmentSlots[1], "squig_biter");
            else if (r < 0.50f) tryUpgrade(1.0f, u.equipmentSlots[1], "squig_bomb");
            tryUpgrade(0.30f, u.armorHeadId, "head_grot_scrap_helmet");
        }
        else if (u.blueprintId == "orks_yoof") {
            tryUpgrade(0.30f, u.meleeWeaponId, "yoof_spear");   // lancia rudimentale (dotazione base alternativa al pugnale)
            tryUpgrade(0.30f, u.secondaryWeaponId, "yoof_slugga_auto");
            tryUpgrade(0.30f, u.meleeWeaponId, "yoof_choppa");
            tryUpgrade(0.35f, u.armorTorsoId, "torso_yoof_scrap");
            if (Random::Chance(0.25f)) { tryUpgrade(1.0f, u.armorLeftArmId, "arm_l_yoof_scrap"); tryUpgrade(1.0f, u.armorRightArmId, "arm_r_yoof_scrap"); }
            if (Random::Chance(0.25f)) { tryUpgrade(1.0f, u.armorLeftLegId, "leg_l_yoof_scrap"); tryUpgrade(1.0f, u.armorRightLegId, "leg_r_yoof_scrap"); }
            tryUpgrade(0.25f, u.equipmentSlots[1], "yoof_food_pack");
        }
        else if (u.blueprintId == "orks_boy") {
            tryUpgrade(0.30f, u.meleeWeaponId, "boy_big_choppa");
            tryUpgrade(0.25f, u.primaryWeaponId, "boy_big_shoota");
            tryUpgrade(0.25f, u.equipmentSlots[1], "boy_mines");
            tryUpgrade(0.30f, u.armorHeadId, "head_boy_helmet");
            if (Random::Chance(0.30f)) { tryUpgrade(1.0f, u.armorLeftArmId, "arm_l_boy_plate"); tryUpgrade(1.0f, u.armorRightArmId, "arm_r_boy_plate"); }
            if (Random::Chance(0.30f)) { tryUpgrade(1.0f, u.armorLeftLegId, "leg_l_boy_plate"); tryUpgrade(1.0f, u.armorRightLegId, "leg_r_boy_plate"); }
            tryUpgrade(0.20f, u.armorTorsoId, "torso_boy_heavy");
        }
        else if (u.blueprintId == "orks_shoota_boy") {
            const float r = Random::Float01();
            if (r < 0.25f) tryUpgrade(1.0f, u.primaryWeaponId, "shoota_big_shoota");
            else if (r < 0.45f) tryUpgrade(1.0f, u.primaryWeaponId, "shoota_rokkit");
            tryUpgrade(0.30f, u.equipmentSlots[1], "shoota_ammo_backpack");
            tryUpgrade(0.30f, u.armorHeadId, "head_shoota_helmet");
            if (Random::Chance(0.30f)) { tryUpgrade(1.0f, u.armorLeftArmId, "arm_l_shoota_plate"); tryUpgrade(1.0f, u.armorRightArmId, "arm_r_shoota_plate"); }
            if (Random::Chance(0.30f)) { tryUpgrade(1.0f, u.armorLeftLegId, "leg_l_shoota_plate"); tryUpgrade(1.0f, u.armorRightLegId, "leg_r_shoota_plate"); }
            tryUpgrade(0.20f, u.armorTorsoId, "torso_shoota_heavy");
        }
        else if (u.blueprintId == "imperium_whiteshield") {
            tryUpgrade(0.5f, u.primaryWeaponId, "whiteshield_autogun");     // alternativa base alla lasgun
            tryUpgrade(0.30f, u.meleeWeaponId, "whiteshield_chainsword");
            if (Random::Chance(0.30f)) { tryUpgrade(1.0f, u.armorLeftArmId, "arm_l_whiteshield_guard"); tryUpgrade(1.0f, u.armorRightArmId, "arm_r_whiteshield_guard"); }
            if (Random::Chance(0.30f)) { tryUpgrade(1.0f, u.armorLeftLegId, "leg_l_whiteshield_guard"); tryUpgrade(1.0f, u.armorRightLegId, "leg_r_whiteshield_guard"); }
            tryUpgrade(0.25f, u.equipmentSlots[1], "whiteshield_lascannon");
        }
        else if (u.blueprintId == "imperium_guardsman_recruit") {
            tryUpgrade(0.40f, u.primaryWeaponId, "guardsman_autogun");        // alternative base
            tryUpgrade(0.35f, u.secondaryWeaponId, "guardsman_autopistol");
            const float r = Random::Float01();                                  // armi speciali (una al massimo)
            if (r < 0.10f) tryUpgrade(1.0f, u.primaryWeaponId, "guardsman_shotgun");
            else if (r < 0.19f) tryUpgrade(1.0f, u.primaryWeaponId, "guardsman_flamer");
            else if (r < 0.28f) tryUpgrade(1.0f, u.primaryWeaponId, "guardsman_plasma");
            else if (r < 0.36f) tryUpgrade(1.0f, u.primaryWeaponId, "guardsman_melta");
            else if (r < 0.44f) tryUpgrade(1.0f, u.primaryWeaponId, "guardsman_long_las");
            else if (r < 0.52f) tryUpgrade(1.0f, u.primaryWeaponId, "guardsman_grenade_launcher");
            tryUpgrade(0.25f, u.meleeWeaponId, "guardsman_chainsword");
            tryUpgrade(0.25f, u.equipmentSlots[1], "guardsman_vox_caster");
        }
        else if (u.blueprintId == "imperium_schola_cadet") {
            tryUpgrade(0.40f, u.primaryWeaponId, "guardsman_autogun");          // alternative base (come il guardsman)
            tryUpgrade(0.35f, u.secondaryWeaponId, "guardsman_autopistol");
            tryUpgrade(0.35f, u.primaryWeaponId, "schola_hotshot_lasgun");
            tryUpgrade(0.30f, u.secondaryWeaponId, "schola_hotshot_laspistol");
            tryUpgrade(0.30f, u.armorTorsoId, "torso_schola_carapace");
            tryUpgrade(0.30f, u.armorHeadId, "head_schola_carapace");
            if (Random::Chance(0.25f)) { tryUpgrade(1.0f, u.armorLeftArmId, "arm_l_schola_carapace"); tryUpgrade(1.0f, u.armorRightArmId, "arm_r_schola_carapace"); }
            if (Random::Chance(0.25f)) { tryUpgrade(1.0f, u.armorLeftLegId, "leg_l_schola_carapace"); tryUpgrade(1.0f, u.armorRightLegId, "leg_r_schola_carapace"); }
            tryUpgrade(0.30f, u.equipmentSlots[1], "schola_ammo_crate");
        }
        else if (u.blueprintId == "spacemarines_battle_brother") {
            tryUpgrade(0.40f, u.primaryWeaponId, "firstborn_auto_bolt_rifle");   // alternative base
            tryUpgrade(0.30f, u.meleeWeaponId, "firstborn_knife");
            tryUpgrade(0.25f, u.primaryWeaponId, "firstborn_heavy_bolter");
            tryUpgrade(0.30f, u.equipmentSlots[0], "firstborn_krak");
            tryUpgrade(0.30f, u.equipmentSlots[2], "firstborn_insignia");
        }
        else if (u.blueprintId == "spacemarines_scout") {
            tryUpgrade(0.30f, u.primaryWeaponId, "scout_shotgun");          // alternative base
            tryUpgrade(0.30f, u.meleeWeaponId, "scout_chainsword");
            const float r = Random::Float01();
            if (r < 0.20f) tryUpgrade(1.0f, u.primaryWeaponId, "scout_sniper");
            else if (r < 0.38f) tryUpgrade(1.0f, u.primaryWeaponId, "scout_heavy_bolter");
            tryUpgrade(0.30f, u.armorHeadId, "head_scout_visor");
            tryUpgrade(0.25f, u.equipmentSlots[1], "scout_camo_cloak");      // al posto dell'auspex (inutile per l'IA)
        }
        else if (u.blueprintId == "spacemarines_neophyte") {
            tryUpgrade(0.50f, u.primaryWeaponId, "aspirant_autogun");      // alternativa base al lasrifle
            tryUpgrade(0.25f, u.primaryWeaponId, "aspirant_shotgun");
            tryUpgrade(0.35f, u.secondaryWeaponId, "aspirant_laspistol");
            tryUpgrade(0.30f, u.meleeWeaponId, "aspirant_chainsword");
        }
        else if (u.blueprintId == "imperium_conscript") {
            tryUpgrade(0.30f, u.equipmentSlots[0], "conscript_frag");
            tryUpgrade(0.25f, u.equipmentSlots[1], "conscript_medkit");
            tryUpgrade(0.35f, u.armorTorsoId, "torso_conscript_flak");
            tryUpgrade(0.35f, u.armorHeadId, "head_conscript_helmet");
        }
        else if (u.blueprintId == "orks_loota") {
            const float r = Random::Float01();
            if (r < 0.25f) tryUpgrade(1.0f, u.primaryWeaponId, "loota_deffgun_beamy");
            else if (r < 0.45f) tryUpgrade(1.0f, u.primaryWeaponId, "loota_deffgun_plasma");
            tryUpgrade(0.30f, u.armorHeadId, "head_loota_helmet");
            tryUpgrade(0.25f, u.equipmentSlots[1], "loota_slit_wall");
        }
    }

    // Crea un'unita' controllata dall'IA: stesse statistiche, scheletro, armatura e armi del giocatore.
    entt::entity GameLayer::SpawnBot(const TrainedUnit& unit, int team, const glm::vec3& spawnPos, float yawDeg, int rosterIndex) {
        auto& reg = m_Scene.GetRegistry();
        const auto* bp = BlueprintManager::Get().GetUnit(unit.blueprintId);
        if (!bp) return entt::null;

        const entt::entity e = m_Scene.CreateEntity();
        auto& t = reg.emplace<TransformComponent>(e);
        t.Position = spawnPos;
        t.Scale = glm::vec3(1.0f);
        t.Rotation = glm::vec3(0.0f, yawDeg, 0.0f);
        t.AimYaw = yawDeg;

        SwapPlayerUnit(m_Scene, e, unit.blueprintId);   // statistiche, salute, scheletro, collider, colore

        // Piedi a terra: il collider avvolge lo scheletro, che puo' scendere sotto l'origine
        if (const auto* col = reg.try_get<ColliderComponent>(e)) t.Position.y = spawnPos.y - (col->localCenterOffset.y - col->halfExtents.y);
        reg.emplace<RigidBodyComponent>(e, glm::vec3(0.0f), 0.0f, false);

        auto& color = reg.get_or_emplace<ColorComponent>(e);
        glm::vec4 c = unit.armorColor;
        if (team == 1) c = glm::mix(c, glm::vec4(0.75f, 0.12f, 0.10f, 1.0f), 0.25f);   // avversari leggermente rossastri
        color.Color = c; color.BaseColor = c;

        auto& stats = reg.get<UnitStatsComponent>(e);
        stats.maxStamina = bp->stamina;
        auto applyPiece = [](const std::string& pieceId, ArmorPiece& piece) {
            if (const auto* abp = BlueprintManager::Get().GetArmorPiece(pieceId)) {
                piece.id = abp->id; piece.name = abp->displayName; piece.armorRating = abp->armorRating;
                piece.coverage = abp->coverage; piece.maxDurability = abp->maxDurability;
                piece.currentDurability = abp->maxDurability; piece.weight = abp->weight; piece.pointCost = abp->pointCost;
                piece.blocksHeadshotBonus = abp->blocks_headshot_bonus;
            }
            else piece = {};
        };
        applyPiece(unit.armorHeadId, stats.armorSet.head);
        applyPiece(unit.armorTorsoId, stats.armorSet.torso);
        applyPiece(unit.armorLeftArmId, stats.armorSet.leftArm);
        applyPiece(unit.armorRightArmId, stats.armorSet.rightArm);
        applyPiece(unit.armorLeftLegId, stats.armorSet.leftLeg);
        applyPiece(unit.armorRightLegId, stats.armorSet.rightLeg);
        stats.currentWeight = unit.CalculateTotalWeight();

        auto& inv = reg.emplace<InventoryComponent>(e);
        inv.primaryWeaponId = unit.primaryWeaponId;
        inv.secondaryWeaponId = unit.secondaryWeaponId;
        inv.meleeWeaponId = unit.meleeWeaponId;
        inv.tool1Id = unit.equipmentSlots[0];
        inv.tool2Id = unit.equipmentSlots[1];
        inv.tool3Id = unit.equipmentSlots[2];
        inv.currentSlot = inv.FirstUsableSlot();   // senza primaria (gretchin) parte dalla secondaria
        reg.emplace<WeaponBlueprintId>(e, inv.GetActiveWeaponId());
        auto& ws = reg.emplace<WeaponStateComponent>(e);
        if (const auto* wbp = BlueprintManager::Get().GetWeapon(inv.GetActiveWeaponId())) ws.currentAmmo = wbp->magazine_size;
        Ammo::Init(reg, e, unit.blueprintId);   // munizioni limitate anche per i bot
        ApplyArmorSkin(e, unit);                // vesti / elmetto visibili
        reg.emplace<CameraRecoilComponent>(e);
        reg.emplace<MeleeStateComponent>(e);
        reg.emplace<WeaponTriggerComponent>(e);
        reg.emplace<TeamComponent>(e, TeamComponent{ team });

        auto& brain = reg.emplace<BotBrainComponent>(e);
        brain.role = bp->ai_role == "ranged" ? BotRole::Ranged : (bp->ai_role == "melee" ? BotRole::Melee : BotRole::Versatile);
        brain.displayName = unit.customName;
        brain.rosterIndex = rosterIndex;
        brain.healthLastThink = bp->health;
        brain.thinkTimer = Random::Range(0.0f, 0.4f);
        brain.advanceSide = Random::Range(-1.0f, 1.0f);
        brain.lastPos = t.Position;

        // Arma visibile in mano (modello a blocchi se esiste, altrimenti un cubo)
        brain.weaponEntity = m_Scene.CreateEntity();
        reg.emplace<TransformComponent>(brain.weaponEntity).Scale = glm::vec3(0.08f, 0.10f, 0.70f);
        reg.emplace<MeshComponent>(brain.weaponEntity, 0u);
        auto& wc = reg.emplace<ColorComponent>(brain.weaponEntity);
        wc.Color = glm::vec4(0.2f, 0.2f, 0.24f, 1.0f); wc.BaseColor = wc.Color;

        m_VsBotUnits[e] = unit;
        return e;
    }

    // Massimo per tipo di unita' ("max_per_squad", es. 4 boyz): le squadre salvate prima della regola
    // potrebbero superarlo, quindi lo si ricontrolla prima di entrare in coda.
    std::string GameLayer::SquadTypeCapProblem() const {
        std::unordered_map<std::string, int> count;
        for (int idx : m_SelectedSquadIndices)
            if (idx >= 0 && idx < static_cast<int>(m_PlayerRoster.size())) ++count[m_PlayerRoster[idx].blueprintId];
        for (const auto& [id, n] : count) {
            const auto* bp = BlueprintManager::Get().GetUnit(id);
            const int maxType = bp ? bp->max_per_squad : 5;
            if (n > maxType) return "Troppi " + (bp ? bp->displayName : id) + " in squadra: " + std::to_string(n) + " (massimo " + std::to_string(maxType) + ")";
        }
        return "";
    }

    bool GameLayer::StartVsBotMatch(int tierIdx) {
        m_VsBotMessage.clear();
        if (tierIdx < 0 || tierIdx > 2 || m_SelectedSquadIndices.empty()) return false;
        const VsBotQueueTier& tier = kVsBotTiers[tierIdx];
        const int squadPoints = CalculateSquadPoints();
        const int squadSize = static_cast<int>(m_SelectedSquadIndices.size());
        if (!VsBotEligible(squadPoints, squadSize, tier) || !SquadTypeCapProblem().empty()) return false;
        auto players = VsBotMakeRoster(tierIdx, { { 0, "Tu" } });
        if (players.empty()) { m_VsBotMessage = "Impossibile formare una squadra avversaria per questa coda."; return false; }
        return VsBotLaunch(tierIdx, std::move(players));
    }

    // Roster della partita: i giocatori veri (squadra 0, al posto di altrettanti bot) e i giocatori IA. Ogni bot pesca
    // uno schema diverso e mescola le fazioni (VsBotBuildMixedSquad), entro il cap della coda; quel che avanza del cap
    // va in equipaggiamento a caso (AddBotExtras, col suo costo). Le squadre dei giocatori veri le porta ognuno da se'.
    std::vector<VsBotPlayer> GameLayer::VsBotMakeRoster(int tierIdx, const std::vector<std::pair<std::uint64_t, std::string>>& humans) {
        const VsBotQueueTier& tier = kVsBotTiers[std::clamp(tierIdx, 0, 2)];
        std::vector<VsBotCandidate> all;
        // Costo di un candidato = unita' con la sua dotazione base (punti battaglia)
        for (const auto& [id, bp] : BlueprintManager::Get().GetUnits()) all.push_back({ id, MakeBotUnit(id, 1).CalculateBattlePoints(), VsBotFactionOf(id), bp.max_per_squad });
        std::sort(all.begin(), all.end(), [](const VsBotCandidate& a, const VsBotCandidate& b) { return a.unitId < b.unitId; });
        std::mt19937 rng(std::random_device{}());
        std::vector<VsBotPlayer> players;
        std::vector<std::string> usedNames;
        int humansIn = 0;
        for (const auto& [id, name] : humans) {
            if (humansIn >= kVsBotTeamSize) break;
            VsBotPlayer h;
            h.name = name; h.team = 0; h.human = true; h.owner = id;
            players.push_back(h);
            usedNames.push_back(name);
            ++humansIn;
        }
        auto makeAi = [&](int team) {
            int style = 0;
            const auto ids = VsBotBuildMixedSquad(all, tier, rng, &style);
            if (ids.empty()) return;
            VsBotPlayer p;
            p.team = team;
            p.style = style;
            // nome dalla fazione dell'unita' piu' cara della squadra
            std::string lead = ids[0]; int leadCost = -1;
            for (const auto& id : ids) for (const auto& c : all) if (c.unitId == id && c.cost > leadCost) { leadCost = c.cost; lead = id; }
            p.name = VsBotRandomName(VsBotFactionOf(lead), usedNames, rng);
            std::unordered_map<std::string, int> counts;
            int sum = 0;
            for (const auto& id : ids) {
                VsBotSlot s;
                s.unit = MakeBotUnit(id, ++counts[id]);
                sum += s.unit.CalculateBattlePoints();
                p.squad.push_back(s);
            }
            // Oggetti extra a caso finche' il budget del giocatore lo permette (la squadra resta sotto il cap):
            // l'ordine delle unita' cambia ogni volta, cosi' non e' sempre la prima a prendersi il budget
            int budget = std::max(0, tier.cap - sum);
            std::vector<size_t> order(p.squad.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::shuffle(order.begin(), order.end(), rng);
            for (size_t i : order) AddBotExtras(p.squad[i].unit, budget);
            for (auto& s : p.squad) s.points = s.unit.CalculateBattlePoints();
            std::shuffle(p.squad.begin(), p.squad.end(), rng);
            players.push_back(p);
        };
        for (int i = humansIn; i < kVsBotTeamSize; ++i) makeAi(0);
        for (int i = 0; i < kVsBotTeamSize; ++i) makeAi(1);
        int enemyPlayers = 0;
        for (const auto& p : players) enemyPlayers += p.team == 1 ? 1 : 0;
        if (enemyPlayers == 0) return {};
        for (const auto& p : players) if (!p.human) {
            int pts = 0; std::string list;
            for (const auto& s : p.squad) { pts += s.points; list += s.unit.blueprintId + " "; }
            SUN_CORE_INFO("Vs Bot: {0} (squadra {1}, {2}) {3} pti: {4}", p.name, p.team, VsBotStyleName(p.style), pts, list);
        }
        return players;
    }

    int GameLayer::VsBotMeIndex() const {
        for (int i = 0; i < static_cast<int>(m_VsBotPlayers.size()); ++i) if (m_VsBotPlayers[i].local) return i;
        return -1;
    }

    // Avvio comune (da solo o in squadra): scena, mappa, basi, la mia squadra (quella dello schieramento, entro il cap
    // della coda), i bot in campo se li simulo io. Poi 20 secondi di riscaldamento chiusi nelle basi.
    bool GameLayer::VsBotLaunch(int tierIdx, std::vector<VsBotPlayer> players) {
        const VsBotQueueTier& tier = kVsBotTiers[std::clamp(tierIdx, 0, 2)];
        const std::uint64_t self = m_Party.SelfId();
        int me = -1;
        for (int i = 0; i < static_cast<int>(players.size()); ++i) {
            auto& p = players[i];
            p.local = p.human && me < 0 && (p.owner == 0 || p.owner == self);
            if (p.local) me = i;
        }
        if (me < 0 || m_PlayerRoster.empty()) { m_VsBotMessage = "Non sei in questa partita."; return false; }
        // La mia squadra: le unita' scelte, finche' stanno nel cap della coda (in squadra ognuno porta la sua)
        {
            auto& my = players[me];
            my.squad.clear();
            int sum = 0;
            for (int idx : m_SelectedSquadIndices) {
                if (idx < 0 || idx >= static_cast<int>(m_PlayerRoster.size())) continue;
                const int pts = m_PlayerRoster[idx].CalculateBattlePoints();
                if (sum + pts > tier.cap) continue;
                VsBotSlot s; s.unit = m_PlayerRoster[idx]; s.rosterIdx = idx; s.points = pts;
                my.squad.push_back(s); sum += pts;
            }
            if (my.squad.empty()) {   // nessuna sta nel cap: la piu' economica fra quelle scelte (o quella del garage)
                int best = -1, bestPts = 1 << 30;
                std::vector<int> cand = m_SelectedSquadIndices;
                if (cand.empty()) cand.push_back(std::clamp(m_SelectedVaultIndex, 0, static_cast<int>(m_PlayerRoster.size()) - 1));
                for (int idx : cand) if (idx >= 0 && idx < static_cast<int>(m_PlayerRoster.size())) { const int pts = m_PlayerRoster[idx].CalculateBattlePoints(); if (pts < bestPts) { bestPts = pts; best = idx; } }
                if (best < 0) return false;
                VsBotSlot s; s.unit = m_PlayerRoster[best]; s.rosterIdx = best; s.points = bestPts;
                my.squad.push_back(s);
            }
        }

        // Scena pulita e mappa 5v5
        auto view = m_Scene.GetRegistry().view<ColliderComponent>(entt::exclude<PlayerTag>);
        for (auto e : view) { m_PhysicsSystem.DestroyJoltBody(m_Scene, e); m_Scene.DestroyEntity(e); }
        m_VsBotUnits.clear();
        if (!ReloadMap("vsbot_trincee")) SUN_CORE_WARN("Vs Bot: mappa caricata senza navmesh, i bot non potranno muoversi");
        m_VsBotSpawns[0] = m_CurrentMapData.playerSpawnPoints;
        m_VsBotSpawns[1] = m_CurrentMapData.enemySpawnPoints;
        glm::vec3 base0(0.0f), base1(0.0f);
        for (const auto& p : m_VsBotSpawns[0]) base0 += p;
        for (const auto& p : m_VsBotSpawns[1]) base1 += p;
        if (!m_VsBotSpawns[0].empty()) base0 /= static_cast<float>(m_VsBotSpawns[0].size());
        if (!m_VsBotSpawns[1].empty()) base1 /= static_cast<float>(m_VsBotSpawns[1].size());
        m_BotAI.SetTeamBases(base0, base1);
        m_VsBotYaw[0] = glm::degrees(std::atan2(-(base1 - base0).x, -(base1 - base0).z));   // squadra 0 guarda verso i nemici
        m_VsBotYaw[1] = m_VsBotYaw[0] + 180.0f;
        // Basi: rettangolo attorno ai punti di schieramento, allargato verso l'esterno (muri invisibili per l'altra squadra)
        for (int t = 0; t < 2; ++t) {
            m_VsBotZones[t] = VsBotZone();
            if (m_VsBotSpawns[t].empty()) continue;
            glm::vec2 mn(1e9f), mx(-1e9f);
            for (const auto& p : m_VsBotSpawns[t]) { mn = glm::min(mn, glm::vec2(p.x, p.z)); mx = glm::max(mx, glm::vec2(p.x, p.z)); }
            m_VsBotZones[t].min = mn - glm::vec2(11.0f, 14.0f);
            m_VsBotZones[t].max = mx + glm::vec2(11.0f, 14.0f);
            m_VsBotZones[t].valid = true;
        }

        // Io = prima unita' della mia squadra; le altre restano in riserva per i rientri
        auto& my = players[me];
        m_VsBotMyTeam = my.team == 1 ? 1 : 0;
        m_ActiveSquad.clear();
        for (const auto& s : my.squad) m_ActiveSquad.push_back(ActiveSquadMember{ s.rosterIdx, true, 100.0f });
        m_CurrentSquadMemberIdx = 0;
        m_KillCount = 0;
        ResetMatchXp();
        m_PlayerDeathHandled = false;
        m_VsBotDeadTime = 0.0f;
        m_VsBotSpectate = 0;
        ClearPlayerDeathState();
        ApplyUnitVisuals(my.squad[0].unit);
        my.squad[0].state = VsBotSlotState::InField;
        my.active = 0;
        m_VsBotPlayers = std::move(players);
        auto& reg = m_Scene.GetRegistry();
        reg.emplace_or_replace<TeamComponent>(m_PlayerEntity, TeamComponent{ m_VsBotMyTeam });
        VsBotPlaceMe(false);
        if (auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity)) hp->currentHealth = hp->maxHealth;

        // I giocatori IA schierano la prima unita' (solo chi simula i bot: in squadra l'host, gli altri ne vedono le copie)
        if (NetIsAuthority()) {
            int spawnIdx[2] = { 0, 0 };
            for (const auto& p : m_VsBotPlayers) if (p.human) ++spawnIdx[p.team == 1 ? 1 : 0];
            for (auto& p : m_VsBotPlayers) {
                if (p.human) continue;
                VsBotSpawnAi(p, 0, VsBotSpawnPoint(p.team, spawnIdx[p.team == 1 ? 1 : 0]++, false));
            }
            m_PhysicsSystem.RefreshBodies(m_Scene);
        }
        int pts[2] = { 0, 0 }, units[2] = { 0, 0 };
        for (const auto& p : m_VsBotPlayers) { pts[p.team == 1 ? 1 : 0] += p.TotalPoints(); units[p.team == 1 ? 1 : 0] += static_cast<int>(p.squad.size()); }
        SUN_CORE_INFO("Vs Bot {0}: squadra 0 {1} pti in {2} unita', squadra 1 {3} pti in {4} unita'", tier.name, pts[0], units[0], pts[1], units[1]);

        g_WaaaghActive = false;
        m_VsBotActive = true;
        m_VsBotTier = std::clamp(tierIdx, 0, 2);
        m_VsBotTime = 0.0f;
        m_VsBotWarmup = kVsBotWarmupTime;
        m_VsBotResult = 0;
        m_VsBotWinner = -2;
        m_VsBotWhy = 0;
        m_VsBotReward = 0;
        m_VsBotEnemyKills = 0;
        m_VsBotEndReason.clear();
        m_IsPaused = false;
        ResetFactionBuff();
        m_GameState = GameState::Gameplay;
        if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        return true;
    }

    // Mi mette al punto di schieramento della mia squadra (all'avvio il mio posto fra i compagni, ai rientri a caso)
    void GameLayer::VsBotPlaceMe(bool respawn) {
        auto& reg = m_Scene.GetRegistry();
        const int team = m_VsBotMyTeam;
        int idx = 0;
        if (respawn) idx = Random::RangeInt(0, std::max(0, static_cast<int>(m_VsBotSpawns[team].size()) - 1));
        else {
            const int me = VsBotMeIndex();
            for (int i = 0; i < me && i < static_cast<int>(m_VsBotPlayers.size()); ++i) if (m_VsBotPlayers[i].human && m_VsBotPlayers[i].team == team) ++idx;
        }
        if (auto* pt = reg.try_get<TransformComponent>(m_PlayerEntity)) {
            pt->Position = VsBotSpawnPoint(team, idx, respawn);
            pt->AimYaw = m_VsBotYaw[team]; pt->Rotation.y = m_VsBotYaw[team]; pt->AimPitch = 0.0f;
            m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, pt->Position);
        }
    }

    // Fine partita dal punto di vista di chi guarda: vittoria se ha vinto la mia squadra
    void GameLayer::VsBotFinish(int winnerTeam, int why) {
        if (m_VsBotResult != 0) return;
        m_VsBotWinner = winnerTeam;
        m_VsBotWhy = why;
        const bool won = winnerTeam == m_VsBotMyTeam;
        if (winnerTeam < 0) EndVsBotMatch(3, why == 0 ? "Entrambe le parti sono state eliminate" : "Tempo scaduto: parita' di punti rimasti");
        else if (why == 0) EndVsBotMatch(won ? 1 : 2, won ? "Tutte le squadre avversarie sono state eliminate" : "Tutte le squadre alleate sono state eliminate");
        else EndVsBotMatch(won ? 1 : 2, won ? "Tempo scaduto: avete piu' punti rimasti" : "Tempo scaduto: gli avversari hanno piu' punti rimasti");
    }

    // Un giocatore vero passa all'altra squadra durante il riscaldamento: prende il posto di un bot di quella squadra,
    // e il bot va nella squadra che lascia (le parti restano 5 contro 5). Lo decide chi simula i bot.
    void GameLayer::VsBotSwitchTeam(int playerIdx) {
        if (!m_VsBotActive || m_VsBotWarmup <= 0.0f || m_VsBotResult != 0) return;
        if (playerIdx < 0 || playerIdx >= static_cast<int>(m_VsBotPlayers.size()) || !m_VsBotPlayers[playerIdx].human) return;
        const int from = m_VsBotPlayers[playerIdx].team == 1 ? 1 : 0, to = 1 - from;
        int b = -1;
        for (int i = static_cast<int>(m_VsBotPlayers.size()) - 1; i >= 0; --i) if (!m_VsBotPlayers[i].human && m_VsBotPlayers[i].team == to) { b = i; break; }
        if (b < 0) { m_PartyToasts.push_back({ "Nell'altra squadra non c'e' un bot da sostituire", 3.0f }); return; }
        auto& bot = m_VsBotPlayers[b];
        auto& reg = m_Scene.GetRegistry();
        if (bot.active >= 0 && bot.active < static_cast<int>(bot.squad.size())) {   // la sua unita' in campo rientra dall'altra base
            auto& s = bot.squad[bot.active];
            if (reg.valid(s.entity)) {
                NetDespawnEnemy(s.entity);
                m_PhysicsSystem.DestroyJoltBody(m_Scene, s.entity);
                m_Scene.DestroyEntity(s.entity);
            }
            m_VsBotUnits.erase(s.entity);
            s.entity = entt::null;
            s.state = VsBotSlotState::Reserve;
            bot.active = -1;
        }
        bot.respawnTimer = 0.0f;
        bot.team = from;
        m_VsBotPlayers[playerIdx].team = to;
        m_Net.rosterTimer = 0.0f;   // in squadra: tutti lo sanno subito
        SUN_CORE_INFO("Vs Bot: {0} passa alla squadra {1} al posto di {2}", m_VsBotPlayers[playerIdx].name, to, bot.name);
    }

    // Basi: durante il riscaldamento ognuno resta nella sua; poi la squadra avversaria non puo' entrare nella base
    // dell'altra (si esce dalla propria, non si entra in quella nemica). Le copie in rete le corregge il loro proprietario.
    void GameLayer::VsBotApplySpawnZones() {
        if (!m_VsBotActive || m_VsBotResult != 0) return;
        auto& reg = m_Scene.GetRegistry();
        const bool warm = m_VsBotWarmup > 0.0f;
        auto view = reg.view<TeamComponent, TransformComponent>(entt::exclude<CorpseComponent, NetGhostComponent>);
        for (auto e : view) {
            const int team = view.get<TeamComponent>(e).team == 1 ? 1 : 0;
            auto& t = view.get<TransformComponent>(e);
            glm::vec2 p(t.Position.x, t.Position.z);
            const glm::vec2 before = p;
            if (warm) {
                const VsBotZone& z = m_VsBotZones[team];
                if (!z.valid) continue;
                p = glm::clamp(p, z.min + glm::vec2(0.4f), z.max - glm::vec2(0.4f));
            }
            else {
                const VsBotZone& z = m_VsBotZones[1 - team];
                if (!z.valid || p.x <= z.min.x || p.x >= z.max.x || p.y <= z.min.y || p.y >= z.max.y) continue;
                // fuori dal lato piu' vicino
                const float dl = p.x - z.min.x, dr = z.max.x - p.x, db = p.y - z.min.y, df = z.max.y - p.y;
                const float m = std::min(std::min(dl, dr), std::min(db, df));
                if (m == dl) p.x = z.min.x - 0.05f; else if (m == dr) p.x = z.max.x + 0.05f;
                else if (m == db) p.y = z.min.y - 0.05f; else p.y = z.max.y + 0.05f;
            }
            if (p == before) continue;
            t.Position.x = p.x; t.Position.z = p.y;
            if (e == m_PlayerEntity) m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, t.Position);
        }
    }

    // Bordo delle basi disegnato a terra: la mia in azzurro, quella avversaria (vietata) in rosso
    void GameLayer::RenderVsBotZones() {
        if (!m_VsBotActive) return;
        const glm::mat4 vp = m_Camera.GetViewProjection();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImGuiIO& io = ImGui::GetIO();
        for (int t = 0; t < 2; ++t) {
            const VsBotZone& z = m_VsBotZones[t];
            if (!z.valid) continue;
            float y = 0.05f;
            if (!m_VsBotSpawns[t].empty()) y = m_VsBotSpawns[t][0].y + 0.05f;
            const bool mine = t == m_VsBotMyTeam;
            const ImU32 col = mine ? IM_COL32(110, 210, 255, 150) : IM_COL32(255, 80, 60, 190);
            const glm::vec2 c[4] = { z.min, glm::vec2(z.max.x, z.min.y), z.max, glm::vec2(z.min.x, z.max.y) };
            for (int k = 0; k < 4; ++k) {
                const glm::vec2 a = c[k], b = c[(k + 1) % 4];
                const int steps = std::max(1, static_cast<int>(glm::length(b - a) / 1.5f));
                for (int i = 0; i < steps; i += 2) {   // tratteggio
                    const glm::vec2 p0 = glm::mix(a, b, static_cast<float>(i) / steps), p1 = glm::mix(a, b, static_cast<float>(i + 1) / steps);
                    const glm::vec4 q0 = vp * glm::vec4(p0.x, y, p0.y, 1.0f), q1 = vp * glm::vec4(p1.x, y, p1.y, 1.0f);
                    if (q0.w <= 0.1f || q1.w <= 0.1f) continue;
                    const ImVec2 s0((q0.x / q0.w * 0.5f + 0.5f) * io.DisplaySize.x, (1.0f - (q0.y / q0.w * 0.5f + 0.5f)) * io.DisplaySize.y);
                    const ImVec2 s1((q1.x / q1.w * 0.5f + 0.5f) * io.DisplaySize.x, (1.0f - (q1.y / q1.w * 0.5f + 0.5f)) * io.DisplaySize.y);
                    dl->AddLine(s0, s1, col, mine ? 2.0f : 3.0f);
                }
            }
        }
    }

    // Rientri e fine partita. Ogni giocatore ha in campo una sola unita': quando cade, l'IA rientra dopo
    // kVsBotAiRespawn secondi con un'altra unita' della sua squadra, tu scegli dalla schermata di morte.
    // Una squadra e' eliminata quando nessun suo giocatore ha piu' unita' in campo o in riserva; allo scadere
    // vince chi ha piu' punti battaglia rimasti (in campo + in riserva).
    void GameLayer::UpdateVsBot(float deltaTime) {
        if (!m_VsBotActive || m_VsBotResult != 0 || (m_IsPaused && !m_Net.active)) return;
        const bool auth = NetIsAuthority();
        if (m_VsBotWarmup > 0.0f) m_VsBotWarmup = std::max(0.0f, m_VsBotWarmup - deltaTime);
        else m_VsBotTime += deltaTime;
        auto& reg = m_Scene.GetRegistry();
        // la mia squadra e' cambiata (passaggio all'altra parte durante il riscaldamento): si rientra dall'altra base
        const int me = VsBotMeIndex();
        if (me >= 0 && (m_VsBotPlayers[me].team == 1 ? 1 : 0) != m_VsBotMyTeam) {
            m_VsBotMyTeam = m_VsBotPlayers[me].team == 1 ? 1 : 0;
            reg.emplace_or_replace<TeamComponent>(m_PlayerEntity, TeamComponent{ m_VsBotMyTeam });
            const auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity);
            if (hp && hp->currentHealth > 0.0f && !reg.all_of<CorpseComponent>(m_PlayerEntity)) VsBotPlaceMe(true);
            m_PartyToasts.push_back({ m_VsBotMyTeam == 0 ? "Ora giochi con la squadra della base nord" : "Ora giochi con la squadra della base sud", 4.0f });
        }
        for (auto& p : m_VsBotPlayers) {
            if (p.human && !p.local) continue;   // gli altri giocatori veri: lo stato lo mandano loro
            if (!p.human && !auth) continue;     // i bot li muove chi li simula (l'host)
            if (p.active >= 0 && p.active < static_cast<int>(p.squad.size())) {
                auto& s = p.squad[p.active];
                bool dead;
                if (p.local) {
                    const auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity);
                    dead = !hp || hp->currentHealth <= 0.0f;
                }
                else {
                    const auto* hp = reg.valid(s.entity) ? reg.try_get<HealthComponent>(s.entity) : nullptr;
                    dead = !hp || hp->currentHealth <= 0.0f || reg.all_of<CorpseComponent>(s.entity);
                }
                if (dead) {
                    m_VsBotUnits.erase(s.entity);
                    s.state = VsBotSlotState::Fallen;
                    s.entity = entt::null;
                    p.active = -1;
                    p.respawnTimer = p.local ? kVsBotHumanRespawn : kVsBotAiRespawn;
                }
                continue;
            }
            p.respawnTimer = std::max(0.0f, p.respawnTimer - deltaTime);
            if (p.human || p.respawnTimer > 0.0f) continue;
            // IA: rientra con un'unita' a caso fra quelle in riserva (l'avversario non sa cosa arriva)
            std::vector<int> reserve;
            for (int i = 0; i < static_cast<int>(p.squad.size()); ++i) if (p.squad[i].state == VsBotSlotState::Reserve) reserve.push_back(i);
            if (reserve.empty()) continue;
            const int pick = reserve[Random::RangeInt(0, static_cast<int>(reserve.size()) - 1)];
            const int nPts = static_cast<int>(m_VsBotSpawns[p.team == 1 ? 1 : 0].size());
            VsBotSpawnAi(p, pick, VsBotSpawnPoint(p.team, Random::RangeInt(0, std::max(0, nPts - 1)), true));
            m_PhysicsSystem.RefreshBodies(m_Scene);
        }
        if (me < 0 || m_VsBotPlayers[me].active < 0) m_VsBotDeadTime += deltaTime;

        // Fine partita: la decide chi simula i bot (in squadra la manda a tutti)
        if (!auth || m_VsBotWarmup > 0.0f) return;
        int left[2] = { 0, 0 }, points[2] = { 0, 0 };
        for (const auto& p : m_VsBotPlayers) {
            // un compagno che non si sente piu' (uscito, caduto) non conta: la sua squadra e' persa
            if (p.human && !p.local && (!m_Net.active || m_Net.remotes.find(p.owner) == m_Net.remotes.end())) continue;
            const int team = p.team == 1 ? 1 : 0;
            left[team] += p.CountState(VsBotSlotState::InField) + p.CountState(VsBotSlotState::Reserve);
            points[team] += p.PointsState(VsBotSlotState::InField) + p.PointsState(VsBotSlotState::Reserve);
        }
        if (left[0] == 0 && left[1] == 0) VsBotFinish(-1, 0);
        else if (left[1] == 0) VsBotFinish(0, 0);
        else if (left[0] == 0) VsBotFinish(1, 0);
        else if (m_VsBotTime >= m_VsBotTimeLimit) {
            if (points[0] > points[1]) VsBotFinish(0, 1);
            else if (points[0] < points[1]) VsBotFinish(1, 1);
            else VsBotFinish(-1, 2);
        }
    }

    void GameLayer::EndVsBotMatch(int result, const char* reason) {
        m_VsBotResult = result;
        m_VsBotEndReason = reason;
        // Ricompensa: punti studio dell'esperienza di partita (ridotti se sei morto o hai perso), energia = meta'.
        // Vittoria: 100% se non sei mai morto, 75% se sei morto. Sconfitta o pareggio: 50% se sei morto, 75% se no.
        float outcome = 1.0f;
        std::string label;
        if (result == 1) { outcome = m_MatchPlayerDied ? 0.75f : 1.0f; label = m_MatchPlayerDied ? "Vittoria, ma sei caduto" : "Vittoria senza mai cadere"; }
        else {
            outcome = m_MatchPlayerDied ? 0.5f : 0.75f;
            label = std::string(result == 3 ? "Pareggio" : "Sconfitta") + (m_MatchPlayerDied ? ", e sei caduto" : ", ma sei sopravvissuto");
        }
        ApplyMatchXp(outcome, label);
        m_VsBotReward = static_cast<int>(m_MatchEnergy);
        m_IsPaused = true;   // la simulazione si ferma sotto la schermata dei risultati
        if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        SUN_CORE_INFO("Vs Bot terminata: {0} (+{1} energia)", reason, m_VsBotReward);
    }

    void GameLayer::LeaveVsBotMatch() {
        auto& reg = m_Scene.GetRegistry();
        std::vector<entt::entity> toDestroy;
        auto view = reg.view<HealthComponent>(entt::exclude<PlayerTag>);
        for (auto e : view) toDestroy.push_back(e);
        for (auto e : toDestroy) {
            if (!reg.valid(e)) continue;
            m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
            m_Scene.DestroyEntity(e);   // l'observer dei bot elimina anche l'arma
        }
        m_VsBotUnits.clear();
        m_VsBotPlayers.clear();
        ClearSummons();
        ResetFactionBuff();
        if (reg.all_of<TeamComponent>(m_PlayerEntity)) reg.remove<TeamComponent>(m_PlayerEntity);
        m_VsBotActive = false;
        m_VsBotResult = 0;
        m_VsBotMyTeam = 0;
        m_VsBotWarmup = 0.0f;
        m_VsBotWinner = -2;
        m_IsPaused = false;
        m_PlayerDeathHandled = false;
        ClearPlayerDeathState();
        if (!m_SelectedSquadIndices.empty() && m_SelectedSquadIndices[0] < static_cast<int>(m_PlayerRoster.size()))
            ApplyUnitVisuals(m_PlayerRoster[m_SelectedSquadIndices[0]]);
        if (auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity)) hp->currentHealth = hp->maxHealth;
        m_GameState = GameState::MainMenu;
        if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    // Punto di rientro della squadra 'team': i = indice del punto (a rotazione), jitter = piccolo scarto per
    // non far comparire due unita' una dentro l'altra.
    glm::vec3 GameLayer::VsBotSpawnPoint(int team, int i, bool jitter) const {
        const auto& pts = m_VsBotSpawns[team == 1 ? 1 : 0];
        glm::vec3 p = pts.empty() ? glm::vec3(static_cast<float>(i) * 1.5f, 0.0f, 0.0f)
                                  : pts[static_cast<size_t>(i) % pts.size()] + glm::vec3(0.0f, 0.0f, static_cast<float>(static_cast<size_t>(i) / pts.size()) * 1.5f);
        if (jitter) { p.x += Random::Range(-1.2f, 1.2f); p.z += Random::Range(-1.2f, 1.2f); }
        return p;
    }

    // Un giocatore IA schiera lo slot indicato della sua squadra.
    void GameLayer::VsBotSpawnAi(VsBotPlayer& p, int slot, const glm::vec3& pos) {
        if (slot < 0 || slot >= static_cast<int>(p.squad.size())) return;
        auto& s = p.squad[slot];
        const entt::entity e = SpawnBot(s.unit, p.team, pos, m_VsBotYaw[p.team == 1 ? 1 : 0], -1);
        if (e == entt::null) { s.state = VsBotSlotState::Fallen; return; }   // blueprint mancante: lo slot e' perso
        if (auto* brain = m_Scene.GetRegistry().try_get<BotBrainComponent>(e)) brain->displayName = p.name + " (" + s.unit.customName + ")";
        s.entity = e;
        s.state = VsBotSlotState::InField;
        p.active = slot;
    }

    // Rientri: il giocatore sceglie un'unita' in riserva della sua squadra e riparte dal punto di schieramento.
    void GameLayer::VsBotRespawnHuman(int slot) {
        const int meIdx = VsBotMeIndex();
        if (meIdx < 0) return;
        auto& me = m_VsBotPlayers[meIdx];
        if (slot < 0 || slot >= static_cast<int>(me.squad.size()) || me.squad[slot].state != VsBotSlotState::Reserve || me.respawnTimer > 0.0f) return;
        auto& s = me.squad[slot];
        auto& reg = m_Scene.GetRegistry();

        ClearPlayerDeathState();   // via ragdoll e CorpseComponent
        ApplyUnitVisuals(s.unit);  // statistiche, armatura, armi, munizioni piene, vesti
        reg.emplace_or_replace<TeamComponent>(m_PlayerEntity, TeamComponent{ m_VsBotMyTeam });
        VsBotPlaceMe(true);
        if (auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity)) hp->currentHealth = hp->maxHealth;
        if (auto* melee = reg.try_get<MeleeStateComponent>(m_PlayerEntity)) {
            melee->currentPhase = MeleePhase::None; melee->currentAttack = MeleeAttackType::None;
            melee->currentAttackNode = ""; melee->stateTimer = 0.0f; melee->bufferedInput = "";
            melee->inputBufferTimer = 0.0f; melee->hasHitInThisAttack = false;
        }
        if (auto* ws = reg.try_get<WeaponStateComponent>(m_PlayerEntity)) {
            ws->isReloading = false; ws->reloadTimer = 0.0f; ws->fireCooldown = 0.0f; ws->currentHeat = 0.0f; ws->isOverheating = false;
        }
        s.state = VsBotSlotState::InField;
        me.active = slot;
        m_CurrentSquadMemberIdx = -1;
        for (size_t i = 0; i < m_ActiveSquad.size(); ++i) if (m_ActiveSquad[i].rosterIndex == s.rosterIdx) m_CurrentSquadMemberIdx = static_cast<int>(i);
        if (m_CurrentSquadMemberIdx >= 0) { m_ActiveSquad[m_CurrentSquadMemberIdx].isAlive = true; m_ActiveSquad[m_CurrentSquadMemberIdx].currentHealth = 100.0f; }
        m_PlayerDeathHandled = false;
        m_VsBotDeadTime = 0.0f;
        if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }

    // Mentre la tua unita' e' a terra la partita continua: IA, armi, esplosivi, fisica e morti come in gioco,
    // ma senza input del giocatore.
    void GameLayer::UpdateVsBotWorld(float deltaTime) {
        m_WeaponSystem.Update(m_Scene, m_PhysicsSystem, deltaTime);   // il cadavere del giocatore e' escluso
        m_EquipmentSystem.Update(m_Scene, deltaTime, &m_BlockModels);
        UpdateSummons(deltaTime);
        m_ExplosiveSystem.Update(m_Scene, deltaTime);
        m_TurretSystem.Update(m_Scene, m_WeaponSystem, m_PhysicsSystem, m_PlayerEntity, false, deltaTime);
        m_PhysicsSystem.Update(m_Scene, deltaTime);
        CorpseSystem::Update(m_Scene, m_PhysicsSystem, deltaTime);
        UpdateHitFeedback(deltaTime);
        m_WeightSystem.Update(m_Scene, deltaTime);
        m_HealthRegen.Update(m_Scene, deltaTime);
        if (m_VsBotWarmup <= 0.0f) m_BotAI.Update(m_Scene, m_NavQuery, deltaTime);   // nel riscaldamento i bot aspettano
        m_MeleeCombat.Update(m_Scene, deltaTime);
        m_SquigSystem.Update(m_Scene, m_NavQuery, deltaTime, m_SquigSkeleton ? &*m_SquigSkeleton : nullptr);
        UpdateSquigVisuals();
        CheckUnitDeaths();
        CollectMatchXp(deltaTime, false);   // a terra niente sopravvivenza
        glm::vec3 playerPos(0.0f);
        if (const auto* pt = m_Scene.GetRegistry().try_get<TransformComponent>(m_PlayerEntity)) playerPos = pt->Position;
        if (auto* ap = m_Scene.GetRegistry().try_get<ActionPoseComponent>(m_PlayerEntity)) ap->repairing = false;
        ActionPose::Update(m_Scene, deltaTime);
        AnimateUnits(deltaTime, playerPos, false);
        ActionPose::ApplyCubes(m_Scene, m_PlayerEntity);
    }

    // Camera da spettatore: dopo un attimo sul tuo cadavere segue un alleato in vita (cambiabile dalla schermata).
    void GameLayer::UpdateVsBotSpectator() {
        if (m_VsBotDeadTime < 1.5f) return;
        auto& reg = m_Scene.GetRegistry();
        std::vector<entt::entity> allies;
        auto view = reg.view<BotBrainComponent, TeamComponent, HealthComponent, TransformComponent>(entt::exclude<CorpseComponent>);
        for (auto e : view) if (view.get<TeamComponent>(e).team == m_VsBotMyTeam && view.get<HealthComponent>(e).currentHealth > 0.0f) allies.push_back(e);
        if (allies.empty()) return;
        std::sort(allies.begin(), allies.end());
        const int n = static_cast<int>(allies.size());
        m_VsBotSpectate = ((m_VsBotSpectate % n) + n) % n;
        const auto& t = reg.get<TransformComponent>(allies[m_VsBotSpectate]);
        m_Camera.SetCameraOffsets(0.0f, 0.6f, 4.5f);
        m_Camera.SetTarget(t.Position + glm::vec3(0.0f, 1.6f, 0.0f), t.AimYaw);
        m_Camera.SetPitch(-12.0f);
        m_Camera.SetYawOffset(0.0f);
        m_Camera.SetViewportSize(m_WindowWidth, m_WindowHeight);
        m_Camera.Update(glm::vec3(0.0f), 1.0f);
        Renderer::SetViewProjection(m_Camera.GetViewProjection());
    }

    // Animazione procedurale di orki e bot (gambe, braccia) e spinta lontano dal giocatore.
    void GameLayer::AnimateUnits(float deltaTime, const glm::vec3& playerPos, bool pushFromPlayer) {
        static float orkAnimTimer = 0.0f;
        orkAnimTimer += deltaTime * 8.0f;

        float totalTime = static_cast<float>(glfwGetTime());

        auto enemyView = m_Scene.GetRegistry().view<TransformComponent, HealthComponent>(
            entt::exclude<PlayerTag, CorpseComponent, SquigComponent>
        );

        for (auto enemyEntity : enemyView) {
            auto& enemyTrans = enemyView.get<TransformComponent>(enemyEntity);

            glm::vec3 distVec = playerPos - enemyTrans.Position;
            float distToPlayer = glm::length(distVec);

            float minDistPlayer = 0.95f;
            glm::vec2 pXZ(playerPos.x, playerPos.z);
            glm::vec2 eXZ(enemyTrans.Position.x, enemyTrans.Position.z);
            float distPE = glm::distance(pXZ, eXZ);

            if (pushFromPlayer && distPE < minDistPlayer && distPE > 0.001f) {
                glm::vec2 pushDir = glm::normalize(eXZ - pXZ);
                glm::vec2 correctedOrkPos = pXZ + pushDir * minDistPlayer;
                enemyTrans.Position.x = correctedOrkPos.x;
                enemyTrans.Position.z = correctedOrkPos.y;
                m_PhysicsSystem.SetBodyPosition(m_Scene, enemyEntity, enemyTrans.Position);
            }

            if (m_Scene.GetRegistry().all_of<SkeletonComponent>(enemyEntity)) {
                auto& skel = m_Scene.GetRegistry().get<SkeletonComponent>(enemyEntity);
                auto* mState = m_Scene.GetRegistry().try_get<MeleeStateComponent>(enemyEntity);
                auto* hordeComp = m_Scene.GetRegistry().try_get<HordeAgentComponent>(enemyEntity);

                const auto* botBrain = m_Scene.GetRegistry().try_get<BotBrainComponent>(enemyEntity);
                bool isAttacking = mState && (mState->currentPhase != MeleePhase::None);
                bool isMoving = (hordeComp && (hordeComp->state != HordeState::Waiting)) || (botBrain && botBrain->isMoving);
                const bool botAiming = botBrain && botBrain->isAiming && !isAttacking;

                float animSpeed = isAttacking ? 4.0f : 10.0f;
                float legAngle = isAttacking ? 5.0f : (isMoving ? std::sin(totalTime * animSpeed) * 35.0f : 0.0f);
                for (auto& block : skel.data.blocks) {
                    if (block.name == "Leg_L") block.jointRotation.x = legAngle;
                    else if (block.name == "Leg_R") block.jointRotation.x = -legAngle;
                    else if (botAiming && (block.name == "Arm_R" || block.name == "Arm_L"))
                        block.jointRotation.x = 78.0f + enemyTrans.AimPitch * 0.65f;   // bot che mira: braccia in avanti
                    else if (block.name == "Arm_R" && !isAttacking) block.jointRotation.x = 25.0f + legAngle * 0.4f;
                    else if (block.name == "Arm_L" && !isAttacking) block.jointRotation.x = 25.0f - legAngle * 0.4f;
                }
            }
        }
    }

    // Arma in mano ai bot: modello a blocchi dell'arma attiva (o un cubo) sul fondo del braccio destro,
    // orientata lungo la mira.
    void GameLayer::UpdateBotVisuals() {
        auto& reg = m_Scene.GetRegistry();
        auto view = reg.view<BotBrainComponent, TransformComponent, SkeletonComponent>(entt::exclude<CorpseComponent>);
        for (auto e : view) {
            auto& brain = view.get<BotBrainComponent>(e);
            if (!reg.valid(brain.weaponEntity)) continue;
            const auto& t = view.get<TransformComponent>(e);
            const auto& skel = view.get<SkeletonComponent>(e);
            int armR = -1;
            for (size_t i = 0; i < skel.data.blocks.size(); ++i) if (skel.data.blocks[i].name == "Arm_R") { armR = static_cast<int>(i); break; }
            if (armR < 0) continue;
            const auto mats = skel.data.ComputeWorldTransforms(t.Position, t.Rotation, skel.scale);
            if (armR >= static_cast<int>(mats.size())) continue;
            const glm::mat4& arm = mats[armR];
            glm::mat4 armRot(1.0f);
            for (int i = 0; i < 3; ++i) armRot[i] = glm::vec4(glm::normalize(glm::vec3(arm[i])), 0.0f);
            const glm::vec3 armSize = skel.data.blocks[armR].size * skel.scale;
            const glm::vec3 hand = glm::vec3(arm[3]) + glm::vec3(armRot * glm::vec4(0.0f, -armSize.y * 0.5f, 0.0f, 0.0f));

            std::string weaponId;
            if (const auto* wid = reg.try_get<WeaponBlueprintId>(e)) weaponId = wid->id;
            const glm::mat4 weaponRot = glm::eulerAngleYXZ(glm::radians(t.AimYaw), glm::radians(t.AimPitch), 0.0f);
            auto& wt = reg.get<TransformComponent>(brain.weaponEntity);
            const BlockModel* model = m_BlockModels.Find(weaponId);
            // Corpo animato: l'arma va in mano al modello 3D
            if (model) SetWeaponBlockModel(brain.weaponEntity, weaponId);
            else { if (reg.all_of<BlockModelComponent>(brain.weaponEntity)) reg.remove<BlockModelComponent>(brain.weaponEntity); wt.Scale = glm::vec3(0.08f, 0.10f, 0.70f); }
            if (PlaceBotWeaponOnHand(e, wt, model)) continue;
            if (model) {
                const glm::vec3 grip = model->hasGripRight ? model->gripRight : glm::vec3(0.0f);
                wt.Position = hand - glm::vec3(weaponRot * glm::vec4(grip, 0.0f));
                SetWeaponBlockModel(brain.weaponEntity, weaponId);
            }
            else {
                if (reg.all_of<BlockModelComponent>(brain.weaponEntity)) reg.remove<BlockModelComponent>(brain.weaponEntity);
                wt.Scale = glm::vec3(0.08f, 0.10f, 0.70f);
                wt.Position = hand + glm::vec3(weaponRot * glm::vec4(0.0f, 0.0f, -0.30f, 0.0f));
            }
            float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
            glm::extractEulerAngleYXZ(weaponRot, yaw, pitch, roll);
            wt.Rotation = glm::degrees(glm::vec3(pitch, yaw, roll));
        }
    }

    // HUD della partita: squadre in vita, tempo, nomi sopra le unita' (alleati sempre, avversari solo se visibili).
    void GameLayer::RenderVsBotHUD() {
        auto& reg = m_Scene.GetRegistry();
        int allyLeft = 0, enemyFallen = 0;
        for (const auto& p : m_VsBotPlayers) {
            if (p.team == m_VsBotMyTeam) allyLeft += p.CountState(VsBotSlotState::InField) + p.CountState(VsBotSlotState::Reserve);
            else enemyFallen += p.CountState(VsBotSlotState::Fallen);
        }

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 12.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::Begin("VsBotHUD", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
        const int left = std::max(0, static_cast<int>(m_VsBotTimeLimit - m_VsBotTime));
        ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "%s", kVsBotTiers[std::clamp(m_VsBotTier, 0, 2)].name);
        ImGui::SameLine(); ImGui::TextColored(ImVec4(0.36f, 0.80f, 0.74f, 1.0f), "  ALLEATI: %d unita' rimaste", allyLeft);
        ImGui::SameLine(); ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "  AVVERSARI: %d abbattute", enemyFallen);
        ImGui::SameLine(); ImGui::Text("   %02d:%02d", left / 60, left % 60);
        ImGui::SameLine(); ImGui::TextDisabled("   [TAB] squadre");
        ImGui::End();

        RenderVsBotZones();
        // Riscaldamento: conto alla rovescia grande e cambio di squadra
        if (m_VsBotWarmup > 0.0f) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            char big[64]; std::snprintf(big, sizeof(big), "LA BATTAGLIA INIZIA TRA %d", static_cast<int>(std::ceil(m_VsBotWarmup)));
            ImFont* f = Ui::FontTitle();
            const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, big);
            const ImVec2 at(io.DisplaySize.x * 0.5f - ts.x * 0.5f, io.DisplaySize.y * 0.22f);
            fg->AddText(f, f->FontSize, ImVec2(at.x + 2.0f, at.y + 2.0f), IM_COL32(0, 0, 0, 200), big);
            fg->AddText(f, f->FontSize, at, Ui::U32(Ui::Col::Gold), big);
            const char* help = m_VsBotMyTeam == 0 ? "Base nord. [T] passa all'altra squadra al posto di un bot (per giocare contro i compagni)"
                                                  : "Base sud. [T] passa all'altra squadra al posto di un bot (per giocare contro i compagni)";
            const ImVec2 hs = ImGui::CalcTextSize(help);
            fg->AddText(ImVec2(io.DisplaySize.x * 0.5f - hs.x * 0.5f, at.y + ts.y + 10.0f), Ui::U32(Ui::Col::Bone, 0.95f), help);
            const char* help2 = "Fino all'inizio si resta nella propria base; poi nessuno puo' entrare nella base avversaria.";
            const ImVec2 h2 = ImGui::CalcTextSize(help2);
            fg->AddText(ImVec2(io.DisplaySize.x * 0.5f - h2.x * 0.5f, at.y + ts.y + 32.0f), Ui::U32(Ui::Col::Ash, 0.95f), help2);
        }

        const glm::mat4 vp = m_Camera.GetViewProjection();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const glm::vec3 camPos = m_Camera.GetPosition();
        auto bots = reg.view<BotBrainComponent, TeamComponent, HealthComponent, TransformComponent>(entt::exclude<CorpseComponent>);
        for (auto e : bots) {
            const auto& h = bots.get<HealthComponent>(e);
            if (h.currentHealth <= 0.0f) continue;
            const bool ally = bots.get<TeamComponent>(e).team == m_VsBotMyTeam;
            const auto& t = bots.get<TransformComponent>(e);
            if (glm::distance(camPos, t.Position) > (ally ? 120.0f : 80.0f)) continue;
            float topY = 2.0f;
            if (const auto* col = reg.try_get<ColliderComponent>(e)) topY = col->localCenterOffset.y + col->halfExtents.y;
            const glm::vec3 anchor = t.Position + glm::vec3(0.0f, topY + 0.3f, 0.0f);
            if (!ally && !HasLineOfSight(m_Scene, camPos, anchor - glm::vec3(0.0f, 0.5f, 0.0f), m_PlayerEntity)) continue;
            const glm::vec4 clip = vp * glm::vec4(anchor, 1.0f);
            if (clip.w <= 0.05f) continue;
            const glm::vec2 sp = WorldToScreen(anchor, vp, m_WindowWidth, m_WindowHeight);
            const auto& brain = bots.get<BotBrainComponent>(e);
            const ImU32 col = ally ? IM_COL32(110, 210, 255, 255) : IM_COL32(255, 95, 80, 255);
            const ImVec2 ts = ImGui::CalcTextSize(brain.displayName.c_str());
            dl->AddText(ImVec2(sp.x - ts.x * 0.5f, sp.y - ts.y - 8.0f), col, brain.displayName.c_str());
            const float frac = std::clamp(h.currentHealth / std::max(1.0f, h.maxHealth), 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(sp.x - 30.0f, sp.y - 5.0f), ImVec2(sp.x + 30.0f, sp.y), IM_COL32(40, 40, 45, 220), 2.0f);
            dl->AddRectFilled(ImVec2(sp.x - 30.0f, sp.y - 5.0f), ImVec2(sp.x - 30.0f + 60.0f * frac, sp.y), col, 2.0f);
        }
    }

    // Icona rappresentativa di un'unita': forma dal ruolo (tondo = tiratore, rombo = mischia, quadrato = versatile),
    // colore dell'unita' e sigla. Riserva = sbiadita, caduta = grigia con una X rossa.
    static void DrawVsBotUnitIcon(ImDrawList* dl, ImVec2 c, float r, const TrainedUnit& u, VsBotSlotState st) {
        const auto* bp = BlueprintManager::Get().GetUnit(u.blueprintId);
        std::string label = "?";
        if (bp) {
            const std::string& n = bp->displayName;
            const auto sp = n.find(' ');
            if (sp != std::string::npos && sp + 1 < n.size()) label = std::string(1, n[0]) + n[sp + 1];
            else label = n.substr(0, std::min<size_t>(2, n.size()));
            for (auto& ch : label) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        float alpha = st == VsBotSlotState::Reserve ? 0.45f : 1.0f;
        ImVec4 col(u.armorColor.r, u.armorColor.g, u.armorColor.b, alpha);
        if (st == VsBotSlotState::Fallen) col = ImVec4(0.32f, 0.32f, 0.34f, 1.0f);
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32(col);
        const ImU32 border = st == VsBotSlotState::InField ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 150, 160, 200);
        const std::string role = bp ? bp->ai_role : "";
        if (role == "ranged") {
            dl->AddCircleFilled(c, r, fill, 20);
            dl->AddCircle(c, r, border, 20, 1.5f);
        }
        else if (role == "melee") {
            const ImVec2 pts[4] = { ImVec2(c.x, c.y - r * 1.15f), ImVec2(c.x + r * 1.15f, c.y), ImVec2(c.x, c.y + r * 1.15f), ImVec2(c.x - r * 1.15f, c.y) };
            dl->AddConvexPolyFilled(pts, 4, fill);
            dl->AddPolyline(pts, 4, border, ImDrawFlags_Closed, 1.5f);
        }
        else {
            dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fill, 3.0f);
            dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), border, 3.0f, 0, 1.5f);
        }
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        const float lum = col.x * 0.3f + col.y * 0.59f + col.z * 0.11f;
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), lum > 0.55f ? IM_COL32(20, 20, 20, 255) : IM_COL32(245, 245, 245, 255), label.c_str());
        if (st == VsBotSlotState::Fallen) {
            dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), IM_COL32(230, 60, 50, 255), 2.0f);
            dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), IM_COL32(230, 60, 50, 255), 2.0f);
        }
    }

    // Giocatore a terra: sceglie con quale unita' della squadra rientrare (dopo kVsBotHumanRespawn secondi).
    // Senza unita' in riserva osserva gli alleati fino alla fine della partita.
    void GameLayer::RenderVsBotDeathUI() {
        if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(24.0f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.0f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420, 0));
        ImGui::Begin("UNITA' CADUTA", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        int chosen = -1;
        const int meIdx = VsBotMeIndex();
        const VsBotPlayer* me = meIdx >= 0 ? &m_VsBotPlayers[meIdx] : nullptr;
        if (me && me->CountState(VsBotSlotState::Reserve) > 0) {
            if (me->respawnTimer > 0.0f) ImGui::TextColored(ImVec4(0.88f, 0.52f, 0.24f, 1.0f), "Rinforzi pronti tra %.0f s", std::ceil(me->respawnTimer));
            else ImGui::TextColored(ImVec4(0.50f, 0.74f, 0.44f, 1.0f), "Scegli l'unita' con cui rientrare:");
            ImGui::Separator(); ImGui::Spacing();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int i = 0; i < static_cast<int>(me->squad.size()); ++i) {
                const auto& s = me->squad[i];
                const auto* bp = BlueprintManager::Get().GetUnit(s.unit.blueprintId);
                const ImVec2 cur = ImGui::GetCursorScreenPos();
                char label[192];
                std::snprintf(label, sizeof(label), "        %s  (%s)  %d pti%s##rientro%d", s.unit.customName.c_str(),
                    bp ? bp->displayName.c_str() : "?", s.points, s.state == VsBotSlotState::Fallen ? "  - CADUTO" : "", i);
                ImGui::BeginDisabled(s.state != VsBotSlotState::Reserve || me->respawnTimer > 0.0f);
                if (Ui::PlainButton(label, ImVec2(-1, 40))) chosen = i;
                ImGui::EndDisabled();
                DrawVsBotUnitIcon(dl, ImVec2(cur.x + 22.0f, cur.y + 20.0f), 12.0f, s.unit, s.state);
            }
        }
        else {
            ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "Non hai piu' unita' in riserva.");
            ImGui::TextWrapped("Osserva i tuoi alleati: la partita continua finche' una delle due parti resta senza unita'.");
        }
        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextDisabled("Spettatore");
        if (Ui::PlainButton("< Alleato precedente", ImVec2(196, 28))) --m_VsBotSpectate;
        ImGui::SameLine();
        if (Ui::PlainButton("Alleato successivo >", ImVec2(196, 28))) ++m_VsBotSpectate;
        ImGui::TextDisabled("Tieni premuto TAB per vedere le squadre");
        ImGui::Spacing();
        const bool leave = Ui::PlainButton("Abbandona la partita", ImVec2(-1, 32));
        ImGui::End();
        if (chosen >= 0) VsBotRespawnHuman(chosen);
        else if (leave) LeaveVsBotMatch();
    }

    // TAB: nomi dei giocatori. Alleati: tutta la squadra con le icone (in campo / riserva / caduta) e i punti
    // rimasti, per organizzarsi. Avversari: solo le unita' gia' abbattute, i punti che hanno speso e quelli
    // che potrebbero avere ancora (cap della coda - punti abbattuti): il resto della squadra e' un'incognita.
    void GameLayer::RenderVsBotScoreboard() {
        if (m_VsBotPlayers.empty()) return;
        const VsBotQueueTier& tier = kVsBotTiers[std::clamp(m_VsBotTier, 0, 2)];
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(820, 0));
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::Begin("VsBotScoreboard", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float iconR = 10.0f, iconStep = 28.0f, rowH = 30.0f;
        const int secs = std::max(0, static_cast<int>(m_VsBotTimeLimit - m_VsBotTime));
        ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.36f, 1.0f), "%s  -  cap squadra %d pti", tier.name, tier.cap);
        ImGui::SameLine(ImGui::GetWindowWidth() - 120.0f); ImGui::Text("%02d:%02d", secs / 60, secs % 60);
        ImGui::Separator();

        auto drawTeam = [&](int team) {
            const bool ally = team == m_VsBotMyTeam;
            int unitsLeft = 0, fallen = 0;
            for (const auto& p : m_VsBotPlayers) if (p.team == team) {
                unitsLeft += p.CountState(VsBotSlotState::InField) + p.CountState(VsBotSlotState::Reserve);
                fallen += p.CountState(VsBotSlotState::Fallen);
            }
            ImGui::Spacing();
            if (ally) ImGui::TextColored(ImVec4(0.36f, 0.80f, 0.74f, 1.0f), "ALLEATI   -   unita' rimaste: %d   cadute: %d", unitsLeft, fallen);
            else ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.17f, 1.0f), "AVVERSARI   -   unita' abbattute: %d", fallen);
            ImGui::Separator();
            for (const auto& p : m_VsBotPlayers) {
                if (p.team != team) continue;
                const ImVec2 row = ImGui::GetCursorScreenPos();
                // Squadre senza limite di unita': le icone vanno a capo (12 per riga) e la riga si allunga
                int toShow = 0;
                for (const auto& s : p.squad) if (ally || s.state == VsBotSlotState::Fallen) ++toShow;
                const int perLine = 12;
                const int lines = std::max(1, (toShow + perLine - 1) / perLine);
                const float h = std::max(rowH, lines * iconStep + 4.0f);
                if (p.human) dl->AddRectFilled(row, ImVec2(row.x + ImGui::GetContentRegionAvail().x, row.y + h), IM_COL32(60, 110, 160, 70), 3.0f);
                const bool out = p.CountState(VsBotSlotState::InField) + p.CountState(VsBotSlotState::Reserve) == 0;
                const ImU32 nameCol = out ? IM_COL32(130, 130, 135, 255) : (ally ? IM_COL32(150, 220, 255, 255) : IM_COL32(255, 150, 135, 255));
                const float cy = row.y + rowH * 0.5f;
                dl->AddText(ImVec2(row.x + 6.0f, cy - 7.0f), nameCol, p.name.c_str());
                float x = row.x + 190.0f;
                int shown = 0;
                for (const auto& s : p.squad) {
                    if (!ally && s.state != VsBotSlotState::Fallen) continue;   // dei nemici si vede solo chi e' caduto
                    const int line = shown / perLine, col = shown % perLine;
                    DrawVsBotUnitIcon(dl, ImVec2(row.x + 190.0f + col * iconStep + iconR, cy + line * iconStep), iconR, s.unit, s.state);
                    ++shown;
                }
                if (!ally && shown == 0) dl->AddText(ImVec2(x, cy - 7.0f), IM_COL32(140, 140, 150, 255), "nessuna unita' abbattuta");
                char info[160];
                if (ally) {
                    const int rem = p.PointsState(VsBotSlotState::InField) + p.PointsState(VsBotSlotState::Reserve);
                    const char* status = p.active >= 0 ? "in campo" : (out ? "eliminato" : "in rientro");
                    std::snprintf(info, sizeof(info), "%s   rimasti %d / %d pti", status, rem, p.TotalPoints());
                }
                else {
                    const int used = p.PointsState(VsBotSlotState::Fallen);
                    std::snprintf(info, sizeof(info), "usati %d pti   rimanenti fino a %d pti", used, std::max(0, tier.cap - used));
                }
                dl->AddText(ImVec2(row.x + 540.0f, cy - 7.0f), IM_COL32(220, 220, 225, 255), info);
                ImGui::Dummy(ImVec2(0.0f, h));
            }
        };
        drawTeam(m_VsBotMyTeam);
        ImGui::Spacing();
        drawTeam(1 - m_VsBotMyTeam);
        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextDisabled("Icone: tondo = tiratore, rombo = mischia, quadrato = versatile.  Piena = in campo, sbiadita = riserva, X = caduta.");
        ImGui::End();
    }

    void GameLayer::RenderVsBotResults() {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(620, 0));
        ImGui::Begin("FINE PARTITA", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        const char* title = m_VsBotResult == 1 ? "VITTORIA" : (m_VsBotResult == 2 ? "SCONFITTA" : "PAREGGIO");
        const ImVec4 col = m_VsBotResult == 1 ? ImVec4(0.50f, 0.74f, 0.44f, 1.0f) : (m_VsBotResult == 2 ? ImVec4(0.90f, 0.30f, 0.17f, 1.0f) : ImVec4(0.95f, 0.77f, 0.36f, 1.0f));
        ImGui::TextColored(col, "%s", title);
        ImGui::Text("%s", m_VsBotEndReason.c_str());
        ImGui::Separator(); ImGui::Spacing();
        const int secs = static_cast<int>(m_VsBotTime);
        ImGui::Text("Coda: %s", kVsBotTiers[std::clamp(m_VsBotTier, 0, 2)].name);
        ImGui::Text("Durata: %02d:%02d", secs / 60, secs % 60);
        ImGui::Text("Avversari eliminati dalla squadra: %d", m_VsBotEnemyKills);
        ImGui::Spacing();
        RenderMatchRecap();
        ImGui::Spacing();
        const bool back = Ui::PlainButton("Torna al garage", ImVec2(-1, 44));
        ImGui::End();
        if (back) LeaveVsBotMatch();
    }

} // namespace SunEngine