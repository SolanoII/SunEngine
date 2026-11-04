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

    static void InitOrkAudio(ma_engine* engine) {
        std::string audioDir = FindValidDir("Audio");

        if (audioDir.empty()) {
            audioDir = "GameData/Audio/";
        }

        std::string startPath = audioDir + "i-say-waaagh.wav";
        if (ma_sound_init_from_file(engine, startPath.c_str(), MA_SOUND_FLAG_DECODE, NULL, NULL, &g_OrkVoices.startWaaaghSound) == MA_SUCCESS) {
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

                if (ma_sound_init_from_file(engine, file.c_str(), MA_SOUND_FLAG_DECODE, NULL, NULL, &item->sound) == MA_SUCCESS) {
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

    static void SpawnOrkBoy(Scene& scene, const glm::vec3& spawnPos) {
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
        enemyColor.Color = glm::vec4(0.15f, 0.60f, 0.15f, 1.0f);
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
        else if (newUnitId == "spacemarines_battle_brother") unitColor = glm::vec4(0.05f, 0.25f, 0.75f, 1.0f);
        else if (newUnitId == "orks_boy") unitColor = glm::vec4(0.15f, 0.60f, 0.15f, 1.0f);
        else if (newUnitId == "orks_nob") unitColor = glm::vec4(0.08f, 0.40f, 0.08f, 1.0f);

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
    bool GameLayer::ReloadMap(const std::string& mapId) {
        m_NavQuery.Destroy();

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
                        ammo->feedback = "Il tuo squig e' stato ucciso";
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
                countsAsKill = team->team != 0;
                if (countsAsKill) ++m_VsBotEnemyKills;
            }

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
                AwardKill();
            }
            else {
                // Nessuno scheletro -> KillUnit non puo' creare il ragdoll e l'unita' resterebbe
                // "morta ma viva" (rientrerebbe in questa lista ogni frame). La rimuovo.
                m_PhysicsSystem.DestroyJoltBody(m_Scene, entity);
                m_Scene.DestroyEntity(entity);
                AwardKill();
            }
        }
    }

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
            m_SkinnedModelLoaded = m_SkinnedTestModel.LoadModel(fbxPath);
            if (m_SkinnedModelLoaded) {
                std::string animsDir = FindValidDir("Animations");
                if (!animsDir.empty()) {
                    m_RifleAnimLoaded = m_RifleAnimController.LoadClips(animsDir + "Human/", fbxPath, &m_SkinnedTestModel);
                }
            }

            // Corpi animati delle altre unita' (stesso rig del guardsman)
            LoadAnimatedBody(Body(AnimatedBodyKind::Ork), modelsDir, "ork_master.fbx", "Orks", "ork");
            LoadAnimatedBody(Body(AnimatedBodyKind::Ogryn), modelsDir, "ogryn_master.fbx", "Ogryn", "ogryn");
            LoadAnimatedBody(Body(AnimatedBodyKind::Neophyte), modelsDir, "neophyte_master.fbx", "Neophyte", "neofita");
            LoadAnimatedBody(Body(AnimatedBodyKind::Astartes), modelsDir, "astartes_master.fbx", "Astartes", "astartes");
            LoadAnimatedBody(Body(AnimatedBodyKind::Gretchin), modelsDir, "gretchin_master.fbx", "Gretchin", "gretchin");
        }

        GLFWwindow* window = glfwGetCurrentContext();
        if (window) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }

        if (ImGui::GetCurrentContext() == nullptr) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::StyleColorsDark();

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

            if (ma_sound_group_init(&m_AudioEngine, 0, NULL, &m_SFXGroup) == MA_SUCCESS) {
                m_SFXGroupInitialized = true;
                ma_sound_group_set_volume(&m_SFXGroup, m_VolumeSFX);
            }
            InitOrkAudio(&m_AudioEngine);

            std::string audioDir = FindValidDir("Audio");
            m_WeaponSystem.InitAudio(
                &m_AudioEngine,
                m_SFXGroupInitialized ? &m_SFXGroup : nullptr,
                audioDir
            );
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
        m_UseAnimatedPlayer = m_Profile.animatedPlayer;
        if (!m_Profile.selectedMapId.empty()) m_SelectedMapId = m_Profile.selectedMapId;
        m_SelectedEventIndex = std::clamp(m_Profile.selectedEventIndex, 0, 4);
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
        m_Profile.animatedPlayer = m_UseAnimatedPlayer;
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

        if (m_SFXGroupInitialized) {
            ma_sound_group_uninit(&m_SFXGroup);
            m_SFXGroupInitialized = false;
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
        if (unitId == "spacemarines_battle_brother") return glm::vec4(0.05f, 0.25f, 0.75f, 1.0f);
        if (unitId == "orks_boy") return glm::vec4(0.15f, 0.60f, 0.15f, 1.0f);
        if (unitId == "orks_nob") return glm::vec4(0.08f, 0.40f, 0.08f, 1.0f);
        if (unitId == "orks_yoof") return glm::vec4(0.30f, 0.68f, 0.25f, 1.0f);
        if (unitId == "orks_gretchin") return glm::vec4(0.45f, 0.62f, 0.28f, 1.0f);
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

        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "RIEPILOGO DELLA PARTITA");
        ImGui::Separator();
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
                ImGui::TableSetColumnIndex(2); ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "+%lld", static_cast<long long>(totals[c].xp));
            }
            ImGui::EndTable();
        }
        if (m_MatchRawXp <= 0 && m_MatchXpTotal <= 0) ImGui::TextDisabled("Nessuna esperienza in questa partita.");
        ImGui::Separator();
        if (m_MatchXpApplied && static_cast<int>(m_MatchRevealTimer / 0.3f) > shown) {
            ImGui::Text("Esperienza della partita: %lld", static_cast<long long>(m_MatchRawXp));
            if (!m_MatchOutcomeLabel.empty()) {
                const ImVec4 oc = m_MatchOutcomeMult >= 1.0f ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f) : (m_MatchOutcomeMult >= 0.75f ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f) : ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
                ImGui::TextColored(oc, "%s: %d%%", m_MatchOutcomeLabel.c_str(), static_cast<int>(std::lround(m_MatchOutcomeMult * 100.0f)));
            }
            if (m_MatchVip) ImGui::TextColored(ImVec4(0.75f, 0.45f, 1.0f, 1.0f), "VIP: +25%%");
            else ImGui::TextDisabled("VIP: non attivo (+25%% nel negozio)");
        }
        ImGui::TextColored(ImVec4(0.65f, 0.80f, 1.0f, 1.0f), "Punti studio: +%lld", static_cast<long long>(m_MatchXpTotal));
        ImGui::SameLine(0.0f, 30.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Energia: +%lld", static_cast<long long>(m_MatchEnergy));
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
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s: %lld PS  (+%lld)", ubp ? ubp->displayName.c_str() : unitId.c_str(),
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
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, now >= n.researchCost ? ImVec4(0.3f, 0.9f, 0.4f, 1.0f) : ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
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
    static const char* kOrderNames[4] = { "Take AIM!", "Take Cover", "MOVE MOVE MOVE!", "Fix Bayonets!" };
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
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + bh), IM_COL32(60, 8, 8, 210), 3.0f);
            const ImU32 col = frac > 0.4f ? IM_COL32(40, 210, 70, 255) : IM_COL32(230, 40, 35, 255);
            if (frac > 0.0f) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w * frac, y + bh), col, 3.0f);
            dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + bh), IM_COL32(0, 0, 0, 200), 3.0f, 0, 1.5f);
            char txt[64];
            std::snprintf(txt, sizeof(txt), "%.0f / %.0f", std::max(0.0f, h->currentHealth), h->maxHealth);
            const ImVec2 ts = ImGui::CalcTextSize(txt);
            dl->AddText(ImVec2(x + (w - ts.x) * 0.5f + 1.0f, y + (bh - ts.y) * 0.5f + 1.0f), IM_COL32(0, 0, 0, 220), txt);
            dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, y + (bh - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), txt);
            y += bh + 5.0f;
        }
        if (const auto* st = reg.try_get<UnitStatsComponent>(m_PlayerEntity)) {
            const float frac = std::clamp(st->stamina / std::max(1.0f, st->maxStamina), 0.0f, 1.0f);
            const float bh = 10.0f;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + bh), IM_COL32(45, 40, 10, 200), 2.0f);
            if (frac > 0.0f) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w * frac, y + bh), IM_COL32(235, 200, 50, 255), 2.0f);
            dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + bh), IM_COL32(0, 0, 0, 200), 2.0f, 0, 1.0f);
            y += bh + 8.0f;
        }

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
        // "Waaagh!": ogni carattere vale 50 punti e si accende di verde da sinistra a destra (anche a meta')
        static const char* kLetters[7] = { "W", "a", "a", "a", "g", "h", "!" };
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
        if (active) std::snprintf(info, sizeof(info), "WAAAGH!  %.0f s", std::ceil(m_WaaaghTimer));
        else if (ready) std::snprintf(info, sizeof(info), "PRONTA: premi [V]");
        else std::snprintf(info, sizeof(info), "%.0f / %.0f", m_WaaaghPoints, kWaaaghMax);
        dl->AddText(ImVec2(lx + 10.0f, y + size * 0.5f - 7.0f), (active || ready) ? on : IM_COL32(200, 200, 200, 220), info);
        if (active) dl->AddText(ImVec2(x, y + size + 2.0f), IM_COL32(170, 240, 170, 230),
            "+15% velocita'  +40% ricarica  +20% colpi in mischia  +15% danni in mischia");
        else if (m_FactionFightIdle > 5.0f && m_WaaaghPoints > 0.0f)
            dl->AddText(ImVec2(x, y + size + 2.0f), IM_COL32(255, 120, 80, 240), "Niente rissa: la WAAAGH! si sta spegnendo!");
    }

    // Animazioni procedurali del modello animato (ProceduralPose.h): accovacciato (se mancano le clip),
    // arrampicata lenta, colpi in mischia e parata, lancio delle granate, oggetti posati a terra (cibo, mine),
    // scatola di munizioni, chiave inglese e costruzione di strutture. Si sommano alla clip in corso.
    void GameLayer::BuildPlayerPose(float deltaTime, RifleAnimationController& controller) {
        m_PlayerPose.Clear();
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(m_PlayerEntity)) return;
        m_PoseTime += deltaTime;

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

        // Accovacciato (solo se non ci sono le clip "crouch idle/walking")
        if (const auto* crouch = reg.try_get<CrouchComponent>(m_PlayerEntity); crouch && !controller.HasCrouchClips())
            PoseLib::Crouch(m_PlayerPose, crouch->amount, thigh, shin);

        // Arrampicata lenta (scavalcamento piu' lungo di 1,2 s)
        const auto* vault = reg.try_get<VaultComponent>(m_PlayerEntity);
        approach(m_PoseClimb, vault && vault->isVaulting && vault->vaultDuration > 1.2f, 6.0f);
        PoseLib::Climb(m_PlayerPose, m_PoseTime, m_PoseClimb);

        // Mischia: fasi dell'attacco dalla macchina a stati della mischia
        if (const auto* melee = reg.try_get<MeleeStateComponent>(m_PlayerEntity)) {
            if (melee->currentPhase == MeleePhase::Stunned) PoseLib::Stunned(m_PlayerPose, 1.0f);
            else if (melee->currentAttack == MeleeAttackType::Parry) {
                const float t = melee->stateTimer;
                PoseLib::Parry(m_PlayerPose, t < 0.08f ? t / 0.08f : (t < 0.28f ? 1.0f : std::max(0.0f, (0.48f - t) / 0.2f)));
            }
            else if (melee->currentPhase == MeleePhase::Windup || melee->currentPhase == MeleePhase::Active || melee->currentPhase == MeleePhase::Recovery) {
                const auto* wbp = DamageResolver::ResolveMeleeWeapon(reg, m_PlayerEntity);
                MeleeTiming tm;
                if (wbp && wbp->hasMoveset) {
                    auto it = wbp->moveset.nodes.find(melee->currentAttackNode);
                    if (it != wbp->moveset.nodes.end()) tm = it->second.timings;
                }
                const int phase = melee->currentPhase == MeleePhase::Windup ? 0 : (melee->currentPhase == MeleePhase::Active ? 1 : 2);
                const float dur = phase == 0 ? tm.windup : (phase == 1 ? tm.active : tm.recovery);
                const int type = melee->currentAttack == MeleeAttackType::Light ? 0 : (melee->currentAttack == MeleeAttackType::Heavy ? 2 : 1);
                PoseLib::Melee(m_PlayerPose, type, phase, melee->stateTimer / std::max(0.01f, dur),
                               wbp && wbp->hand_type == HandType::TwoHanded);
            }
        }

        // Dotazione: stato rilevato da ActionPose (lo stesso usato per gli scheletri a blocchi dei bot)
        const auto* ap = reg.try_get<ActionPoseComponent>(m_PlayerEntity);
        if (ap && m_GameState == GameState::Gameplay) {   // nel garage lo stato delle azioni non si aggiorna
            PoseLib::Give(m_PlayerPose, m_PoseTime, ap->give);
            PoseLib::Build(m_PlayerPose, m_PoseTime, ap->build, thigh, shin);
            PoseLib::Repair(m_PlayerPose, m_PoseTime, ap->repair);
            PoseLib::SelfHeal(m_PlayerPose, m_PoseTime, ap->medkit);
            PoseLib::VoxCall(m_PlayerPose, ap->vox);
            PoseLib::Gunner(m_PlayerPose, ap->gunner, thigh, shin);
            if (ap->action == 1) PoseLib::Throw(m_PlayerPose, ap->time / ap->duration);
            else if (ap->action == 2) PoseLib::UseItem(m_PlayerPose, ap->time / ap->duration, thigh, shin);
        }
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
            const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str());
            y -= sz.y;
            const ImVec2 p(right - sz.x, y);
            dl->AddText(font, size, ImVec2(p.x + 2.0f, p.y + 2.0f), IM_COL32(0, 0, 0, 170), t.c_str());
            dl->AddText(font, size, p, col, t.c_str());
            y -= 4.0f;
        };

        const std::string heldId = inv->GetActiveWeaponId();
        const auto* held = BlueprintManager::Get().GetWeapon(heldId);
        const int rangedIdx = Ammo::HeldRangedIndex(*inv);
        const ImU32 white = IM_COL32(235, 235, 235, 255), grey = IM_COL32(160, 160, 160, 255);
        const ImU32 red = IM_COL32(255, 80, 60, 255), yellow = IM_COL32(255, 200, 60, 255);

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
                else if (type == "vox_caster") textRight("Chiamata al vox...", baseSize, yellow);
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
            if (type != "auspex")   // l'auspex e' passivo: niente usi
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
        in.sprinting = Input::IsKeyPressed(GLFW_KEY_LEFT_SHIFT);
        in.aiming = isAiming;

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
        const std::string path = modelsDir + "Characters/" + fbxName;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            SUN_CORE_WARN("Modello {0} non trovato ({1}): usera' il modello umano", label, path);
            return;
        }
        body.modelLoaded = body.model.LoadModel(path);
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
        AnimatedBodyKind kind = AnimatedBodyKind::Human;
        const auto* bp = uid ? BlueprintManager::Get().GetUnit(uid->id) : nullptr;
        if (bp && !bp->animated_body.empty()) {
            // Indicato nel blueprint dell'unita' ("animated_body")
            const std::string& b = bp->animated_body;
            if (b == "ork") kind = AnimatedBodyKind::Ork;
            else if (b == "ogryn") kind = AnimatedBodyKind::Ogryn;
            else if (b == "gretchin") kind = AnimatedBodyKind::Gretchin;
            else if (b == "neophyte") kind = AnimatedBodyKind::Neophyte;
            else if (b == "astartes") kind = AnimatedBodyKind::Astartes;
        }
        else if (uid) {
            // Senza indicazione: dedotto dall'id
            const std::string& id = uid->id;
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
        }

        const bool playerOk = reg.valid(m_PlayerEntity) && reg.all_of<TransformComponent>(m_PlayerEntity);
        const bool wantAnimated = m_UseAnimatedPlayer && modelReady
            && playerOk && !reg.all_of<CorpseComponent>(m_PlayerEntity);

        if (!wantAnimated) {
            // Modalita' cuboni (o giocatore morto: ragdoll a cubi): niente modello animato
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
        vt.Scale = glm::vec3(m_AnimatedModelScale * unitScale);

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
        if ((m_GameState == GameState::Gameplay || m_GameState == GameState::MainMenu) && IsAnimatedPlayerActive()) {
            if (const auto* skin = reg.try_get<SkinComponent>(m_PlayerEntity))
                for (const auto& p : skin->parts) if (!p.bone.empty()) parts.push_back(&p);
        }
        // Strati (vestiti, armature) disegnati sopra il corpo animato con le sue stesse ossa
        if (reg.valid(m_AnimatedPlayerEntity) && reg.all_of<SkinnedModelInstanceComponent>(m_AnimatedPlayerEntity)) {
            auto& inst = reg.get<SkinnedModelInstanceComponent>(m_AnimatedPlayerEntity);
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
        }

    }

    void GameLayer::OnUpdate(float deltaTime)
    {
        m_DeferredRenderer.SetFrameDelta(deltaTime);
        GLFWwindow* window = glfwGetCurrentContext();
        UpdateWindowSize();

        // Autosave del profilo ogni 2 secondi (scrive solo se qualcosa e' cambiato)
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

        // Fuori dalla partita niente squig in giro (garage, editor)
        if (m_GameState != GameState::Gameplay) {
            XpEventLog().clear();   // eventi rimasti fuori partita: non contano
            m_SquigSystem.Clear(m_Scene);
            m_ExplosiveSystem.Clear(m_Scene);
            m_TurretSystem.Clear(m_Scene);
            ClearSummons();
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

        if (m_GameState == GameState::MainMenu) {
            m_GarageCameraAngle += deltaTime * 25.0f;
            if (m_GarageCameraAngle >= 360.0f) m_GarageCameraAngle -= 360.0f;

            glm::vec3 garageTargetPos = transform.Position + glm::vec3(0.0f, 0.9f, 0.0f);

            m_Camera.SetViewportSize(m_WindowWidth, m_WindowHeight);
            m_Camera.SetCameraOffsets(0.0f, 0.2f, m_GarageCameraDistance);
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
                m_VolumeSFX
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

            m_HordeAI.Update(m_Scene, m_NavQuery, deltaTime);
            UpdateSummons(deltaTime);
            if (m_VsBotActive || HasSummons()) m_BotAI.Update(m_Scene, m_NavQuery, deltaTime);
            m_MeleeCombat.Update(m_Scene, deltaTime);
            // Compagni squig (seguono il padrone e attaccano quello a cui spara) e riparazione con la chiave inglese
            if (!m_SquigSkeletonLoaded) {   // un solo tentativo: se manca il file lo squig resta un cubo
                m_SquigSkeleton = SkeletonLoader::LoadFromFile(FindValidDir("Skeletons") + "squig.json");
                m_SquigSkeletonLoaded = true;
            }
            m_SquigSystem.Update(m_Scene, m_NavQuery, deltaTime, m_SquigSkeleton ? &*m_SquigSkeleton : nullptr);
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

            glm::vec3 lookTarget = transform.Position + glm::vec3(0.0f, unitHeight * 0.65f, 0.0f);
            float totalPitch = transform.AimPitch + recoilPitch;
            float totalYaw = transform.AimYaw + recoilYaw;

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
            m_Camera.SetYawOffset(recoilYaw);
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
                g_WaaaghTimer += deltaTime;

                if (g_WaaaghTimer >= 10.0f) {
                    g_WaaaghTimer = 0.0f;

                    auto livingEnemyView = m_Scene.GetRegistry().view<HealthComponent>(
                        entt::exclude<PlayerTag, CorpseComponent, TrainingDummyComponent, SquigComponent>
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

                            glm::vec3 spawnPos = transform.Position + glm::vec3(std::cos(angle) * distance, 0.0f, std::sin(angle) * distance);
                            SpawnOrkBoy(m_Scene, spawnPos);
                        }
                    }

                    g_WaaaghSpawnCount = std::min(g_WaaaghSpawnCount * 2, 12);
                }

                UpdateOrkVoices(m_Scene, m_PlayerEntity, deltaTime, m_VolumeVoice);
            }

            AnimateUnits(deltaTime, transform.Position, true);
            ActionPose::ApplyCubes(m_Scene, m_PlayerEntity);   // pose delle azioni sugli scheletri a blocchi (bot e hitbox)
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
        // FIX #16: le particelle non avanzano in pausa
        if (!m_IsPaused) {
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
        SyncCloakVisuals();
        if (!m_IsPaused) UpdateFactionBuff(deltaTime);   // WAAAGH! (anche mentre sei a terra: il contatore resta)
        if (m_VsBotActive) {
            UpdateBotVisuals();
            UpdateVsBot(deltaTime);
        }
        else if (m_GameState == GameState::Gameplay && HasSummons()) UpdateBotVisuals();   // rinforzi del vox in modalita' orda

        auto orkWeaponView = m_Scene.GetRegistry().view<HordeAgentComponent, TransformComponent, SkeletonComponent>();
        for (auto orkEnt : orkWeaponView) {
            auto& horde = orkWeaponView.get<HordeAgentComponent>(orkEnt);
            auto& orkTrans = orkWeaponView.get<TransformComponent>(orkEnt);
            auto& skel = orkWeaponView.get<SkeletonComponent>(orkEnt);

            if (m_Scene.GetRegistry().valid(horde.weaponEntity)) {
                auto& wTrans = m_Scene.GetRegistry().get<TransformComponent>(horde.weaponEntity);
                const auto* wBP = BlueprintManager::Get().GetWeapon("ork_choppa_1h");

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
            m_DeferredRenderer.SetSunDirection(glm::vec3(-0.4f, -1.0f, 0.3f));
            m_DeferredRenderer.GetShadowMap().UpdateCascades(m_Camera, glm::vec3(-0.4f, -1.0f, 0.3f), 60.0f);

            // ===== SHADOW PASS =====
            const glm::vec3 sunDir = glm::normalize(glm::vec3(-0.4f, -1.0f, 0.3f));
            m_DeferredRenderer.SetSunDirection(sunDir);

            auto& shadowMap = m_DeferredRenderer.GetShadowMap();
            shadowMap.UpdateCascades(m_Camera, sunDir, 60.0f);

            for (int c = 0; c < ShadowMap::CASCADE_COUNT; ++c) {
                shadowMap.BeginCascade(c);

                // Disegna TUTTI i cubi della scena (per ora solo i cubi, in futuro le mesh)
                auto view = m_Scene.GetRegistry().view<TransformComponent, MeshComponent, ColorComponent>();
                for (auto entity : view) {
                    // Il giocatore col modello animato non proietta l'ombra del suo cubo hitbox
                    if (m_Scene.GetRegistry().all_of<HideProceduralVisualTag>(entity)) continue;
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

                shadowMap.EndCascade();
            }

            shadowMap.UploadMatrices();
            // ===== FINE SHADOW PASS =====
            m_DeferredRenderer.BeginGeometryPass(m_Camera);
            m_RenderSystem.RenderDeferred(m_Scene, m_DeferredRenderer);
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
        m_ParticleSystem.Render(m_Scene);
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

                    glm::vec4 boxColor = isActive ? glm::vec4(0.0f, 0.8f, 1.0f, 0.9f) :
                        hasWeapon ? glm::vec4(0.25f, 0.25f, 0.35f, 0.8f) :
                        glm::vec4(0.12f, 0.12f, 0.12f, 0.5f);

                    Renderer::DrawQuadUI(pos, glm::vec2(slotSize, slotSize), boxColor, screen);

                    if (isActive) {
                        Renderer::DrawQuadUI(pos + glm::vec2(3.0f, 3.0f), glm::vec2(slotSize - 6.0f, 4.0f), glm::vec4(1.0f, 0.9f, 0.0f, 1.0f), screen);
                    }
                }
            }

            float dotSize = 4.0f;
            glm::vec2 dotPos = center - glm::vec2(dotSize * 0.5f);
            Renderer::DrawQuadUI(dotPos, glm::vec2(dotSize, dotSize), glm::vec4(1.0f, 1.0f, 1.0f, 0.9f), screen);

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

        OnImGuiRender();
    }

    void GameLayer::OnImGuiRender()
    {
        GLFWwindow* window = glfwGetCurrentContext();
        if (!window) return;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (m_GameState == GameState::MainMenu) {
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
                RenderTrainingDummyLabels();
                RenderHitMarkers();
                RenderAmmoHUD();
                RenderPlayerStatusHUD();
                RenderOrdersRadial();
                RenderAuspex();
                RenderTurretHUD();
                if (m_VsBotActive) RenderVsBotHUD();
                else {
                    // Uccisioni dell'orda in alto a destra (in alto a sinistra ci sono salute, stamina e buff)
                    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 20.0f, 20.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
                    ImGui::Begin("KillCounterOverlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
                    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "ORKI ELIMINATI: %d", m_KillCount);
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
                ImGui::SetNextWindowSize(ImVec2(380, 420));

                ImGui::Begin("MENU DI PAUSA", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
                ImGui::Text("--- SUN ENGINE ---");
                ImGui::Separator(); ImGui::Spacing();

                if (ImGui::Button("Riprendi Gioco", ImVec2(-1, 40))) {
                    m_IsPaused = false;
                    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                }

                ImGui::Spacing();

                if (ImGui::Button("Opzioni", ImVec2(-1, 40))) {
                    m_ShowOptionsMenu = true;
                }

                ImGui::Spacing();

                bool backToMenu = ImGui::Button("Torna al Menu Principale", ImVec2(-1, 40));
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

                if (ImGui::Button("Esci dal Gioco", ImVec2(-1, 40))) {
                    // FIX #15: uscita pulita. Application::Run controlla glfwWindowShouldClose, poi
                    // vengono eseguiti i distruttori (ImGui/audio/GL). std::exit(0) li saltava tutti.
                    if (window) glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
                ImGui::End();

                RenderOptionsMenu();
            }
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    void GameLayer::RenderTopNavBar() {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, 60));
        ImGui::Begin("TopBar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        ImGui::SetCursorPos(ImVec2(20, 15));
        ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "PROJECT TRAZYN");

        ImGui::SetCursorPos(ImVec2(std::max(190.0f, io.DisplaySize.x * 0.5f - 345.0f), 10));

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);

        // Pulsante di navigazione: evidenziato se attivo. Ritorna true al click.
        auto navButton = [&](const char* label, float width, bool active, const ImVec4& activeColor) {
            ImGui::PushStyleColor(ImGuiCol_Button, active ? activeColor : ImVec4(0.1f, 0.1f, 0.15f, 1.0f));
            bool clicked = ImGui::Button(label, ImVec2(width, 40));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            return clicked;
        };
        const ImVec4 activeBlue(0.2f, 0.5f, 0.8f, 1.0f);
        const bool inMenu = (m_GameState == GameState::MainMenu);

        if (navButton("GARAGE", 120, inMenu && m_CurrentTab == MenuTab::StasisVault, activeBlue)) {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::StasisVault;
            if (!m_PlayerRoster.empty() && m_SelectedVaultIndex < static_cast<int>(m_PlayerRoster.size())) {
                ApplyUnitVisuals(m_PlayerRoster[m_SelectedVaultIndex]);
            }
        }

        if (navButton("SCHIERAMENTO", 150, inMenu && m_CurrentTab == MenuTab::Deployment, activeBlue)) {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::Deployment;
            if (!m_SelectedSquadIndices.empty() && m_SelectedSquadIndices[0] < static_cast<int>(m_PlayerRoster.size())) {
                ApplyUnitVisuals(m_PlayerRoster[m_SelectedSquadIndices[0]]);
            }
        }

        if (navButton("RICERCA", 100, inMenu && m_CurrentTab == MenuTab::Research, activeBlue)) {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::Research;
        }

        if (navButton("NEGOZIO", 100, inMenu && m_CurrentTab == MenuTab::Shop, ImVec4(0.55f, 0.30f, 0.80f, 1.0f))) {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::Shop;
        }

        if (navButton("EDITOR", 90, m_GameState == GameState::MapEditor, ImVec4(0.8f, 0.4f, 0.0f, 1.0f))) {
            m_GameState = GameState::MapEditor;
            if (m_EditorMap.structures.empty()) {
                m_EditorMap.playerSpawns.push_back(glm::vec3(0.0f, 0.5f, 10.0f));
                m_EditorMap.enemySpawns.push_back(glm::vec3(0.0f, 0.5f, -10.0f));
            }
            SyncEditorEntitiesWithScene();
        }

        if (navButton("OPZIONI", 90, false, activeBlue)) {
            m_ShowOptionsMenu = true;
        }

        // Valute del profilo, allineate a destra
        {
            char energyBuf[48], stoneBuf[48];
            std::snprintf(energyBuf, sizeof(energyBuf), "ENERGIA: %lld", static_cast<long long>(m_Profile.energy));
            std::snprintf(stoneBuf, sizeof(stoneBuf), "PIETRA NERA: %lld", static_cast<long long>(m_Profile.blackStone));
            const float spacing = 24.0f;
            const float totalW = ImGui::CalcTextSize(energyBuf).x + ImGui::CalcTextSize(stoneBuf).x + spacing;
            ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - totalW - 20.0f, 19.0f));
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s", energyBuf);
            ImGui::SameLine(0.0f, spacing);
            ImGui::TextColored(ImVec4(0.7f, 0.4f, 1.0f, 1.0f), "%s", stoneBuf);
        }

        ImGui::PopStyleVar();
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
            m_PlayerRoster.push_back(TrainedUnit{ "Snik", "orks_gretchin", glm::vec4(0.45f, 0.62f, 0.28f, 1.0f) });
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
        ImGui::SetNextWindowPos(ImVec2(20.0f, 70.0f));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - 40.0f, io.DisplaySize.y - 90.0f));
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::Begin("RICERCA", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

        if (!m_ResearchTree.IsLoaded()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Albero di ricerca non caricato.");
            ImGui::TextDisabled("Manca GameData/Research/research_tree.json (controlla anche la copia nella cartella di build).");
            ImGui::End();
            return;
        }

        if (m_ResearchMessageTimer > 0.0f) m_ResearchMessageTimer -= io.DeltaTime;

        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "ENERGIA: %lld", static_cast<long long>(m_Profile.energy));
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextDisabled("I punti studio si guadagnano giocando con un'unita' e si spendono per sbloccare le unita' successive.");
        ImGui::Spacing();

        if (ImGui::BeginTabBar("##ResearchFactions")) {
            for (const auto& faction : m_ResearchTree.GetFactions()) {
                if (ImGui::BeginTabItem(faction.name.c_str())) {
                    DrawResearchFactionTree(faction);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    void GameLayer::DrawResearchFactionTree(const ResearchFaction& faction) {
        ImGuiIO& io = ImGui::GetIO();
        const float nodeW = 210.0f, nodeH = 88.0f, gapX = 90.0f, gapY = 26.0f, pad = 24.0f;

        // Nell'albero solo i personaggi; gli oggetti di ciascuno si vedono selezionandolo (pannello sotto)
        float maxCol = 0.0f, maxRow = 0.0f;
        for (const auto& n : faction.nodes) { if (n.unitId.empty()) continue; maxCol = std::max(maxCol, n.col); maxRow = std::max(maxRow, n.row); }
        const float totalW = pad * 2.0f + (maxCol + 1.0f) * nodeW + maxCol * gapX;
        const float totalH = pad * 2.0f + (maxRow + 1.0f) * nodeH + maxRow * gapY;

        auto col32 = [](float r, float g, float b, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(r, g, b, a)); };

        // Selezione valida solo se appartiene a questa fazione
        const ResearchNode* selected = nullptr;
        for (const auto& n : faction.nodes) if (n.id == m_ResearchSelectedNode) selected = &n;
        if (!selected && !faction.nodes.empty()) selected = &faction.nodes.front();
        // Personaggio evidenziato nell'albero: quello selezionato, o il padrone dell'oggetto selezionato
        const ResearchNode* selectedUnit = selected;
        if (selected && selected->unitId.empty()) selectedUnit = m_ResearchTree.FindStudySource(*selected);

        // Ricerche che si pagano coi punti studio di un personaggio (oggetti e personaggi successivi)
        auto researchesOf = [&](const ResearchNode& unitNode) { return m_ResearchTree.ResearchesOf(unitNode); };
        // Maestria: tutte le ricerche legate al personaggio completate (stellina dorata)
        auto isMastered = [&](const ResearchNode& unitNode) { return m_ResearchTree.IsMastered(m_Profile, unitNode); };
        auto drawStar = [](ImDrawList* d, ImVec2 c, float r, ImU32 fill, ImU32 edge) {
            ImVec2 pts[10];
            for (int i = 0; i < 10; ++i) {
                const float ang = -1.5707963f + i * 0.6283185f;
                const float rr = (i % 2) ? r * 0.45f : r;
                pts[i] = ImVec2(c.x + std::cos(ang) * rr, c.y + std::sin(ang) * rr);
            }
            for (int i = 0; i < 10; ++i) d->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], fill);
            for (int i = 0; i < 10; ++i) d->AddLine(pts[i], pts[(i + 1) % 10], edge, 1.2f);
        };

        // ---------- Albero (da sinistra a destra) ----------
        ImGui::BeginChild(("##tree_" + faction.id).c_str(), ImVec2(0, io.DisplaySize.y * 0.48f), true,
            ImGuiWindowFlags_HorizontalScrollbar);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        auto nodePos = [&](const ResearchNode& n) {
            return ImVec2(origin.x + pad + n.col * (nodeW + gapX), origin.y + pad + n.row * (nodeH + gapY));
        };

        // Connettori padre -> figlio (linee a gomito)
        for (const auto& n : faction.nodes) {
            if (n.unitId.empty()) continue;   // oggetti: non nell'albero
            const ResearchNode* parent = m_ResearchTree.FindParent(n);
            if (!parent) continue;
            const ImVec2 pp = nodePos(*parent), cp = nodePos(n);
            const ImVec2 a(pp.x + nodeW, pp.y + nodeH * 0.5f), b(cp.x, cp.y + nodeH * 0.5f);
            const float midX = (a.x + b.x) * 0.5f;
            const bool unlocked = m_ResearchTree.IsResearched(m_Profile, n);
            const ImU32 lc = unlocked ? col32(faction.accent[0], faction.accent[1], faction.accent[2]) : col32(0.35f, 0.35f, 0.40f);
            dl->AddLine(a, ImVec2(midX, a.y), lc, 2.0f);
            dl->AddLine(ImVec2(midX, a.y), ImVec2(midX, b.y), lc, 2.0f);
            dl->AddLine(ImVec2(midX, b.y), b, lc, 2.0f);
        }
        // Padri alternativi (ne basta uno): stesso connettore a gomito anche dagli altri padri
        for (const auto& n : faction.nodes) {
            if (n.unitId.empty() || n.anyParents.empty()) continue;
            for (const ResearchNode* par : m_ResearchTree.Parents(n)) {
                if (par->id == n.parentId) continue;   // il primo e' gia' disegnato sopra
                const ImVec2 pp = nodePos(*par), cp = nodePos(n);
                const ImVec2 a(pp.x + nodeW, pp.y + nodeH * 0.5f), b(cp.x, cp.y + nodeH * 0.5f);
                const float midX = (a.x + b.x) * 0.5f;
                const bool unlocked = m_ResearchTree.IsResearched(m_Profile, n);
                const ImU32 lc = unlocked ? col32(faction.accent[0], faction.accent[1], faction.accent[2]) : col32(0.35f, 0.35f, 0.40f);
                dl->AddLine(a, ImVec2(midX, a.y), lc, 2.0f);
                dl->AddLine(ImVec2(midX, a.y), ImVec2(midX, b.y), lc, 2.0f);
                dl->AddLine(ImVec2(midX, b.y), b, lc, 2.0f);
            }
        }
        // Rami che si riuniscono: dall'unita' da padroneggiare scende (o sale) una linea dorata fino alla riga del
        // nodo e poi entra da sinistra. Dorata piena = stellina ottenuta.
        for (const auto& n : faction.nodes) {
            if (n.unitId.empty()) continue;
            for (const auto& reqId : n.requiresMastered) {
                if (reqId == n.parentId) continue;   // gia' collegato dal ramo normale
                const ResearchNode* req = m_ResearchTree.FindNode(reqId);
                if (!req) continue;
                const ImVec2 rp = nodePos(*req), cp = nodePos(n);
                const bool down = cp.y > rp.y;
                const ImVec2 a(rp.x + nodeW * 0.5f, down ? rp.y + nodeH : rp.y);
                const ImVec2 b(cp.x, cp.y + nodeH * 0.5f);
                const ImU32 lc = isMastered(*req) ? col32(1.0f, 0.82f, 0.15f) : col32(0.55f, 0.45f, 0.20f);
                dl->AddLine(a, ImVec2(a.x, b.y), lc, 2.0f);
                dl->AddLine(ImVec2(a.x, b.y), b, lc, 2.0f);
                drawStar(dl, ImVec2(a.x, (a.y + b.y) * 0.5f), 7.0f, lc, col32(0.3f, 0.2f, 0.0f));
            }
        }

        // Nodi (solo personaggi)
        for (const auto& n : faction.nodes) {
            if (n.unitId.empty()) continue;
            const ImVec2 p = nodePos(n);
            const ImVec2 p2(p.x + nodeW, p.y + nodeH);
            const ResearchStatus st = m_ResearchTree.GetStatus(m_Profile, n);
            const bool isItem = !n.itemId.empty();
            const bool hasBp = isItem ? (BlueprintManager::Get().GetWeapon(n.itemId) != nullptr || BlueprintManager::Get().GetArmorPiece(n.itemId) != nullptr)
                                      : BlueprintManager::Get().GetUnit(n.unitId) != nullptr;
            int owned = 0;
            for (const auto& u : m_PlayerRoster) owned += isItem ? (u.OwnsItem(n.itemId) ? 1 : 0) : (u.blueprintId == n.unitId ? 1 : 0);

            ImVec4 border(0.40f, 0.40f, 0.45f, 1.0f);                       // bloccato
            if (st == ResearchStatus::Researched) border = ImVec4(0.30f, 0.85f, 0.40f, 1.0f);
            else if (st == ResearchStatus::Researchable) border = ImVec4(1.0f, 0.80f, 0.20f, 1.0f);

            ImGui::SetCursorScreenPos(p);
            ImGui::InvisibleButton(("##node_" + n.id).c_str(), ImVec2(nodeW, nodeH));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) m_ResearchSelectedNode = n.id;
            const bool isSel = (selectedUnit && selectedUnit->id == n.id);

            dl->AddRectFilled(p, p2, col32(0.10f, 0.11f, 0.14f), 6.0f);
            dl->AddRectFilled(p, ImVec2(p.x + 7.0f, p2.y), col32(n.color[0], n.color[1], n.color[2]), 6.0f);   // banda colore unita'
            dl->AddRect(p, p2, ImGui::GetColorU32(border), 6.0f, 0, isSel ? 3.0f : (hovered ? 2.5f : 1.5f));

            dl->PushClipRect(p, p2, true);
            const float tx = p.x + 16.0f;
            dl->AddText(ImVec2(tx, p.y + 8.0f), col32(0.95f, 0.95f, 0.95f), n.name.c_str());
            char line2[96];
            if (isItem) {
                const ResearchNode* src = m_ResearchTree.FindStudySource(n);
                std::snprintf(line2, sizeof(line2), "OGGETTO   PS %s: %lld", src ? src->name.c_str() : "-",
                    static_cast<long long>(src ? ResearchTree::GetStudyPoints(m_Profile, src->unitId) : 0));
            }
            else {
                int itemsTotal = 0, itemsDone = 0;
                for (const auto* r : researchesOf(n)) if (!r->itemId.empty()) { ++itemsTotal; itemsDone += m_ResearchTree.IsResearched(m_Profile, *r) ? 1 : 0; }
                if (itemsTotal > 0)
                    std::snprintf(line2, sizeof(line2), "T%d   PS: %lld   oggetti %d/%d", n.tier, static_cast<long long>(ResearchTree::GetStudyPoints(m_Profile, n.unitId)), itemsDone, itemsTotal);
                else std::snprintf(line2, sizeof(line2), "T%d   PS: %lld", n.tier, static_cast<long long>(ResearchTree::GetStudyPoints(m_Profile, n.unitId)));
            }
            dl->AddText(ImVec2(tx, p.y + 30.0f), col32(0.65f, 0.80f, 1.0f), line2);
            char line3[96];
            if (!hasBp && isItem) std::snprintf(line3, sizeof(line3), "BLUEPRINT MANCANTE");
            else if (!hasBp && st == ResearchStatus::Researched) std::snprintf(line3, sizeof(line3), "IN ARRIVO   (sbloccata)");
            else if (!hasBp && st == ResearchStatus::Researchable) std::snprintf(line3, sizeof(line3), "IN ARRIVO   ricerca: %lld PS", static_cast<long long>(n.researchCost));
            else if (st == ResearchStatus::LockedMastery) std::snprintf(line3, sizeof(line3), "SERVE MAESTRIA");
            else if (!hasBp && st == ResearchStatus::LockedPoints) std::snprintf(line3, sizeof(line3), "IN ARRIVO   servono %lld PS", static_cast<long long>(n.researchCost));
            else if (!hasBp) std::snprintf(line3, sizeof(line3), "IN ARRIVO");
            else if (st == ResearchStatus::Researched && isItem) std::snprintf(line3, sizeof(line3), "SBLOCCATO   comprato da: %d", owned);
            else if (st == ResearchStatus::Researched) std::snprintf(line3, sizeof(line3), "SBLOCCATA   in garage: %d", owned);
            else if (st == ResearchStatus::Researchable) std::snprintf(line3, sizeof(line3), "RICERCA: %lld PS", static_cast<long long>(n.researchCost));
            else if (st == ResearchStatus::LockedPoints) std::snprintf(line3, sizeof(line3), "servono %lld PS", static_cast<long long>(n.researchCost));
            else std::snprintf(line3, sizeof(line3), "BLOCCATA");
            dl->AddText(ImVec2(tx, p.y + 54.0f), ImGui::GetColorU32(border), line3);
            dl->PopClipRect();
            // Stellina dorata: maestria completa del personaggio
            if (!isItem && isMastered(n)) drawStar(dl, ImVec2(p2.x - 16.0f, p.y + 16.0f), 11.0f, col32(1.0f, 0.82f, 0.15f), col32(0.55f, 0.38f, 0.0f));
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(totalW, totalH));   // estende l'area scorrevole fino ai nodi piu' lontani
        ImGui::EndChild();

        // ---------- Dettaglio del nodo selezionato ----------
        ImGui::BeginChild(("##detail_" + faction.id).c_str(), ImVec2(0, 0), true);
        if (selected) {
            const ResearchNode& n = *selected;
            const ResearchNode* parent = m_ResearchTree.FindParent(n);
            const ResearchStatus st = m_ResearchTree.GetStatus(m_Profile, n);
            const auto* bp = BlueprintManager::Get().GetUnit(n.unitId);
            const auto* itemBp = BlueprintManager::Get().GetWeapon(n.itemId);
            const bool isItem = !n.itemId.empty();
            const ResearchNode* studySource = m_ResearchTree.FindStudySource(n);

            if (isItem && studySource) {
                if (ImGui::SmallButton(("< " + studySource->name).c_str())) m_ResearchSelectedNode = studySource->id;
                ImGui::SameLine();
            }
            ImGui::TextColored(ImVec4(faction.accent[0], faction.accent[1], faction.accent[2], 1.0f), "%s   (Tier %d)", n.name.c_str(), n.tier);
            ImGui::SameLine(0.0f, 20.0f);
            ImGui::TextDisabled("%s", isItem ? n.itemId.c_str() : n.unitId.c_str());
            if (!isItem && isMastered(n)) {
                ImGui::SameLine(0.0f, 20.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.15f, 1.0f), "MAESTRIA COMPLETA");
            }
            ImGui::Separator();

            ImGui::Columns(2, "##detailcols", false);
            const auto* armorBp = BlueprintManager::Get().GetArmorPiece(n.itemId);
            if (isItem && armorBp) {
                ImGui::Text("Armatura | AR: %d | Durabilita': %.0f | Copertura: %.0f%% | Peso: %.1f kg", armorBp->armorRating,
                    armorBp->maxDurability, armorBp->coverage * 100.0f, armorBp->weight);
                if (armorBp->blocks_headshot_bonus) ImGui::TextWrapped("Finche' regge annulla il bonus del colpo in testa.");
            }
            else if (isItem) {
                if (itemBp) {
                    if (itemBp->is_equipment) ImGui::Text("Dotazione tattica | Usi: %d | Peso: %.1f kg", itemBp->equipment_uses, itemBp->weight_kg);
                    else if (itemBp->is_melee) ImGui::Text("Arma da mischia | Danno: %.0f | Peso: %.1f kg", itemBp->base_damage, itemBp->weight_kg);
                    else ImGui::Text("Arma da fuoco | Danno: %.0f x%d | Colpi: %d + %d caricatori | Ricarica: %.1f s", itemBp->base_damage,
                        std::max(1, itemBp->pellet_count), itemBp->magazine_size, itemBp->spare_magazines, itemBp->reload_time_sec);
                    if (!itemBp->allowed_units.empty()) {
                        std::string who;
                        for (const auto& u : itemBp->allowed_units) {
                            const auto* ub = BlueprintManager::Get().GetUnit(u);
                            who += (who.empty() ? "" : ", ") + (ub ? ub->displayName : u);
                        }
                        ImGui::TextWrapped("Per: %s", who.c_str());
                    }
                }
                else ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Blueprint '%s' non trovato in GameData/Weapons.", n.itemId.c_str());
            }
            else if (bp) {
                ImGui::Text("Salute: %.0f   Velocita': %.1f   Armatura: %d", bp->health, bp->base_speed, bp->armor_rating);
                ImGui::Text("Forza: %.0f   Mischia: %.0f   Mira: %.0f", bp->strength, bp->melee_skill, bp->accuracy_skill);
                ImGui::Text("Costo in punti squadra: %d (+ equipaggiamento)", bp->point_cost);
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "UNITA' IN ARRIVO");
                ImGui::TextWrapped("Non ancora giocabile ('%s' non e' in GameData/Units): si puo' gia' ricercare per proseguire "
                                   "nel ramo, ma non ancora acquistare.", n.unitId.c_str());
            }
            if (!isItem) ImGui::Text("Punti studio di questa unita': %lld", static_cast<long long>(ResearchTree::GetStudyPoints(m_Profile, n.unitId)));
            ImGui::NextColumn();

            auto actionButton = [&](const char* label, bool enabled) {
                if (!enabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.40f);
                const bool clicked = ImGui::Button(label, ImVec2(320, 36));
                if (!enabled) ImGui::PopStyleVar();
                return clicked && enabled;
            };
            auto setMessage = [&](const std::string& msg) { m_ResearchMessage = msg; m_ResearchMessageTimer = 4.0f; };

            if (st != ResearchStatus::Researched) {
                // Fase 1: ricerca con i punti studio del PADRE
                if (const ResearchNode* payer = m_ResearchTree.PaySource(m_Profile, n)) {
                    ImGui::Text("Punti studio di %s: %lld / %lld", payer->name.c_str(),
                        static_cast<long long>(ResearchTree::GetStudyPoints(m_Profile, payer->unitId)),
                        static_cast<long long>(n.researchCost));
                }
                // Padri alternativi: ne basta uno (sbloccato e, per un personaggio, con la stellina)
                std::string alternatives;
                for (const ResearchNode* par : m_ResearchTree.Parents(n)) alternatives += (alternatives.empty() ? "" : " oppure ") + par->name;
                if (st == ResearchStatus::LockedParent && parent) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Sblocca prima: %s", n.anyParents.empty() ? parent->name.c_str() : alternatives.c_str());
                }
                if (!n.anyParents.empty() && !isItem) {
                    const bool ok = m_ResearchTree.AnyParentReady(m_Profile, n);
                    ImGui::TextColored(ok ? ImVec4(1.0f, 0.82f, 0.15f, 1.0f) : ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s Maestria di %s",
                        ok ? "[*]" : "[ ]", alternatives.c_str());
                }
                // Maestria richiesta: il personaggio padre (e i rami che si riuniscono) completati fino alla stellina
                for (const ResearchNode* req : m_ResearchTree.MasteryRequirements(n)) {
                    const std::string& reqId = req->id;
                    const bool ok = isMastered(*req);
                    ImGui::TextColored(ok ? ImVec4(1.0f, 0.82f, 0.15f, 1.0f) : ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s Maestria di %s",
                        ok ? "[*]" : "[ ]", req->name.c_str());
                    if (!ok) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(("vai##req_" + reqId).c_str())) m_ResearchSelectedNode = reqId;
                    }
                }
                char label[96];
                std::snprintf(label, sizeof(label), "RICERCA  (-%lld PS)", static_cast<long long>(n.researchCost));
                if (actionButton(label, st == ResearchStatus::Researchable)) {
                    if (m_ResearchTree.Research(m_Profile, n)) setMessage("Ricerca completata: " + n.name);
                }
            }
            else if (isItem) {
                // Oggetto: si compra nel garage, unita' per unita' (Personalizza -> Equipaggiamento)
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Ricercato.");
                ImGui::TextWrapped("Si compra nel garage per ogni unita' che lo deve usare: Personalizza -> Equipaggiamento, "
                                   "scegli l'oggetto e conferma l'acquisto (%lld energia).", static_cast<long long>(n.purchaseCost));
            }
            else {
                // Fase 2: acquisto con energia -> nuova unita' nel garage
                ImGui::Text("Unita' in garage: %d / %d", static_cast<int>(m_PlayerRoster.size()), MAX_ROSTER_SIZE);
                char label[96];
                std::snprintf(label, sizeof(label), "ACQUISTA UNITA  (%lld ENERGIA)", static_cast<long long>(n.purchaseCost));
                const bool roomLeft = static_cast<int>(m_PlayerRoster.size()) < MAX_ROSTER_SIZE;
                const bool canPay = m_Profile.energy >= n.purchaseCost;
                if (actionButton(label, bp != nullptr && roomLeft && canPay)) {
                    if (PurchaseResearchedUnit(n)) setMessage("Aggiunta al garage: " + n.name);
                }
                if (!roomLeft) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Garage pieno.");
                else if (!canPay) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Energia insufficiente.");
            }
            ImGui::Columns(1);

            // Oggetti del personaggio: si ricercano coi suoi punti studio. Click = dettaglio e ricerca.
            if (!isItem) {
                std::vector<const ResearchNode*> items;
                for (const auto* r : researchesOf(n)) if (!r->itemId.empty()) items.push_back(r);
                std::sort(items.begin(), items.end(), [](const ResearchNode* a, const ResearchNode* b) {
                    return a->row != b->row ? a->row < b->row : a->col < b->col; });
                if (!items.empty()) {
                    int done = 0;
                    for (const auto* r : items) done += m_ResearchTree.IsResearched(m_Profile, *r) ? 1 : 0;
                    ImGui::Spacing(); ImGui::Separator();
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "EQUIPAGGIAMENTO DA RICERCARE  (%d / %d)", done, static_cast<int>(items.size()));
                    for (const auto* r : items) {
                        const ResearchStatus rs = m_ResearchTree.GetStatus(m_Profile, *r);
                        int depth = 0;   // oggetti a catena (es. busto -> spallacci -> gambali): rientro
                        for (const ResearchNode* q = m_ResearchTree.FindParent(*r); q && q->unitId.empty(); q = m_ResearchTree.FindParent(*q)) ++depth;
                        char status[96];
                        ImVec4 sc(0.6f, 0.6f, 0.65f, 1.0f);
                        if (rs == ResearchStatus::Researched) { std::snprintf(status, sizeof(status), "RICERCATO"); sc = ImVec4(0.3f, 0.85f, 0.4f, 1.0f); }
                        else if (rs == ResearchStatus::Researchable) { std::snprintf(status, sizeof(status), "RICERCA: %lld PS", static_cast<long long>(r->researchCost)); sc = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); }
                        else if (rs == ResearchStatus::LockedPoints) std::snprintf(status, sizeof(status), "servono %lld PS", static_cast<long long>(r->researchCost));
                        else { const ResearchNode* q = m_ResearchTree.FindParent(*r); std::snprintf(status, sizeof(status), "prima: %s", q ? q->name.c_str() : "-"); }
                        ImGui::PushID(r->id.c_str());
                        ImGui::Indent(12.0f + depth * 18.0f);
                        if (ImGui::Selectable(r->name.c_str(), false, 0, ImVec2(260.0f, 0.0f))) m_ResearchSelectedNode = r->id;
                        ImGui::SameLine(300.0f);
                        ImGui::TextColored(sc, "%s", status);
                        ImGui::Unindent(12.0f + depth * 18.0f);
                        ImGui::PopID();
                    }
                }
            }

            if (m_ResearchMessageTimer > 0.0f && !m_ResearchMessage.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "%s", m_ResearchMessage.c_str());
            }
        }
        ImGui::EndChild();
    }

    // ===================== NEGOZIO =====================
    void GameLayer::RenderShopUI() {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(20.0f, 70.0f));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - 40.0f, io.DisplaySize.y - 90.0f));
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::Begin("NEGOZIO", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

        ImGui::TextColored(ImVec4(0.7f, 0.4f, 1.0f, 1.0f), "PIETRA NERA: %lld", static_cast<long long>(m_Profile.blackStone));
        ImGui::Separator();

        ImGui::Spacing();
        // ---- VIP: +25% di punti studio ed energia a fine partita, per sempre ----
        constexpr int64_t kVipPrice = 1000;
        ImGui::BeginChild("##vip", ImVec2(420, 190), true);
        ImGui::TextColored(ImVec4(0.75f, 0.45f, 1.0f, 1.0f), "VIP");
        ImGui::Separator();
        ImGui::TextWrapped("+25%% di punti studio ed energia alla fine di ogni partita, in tutti i casi "
                           "(anche quando la ricompensa e' ridotta perche' sei caduto o hai perso).");
        ImGui::Spacing();
        if (m_Profile.vip) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "VIP ATTIVO");
        }
        else {
            const bool canBuy = m_Profile.blackStone >= kVipPrice;
            char label[64];
            std::snprintf(label, sizeof(label), "ACQUISTA  (%lld PIETRA NERA)", static_cast<long long>(kVipPrice));
            if (!canBuy) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
            if (ImGui::Button(label, ImVec2(-1, 40)) && canBuy) {
                m_Profile.blackStone -= kVipPrice;
                m_Profile.vip = true;
                SaveProfileIfChanged();
                SUN_CORE_INFO("Negozio: VIP acquistato");
            }
            if (!canBuy) {
                ImGui::PopStyleVar();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Pietra Nera insufficiente.");
            }
        }
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::TextDisabled("Nel negozio si usa solo la valuta premium (Pietra Nera).");

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
            if (isSelected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.4f, 0.7f, 1.0f));
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));

            std::string btnText = m_PlayerRoster[i].customName + "\n(" + m_PlayerRoster[i].blueprintId + ")  EXP " + std::to_string(m_PlayerRoster[i].exp);
            if (ImGui::Button(btnText.c_str(), ImVec2(-1, 50))) {
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
            if (ImGui::Button("+ NUOVA UNITA (RICERCA)", ImVec2(-1, 40))) {
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

            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", activeUnit.customName.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", activeUnit.customName.c_str());
            }

            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Classe: %s", currentBp ? currentBp->displayName.c_str() : "Sconosciuta");
            if (currentBp && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("ID Blueprint: %s", currentBp->displayName.c_str());
            }

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "STATISTICHE COMBATTIMENTO");
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

            ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "DOTAZIONE ATTUALE");

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
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.7f, 0.3f, 1.0f));
            if (ImGui::Button("MODIFICA ED EQUIPAGGIA", ImVec2(-1, 60))) {
                m_ShowCustomizeModal = true;
                m_CustomizeMainTab = 0;
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
                ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "FASE 1: SELEZIONA LA FAZIONE");
                ImGui::Separator(); ImGui::Spacing();

                if (ImGui::Button("IMPERIUM OF MAN", ImVec2(280, 45))) {
                    m_SelectedFaction = "Imperium";
                    m_RecruitStep = 1;
                }
                if (ImGui::Button("SPACE MARINES", ImVec2(280, 45))) {
                    m_SelectedFaction = "SpaceMarines";
                    m_RecruitStep = 1;
                }
                if (ImGui::Button("ORKS", ImVec2(280, 45))) {
                    m_SelectedFaction = "Orks";
                    m_RecruitStep = 1;
                }
            }
            else if (m_RecruitStep == 1) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "FASE 2: SELEZIONA TRUPPA (%s)", m_SelectedFaction.c_str());
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
                    if (ImGui::Button("Recluta della Guardia (10 Pti)\n(Guardsman Recruit)", ImVec2(280, 50))) {
                        recruitAction("imperium_guardsman_recruit", "Recluta Vance", glm::vec4(0.65f, 0.50f, 0.35f, 1.0f));
                    }
                    if (ImGui::Button("Kasrkin (25 Pti)\n(Soldato Scelto)", ImVec2(280, 50))) {
                        recruitAction("imperium_kasrkin", "Kasrkin Thorne", glm::vec4(0.35f, 0.25f, 0.18f, 1.0f));
                    }
                }
                else if (m_SelectedFaction == "SpaceMarines") {
                    if (ImGui::Button("Neofita Astartes (40 Pti)\n(Neophyte)", ImVec2(280, 50))) {
                        recruitAction("spacemarines_neophyte", "Neofita Corvo", glm::vec4(0.92f, 0.72f, 0.68f, 1.0f));
                    }
                    if (ImGui::Button("Fratello di Battaglia (75 Pti)\n(Battle Brother)", ImVec2(280, 50))) {
                        recruitAction("spacemarines_battle_brother", "Fratello Titus", glm::vec4(0.05f, 0.25f, 0.75f, 1.0f));
                    }
                }
                else if (m_SelectedFaction == "Orks") {
                    if (ImGui::Button("Ork Boy (15 Pti)\n(Fante Orco)", ImVec2(280, 50))) {
                        recruitAction("orks_boy", "Gor-Gath", glm::vec4(0.15f, 0.60f, 0.15f, 1.0f));
                    }
                    if (ImGui::Button("Ork Nob (50 Pti)\n(Capoclan Orco)", ImVec2(280, 50))) {
                        recruitAction("orks_nob", "Uzgrak", glm::vec4(0.08f, 0.40f, 0.08f, 1.0f));
                    }
                }

                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                if (ImGui::Button("< Indietro alla Fazione", ImVec2(150, 30))) {
                    m_RecruitStep = 0;
                }
            }

            ImGui::EndPopup();
        }
    }

    void GameLayer::RenderCustomizeModal() {
        if (!m_ShowCustomizeModal) return;

        ImGui::OpenPopup("MODIFICA UNITA'");
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(620, 520));

        if (ImGui::BeginPopupModal("MODIFICA UNITA'", &m_ShowCustomizeModal, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {

            if (m_PlayerRoster.empty() || m_SelectedVaultIndex >= m_PlayerRoster.size()) {
                m_ShowCustomizeModal = false;
                ImGui::EndPopup();
                return;
            }

            TrainedUnit& activeUnit = m_PlayerRoster[m_SelectedVaultIndex];

            ImGui::BeginGroup();
            if (m_CustomizeMainTab == 0) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.4f, 0.7f, 1.0f));
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("ASPETTO", ImVec2(185, 40))) m_CustomizeMainTab = 0;
            ImGui::PopStyleColor();

            ImGui::SameLine();
            if (m_CustomizeMainTab == 1) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.4f, 0.7f, 1.0f));
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("EQUIPAGGIAMENTO", ImVec2(185, 40))) m_CustomizeMainTab = 1;
            ImGui::PopStyleColor();

            ImGui::SameLine();
            if (m_CustomizeMainTab == 2) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.4f, 0.7f, 1.0f));
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("ARMATURA", ImVec2(185, 40))) m_CustomizeMainTab = 2;
            ImGui::PopStyleColor();
            ImGui::EndGroup();

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            if (m_CustomizeMainTab == 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "ANAGRAFICA E IDENTITA'");
                char nameBuf[64];
                std::snprintf(nameBuf, sizeof(nameBuf), "%s", activeUnit.customName.c_str());
                if (ImGui::InputText("Nome Unita'", nameBuf, IM_ARRAYSIZE(nameBuf))) {
                    activeUnit.customName = nameBuf;
                }

                ImGui::Spacing(); ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "VERNICIATURA E SKIN");
                if (ImGui::ColorEdit3("Colore Primario", glm::value_ptr(activeUnit.armorColor))) {
                    ApplyUnitVisuals(activeUnit);
                }

                ImGui::Spacing(); ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[COMING SOON] - Personalizzazione Volto");
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[COMING SOON] - Protesi Bioniche e Accessori");
            }
            else if (m_CustomizeMainTab == 1) {

                if (ImGui::RadioButton("Armi da Fuoco / Corpo a Corpo", m_EquipSubTab == 0)) m_EquipSubTab = 0;
                ImGui::SameLine();
                if (ImGui::RadioButton("Dotazione Tattica", m_EquipSubTab == 1)) m_EquipSubTab = 1;
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                if (m_EquipSubTab == 0) {
                    ImGui::Text("Filtro Categoria Armi:");
                    if (ImGui::Button("Armi Laser", ImVec2(100, 25))) m_WeaponCategoryTab = 0; ImGui::SameLine();
                    if (ImGui::Button("Proiettili", ImVec2(100, 25))) m_WeaponCategoryTab = 1; ImGui::SameLine();
                    if (ImGui::Button("Plasma (WIP)", ImVec2(100, 25))) m_WeaponCategoryTab = 2;
                    ImGui::Spacing();

                    if (m_WeaponCategoryTab == 2) {
                        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Le armi al Plasma sono attualmente in sviluppo nella Forgia.");
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
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "SLOT EQUIPAGGIAMENTO (tasti 4 - 5 - 6 in partita)");
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
                    if (ImGui::Button("Ripristina dotazione base", ImVec2(220, 28))) {
                        activeUnit.ApplyDefaultLoadout();
                        ApplyUnitVisuals(activeUnit);
                    }
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Scatola di munizioni in mano: click sinistro rifornisce te,");
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "click destro un compagno entro la portata.");
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Squig: basta averlo in uno slot, ti segue da solo.");
                }
                // Esito dell'ultimo acquisto di un oggetto ricercato
                if (m_ResearchMessageTimer > 0.0f && !m_ResearchMessage.empty()) {
                    m_ResearchMessageTimer -= ImGui::GetIO().DeltaTime;
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "%s", m_ResearchMessage.c_str());
                }
            }
            else if (m_CustomizeMainTab == 2) {
                const auto* unitBp = BlueprintManager::Get().GetUnit(activeUnit.blueprintId);
                float maxWeight = unitBp ? unitBp->weight_capacity : 60.0f;
                float currentWeight = activeUnit.CalculateTotalWeight();
                int unitPoints = activeUnit.CalculateBattlePoints();

                ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "ISPEZIONE HITBOX E PIASTRE CORAZZATE");
                ImGui::Text("Punti Unita': %d Pti  |  Carico: %.1f / %.1f kg", unitPoints, currentWeight, maxWeight);

                float weightRatio = std::clamp(currentWeight / maxWeight, 0.0f, 1.0f);
                ImVec4 weightColor = (weightRatio > 1.0f) ? ImVec4(1.0f, 0.1f, 0.1f, 1.0f) : ImVec4(0.2f, 0.7f, 1.0f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, weightColor);
                ImGui::ProgressBar(weightRatio, ImVec2(-1, 12), "");
                ImGui::PopStyleColor();

                if (currentWeight > maxWeight) {
                    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "ATTENZIONE: Sovraccarico! L'unita' subira' penalita' di movimento.");
                }

                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                auto drawArmorSlotCombo = [&](const char* slotLabel, HitboxLocation location, std::string& currentId) {
                    const auto* currentBp = BlueprintManager::Get().GetArmorPiece(currentId);

                    ImGui::PushID(slotLabel);
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "[%s]", slotLabel);

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
                        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "  ! ATTENZIONE: Questo arto subira' danni letali completi !");
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
                if (ImGui::Button("Ripristina armatura base", ImVec2(220, 26))) {
                    activeUnit.ApplyDefaultArmor();
                    ApplyUnitVisuals(activeUnit);
                }
                if (m_ResearchMessageTimer > 0.0f && !m_ResearchMessage.empty()) {
                    m_ResearchMessageTimer -= ImGui::GetIO().DeltaTime;
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "%s", m_ResearchMessage.c_str());
                }
            }

            ImGui::SetCursorPosY(465.0f);
            ImGui::Separator(); ImGui::Spacing();
            if (ImGui::Button("SALVA E CHIUDI", ImVec2(-1, 35))) {
                m_ShowCustomizeModal = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void GameLayer::RenderMapEditorUI() {
        if (m_GameState != GameState::MapEditor) return;

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 410.0f, 65.0f));
        ImGui::SetNextWindowSize(ImVec2(400, io.DisplaySize.y - 80.0f));

        ImGui::Begin("MAP BUILDER (Solemnace Forge)", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.3f, 0.0f, 1.0f));
        if (ImGui::Button("+ CREA NUOVA MAPPA DA ZERO", ImVec2(-1, 35))) {
            CreateNewMap();
        }
        ImGui::PopStyleColor();

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::Text("Modalita' Manipolazione (Tasti Rapidi):");
        if (m_GizmoMode == EditorGizmoMode::Translate) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.8f, 1.0f));
        else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("[W] Sposta", ImVec2(110, 28))) m_GizmoMode = EditorGizmoMode::Translate;
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (m_GizmoMode == EditorGizmoMode::Rotate) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.8f, 1.0f));
        else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("[E] Ruota", ImVec2(110, 28))) m_GizmoMode = EditorGizmoMode::Rotate;
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (m_GizmoMode == EditorGizmoMode::Scale) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.8f, 1.0f));
        else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("[R] Scala", ImVec2(110, 28))) m_GizmoMode = EditorGizmoMode::Scale;
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
            if (ImGui::Button("Aggiungi Spawn Giocatore")) m_EditorMap.playerSpawns.push_back(m_EditorCameraTarget);
            for (size_t i = 0; i < m_EditorMap.playerSpawns.size(); ++i) {
                ImGui::PushID(static_cast<int>(i) + 1000);
                ImGui::DragFloat3("Spawn P.", glm::value_ptr(m_EditorMap.playerSpawns[i]), 0.5f);
                ImGui::SameLine();
                if (ImGui::Button("X")) { m_EditorMap.playerSpawns.erase(m_EditorMap.playerSpawns.begin() + i); }
                ImGui::PopID();
            }

            ImGui::Separator();

            if (ImGui::Button("Aggiungi Spawn Nemici")) m_EditorMap.enemySpawns.push_back(m_EditorCameraTarget);
            for (size_t i = 0; i < m_EditorMap.enemySpawns.size(); ++i) {
                ImGui::PushID(static_cast<int>(i) + 2000);
                ImGui::DragFloat3("Spawn E.", glm::value_ptr(m_EditorMap.enemySpawns[i]), 0.5f);
                ImGui::SameLine();
                if (ImGui::Button("X")) { m_EditorMap.enemySpawns.erase(m_EditorMap.enemySpawns.begin() + i); }
                ImGui::PopID();
            }
        }

        if (ImGui::CollapsingHeader("3. Inserisci Blocchi Primitivi", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto addBlockShape = [&](const std::string& shapeType, const std::string& label, const glm::vec3& defaultScale) {
                if (ImGui::Button(label.c_str(), ImVec2(115, 28))) {
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
                if (ImGui::Button(label.c_str(), ImVec2(115, 32))) {
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
            ImGui::TextColored(ImVec4(0.0f, 0.80f, 1.0f, 1.0f), "Modifica [%s - %s]:", st.type.c_str(), st.shape.c_str());

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

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("Elimina Oggetto", ImVec2(-1, 26))) {
                m_EditorMap.structures.erase(m_EditorMap.structures.begin() + m_SelectedStructureIdx);
                m_SelectedStructureIdx = -1;
                SyncEditorEntitiesWithScene();
            }
            ImGui::PopStyleColor();
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.2f, 1.0f));
        if (ImGui::Button("SALVA E GIOCA IN-GAME", ImVec2(-1, 38))) {
            SaveMapToJSON();
            LoadMapIntoPreview();
        }
        ImGui::PopStyleColor();

        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.4f, 0.7f, 1.0f));
        if (ImGui::Button("ESPORTA PER CONDIVISIONE (External)", ImVec2(-1, 38))) {
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

        ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "1. SELEZIONA MAPPA TARGET");

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

        ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "2. MODALITA' DI GIOCO");

        const char* events[] = {
            "Difesa del Settore (Ondata Orki)",
            "Recupero Dati (Tyranidi)",
            "Incursione Profonda",
            "SANDBOX (Esplorazione Libera)",
            "VS BOT (5 contro 5)"
        };

        for (int i = 0; i < 5; ++i) {
            bool isSelected = (m_SelectedEventIndex == i);
            if (isSelected) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.8f, 0.4f, 0.0f, 1.0f));

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
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "[VS BOT - 5 CONTRO 5]");
            ImGui::TextWrapped("La tua squadra (fino a 5 unita', completata da alleati IA) contro 5 bot su 'Trincee di "
                "Solemnace'. Vince chi elimina la squadra avversaria; dopo 10 minuti vince chi ha piu' punti in campo. "
                "Se cadi prendi il controllo di un alleato vivo. Scegli la coda nel pannello di destra.");
        }
        else if (m_SelectedEventIndex == 3) {
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "[MODALITA' SANDBOX]");
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

            if (isInSquad) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.65f, 0.15f, 0.9f));
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.20f, 0.6f));

            std::string btnText = isInSquad ? "[IN SQUADRA] " + label : "[RISERVA] " + label;
            if (ImGui::Button(btnText.c_str(), ImVec2(-1, 40))) {
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

        ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "COMPOSIZIONE SQUADRA (%d Unita):", static_cast<int>(m_SelectedSquadIndices.size()));
        ImGui::Separator(); ImGui::Spacing();

        for (size_t squadIdx = 0; squadIdx < m_SelectedSquadIndices.size(); ++squadIdx) {
            int rosterIdx = m_SelectedSquadIndices[squadIdx];
            if (rosterIdx >= 0 && rosterIdx < static_cast<int>(m_PlayerRoster.size())) {
                const auto& unit = m_PlayerRoster[rosterIdx];
                if (squadIdx == 0) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "LEADER APRIPISTA: %s", unit.customName.c_str());
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

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.25f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.35f, 0.15f, 1.0f));
        if (ImGui::Button("AVVIA SIMULAZIONE", ImVec2(-1, 65))) {
            if (!m_SelectedSquadIndices.empty()) {

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
                        playerTrans.Position = m_CurrentMapData.playerSpawnPoints[0];
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
                    PlayStartWaaagh(m_VolumeVoice);
                }
                else {
                    g_WaaaghActive = false;
                }

                ResetFactionBuff();
                m_GameState = GameState::Gameplay;
                if (window) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            }
        }
        ImGui::PopStyleColor(2);

        ImGui::End();
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
            ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "--- SQUADRONE ELIMINATO ---");
            ImGui::Separator(); ImGui::Spacing();
            ImGui::TextWrapped("Tutti i membri della squadra schierati sono stati travolti.");

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "PUNTEGGIO FINALE:");
            ImGui::Text("Orki Eliminati: %d", m_KillCount);
            ImGui::Spacing();
            RenderMatchRecap();

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            if (ImGui::Button("Torna al Menu Principale (Garage)", ImVec2(-1, 50))) {
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
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "UNITA' SOPRAVVISSUTE:");
        ImGui::Text("Orki eliminati finora: %d", m_KillCount);
        ImGui::Text("Seleziona quale membro far scendere sul campo:");
        ImGui::Separator(); ImGui::Spacing();

        for (size_t i = 0; i < m_ActiveSquad.size(); ++i) {
            auto& member = m_ActiveSquad[i];
            if (member.rosterIndex >= 0 && member.rosterIndex < static_cast<int>(m_PlayerRoster.size())) {
                const auto& unit = m_PlayerRoster[member.rosterIndex];

                if (member.isAlive) {
                    std::string label = "SCHIERA: " + unit.customName + " [" + unit.blueprintId + "]";
                    if (ImGui::Button(label.c_str(), ImVec2(-1, 45))) {
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

    void GameLayer::RenderOptionsMenu() {
        if (!m_ShowOptionsMenu) return;

        ImGui::OpenPopup("Opzioni di Gioco");
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(450, 360));

        if (ImGui::BeginPopupModal("Opzioni di Gioco", &m_ShowOptionsMenu, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {

            if (ImGui::BeginTabBar("OptionsTabBar")) {

                if (ImGui::BeginTabItem("VIDEO")) {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "IMPOSTAZIONI DISPLAY");
                    ImGui::Separator(); ImGui::Spacing();

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

                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                    if (ImGui::Button("Applica Video", ImVec2(140, 35))) {
                        ApplyDisplaySettings(g_Resolutions[m_SelectedResolutionIdx].width, g_Resolutions[m_SelectedResolutionIdx].height, m_IsFullscreen);
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("AUDIO")) {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "CONTROLLO VOLUMI CANALI");
                    ImGui::Separator(); ImGui::Spacing();

                    ImGui::Text("Ambienza & Effetti Gioco (SFX):");
                    if (ImGui::SliderFloat("##VolumeSFX", &m_VolumeSFX, 0.0f, 1.0f, "%.2f")) {
                        if (m_SFXGroupInitialized) {
                            ma_sound_group_set_volume(&m_SFXGroup, m_VolumeSFX);
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Text("Voci, Schiamazzi & Versi:");
                    if (ImGui::SliderFloat("##VolumeVoice", &m_VolumeVoice, 0.0f, 1.0f, "%.2f")) {
                        if (g_OrkVoices.startSoundLoaded) {
                            ma_sound_set_volume(&g_OrkVoices.startWaaaghSound, m_VolumeVoice);
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Text("Musica di Sottofondo:");
                    ImGui::SliderFloat("##VolumeMusic", &m_VolumeMusic, 0.0f, 1.0f, "%.2f");

                    ImGui::Spacing();
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
                    if (!animAvailable) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Modello/animazioni non caricati.");

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
                    ImGui::TextDisabled("Arma degli orchi (choppa):");
                    ImGui::DragFloat3("Pugno sul braccio (m, dal fondo)", &m_OrkWeaponHoldOffset.x, 0.01f, -1.0f, 1.0f, "%.2f");
                    ImGui::DragFloat3("Rotazione lama (gradi)", &m_OrkWeaponModelRotation.x, 1.0f, -180.0f, 180.0f, "%.0f");
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::SetCursorPosY(310.0f);
            ImGui::Separator(); ImGui::Spacing();
            if (ImGui::Button("Chiudi Menu", ImVec2(-1, 32))) {
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
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "VS BOT - SCEGLI LA CODA");
        ImGui::Text("La tua squadra: %d punti battaglia, %d unita'", pts, n);
        ImGui::TextWrapped("5 contro 5: tu e 4 alleati IA contro 5 avversari IA. Ogni giocatore porta una squadra entro il cap "
            "della coda (unita' + equipaggiamento) e rientra con un'altra unita' quando quella in campo cade.");
        ImGui::Spacing();

        const std::string capProblem = SquadTypeCapProblem();
        if (!capProblem.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", capProblem.c_str());
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

            ImGui::PushStyleColor(ImGuiCol_Button, ok ? ImVec4(0.15f, 0.55f, 0.20f, 1.0f) : ImVec4(0.35f, 0.12f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ok ? ImVec4(0.20f, 0.70f, 0.28f, 1.0f) : ImVec4(0.35f, 0.12f, 0.12f, 1.0f));
            ImGui::BeginDisabled(!ok);
            if (ImGui::Button(label, ImVec2(-1, 58))) {
                StartVsBotMatch(i);
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(2);
            ImGui::TextColored(ok ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", status);
            ImGui::Spacing();
        }
        if (!m_VsBotMessage.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", m_VsBotMessage.c_str());
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

        // Squadre: ogni giocatore IA (4 alleati + 5 avversari) porta quante piu' unita' possibile senza sforare
        // il cap della coda. Alleati della fazione del capo squadra se possibile, avversari tutti di una fazione.
        std::vector<VsBotCandidate> all;
        // Costo di un candidato = unita' con la sua dotazione base (punti battaglia)
        for (const auto& [id, bp] : BlueprintManager::Get().GetUnits()) all.push_back({ id, MakeBotUnit(id, 1).CalculateBattlePoints(), VsBotFactionOf(id), bp.max_per_squad });
        std::sort(all.begin(), all.end(), [](const VsBotCandidate& a, const VsBotCandidate& b) { return a.unitId < b.unitId; });
        std::mt19937 rng(std::random_device{}());
        const TrainedUnit& leader = m_PlayerRoster[m_SelectedSquadIndices[0]];
        const std::string allyFaction = VsBotFactionOf(leader.blueprintId);
        std::vector<std::string> factions;
        for (const auto& c : all) if (std::find(factions.begin(), factions.end(), c.faction) == factions.end()) factions.push_back(c.faction);
        std::shuffle(factions.begin(), factions.end(), rng);
        std::string enemyFaction;
        for (const auto& f : factions) if (!VsBotBuildSquad(all, f, tier, rng, true).empty()) { enemyFaction = f; break; }

        std::vector<VsBotPlayer> players;
        std::vector<std::string> usedNames;
        {
            VsBotPlayer me;
            me.name = "Tu";
            me.team = 0;
            me.human = true;
            for (int rosterIdx : m_SelectedSquadIndices) {
                VsBotSlot s;
                s.unit = m_PlayerRoster[rosterIdx];
                s.rosterIdx = rosterIdx;
                s.points = s.unit.CalculateBattlePoints();
                me.squad.push_back(s);
            }
            players.push_back(me);
        }
        auto makeAi = [&](int team, const std::string& faction) {
            const auto ids = VsBotBuildSquad(all, faction, tier, rng, team == 1 ? !enemyFaction.empty() : false);
            if (ids.empty()) return;
            VsBotPlayer p;
            p.team = team;
            p.name = VsBotRandomName(faction, usedNames, rng);
            std::unordered_map<std::string, int> counts;
            int sum = 0;
            for (const auto& id : ids) {
                VsBotSlot s;
                s.unit = MakeBotUnit(id, ++counts[id]);
                sum += s.unit.CalculateBattlePoints();
                p.squad.push_back(s);
            }
            // Oggetti extra a caso finche' il budget del giocatore lo permette (la squadra resta sotto il cap)
            int budget = std::max(0, tier.cap - sum);
            for (auto& s : p.squad) { AddBotExtras(s.unit, budget); s.points = s.unit.CalculateBattlePoints(); }
            players.push_back(p);
        };
        for (int i = 1; i < kVsBotTeamSize; ++i) makeAi(0, allyFaction);
        for (int i = 0; i < kVsBotTeamSize; ++i) makeAi(1, enemyFaction.empty() ? factions[rng() % factions.size()] : enemyFaction);
        int enemyPlayers = 0;
        for (const auto& p : players) enemyPlayers += p.team == 1 ? 1 : 0;
        if (enemyPlayers == 0) {
            m_VsBotMessage = "Impossibile formare una squadra avversaria per questa coda.";
            return false;
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
        const float yaw0 = m_VsBotYaw[0];

        // Giocatore = prima unita' della sua squadra; le altre restano in riserva per i rientri
        m_ActiveSquad.clear();
        for (int rosterIdx : m_SelectedSquadIndices) m_ActiveSquad.push_back(ActiveSquadMember{ rosterIdx, true, 100.0f });
        m_CurrentSquadMemberIdx = 0;
        m_KillCount = 0;
        ResetMatchXp();
        m_PlayerDeathHandled = false;
        m_VsBotDeadTime = 0.0f;
        m_VsBotSpectate = 0;
        ClearPlayerDeathState();
        ApplyUnitVisuals(leader);
        auto& reg = m_Scene.GetRegistry();
        reg.emplace_or_replace<TeamComponent>(m_PlayerEntity, TeamComponent{ 0 });
        if (auto* pt = reg.try_get<TransformComponent>(m_PlayerEntity)) {
            pt->Position = VsBotSpawnPoint(0, 0, false);
            pt->AimYaw = yaw0; pt->Rotation.y = yaw0; pt->AimPitch = 0.0f;
            m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, pt->Position);
        }
        if (auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity)) hp->currentHealth = hp->maxHealth;

        // Ogni giocatore IA schiera la prima unita' della sua squadra
        m_VsBotPlayers = std::move(players);
        m_VsBotPlayers[0].squad[0].state = VsBotSlotState::InField;
        m_VsBotPlayers[0].active = 0;
        int spawnIdx[2] = { 1, 0 };
        int allyPts = 0, enemyPts = 0, allyUnits = 0, enemyUnits = 0;
        for (auto& p : m_VsBotPlayers) {
            (p.team == 0 ? allyPts : enemyPts) += p.TotalPoints();
            (p.team == 0 ? allyUnits : enemyUnits) += static_cast<int>(p.squad.size());
            if (p.human) continue;
            VsBotSpawnAi(p, 0, VsBotSpawnPoint(p.team, spawnIdx[p.team]++, false));
        }
        m_PhysicsSystem.RefreshBodies(m_Scene);
        SUN_CORE_INFO("Vs Bot {0}: alleati {1} pti in {2} unita', avversari {3} pti in {4} unita' ({5})",
            tier.name, allyPts, allyUnits, enemyPts, enemyUnits, enemyFaction);

        g_WaaaghActive = false;
        m_VsBotActive = true;
        m_VsBotTier = tierIdx;
        m_VsBotTime = 0.0f;
        m_VsBotResult = 0;
        m_VsBotReward = 0;
        m_VsBotEnemyKills = 0;
        m_VsBotEndReason.clear();
        m_IsPaused = false;
        ResetFactionBuff();
        m_GameState = GameState::Gameplay;
        if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        return true;
    }

    // Rientri e fine partita. Ogni giocatore ha in campo una sola unita': quando cade, l'IA rientra dopo
    // kVsBotAiRespawn secondi con un'altra unita' della sua squadra, tu scegli dalla schermata di morte.
    // Una squadra e' eliminata quando nessun suo giocatore ha piu' unita' in campo o in riserva; allo scadere
    // vince chi ha piu' punti battaglia rimasti (in campo + in riserva).
    void GameLayer::UpdateVsBot(float deltaTime) {
        if (!m_VsBotActive || m_VsBotResult != 0 || m_IsPaused) return;
        m_VsBotTime += deltaTime;
        auto& reg = m_Scene.GetRegistry();
        for (auto& p : m_VsBotPlayers) {
            if (p.active >= 0 && p.active < static_cast<int>(p.squad.size())) {
                auto& s = p.squad[p.active];
                bool dead;
                if (p.human) {
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
                    p.respawnTimer = p.human ? kVsBotHumanRespawn : kVsBotAiRespawn;
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
        if (m_VsBotPlayers.empty() || !m_VsBotPlayers[0].human || m_VsBotPlayers[0].active < 0) m_VsBotDeadTime += deltaTime;

        int left[2] = { 0, 0 }, points[2] = { 0, 0 };
        for (const auto& p : m_VsBotPlayers) {
            const int team = p.team == 1 ? 1 : 0;
            left[team] += p.CountState(VsBotSlotState::InField) + p.CountState(VsBotSlotState::Reserve);
            points[team] += p.PointsState(VsBotSlotState::InField) + p.PointsState(VsBotSlotState::Reserve);
        }
        if (left[1] == 0) EndVsBotMatch(1, "Tutte le squadre avversarie sono state eliminate");
        else if (left[0] == 0) EndVsBotMatch(2, "Tutte le squadre alleate sono state eliminate");
        else if (m_VsBotTime >= m_VsBotTimeLimit) {
            if (points[0] > points[1]) EndVsBotMatch(1, "Tempo scaduto: avete piu' punti rimasti");
            else if (points[0] < points[1]) EndVsBotMatch(2, "Tempo scaduto: gli avversari hanno piu' punti rimasti");
            else EndVsBotMatch(3, "Tempo scaduto: parita' di punti rimasti");
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
        if (m_VsBotPlayers.empty() || !m_VsBotPlayers[0].human) return;
        auto& me = m_VsBotPlayers[0];
        if (slot < 0 || slot >= static_cast<int>(me.squad.size()) || me.squad[slot].state != VsBotSlotState::Reserve || me.respawnTimer > 0.0f) return;
        auto& s = me.squad[slot];
        auto& reg = m_Scene.GetRegistry();

        ClearPlayerDeathState();   // via ragdoll e CorpseComponent
        ApplyUnitVisuals(s.unit);  // statistiche, armatura, armi, munizioni piene, vesti
        reg.emplace_or_replace<TeamComponent>(m_PlayerEntity, TeamComponent{ 0 });
        if (auto* pt = reg.try_get<TransformComponent>(m_PlayerEntity)) {
            pt->Position = VsBotSpawnPoint(0, Random::RangeInt(0, std::max(0, static_cast<int>(m_VsBotSpawns[0].size()) - 1)), true);
            pt->AimYaw = m_VsBotYaw[0]; pt->Rotation.y = m_VsBotYaw[0]; pt->AimPitch = 0.0f;
            m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, pt->Position);
        }
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
        m_BotAI.Update(m_Scene, m_NavQuery, deltaTime);
        m_MeleeCombat.Update(m_Scene, deltaTime);
        m_SquigSystem.Update(m_Scene, m_NavQuery, deltaTime, m_SquigSkeleton ? &*m_SquigSkeleton : nullptr);
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
        for (auto e : view) if (view.get<TeamComponent>(e).team == 0 && view.get<HealthComponent>(e).currentHealth > 0.0f) allies.push_back(e);
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
            if (p.team == 0) allyLeft += p.CountState(VsBotSlotState::InField) + p.CountState(VsBotSlotState::Reserve);
            else enemyFallen += p.CountState(VsBotSlotState::Fallen);
        }

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 12.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::Begin("VsBotHUD", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
        const int left = std::max(0, static_cast<int>(m_VsBotTimeLimit - m_VsBotTime));
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s", kVsBotTiers[std::clamp(m_VsBotTier, 0, 2)].name);
        ImGui::SameLine(); ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "  ALLEATI: %d unita' rimaste", allyLeft);
        ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "  AVVERSARI: %d abbattute", enemyFallen);
        ImGui::SameLine(); ImGui::Text("   %02d:%02d", left / 60, left % 60);
        ImGui::SameLine(); ImGui::TextDisabled("   [TAB] squadre");
        ImGui::End();

        const glm::mat4 vp = m_Camera.GetViewProjection();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const glm::vec3 camPos = m_Camera.GetPosition();
        auto bots = reg.view<BotBrainComponent, TeamComponent, HealthComponent, TransformComponent>(entt::exclude<CorpseComponent>);
        for (auto e : bots) {
            const auto& h = bots.get<HealthComponent>(e);
            if (h.currentHealth <= 0.0f) continue;
            const bool ally = bots.get<TeamComponent>(e).team == 0;
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
        const VsBotPlayer* me = (!m_VsBotPlayers.empty() && m_VsBotPlayers[0].human) ? &m_VsBotPlayers[0] : nullptr;
        if (me && me->CountState(VsBotSlotState::Reserve) > 0) {
            if (me->respawnTimer > 0.0f) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Rinforzi pronti tra %.0f s", std::ceil(me->respawnTimer));
            else ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Scegli l'unita' con cui rientrare:");
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
                if (ImGui::Button(label, ImVec2(-1, 40))) chosen = i;
                ImGui::EndDisabled();
                DrawVsBotUnitIcon(dl, ImVec2(cur.x + 22.0f, cur.y + 20.0f), 12.0f, s.unit, s.state);
            }
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Non hai piu' unita' in riserva.");
            ImGui::TextWrapped("Osserva i tuoi alleati: la partita continua finche' una delle due parti resta senza unita'.");
        }
        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextDisabled("Spettatore");
        if (ImGui::Button("< Alleato precedente", ImVec2(196, 28))) --m_VsBotSpectate;
        ImGui::SameLine();
        if (ImGui::Button("Alleato successivo >", ImVec2(196, 28))) ++m_VsBotSpectate;
        ImGui::TextDisabled("Tieni premuto TAB per vedere le squadre");
        ImGui::Spacing();
        const bool leave = ImGui::Button("Abbandona la partita", ImVec2(-1, 32));
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
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s  -  cap squadra %d pti", tier.name, tier.cap);
        ImGui::SameLine(ImGui::GetWindowWidth() - 120.0f); ImGui::Text("%02d:%02d", secs / 60, secs % 60);
        ImGui::Separator();

        auto drawTeam = [&](int team) {
            const bool ally = team == 0;
            int unitsLeft = 0, fallen = 0;
            for (const auto& p : m_VsBotPlayers) if (p.team == team) {
                unitsLeft += p.CountState(VsBotSlotState::InField) + p.CountState(VsBotSlotState::Reserve);
                fallen += p.CountState(VsBotSlotState::Fallen);
            }
            ImGui::Spacing();
            if (ally) ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "ALLEATI   -   unita' rimaste: %d   cadute: %d", unitsLeft, fallen);
            else ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "AVVERSARI   -   unita' abbattute: %d", fallen);
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
        drawTeam(0);
        ImGui::Spacing();
        drawTeam(1);
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
        const ImVec4 col = m_VsBotResult == 1 ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f) : (m_VsBotResult == 2 ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) : ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
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
        const bool back = ImGui::Button("Torna al garage", ImVec2(-1, 44));
        ImGui::End();
        if (back) LeaveVsBotMatch();
    }

} // namespace SunEngine