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
#include "Engine/AssetManager/BlueprintManager.h"
#include "Engine/Physics/SkeletonLoader.h"
#include "Engine/Scene/MapLoader.h"
#include "Engine/Navigation/NavMeshQuery.h"

#include <cstdlib>
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

    static void SwapPlayerUnit(Scene& scene, entt::entity playerEntity, const std::string& newUnitId) {
        const auto* bp = BlueprintManager::Get().GetUnit(newUnitId);
        if (!bp) return;

        scene.GetRegistry().emplace_or_replace<MeshComponent>(playerEntity, 0u);
        scene.GetRegistry().get_or_emplace<UnitBlueprintId>(playerEntity).id = newUnitId;

        auto& stats = scene.GetRegistry().get_or_emplace<UnitStatsComponent>(playerEntity);
        stats.baseSpeed = bp->base_speed;
        stats.stamina = bp->stamina;
        stats.staminaRegen = bp->stamina_regen;
        stats.maxHealth = bp->health;
        stats.currentHealth = bp->health;
        stats.toughness = bp->toughness;
        stats.strength = bp->strength;
        stats.accuracySkill = bp->accuracy_skill;
        stats.meleeSkill = bp->melee_skill;
        stats.morale = bp->morale;
        stats.suppressionThreshold = static_cast<float>(bp->suppression_threshold);
        stats.visionRange = bp->vision_range;
        stats.fovDegrees = bp->fov_degrees;
        stats.weightCapacity = bp->weight_capacity;
        std::string skeletonsDir = FindValidDir("Skeletons");
        std::string skelPath = skeletonsDir + newUnitId + ".json";
        auto skeletonData = SkeletonLoader::LoadFromFile(skelPath);
        if (skeletonData) {
            auto& skelComp = scene.GetRegistry().emplace_or_replace<SkeletonComponent>(playerEntity);
            skelComp.data = *skeletonData;
            skelComp.scale = 1.0f;
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

            std::string activeId = inv.GetActiveWeaponId();
            if (activeId.empty()) {
                inv.currentSlot = 0;
                activeId = inv.primaryWeaponId;
            }

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
            }
        }
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
                const auto* bp = BlueprintManager::Get().GetUnit(m_PlayerRoster[rosterIdx].blueprintId);
                if (bp) total += bp->point_cost;
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
            SUN_CORE_WARN("Unita' eliminata! Attivazione Ragdoll Jolt Physics...");

            // FIX #3: l'arma dell'ork sparisce alla morte (il ragdoll non la regge in mano).
            // L'observer on_destroy copre comunque i casi in cui l'ork sparisce senza passare di qui.
            if (auto* horde = reg.try_get<HordeAgentComponent>(entity)) {
                if (horde->weaponEntity != entt::null && reg.valid(horde->weaponEntity)) {
                    reg.destroy(horde->weaponEntity);
                }
                horde->weaponEntity = entt::null;
            }

            glm::vec3 deathImpulse(0.0f, 1.5f, -3.5f);
            m_PhysicsSystem.KillUnit(m_Scene, entity, deathImpulse);

            if (reg.all_of<CorpseComponent>(entity)) {
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

        TrainedUnit unit1{ "Sgt. Kael", "imperium_kasrkin", glm::vec4(0.35f, 0.45f, 0.25f, 1.0f) };
        TrainedUnit unit2{ "Recluta Vance", "imperium_guardsman_recruit", glm::vec4(0.65f, 0.50f, 0.35f, 1.0f) };
        TrainedUnit unit3{ "Gor-Gath", "orks_boy", glm::vec4(0.15f, 0.60f, 0.15f, 1.0f) };

        m_PlayerRoster.push_back(unit1);
        m_PlayerRoster.push_back(unit2);
        m_PlayerRoster.push_back(unit3);

        // PROFILO: se esiste un salvataggio sostituisce il roster di default e la squadra selezionata
        LoadRosterFromProfile();
        LoadResearchTree();   // dopo il roster: le unita' gia' possedute risultano sbloccate
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
        m_SelectedEventIndex = std::clamp(m_Profile.selectedEventIndex, 0, 3);
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
                t.primaryWeaponId = u.value("primaryWeaponId", t.primaryWeaponId);
                t.secondaryWeaponId = u.value("secondaryWeaponId", t.secondaryWeaponId);
                t.meleeWeaponId = u.value("meleeWeaponId", t.meleeWeaponId);
                if (u.contains("equipmentSlots") && u["equipmentSlots"].is_array()) {
                    for (size_t i = 0; i < 3 && i < u["equipmentSlots"].size(); ++i)
                        t.equipmentSlots[i] = u["equipmentSlots"][i].get<std::string>();
                }
                t.armorHeadId = u.value("armorHeadId", t.armorHeadId);
                t.armorTorsoId = u.value("armorTorsoId", t.armorTorsoId);
                t.armorLeftArmId = u.value("armorLeftArmId", t.armorLeftArmId);
                t.armorRightArmId = u.value("armorRightArmId", t.armorRightArmId);
                t.armorLeftLegId = u.value("armorLeftLegId", t.armorLeftLegId);
                t.armorRightLegId = u.value("armorRightLegId", t.armorRightLegId);
                t.exp = std::max(0, u.value("exp", 0));
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

    // Ricompensa per un nemico eliminato: +1 kill, +1 energia (valuta principale), +1 exp all'unita'
    // attualmente in campo e all'arma attiva. La Pietra Nera NON si guadagna in partita.
    void GameLayer::AwardKill() {
        m_KillCount++;
        m_Profile.energy += 1;

        if (m_CurrentSquadMemberIdx >= 0 && m_CurrentSquadMemberIdx < static_cast<int>(m_ActiveSquad.size())) {
            int rosterIdx = m_ActiveSquad[m_CurrentSquadMemberIdx].rosterIndex;
            if (rosterIdx >= 0 && rosterIdx < static_cast<int>(m_PlayerRoster.size())) {
                m_PlayerRoster[rosterIdx].exp += 1;
                // Punti studio del TIPO di unita' in campo: servono a sbloccare la ricerca dei suoi figli
                m_Profile.studyPoints[m_PlayerRoster[rosterIdx].blueprintId] += 1;
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
            auto skeleton = SkeletonLoader::LoadFromFile(skeletonsDir + spawn.unitId + ".json");
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
            skel.scale = 1.0f;
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
            const auto mats = skel.data.ComputeWorldTransforms(glm::vec3(0.0f), glm::vec3(0.0f, spawn.yawDegrees, 0.0f), 1.0f);
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
            }
            // Fuoco: solo armi a distanza, grilletto premuto, non in ricarica e con munizioni
            if (auto* wState = reg.try_get<WeaponStateComponent>(m_PlayerEntity)) {
                bool isMelee = false;
                if (auto* wbp = reg.try_get<WeaponBlueprintId>(m_PlayerEntity)) {
                    if (const auto* bp = BlueprintManager::Get().GetWeapon(wbp->id)) isMelee = bp->is_melee;
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

    bool GameLayer::IsAnimatedPlayerActive() {
        auto& reg = m_Scene.GetRegistry();
        return m_UseAnimatedPlayer && m_RifleAnimLoaded && m_SkinnedModelLoaded
            && reg.valid(m_AnimatedPlayerEntity) && reg.valid(m_PlayerEntity)
            && !reg.all_of<CorpseComponent>(m_PlayerEntity);
    }

    // Aggiorna il controller delle animazioni e, se scelto il modello animato, la sua entity visiva:
    // segue il player (posizione + direzione del corpo), nasconde i cubi (che restano hitbox).
    void GameLayer::UpdateAnimatedPlayer(float deltaTime, const RifleAnimInput& input) {
        auto& reg = m_Scene.GetRegistry();

        if (m_RifleAnimLoaded) {
            m_RifleAnimController.Update(deltaTime, input);
        }

        const bool playerOk = reg.valid(m_PlayerEntity) && reg.all_of<TransformComponent>(m_PlayerEntity);
        const bool wantAnimated = m_UseAnimatedPlayer && m_RifleAnimLoaded && m_SkinnedModelLoaded
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
            auto& inst = reg.emplace<SkinnedModelInstanceComponent>(m_AnimatedPlayerEntity);
            inst.sourceModel = &m_SkinnedTestModel;
            inst.boneMatrices = &m_RifleAnimController.GetFinalBoneMatrices();
        }
        if (!reg.all_of<HideProceduralVisualTag>(m_PlayerEntity)) {
            reg.emplace<HideProceduralVisualTag>(m_PlayerEntity);
        }

        const auto& pt = reg.get<TransformComponent>(m_PlayerEntity);
        auto& vt = reg.get<TransformComponent>(m_AnimatedPlayerEntity);
        vt.Position = pt.Position;   // Position del player = piedi, come l'origine del modello
        vt.Rotation = glm::vec3(0.0f, pt.Rotation.y + m_AnimatedYawOffset, 0.0f);
        vt.Scale = glm::vec3(m_AnimatedModelScale);

        // Colore armatura scelto nel garage come albedo del modello
        if (auto* pc = reg.try_get<ColorComponent>(m_PlayerEntity)) {
            reg.get<SkinnedModelInstanceComponent>(m_AnimatedPlayerEntity).albedo = pc->BaseColor;
        }
    }

    // Posiziona un'arma (cubo) sulla mano destra/sinistra del modello animato. Posizione = osso della
    // mano + offset (destra/su/avanti rispetto al corpo); orientamento = direzione del corpo con
    // inclinazione di mira, oppure quello della mano (m_WeaponFollowsHand). Tutto tarabile dalle Opzioni.
    bool GameLayer::PlaceWeaponOnAnimatedHand(bool leftHand, TransformComponent& weaponTransform, const BlockModel* model) {
        auto& reg = m_Scene.GetRegistry();
        glm::mat4 bone(1.0f);
        const char* boneName = leftHand ? "mixamorig1:LeftHand" : "mixamorig1:RightHand";
        if (!m_RifleAnimController.GetBoneModelTransform(boneName, bone)) return false;

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
            if (!leftHand && model->hasGripLeft && m_RifleAnimController.GetBoneModelTransform("mixamorig1:LeftHand", boneL)) {
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
            if (m_GameState == GameState::Gameplay) {
                m_IsPaused = !m_IsPaused;
                if (window) {
                    glfwSetInputMode(window, GLFW_CURSOR, m_IsPaused ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                }
            }
        }
        escPressedLastFrame = escPressed;

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

            // Modello animato anche nel garage (idle), poi arma sulla mano
            UpdateAnimatedPlayer(deltaTime, RifleAnimInput{});

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

            transform.AimYaw -= mouseDelta.x * mouseSensitivity;
            transform.AimPitch -= mouseDelta.y * mouseSensitivity;
            transform.AimPitch = std::clamp(transform.AimPitch, -85.0f, 85.0f);

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
            >(m_PlayerEntity))
            {
                auto& inv = m_Scene.GetRegistry().get<InventoryComponent>(m_PlayerEntity);
                auto& stats = m_Scene.GetRegistry().get<UnitStatsComponent>(m_PlayerEntity);
                auto& melee = m_Scene.GetRegistry().get_or_emplace<MeleeStateComponent>(m_PlayerEntity);
                auto& wState = m_Scene.GetRegistry().get<WeaponStateComponent>(m_PlayerEntity);
                auto& wBlueprintId = m_Scene.GetRegistry().get<WeaponBlueprintId>(m_PlayerEntity);

                int requestedSlot = -1;
                if (Input::IsKeyPressed(GLFW_KEY_1)) requestedSlot = 0;
                else if (Input::IsKeyPressed(GLFW_KEY_2)) requestedSlot = 1;
                else if (Input::IsKeyPressed(GLFW_KEY_3)) requestedSlot = 2;
                else if (Input::IsKeyPressed(GLFW_KEY_4)) requestedSlot = 3;
                else if (Input::IsKeyPressed(GLFW_KEY_5)) requestedSlot = 4;
                else if (Input::IsKeyPressed(GLFW_KEY_6)) requestedSlot = 5;
                else if (Input::IsKeyPressed(GLFW_KEY_7)) requestedSlot = 6;

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

                        const auto* weaponBP = BlueprintManager::Get().GetWeapon(targetWeaponId);
                        if (weaponBP) {
                            wState.currentAmmo = weaponBP->magazine_size;
                        }
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

                std::string meleeWeaponId = inv.meleeWeaponId;
                if (meleeWeaponId.empty() || meleeWeaponId == "none") {
                    meleeWeaponId = "standard_chainsword_1h";
                }
                const auto* meleeBP = BlueprintManager::Get().GetWeapon(meleeWeaponId);

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

                if (stance == EquipStance::MeleePure) {
                    static bool lmbWasDown = false;
                    bool lmbIsDown = Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
                    if (lmbIsDown && !lmbWasDown) {
                        triggerMeleeNode("LMB", "normal_1", MeleeAttackType::Normal);
                    }
                    lmbWasDown = lmbIsDown;

                    static bool rmbWasDown = false;
                    bool rmbIsDown = Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT);
                    if (rmbIsDown && !rmbWasDown) {
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
            m_WeaponSystem.Update(m_Scene, m_PhysicsSystem, deltaTime);
            m_PhysicsSystem.Update(m_Scene, deltaTime);
            // FIX #2: despawn dei cadaveri dopo maxLifetime (ragdoll Jolt + entity)
            CorpseSystem::Update(m_Scene, m_PhysicsSystem, deltaTime);
            UpdateHitFeedback(deltaTime);
            UpdateTrainingDummies(deltaTime);
            m_WeightSystem.Update(m_Scene, deltaTime);

            m_HordeAI.Update(m_Scene, m_NavQuery, deltaTime);
            m_MeleeCombat.Update(m_Scene, deltaTime);

            // F2: cambia al volo tra personaggio a cuboni e personaggio animato
            {
                const bool f2 = Input::IsKeyPressed(GLFW_KEY_F2);
                if (f2 && !m_F2WasDown) m_UseAnimatedPlayer = !m_UseAnimatedPlayer;
                m_F2WasDown = f2;
            }

            // Comandi reali del giocatore -> animazioni del personaggio animato
            UpdateAnimatedPlayer(deltaTime, BuildPlayerAnimInput(isAiming, deltaTime));

            CheckUnitDeaths();

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

            Renderer::SetViewProjection(m_Camera.GetViewProjection());

            if (g_WaaaghActive) {
                g_WaaaghTimer += deltaTime;

                if (g_WaaaghTimer >= 10.0f) {
                    g_WaaaghTimer = 0.0f;

                    auto livingEnemyView = m_Scene.GetRegistry().view<HealthComponent>(
                        entt::exclude<PlayerTag, CorpseComponent, TrainingDummyComponent>
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

            static float orkAnimTimer = 0.0f;
            orkAnimTimer += deltaTime * 8.0f;

            float totalTime = static_cast<float>(glfwGetTime());

            auto enemyView = m_Scene.GetRegistry().view<TransformComponent, HealthComponent>(
                entt::exclude<PlayerTag, CorpseComponent>
            );

            for (auto enemyEntity : enemyView) {
                auto& enemyTrans = enemyView.get<TransformComponent>(enemyEntity);

                glm::vec3 distVec = transform.Position - enemyTrans.Position;
                float distToPlayer = glm::length(distVec);

                float minDistPlayer = 0.95f;
                glm::vec2 pXZ(transform.Position.x, transform.Position.z);
                glm::vec2 eXZ(enemyTrans.Position.x, enemyTrans.Position.z);
                float distPE = glm::distance(pXZ, eXZ);

                if (distPE < minDistPlayer && distPE > 0.001f) {
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

                    bool isAttacking = mState && (mState->currentPhase != MeleePhase::None);
                    bool isMoving = hordeComp && (hordeComp->state != HordeState::Waiting);

                    float animSpeed = isAttacking ? 4.0f : 10.0f;
                    float legAngle = isAttacking ? 5.0f : (isMoving ? std::sin(totalTime * animSpeed) * 35.0f : 0.0f);
                    for (auto& block : skel.data.blocks) {
                        if (block.name == "Leg_L") block.jointRotation.x = legAngle;
                        else if (block.name == "Leg_R") block.jointRotation.x = -legAngle;
                        else if (block.name == "Arm_R" && !isAttacking) block.jointRotation.x = 25.0f + legAngle * 0.4f;
                        else if (block.name == "Arm_L" && !isAttacking) block.jointRotation.x = 25.0f - legAngle * 0.4f;
                    }
                }
            }
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
            m_DeferredRenderer.EndGeometryPassAndPresent(m_Camera);
        }
        else {
            // Fallback al vecchio renderer (solo se DeferredRenderer non è init)
            m_RenderSystem.Render(m_Scene);
        }
        m_ParticleSystem.Render(m_Scene);
        // (rimossi i due modelli di prova disegnati accanto al giocatore)

        if (m_GameState == GameState::Gameplay) {
            glm::vec2 screen = glm::vec2(m_WindowWidth, m_WindowHeight);
            glm::vec2 center = screen * 0.5f;

            if (m_Scene.GetRegistry().all_of<InventoryComponent>(m_PlayerEntity)) {
                auto& inv = m_Scene.GetRegistry().get<InventoryComponent>(m_PlayerEntity);

                float slotSize = 50.0f;
                float slotSpacing = 10.0f;
                float startX = (screen.x - (2.0f * slotSize + slotSpacing)) * 0.5f;
                float startY = 15.0f;

                for (int i = 0; i < 2; ++i) {
                    glm::vec2 pos = glm::vec2(startX + i * (slotSize + slotSpacing), startY);
                    bool isActive = (i == inv.currentSlot);
                    std::string wId = (i == 0) ? inv.primaryWeaponId : inv.secondaryWeaponId;
                    bool hasWeapon = !wId.empty();

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

            if (m_Scene.GetRegistry().all_of<HealthComponent>(m_PlayerEntity)) {
                auto& health = m_Scene.GetRegistry().get<HealthComponent>(m_PlayerEntity);
                float hpRatio = std::clamp(std::max(health.currentHealth / health.maxHealth, 0.0f), 0.0f, 1.0f);

                float hpBarW = 250.0f;
                float hpBarH = 30.0f;
                glm::vec2 hpBarPos = glm::vec2(screen.x - hpBarW - 30.0f, 30.0f);

                Renderer::DrawQuadUI(hpBarPos, glm::vec2(hpBarW, hpBarH), glm::vec4(0.3f, 0.0f, 0.0f, 0.8f), screen);
                glm::vec4 hpColor = (hpRatio > 0.4f) ? glm::vec4(0.0f, 1.0f, 0.2f, 1.0f) : glm::vec4(1.0f, 0.1f, 0.1f, 1.0f);
                Renderer::DrawQuadUI(hpBarPos, glm::vec2(hpBarW * hpRatio, hpBarH), hpColor, screen);
            }
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
                ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f));
                ImGui::Begin("KillCounterOverlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "ORKI ELIMINATI: %d", m_KillCount);
                ImGui::End();
            }

            if (isPlayerDead) {
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

                if (ImGui::Button("Torna al Menu Principale", ImVec2(-1, 40))) {
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
        std::vector<std::string> owned;
        for (const auto& u : m_PlayerRoster) owned.push_back(u.blueprintId);
        m_ResearchTree.GrandfatherUnits(m_Profile, owned);   // chi ha gia' un garage non perde nulla
        SUN_CORE_INFO("Albero di ricerca caricato: {0} fazioni", m_ResearchTree.GetFactions().size());
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

        float maxCol = 0.0f, maxRow = 0.0f;
        for (const auto& n : faction.nodes) { maxCol = std::max(maxCol, n.col); maxRow = std::max(maxRow, n.row); }
        const float totalW = pad * 2.0f + (maxCol + 1.0f) * nodeW + maxCol * gapX;
        const float totalH = pad * 2.0f + (maxRow + 1.0f) * nodeH + maxRow * gapY;

        auto col32 = [](float r, float g, float b, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(r, g, b, a)); };

        // Selezione valida solo se appartiene a questa fazione
        const ResearchNode* selected = nullptr;
        for (const auto& n : faction.nodes) if (n.id == m_ResearchSelectedNode) selected = &n;
        if (!selected && !faction.nodes.empty()) selected = &faction.nodes.front();

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

        // Nodi
        for (const auto& n : faction.nodes) {
            const ImVec2 p = nodePos(n);
            const ImVec2 p2(p.x + nodeW, p.y + nodeH);
            const ResearchStatus st = m_ResearchTree.GetStatus(m_Profile, n);
            const bool hasBp = BlueprintManager::Get().GetUnit(n.unitId) != nullptr;
            int owned = 0;
            for (const auto& u : m_PlayerRoster) if (u.blueprintId == n.unitId) ++owned;

            ImVec4 border(0.40f, 0.40f, 0.45f, 1.0f);                       // bloccato
            if (st == ResearchStatus::Researched) border = ImVec4(0.30f, 0.85f, 0.40f, 1.0f);
            else if (st == ResearchStatus::Researchable) border = ImVec4(1.0f, 0.80f, 0.20f, 1.0f);

            ImGui::SetCursorScreenPos(p);
            ImGui::InvisibleButton(("##node_" + n.id).c_str(), ImVec2(nodeW, nodeH));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) m_ResearchSelectedNode = n.id;
            const bool isSel = (selected && selected->id == n.id);

            dl->AddRectFilled(p, p2, col32(0.10f, 0.11f, 0.14f), 6.0f);
            dl->AddRectFilled(p, ImVec2(p.x + 7.0f, p2.y), col32(n.color[0], n.color[1], n.color[2]), 6.0f);   // banda colore unita'
            dl->AddRect(p, p2, ImGui::GetColorU32(border), 6.0f, 0, isSel ? 3.0f : (hovered ? 2.5f : 1.5f));

            dl->PushClipRect(p, p2, true);
            const float tx = p.x + 16.0f;
            dl->AddText(ImVec2(tx, p.y + 8.0f), col32(0.95f, 0.95f, 0.95f), n.name.c_str());
            char line2[96];
            std::snprintf(line2, sizeof(line2), "T%d   PS: %lld", n.tier, static_cast<long long>(ResearchTree::GetStudyPoints(m_Profile, n.unitId)));
            dl->AddText(ImVec2(tx, p.y + 30.0f), col32(0.65f, 0.80f, 1.0f), line2);
            char line3[96];
            if (!hasBp) std::snprintf(line3, sizeof(line3), "BLUEPRINT MANCANTE");
            else if (st == ResearchStatus::Researched) std::snprintf(line3, sizeof(line3), "SBLOCCATA   in garage: %d", owned);
            else if (st == ResearchStatus::Researchable) std::snprintf(line3, sizeof(line3), "RICERCA: %lld PS", static_cast<long long>(n.researchCost));
            else if (st == ResearchStatus::LockedPoints) std::snprintf(line3, sizeof(line3), "servono %lld PS", static_cast<long long>(n.researchCost));
            else std::snprintf(line3, sizeof(line3), "BLOCCATA");
            dl->AddText(ImVec2(tx, p.y + 54.0f), ImGui::GetColorU32(border), line3);
            dl->PopClipRect();
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

            ImGui::TextColored(ImVec4(faction.accent[0], faction.accent[1], faction.accent[2], 1.0f), "%s   (Tier %d)", n.name.c_str(), n.tier);
            ImGui::SameLine(0.0f, 20.0f);
            ImGui::TextDisabled("%s", n.unitId.c_str());
            ImGui::Separator();

            ImGui::Columns(2, "##detailcols", false);
            if (bp) {
                ImGui::Text("Salute: %.0f   Velocita': %.1f   Armatura: %d", bp->health, bp->base_speed, bp->armor_rating);
                ImGui::Text("Forza: %.0f   Mischia: %.0f   Mira: %.0f", bp->strength, bp->melee_skill, bp->accuracy_skill);
                ImGui::Text("Costo in punti squadra: %d", bp->point_cost);
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Blueprint '%s' non trovato in GameData/Units.", n.unitId.c_str());
            }
            ImGui::Text("Punti studio di questa unita': %lld", static_cast<long long>(ResearchTree::GetStudyPoints(m_Profile, n.unitId)));
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
                if (parent) {
                    ImGui::Text("Punti studio di %s: %lld / %lld", parent->name.c_str(),
                        static_cast<long long>(ResearchTree::GetStudyPoints(m_Profile, parent->unitId)),
                        static_cast<long long>(n.researchCost));
                }
                if (st == ResearchStatus::LockedParent && parent) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Sblocca prima: %s", parent->name.c_str());
                }
                char label[96];
                std::snprintf(label, sizeof(label), "RICERCA  (-%lld PS)", static_cast<long long>(n.researchCost));
                if (actionButton(label, st == ResearchStatus::Researchable)) {
                    if (m_ResearchTree.Research(m_Profile, n)) setMessage("Ricerca completata: " + n.name);
                }
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

        const char* msg = "NEGOZIO - WORK IN PROGRESS";
        const ImVec2 sz = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - 40.0f - sz.x) * 0.5f, (io.DisplaySize.y - 90.0f) * 0.4f));
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.75f, 1.0f), "%s", msg);
        const char* sub = "Qui si usera' solo la valuta premium (Pietra Nera).";
        const ImVec2 sz2 = ImGui::CalcTextSize(sub);
        ImGui::SetCursorPosX((io.DisplaySize.x - 40.0f - sz2.x) * 0.5f);
        ImGui::TextDisabled("%s", sub);

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
                            for (const auto& [wId, wBP] : BlueprintManager::Get().GetWeapons()) {
                                if (wBP.slot_type == WeaponSlotType::Primary) {
                                    if (ImGui::Selectable(WeaponLabel(wBP.displayName, wId).c_str(), activeUnit.primaryWeaponId == wId)) {
                                        activeUnit.primaryWeaponId = wId;
                                        ApplyUnitVisuals(activeUnit);
                                    }
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip("%s (Fazione: %s | Danno: %.0f)", wBP.displayName.c_str(), wBP.faction.c_str(), wBP.base_damage);
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
                                if (wBP.slot_type == WeaponSlotType::Secondary) {
                                    if (ImGui::Selectable(WeaponLabel(wBP.displayName, wId).c_str(), activeUnit.secondaryWeaponId == wId)) {
                                        activeUnit.secondaryWeaponId = wId;
                                        ApplyUnitVisuals(activeUnit);
                                    }
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip("%s (Fazione: %s | Danno: %.0f)", wBP.displayName.c_str(), wBP.faction.c_str(), wBP.base_damage);
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

                        if (ImGui::BeginCombo("Seleziona Arma Melee", meleeName.c_str())) {

                            if (ImGui::Selectable("[Nessuna / Disarmato]", activeUnit.meleeWeaponId.empty() || activeUnit.meleeWeaponId == "none")) {
                                activeUnit.meleeWeaponId = "";
                                ApplyUnitVisuals(activeUnit);
                            }

                            for (const auto& [wId, wBP] : BlueprintManager::Get().GetWeapons()) {
                                if (wBP.slot_type == WeaponSlotType::Melee || wBP.is_melee) {
                                    if (ImGui::Selectable(WeaponLabel(wBP.displayName, wId).c_str(), activeUnit.meleeWeaponId == wId)) {
                                        activeUnit.meleeWeaponId = wId;
                                        ApplyUnitVisuals(activeUnit);
                                    }
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
                    ImGui::Text("Filtro Categoria Dotazione:");
                    if (ImGui::Button("Granate", ImVec2(100, 25))) m_GearCategoryTab = 0; ImGui::SameLine();
                    if (ImGui::Button("Supporto", ImVec2(100, 25))) m_GearCategoryTab = 1; ImGui::SameLine();
                    if (ImGui::Button("Curativi", ImVec2(100, 25))) m_GearCategoryTab = 2; ImGui::SameLine();
                    if (ImGui::Button("Rinforzi", ImVec2(100, 25))) m_GearCategoryTab = 3;
                    ImGui::Spacing(); ImGui::Spacing();

                    ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "[SISTEMA IN SVILUPPO]");
                    ImGui::Text("Le categorie di dotazione sono in via di integrazione con l'inventario.");
                }
            }
            else if (m_CustomizeMainTab == 2) {
                const auto* unitBp = BlueprintManager::Get().GetUnit(activeUnit.blueprintId);
                float maxWeight = unitBp ? unitBp->weight_capacity : 60.0f;
                float currentWeight = activeUnit.CalculateTotalWeight();
                int unitPoints = activeUnit.CalculateTotalPoints();

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
                            bool isAllowedForThisUnit = aBp.unitRestriction.empty() || (aBp.unitRestriction == activeUnit.blueprintId);

                            if (isCorrectLocation && isAllowedForThisUnit) {
                                bool isSelected = (currentId == aId);
                                std::string itemLabel = aBp.displayName + " | AR: " + std::to_string(aBp.armorRating) +
                                    " | HP: " + std::to_string((int)aBp.maxDurability) +
                                    " | " + std::to_string((int)aBp.pointCost) + " Pti";

                                if (ImGui::Selectable(itemLabel.c_str(), isSelected)) {
                                    currentId = aId;
                                    ApplyUnitVisuals(activeUnit);
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
            "SANDBOX (Esplorazione Libera)"
        };

        for (int i = 0; i < 4; ++i) {
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

        if (m_SelectedEventIndex == 3) {
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
        ImGui::Text("Punti Squadra: %d / %d", squadPoints, MAX_SQUAD_POINTS);
        ImGui::ProgressBar(static_cast<float>(squadPoints) / static_cast<float>(MAX_SQUAD_POINTS), ImVec2(-1, 15), "");
        ImGui::Separator(); ImGui::Spacing();

        for (int i = 0; i < static_cast<int>(m_PlayerRoster.size()); ++i) {
            auto it = std::find(m_SelectedSquadIndices.begin(), m_SelectedSquadIndices.end(), i);
            bool isInSquad = (it != m_SelectedSquadIndices.end());

            const auto* bp = BlueprintManager::Get().GetUnit(m_PlayerRoster[i].blueprintId);
            int cost = bp ? bp->point_cost : 10;

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
                    if (squadPoints + cost <= MAX_SQUAD_POINTS) {
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
                ImGui::SetTooltip("%s\nBlueprint: %s | Costo Punti: %d",
                    m_PlayerRoster[i].customName.c_str(), m_PlayerRoster[i].blueprintId.c_str(), cost);
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
            ImGui::Begin("SQUADRA DISTRUTTA", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
            ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "--- SQUADRONE ELIMINATO ---");
            ImGui::Separator(); ImGui::Spacing();
            ImGui::TextWrapped("Tutti i membri della squadra schierati sono stati travolti.");

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "PUNTEGGIO FINALE:");
            ImGui::Text("Orki Eliminati: %d", m_KillCount);

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
                    const bool animAvailable = m_RifleAnimLoaded && m_SkinnedModelLoaded;
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

} // namespace SunEngine