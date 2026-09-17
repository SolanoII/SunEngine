#define NOMINMAX
#include <glad/glad.h>

#include "GameLayer.h"
#include "Engine/Core/Application/Application.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/MathUtils.h"     // <-- AGGIUNGI
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
#include <algorithm>
#include <cmath>
#include <vector>
#include <memory>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

extern std::string FindValidDir(const std::string& targetDirName);

namespace SunEngine {

    struct ProcessedCorpseTag {};

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
        bool startSoundPlayed = false;

        std::vector<std::unique_ptr<OrkVoicePoolItem>> voicePool;
        std::vector<std::string> variationFiles;

        float nextShoutTimer = 3.0f;
        std::vector<PendingShout> pendingShouts;
    } g_OrkVoices;

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
        scene.GetRegistry().emplace<ColliderComponent>(entity, glm::vec3(0.5f));
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

            float screenW = 1280.0f, screenH = 720.0f;
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
        float screenW = 1280.0f, screenH = 720.0f;

        glm::vec2 center2D = WorldToScreen(origin, vp, screenW, screenH);
        glm::vec2 posX2D = WorldToScreen(origin + glm::vec3(3.0f, 0.0f, 0.0f), vp, screenW, screenH);
        glm::vec2 posY2D = WorldToScreen(origin + glm::vec3(0.0f, 3.0f, 0.0f), vp, screenW, screenH);
        glm::vec2 posZ2D = WorldToScreen(origin + glm::vec3(0.0f, 0.0f, 3.0f), vp, screenW, screenH);

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

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
            ma_sound_seek_to_pcm_frame(&g_OrkVoices.startWaaaghSound, 0);
            ma_sound_set_volume(&g_OrkVoices.startWaaaghSound, voiceVolume);
            ma_sound_start(&g_OrkVoices.startWaaaghSound);
            g_OrkVoices.startSoundPlayed = true;
        }
    }

    static void PlayOrkVoiceAtPos(const glm::vec3& orkPos, const glm::vec3& playerPos, float voiceVolume, float pitchMin = 0.90f, float pitchMax = 1.10f) {
        if (g_OrkVoices.voicePool.empty() || voiceVolume <= 0.001f) return;

        float distToPlayer = glm::distance(orkPos, playerPos);
        if (distToPlayer > 35.0f) return;

        float volume = CalculateSpatialVolume(distToPlayer) * voiceVolume;
        if (volume <= 0.005f) return;

        for (auto& item : g_OrkVoices.voicePool) {
            if (!ma_sound_is_playing(&item->sound)) {
                float randomPitch = Random::Range(pitchMin, pitchMax);
                ma_sound_set_volume(&item->sound, volume);
                ma_sound_set_pitch(&item->sound, randomPitch);
                ma_sound_set_position(&item->sound, orkPos.x, orkPos.y, orkPos.z);
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

        float groundY = GetGroundHeightAt(scene, spawnPos, spawnPos.y);
        enemyTransform.Position = spawnPos;
        enemyTransform.Position.y = groundY;
        enemyTransform.Scale = glm::vec3(1.0f, 1.0f, 1.0f);

        scene.GetRegistry().emplace<MeshComponent>(enemyEntity, 0u);
        scene.GetRegistry().emplace<HealthComponent>(enemyEntity, 50.0f, 50.0f);

        scene.GetRegistry().emplace<ColliderComponent>(enemyEntity, glm::vec3(0.35f, 0.65f, 0.35f));
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

            // --- FIX: adatta il collider ai bounds reali del nuovo skeleton ---
            // Calcoliamo gli 8 vertici del cubo unitario [-0.5, 0.5]^3 per ogni blocco,
            // trasformati dallo skeleton in posa A (rotazioni a zero, posizione zero).
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

            // Il transform.Position del player è "ai piedi". RefreshBodies() centra
            // automaticamente il box Jolt a `Position + halfExtents.y`, quindi il box
            // va da Position.y a Position.y + altezzaReale. Nessun offset extra necessario.
            if (auto* col = scene.GetRegistry().try_get<ColliderComponent>(playerEntity)) {
                col->halfExtents = glm::vec3(
                    std::max(0.10f, (maxX - minX) * 0.5f),
                    std::max(0.10f, (maxY - minY) * 0.5f),
                    std::max(0.10f, (maxZ - minZ) * 0.5f)
                );
            }
            else {
                scene.GetRegistry().emplace<ColliderComponent>(playerEntity, glm::vec3(
                    std::max(0.10f, (maxX - minX) * 0.5f),
                    std::max(0.10f, (maxY - minY) * 0.5f),
                    std::max(0.10f, (maxZ - minZ) * 0.5f)
                ));
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
                wState.burstRemaining = 0;   // <-- AGGIUNGI: evita che una raffica a meta' passi all'arma nuova
                const auto* weaponBP = BlueprintManager::Get().GetWeapon(activeId);
                if (weaponBP) wState.currentAmmo = weaponBP->magazine_size;
            }
        }
    }
    bool GameLayer::ReloadMap(const std::string& mapId) {
        // 1. Smonta la query PRIMA di distruggere la vecchia mesh
        m_NavQuery.Destroy();

        // 2. Carica la nuova mappa (move assignment libera la vecchia NavMesh)
        std::string mapsDir = FindValidDir("Maps");
        if (mapsDir.empty()) mapsDir = "GameData/Maps/";

        m_CurrentMapData = MapLoader::LoadFromFile(m_Scene, mapsDir + mapId + ".json");
        m_PhysicsSystem.RefreshBodies(m_Scene);

        // 3. Aggancia la query alla nuova mesh
        if (m_CurrentMapData.navMesh) {
            if (!m_NavQuery.Init(m_CurrentMapData.navMesh.get())) {
                SUN_CORE_ERROR("NavMeshQuery::Init fallita dopo reload mappa '{0}'", mapId);
                return false;
            }
        }
        else {
            SUN_CORE_WARN("Mappa '{0}' senza NavMesh: pathfinding disabilitato.", mapId);
        }

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
        glViewport(0, 0, width, height);
        m_Camera.SetViewportSize(static_cast<float>(width), static_cast<float>(height));
    }

    void GameLayer::CheckUnitDeaths() {
        auto view = m_Scene.GetRegistry().view<HealthComponent, TransformComponent>(
            entt::exclude<CorpseComponent, PlayerTag>
        );

        for (auto entity : view) {
            auto& health = view.get<HealthComponent>(entity);
            if (health.currentHealth <= 0.0f) {
                SUN_CORE_WARN("Unita' eliminata! Attivazione Ragdoll Jolt Physics...");
                glm::vec3 deathImpulse(0.0f, 1.5f, -3.5f);
                m_PhysicsSystem.KillUnit(m_Scene, entity, deathImpulse);
            }
        }
    }

    GameLayer::GameLayer() : Layer("Game Layer")
    {
        m_PhysicsSystem.Init(m_Scene);

        m_StaticShader.Init();

        std::string modelsDir = FindValidDir("Models");
        if (!modelsDir.empty()) {
            std::string fbxPath = modelsDir + "Characters/guardsman_master.fbx";
            m_TestModelLoaded = m_TestModel.LoadModel(fbxPath);

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

        m_Camera.SetViewportSize(1280.0f, 720.0f);

        m_PlayerEntity = m_Scene.CreateEntity();
        auto& playerTransform = m_Scene.GetRegistry().emplace<TransformComponent>(m_PlayerEntity);
        m_Scene.GetRegistry().emplace<PlayerTag>(m_PlayerEntity);

        playerTransform.Position = glm::vec3(0.0f, 0.0f, 0.0f);
        playerTransform.Scale = glm::vec3(1.0f, 1.0f, 1.0f);

        m_Scene.GetRegistry().emplace<ColliderComponent>(m_PlayerEntity, glm::vec3(0.3f, 0.62f, 0.3f));
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
        if (ma_engine_init(NULL, &m_AudioEngine) != MA_SUCCESS) {
            SUN_CORE_ERROR("ERRORE: Impossibile inizializzare MiniAudio!");
        }
        else {
            // Crea un gruppo per gli SFX (passi, spari, ecc.)
            if (ma_sound_group_init(&m_AudioEngine, 0, NULL, &m_SFXGroup) == MA_SUCCESS) {
                m_SFXGroupInitialized = true;
                ma_sound_group_set_volume(&m_SFXGroup, m_VolumeSFX);
            }
            InitOrkAudio(&m_AudioEngine);

            // Inizializza il pool audio del WeaponSystem (spari positional)
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

        m_SelectedSquadIndices.push_back(0);
        ApplyUnitVisuals(m_PlayerRoster[0]);
    }

    GameLayer::~GameLayer()
    {
        m_NavQuery.Destroy();

        // 1. Ferma i pool audio dei sistemi PRIMA del gruppo (altrimenti i sound
        //    attaccati al gruppo diventano dangling quando il gruppo viene distrutto)
        m_WeaponSystem.ShutdownAudio();
        m_MovementSystem.ShutdownAudio();

        // 2. Voci Ork
        if (g_OrkVoices.startSoundLoaded) {
            ma_sound_uninit(&g_OrkVoices.startWaaaghSound);
            g_OrkVoices.startSoundLoaded = false;
        }
        for (auto& item : g_OrkVoices.voicePool) {
            if (item) ma_sound_uninit(&item->sound);
        }
        g_OrkVoices.voicePool.clear();

        // 3. Gruppo SFX
        if (m_SFXGroupInitialized) {
            ma_sound_group_uninit(&m_SFXGroup);
            m_SFXGroupInitialized = false;
        }

        // 4. Engine
        ma_engine_uninit(&m_AudioEngine);

        // m_CurrentMapData.navMesh verrà liberata automaticamente da unique_ptr
    }

    void GameLayer::OnUpdate(float deltaTime)
    {
        GLFWwindow* window = glfwGetCurrentContext();

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

            m_Camera.SetViewportSize(1280.0f, 720.0f);
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

            m_Camera.SetViewportSize(1280.0f, 720.0f);
            m_Camera.SetCameraOffsets(0.0f, 0.2f, m_GarageCameraDistance);
            m_Camera.SetTarget(garageTargetPos, m_GarageCameraAngle);
            m_Camera.SetPitch(-5.0f);
            m_Camera.Update(glm::vec3(0.0f), 1.0f);

            Renderer::SetViewProjection(m_Camera.GetViewProjection());
            return;
        }
        // --- Rilevamento morte del player giocante (una sola volta per morte) ---
        auto* hp = m_Scene.GetRegistry().try_get<HealthComponent>(m_PlayerEntity);
        bool isPlayerAlive = hp && (hp->currentHealth > 0.0f);

        if (hp && !isPlayerAlive && !m_PlayerDeathHandled) {
            if (m_CurrentSquadMemberIdx >= 0 &&
                m_CurrentSquadMemberIdx < static_cast<int>(m_ActiveSquad.size())) {
                m_ActiveSquad[m_CurrentSquadMemberIdx].isAlive = false;
                m_ActiveSquad[m_CurrentSquadMemberIdx].currentHealth = 0.0f;
            }
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

            auto corpseView = m_Scene.GetRegistry().view<CorpseComponent>(entt::exclude<ProcessedCorpseTag>);
            for (auto entity : corpseView) {
                m_KillCount++;
                m_Scene.GetRegistry().emplace<ProcessedCorpseTag>(entity);
            }

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

            m_MovementSystem.Update(m_Scene, m_PhysicsSystem, &m_AudioEngine, &m_SFXGroup, deltaTime, m_VolumeSFX);
            m_WeaponSystem.Update(m_Scene, m_PhysicsSystem, deltaTime);
            m_PhysicsSystem.Update(m_Scene, deltaTime);
            m_WeightSystem.Update(m_Scene, deltaTime);

            m_HordeAI.Update(m_Scene, m_NavQuery, deltaTime);
            m_MeleeCombat.Update(m_Scene, deltaTime);

            if (m_RifleAnimLoaded) {
                bool fwd = Input::IsKeyPressed(GLFW_KEY_W);
                bool back = Input::IsKeyPressed(GLFW_KEY_S);
                bool left = Input::IsKeyPressed(GLFW_KEY_A);
                bool right = Input::IsKeyPressed(GLFW_KEY_D);
                bool moving = fwd || back || left || right;
                bool sprint = Input::IsKeyPressed(GLFW_KEY_LEFT_SHIFT);

                m_RifleAnimController.Update(deltaTime, moving, fwd, back, left, right, sprint, isAiming);
            }

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
                entt::exclude<PlayerTag, HealthComponent, CorpseComponent>   // <-- AGGIUNTO
            );

            float minHitT = dynamicDistance;
            glm::vec3 rayDir = glm::normalize(idealCamPos - lookTarget);

            for (auto entity : colliderView) {
                const auto& col = colliderView.get<ColliderComponent>(entity);
                const auto& trans = colliderView.get<TransformComponent>(entity);

                glm::vec3 half = col.halfExtents * trans.Scale;
                glm::vec3 minBox = trans.Position - half;
                glm::vec3 maxBox = trans.Position + half;

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
            float groundY = GetGroundHeightAt(m_Scene, projectedCamPos, projectedCamPos.y);
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
            m_Camera.SetViewportSize(1280.0f, 720.0f, extraFov);
            m_Camera.Update(shakeOffset, 1.0f);

            Renderer::SetViewProjection(m_Camera.GetViewProjection());

            if (g_WaaaghActive) {
                g_WaaaghTimer += deltaTime;

                if (g_WaaaghTimer >= 10.0f) {
                    g_WaaaghTimer = 0.0f;

                    auto livingEnemyView = m_Scene.GetRegistry().view<HealthComponent>(
                        entt::exclude<PlayerTag, CorpseComponent>
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

        m_ParticleSystem.Update(m_Scene, deltaTime);

        if (m_Scene.GetRegistry().valid(m_PlayerEntity)) {
            static entt::entity s_LeftWeaponEntity = entt::null;
            if (!m_Scene.GetRegistry().valid(s_LeftWeaponEntity)) {
                s_LeftWeaponEntity = m_Scene.CreateEntity();
                m_Scene.GetRegistry().emplace<TransformComponent>(s_LeftWeaponEntity);
                m_Scene.GetRegistry().emplace<MeshComponent>(s_LeftWeaponEntity, 0u);
                auto& c = m_Scene.GetRegistry().emplace<ColorComponent>(s_LeftWeaponEntity);
                c.Color = glm::vec4(1.0f);
                c.BaseColor = glm::vec4(1.0f);
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
                    if (m_Scene.GetRegistry().all_of<SkeletonComponent>(m_PlayerEntity)) {
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

                    if (m_Scene.GetRegistry().all_of<ColorComponent>(m_WeaponEntity)) {
                        auto& wColor = m_Scene.GetRegistry().get<ColorComponent>(m_WeaponEntity);
                        glm::vec4 col = wBPR->primary_color;
                        if (glm::length(glm::vec3(col)) < 0.05f) col = wBPR->is_melee ? glm::vec4(0.85f, 0.75f, 0.2f, 1.0f) : glm::vec4(0.8f, 0.2f, 0.2f, 1.0f);
                        wColor.Color = col;
                        wColor.BaseColor = col;
                    }
                }
            }

            auto& wTransL = m_Scene.GetRegistry().get<TransformComponent>(s_LeftWeaponEntity);
            if (!leftWeaponId.empty()) {
                const auto* wBPL = BlueprintManager::Get().GetWeapon(leftWeaponId);
                if (wBPL) {
                    if (m_Scene.GetRegistry().all_of<SkeletonComponent>(m_PlayerEntity)) {
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

                    if (m_Scene.GetRegistry().all_of<ColorComponent>(s_LeftWeaponEntity)) {
                        auto& wColorL = m_Scene.GetRegistry().get<ColorComponent>(s_LeftWeaponEntity);
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

    void GameLayer::OnRender()
    {
        m_RenderSystem.Render(m_Scene);
        m_ParticleSystem.Render(m_Scene);
        glm::vec3 debugAnchorPos(0.0f);
        if (m_Scene.GetRegistry().valid(m_PlayerEntity) &&
            m_Scene.GetRegistry().all_of<TransformComponent>(m_PlayerEntity)) {
            debugAnchorPos = m_Scene.GetRegistry().get<TransformComponent>(m_PlayerEntity).Position;
        }

        if (m_TestModelLoaded) {
            m_StaticShader.Bind();

            glm::mat4 modelMat = glm::translate(glm::mat4(1.0f), debugAnchorPos + glm::vec3(2.0f, 0.0f, 0.0f));
            modelMat = glm::scale(modelMat, glm::vec3(0.01f));
            glUniformMatrix4fv(glGetUniformLocation(m_StaticShader.ID, "u_Model"), 1, GL_FALSE, glm::value_ptr(modelMat));
            glUniformMatrix4fv(glGetUniformLocation(m_StaticShader.ID, "u_ViewProjection"), 1, GL_FALSE, glm::value_ptr(m_Camera.GetViewProjection()));
            glUniform4f(glGetUniformLocation(m_StaticShader.ID, "u_Color"), 0.65f, 0.50f, 0.35f, 1.0f);

            m_TestModel.Draw(m_StaticShader.ID);

            m_StaticShader.Unbind();
        }

        if (m_SkinnedModelLoaded) {
            m_SkinnedShader.Bind();

            glm::mat4 modelMat = glm::translate(glm::mat4(1.0f), debugAnchorPos + glm::vec3(4.0f, 0.0f, 0.0f));
            modelMat = glm::scale(modelMat, glm::vec3(0.01f));

            static std::vector<glm::mat4> s_IdentityBones;
            const std::vector<glm::mat4>* boneMatrices;
            if (m_RifleAnimLoaded) {
                boneMatrices = &m_RifleAnimController.GetFinalBoneMatrices();
                if (boneMatrices->empty()) {
                    if (s_IdentityBones.empty()) s_IdentityBones.assign(m_SkinnedTestModel.boneInfoMap.size(), glm::mat4(1.0f));
                    boneMatrices = &s_IdentityBones;
                }
            }
            else {
                if (s_IdentityBones.empty()) s_IdentityBones.assign(m_SkinnedTestModel.boneInfoMap.size(), glm::mat4(1.0f));
                boneMatrices = &s_IdentityBones;
            }

            glUniformMatrix4fv(glGetUniformLocation(m_SkinnedShader.ID, "u_Model"), 1, GL_FALSE, glm::value_ptr(modelMat));
            glUniformMatrix4fv(glGetUniformLocation(m_SkinnedShader.ID, "u_ViewProjection"), 1, GL_FALSE, glm::value_ptr(m_Camera.GetViewProjection()));
            glUniform4f(glGetUniformLocation(m_SkinnedShader.ID, "u_Color"), 0.55f, 0.55f, 0.60f, 1.0f);
            glUniformMatrix4fv(glGetUniformLocation(m_SkinnedShader.ID, "u_BoneMatrices"), (GLsizei)boneMatrices->size(), GL_FALSE, glm::value_ptr((*boneMatrices)[0]));

            m_SkinnedTestModel.Draw();

            m_SkinnedShader.Unbind();
        }

        if (m_GameState == GameState::Gameplay) {
            glm::vec2 screen = glm::vec2(1280.0f, 720.0f);
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

                Renderer::DrawQuadUI(glm::vec2(1000.0f, 30.0f), glm::vec2(250.0f, 30.0f), glm::vec4(0.3f, 0.0f, 0.0f, 0.8f), screen);
                glm::vec4 hpColor = (hpRatio > 0.4f) ? glm::vec4(0.0f, 1.0f, 0.2f, 1.0f) : glm::vec4(1.0f, 0.1f, 0.1f, 1.0f);
                Renderer::DrawQuadUI(glm::vec2(1000.0f, 30.0f), glm::vec2(250.0f * hpRatio, 30.0f), hpColor, screen);
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
                    if (window) glfwSetWindowShouldClose(window, GLFW_TRUE);
                    std::exit(0);
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

        ImGui::SetCursorPos(ImVec2(io.DisplaySize.x * 0.5f - 340.0f, 10));

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);

        if (m_GameState == GameState::MainMenu && m_CurrentTab == MenuTab::StasisVault)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1.0f));
        else
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.1f, 0.15f, 1.0f));

        if (ImGui::Button("CRIPTA DI STASI (GARAGE)", ImVec2(190, 40))) {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::StasisVault;
            if (!m_PlayerRoster.empty() && m_SelectedVaultIndex < static_cast<int>(m_PlayerRoster.size())) {
                ApplyUnitVisuals(m_PlayerRoster[m_SelectedVaultIndex]);
            }
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        if (m_GameState == GameState::MainMenu && m_CurrentTab == MenuTab::Deployment)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1.0f));
        else
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.1f, 0.15f, 1.0f));

        if (ImGui::Button("SCHIERAMENTO SQUADRA", ImVec2(190, 40))) {
            m_GameState = GameState::MainMenu;
            m_CurrentTab = MenuTab::Deployment;
            if (!m_SelectedSquadIndices.empty() && m_SelectedSquadIndices[0] < static_cast<int>(m_PlayerRoster.size())) {
                ApplyUnitVisuals(m_PlayerRoster[m_SelectedSquadIndices[0]]);
            }
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        if (m_GameState == GameState::MapEditor)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.0f, 1.0f));
        else
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.20f, 1.0f));

        if (ImGui::Button("EDITOR MAPPE", ImVec2(150, 40))) {
            m_GameState = GameState::MapEditor;
            if (m_EditorMap.structures.empty()) {
                m_EditorMap.playerSpawns.push_back(glm::vec3(0.0f, 0.5f, 10.0f));
                m_EditorMap.enemySpawns.push_back(glm::vec3(0.0f, 0.5f, -10.0f));
            }
            SyncEditorEntitiesWithScene();
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.20f, 1.0f));
        if (ImGui::Button("OPZIONI", ImVec2(110, 40))) {
            m_ShowOptionsMenu = true;
        }
        ImGui::PopStyleColor();

        ImGui::PopStyleVar();
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

            std::string btnText = m_PlayerRoster[i].customName + "\n(" + m_PlayerRoster[i].blueprintId + ")";
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
            if (ImGui::Button("+ ADDESTRA NUOVA UNITA", ImVec2(-1, 40))) {
                m_ShowRecruitModal = true;
                m_RecruitStep = 0;
                m_SelectedFaction = "";
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
                strcpy_s(nameBuf, activeUnit.customName.c_str());
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
                                    if (ImGui::Selectable(wBP.displayName.c_str(), activeUnit.primaryWeaponId == wId)) {
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
                                    if (ImGui::Selectable(wBP.displayName.c_str(), activeUnit.secondaryWeaponId == wId)) {
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
                                    if (ImGui::Selectable(wBP.displayName.c_str(), activeUnit.meleeWeaponId == wId)) {
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
            char idBuf[64]; strcpy_s(idBuf, m_EditorMap.id.c_str());
            if (ImGui::InputText("ID File (.json)", idBuf, IM_ARRAYSIZE(idBuf))) m_EditorMap.id = idBuf;

            char nameBuf[128]; strcpy_s(nameBuf, m_EditorMap.name.c_str());
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

        const char* defaultMaps[] = { "warzone_sector4", "vaulting_test" };
        static int selectedMapComboIdx = 0;

        if (ImGui::BeginCombo("Mappa Predefinita", defaultMaps[selectedMapComboIdx])) {
            for (int i = 0; i < 2; ++i) {
                bool isSelected = (selectedMapComboIdx == i);
                if (ImGui::Selectable(defaultMaps[i], isSelected)) {
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
        strcpy_s(customMapBuf, m_SelectedMapId.c_str());
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

                        // Reset stato combattimento del player (respawn pulito)
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
                        }

                        // Reset del flag di morte: senza questo la prossima morte non triggera
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