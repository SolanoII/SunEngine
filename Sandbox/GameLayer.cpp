#define NOMINMAX
#include <glad/glad.h>

#include "GameLayer.h"
#include "Engine/Core/Application/Application.h"
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

struct ProcessedCorpseTag {};

struct RaycastHitResult {
    bool hasHit = false;
    float distance = 10000.0f;
    glm::vec3 hitPoint{ 0.0f };
    entt::entity hitEntity = entt::null;
    bool isStaticObstacle = false;
};

static float LerpAngle(float a, float b, float t) {
    float diff = std::fmod(b - a + 180.0f, 360.0f) - 180.0f;
    if (diff < -180.0f) diff += 360.0f;
    return a + diff * std::clamp(t, 0.0f, 1.0f);
}

static glm::vec2 WorldToScreen(const glm::vec3& worldPos, const glm::mat4& viewProj, float screenWidth, float screenHeight) {
    glm::vec4 clipPos = viewProj * glm::vec4(worldPos, 1.0f);
    if (clipPos.w <= 0.001f) return glm::vec2(-10000.0f);
    glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
    float x = (ndc.x + 1.0f) * 0.5f * screenWidth;
    float y = (1.0f - ndc.y) * 0.5f * screenHeight;
    return glm::vec2(x, y);
}

static float GetGroundHeightAt(SunEngine::Scene& scene, const glm::vec3& position, float currentY = 0.0f) {
    float maxGroundY = 0.0f;
    auto view = scene.GetRegistry().view<SunEngine::ColliderComponent, SunEngine::TransformComponent>(
        entt::exclude<SunEngine::PlayerTag, SunEngine::HealthComponent>
    );

    for (auto entity : view) {
        const auto& col = view.get<SunEngine::ColliderComponent>(entity);
        const auto& trans = view.get<SunEngine::TransformComponent>(entity);

        glm::vec3 halfExtents = col.halfExtents * trans.Scale;
        glm::vec3 minBox = trans.Position - halfExtents;
        glm::vec3 maxBox = trans.Position + halfExtents;

        if (position.x >= minBox.x - 0.2f && position.x <= maxBox.x + 0.2f &&
            position.z >= minBox.z - 0.2f && position.z <= maxBox.z + 0.2f) {

            if (maxBox.y > maxGroundY && maxBox.y <= currentY + 0.6f) {
                maxGroundY = maxBox.y;
            }
        }
    }
    return maxGroundY;
}

static RaycastHitResult RaycastSceneFull(SunEngine::Scene& scene, const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDist, entt::entity ignoreEntity) {
    RaycastHitResult closestHit;
    closestHit.distance = maxDist;

    auto staticView = scene.GetRegistry().view<SunEngine::ColliderComponent, SunEngine::TransformComponent>(
        entt::exclude<SunEngine::PlayerTag, SunEngine::HealthComponent, SunEngine::ProjectileComponent>
    );

    for (auto entity : staticView) {
        const auto& col = staticView.get<SunEngine::ColliderComponent>(entity);
        const auto& trans = staticView.get<SunEngine::TransformComponent>(entity);

        glm::vec3 halfExtents = col.halfExtents * trans.Scale;
        glm::vec3 minBox = trans.Position - halfExtents;
        glm::vec3 maxBox = trans.Position + halfExtents;

        float tMin = 0.0f, tMax = closestHit.distance;
        bool hit = true;

        for (int i = 0; i < 3; ++i) {
            if (std::abs(rayDir[i]) < 0.0001f) {
                if (rayOrigin[i] < minBox[i] || rayOrigin[i] > maxBox[i]) { hit = false; break; }
            }
            else {
                float invD = 1.0f / rayDir[i];
                float t0 = (minBox[i] - rayOrigin[i]) * invD;
                float t1 = (maxBox[i] - rayOrigin[i]) * invD;
                if (invD < 0.0f) std::swap(t0, t1);
                tMin = std::max(tMin, t0);
                tMax = std::min(tMax, t1);
                if (tMax < tMin) { hit = false; break; }
            }
        }

        if (hit && tMin > 0.0f && tMin < closestHit.distance) {
            closestHit.hasHit = true;
            closestHit.distance = tMin;
            closestHit.hitPoint = rayOrigin + rayDir * tMin;
            closestHit.hitEntity = entity;
            closestHit.isStaticObstacle = true;
        }
    }

    auto unitView = scene.GetRegistry().view<SunEngine::TransformComponent, SunEngine::HealthComponent>(
        entt::exclude<SunEngine::ProjectileComponent, SunEngine::CorpseComponent>
    );

    for (auto entity : unitView) {
        if (entity == ignoreEntity) continue;

        const auto& trans = unitView.get<SunEngine::TransformComponent>(entity);
        glm::vec3 size(0.8f, 1.8f, 0.8f);

        if (scene.GetRegistry().all_of<SunEngine::ColliderComponent>(entity)) {
            const auto& col = scene.GetRegistry().get<SunEngine::ColliderComponent>(entity);
            size = col.halfExtents * 2.0f * trans.Scale;
        }

        glm::vec3 minBox = trans.Position - (size * 0.5f);
        glm::vec3 maxBox = trans.Position + (size * 0.5f);

        float tMin = 0.0f, tMax = closestHit.distance;
        bool hit = true;

        for (int i = 0; i < 3; ++i) {
            if (std::abs(rayDir[i]) < 0.0001f) {
                if (rayOrigin[i] < minBox[i] || rayOrigin[i] > maxBox[i]) { hit = false; break; }
            }
            else {
                float invD = 1.0f / rayDir[i];
                float t0 = (minBox[i] - rayOrigin[i]) * invD;
                float t1 = (maxBox[i] - rayOrigin[i]) * invD;
                if (invD < 0.0f) std::swap(t0, t1);
                tMin = std::max(tMin, t0);
                tMax = std::min(tMax, t1);
                if (tMax < tMin) { hit = false; break; }
            }
        }

        if (hit && tMin > 0.0f && tMin < closestHit.distance) {
            closestHit.hasHit = true;
            closestHit.distance = tMin;
            closestHit.hitPoint = rayOrigin + rayDir * tMin;
            closestHit.hitEntity = entity;
            closestHit.isStaticObstacle = false;
        }
    }

    return closestHit;
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

float CalculateSpatialVolume(float distance) {
    if (distance <= 1.0f) return 1.0f;
    if (distance >= 101.0f) return 0.0f;
    return 1.0f - ((distance - 1.0f) * 0.01f);
}

static void SpawnSubBlock(SunEngine::Scene& scene, SunEngine::PhysicsSystem& physicsSystem,
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

    auto& trans = scene.GetRegistry().emplace<SunEngine::TransformComponent>(entity);
    trans.Position = glm::vec3(worldMat[3]);
    trans.Rotation = parentRotDeg + localRotDeg;
    trans.Scale = localScale;

    scene.GetRegistry().emplace<SunEngine::MeshComponent>(entity, 0u);
    scene.GetRegistry().emplace<SunEngine::ColliderComponent>(entity, glm::vec3(0.5f));
    scene.GetRegistry().emplace<SunEngine::RigidBodyComponent>(entity, glm::vec3(0.0f), 0.0f, true);

    auto& c = scene.GetRegistry().emplace<SunEngine::ColorComponent>(entity);
    c.Color = isSelected ? glm::vec4(1.0f, 0.85f, 0.1f, 1.0f) : color;
    c.BaseColor = c.Color;
}

void GameLayer::SyncEditorEntitiesWithScene() {
    auto view = m_Scene.GetRegistry().view<SunEngine::ColliderComponent>(entt::exclude<SunEngine::PlayerTag>);
    for (auto e : view) {
        m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
        m_Scene.DestroyEntity(e);
    }

    auto terrainEntity = m_Scene.CreateEntity();
    auto& tTrans = m_Scene.GetRegistry().emplace<SunEngine::TransformComponent>(terrainEntity);
    tTrans.Position = glm::vec3(0.0f, -0.5f, 0.0f);
    tTrans.Scale = m_EditorMap.terrainSize;

    m_Scene.GetRegistry().emplace<SunEngine::MeshComponent>(terrainEntity, 0u);
    m_Scene.GetRegistry().emplace<SunEngine::ColliderComponent>(terrainEntity, glm::vec3(0.5f));
    m_Scene.GetRegistry().emplace<SunEngine::RigidBodyComponent>(terrainEntity, glm::vec3(0.0f), 0.0f, true);

    auto& tColor = m_Scene.GetRegistry().emplace<SunEngine::ColorComponent>(terrainEntity);
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

    int randomId = rand() % 9000 + 1000;
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

    if (SunEngine::Input::IsKeyPressed(GLFW_KEY_W) && !SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_GizmoMode = EditorGizmoMode::Translate;
    if (SunEngine::Input::IsKeyPressed(GLFW_KEY_E) && !SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_GizmoMode = EditorGizmoMode::Rotate;
    if (SunEngine::Input::IsKeyPressed(GLFW_KEY_R) && !SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_GizmoMode = EditorGizmoMode::Scale;

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) return;

    bool leftClicked = SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);

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
        glm::vec2 mouseDelta = SunEngine::Input::GetMouseDelta();

        if (leftClicked && (std::abs(mouseDelta.x) > 0.01f || std::abs(mouseDelta.y) > 0.01f)) {
            float speed = 0.15f;

            if (m_GizmoMode == EditorGizmoMode::Translate) {
                if (SunEngine::Input::IsKeyPressed(GLFW_KEY_LEFT_SHIFT)) {
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

void InitOrkAudio(ma_engine* engine) {
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

void PlayStartWaaagh(float voiceVolume) {
    if (g_OrkVoices.startSoundLoaded && voiceVolume > 0.001f) {
        ma_sound_seek_to_pcm_frame(&g_OrkVoices.startWaaaghSound, 0);
        ma_sound_set_volume(&g_OrkVoices.startWaaaghSound, voiceVolume);
        ma_sound_start(&g_OrkVoices.startWaaaghSound);
        g_OrkVoices.startSoundPlayed = true;
    }
}

void PlayOrkVoiceAtPos(const glm::vec3& orkPos, const glm::vec3& playerPos, float voiceVolume, float pitchMin = 0.90f, float pitchMax = 1.10f) {
    if (g_OrkVoices.voicePool.empty() || voiceVolume <= 0.001f) return;

    float volume = CalculateSpatialVolume(glm::distance(orkPos, playerPos)) * voiceVolume;
    if (volume <= 0.005f) return;

    for (auto& item : g_OrkVoices.voicePool) {
        if (!ma_sound_is_playing(&item->sound)) {
            float randomFactor = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
            float randomPitch = pitchMin + randomFactor * (pitchMax - pitchMin);

            ma_sound_set_volume(&item->sound, volume);
            ma_sound_set_pitch(&item->sound, randomPitch);
            ma_sound_set_position(&item->sound, orkPos.x, orkPos.y, orkPos.z);
            ma_sound_seek_to_pcm_frame(&item->sound, 0);
            ma_sound_start(&item->sound);
            break;
        }
    }
}

void TriggerOrkShoutAndProvoke(entt::entity speakerEntity, SunEngine::Scene& scene, const glm::vec3& playerPos, float voiceVolume) {
    if (!scene.GetRegistry().valid(speakerEntity)) return;

    const auto& speakerPos = scene.GetRegistry().get<SunEngine::TransformComponent>(speakerEntity).Position;
    PlayOrkVoiceAtPos(speakerPos, playerPos, voiceVolume);

    auto orkView = scene.GetRegistry().view<SunEngine::TransformComponent, SunEngine::HealthComponent>(
        entt::exclude<SunEngine::PlayerTag, SunEngine::CorpseComponent>
    );

    for (auto otherEntity : orkView) {
        if (otherEntity == speakerEntity) continue;
        if (g_OrkVoices.pendingShouts.size() >= 6) break;

        const auto& otherPos = orkView.get<SunEngine::TransformComponent>(otherEntity).Position;
        float distToSpeaker = glm::distance(speakerPos, otherPos);
        float perceivedVolume = CalculateSpatialVolume(distToSpeaker);

        if (perceivedVolume > 0.05f) {
            float provocationChance = perceivedVolume * 0.65f;
            float roll = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);

            if (roll < provocationChance) {
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
                    shout.delayTimer = 0.2f + (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * 0.6f;
                    g_OrkVoices.pendingShouts.push_back(shout);
                }
            }
        }
    }
}

void UpdateOrkVoices(SunEngine::Scene& scene, entt::entity playerEntity, float deltaTime, float voiceVolume) {
    std::vector<entt::entity> livingOrks;
    auto orkView = scene.GetRegistry().view<SunEngine::TransformComponent, SunEngine::HealthComponent>(
        entt::exclude<SunEngine::PlayerTag, SunEngine::CorpseComponent>
    );

    for (auto entity : orkView) {
        livingOrks.push_back(entity);
    }

    if (livingOrks.empty()) {
        g_OrkVoices.pendingShouts.clear();
        return;
    }

    const auto& playerPos = scene.GetRegistry().get<SunEngine::TransformComponent>(playerEntity).Position;

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
        entt::entity leaderOrk = livingOrks[rand() % livingOrks.size()];
        TriggerOrkShoutAndProvoke(leaderOrk, scene, playerPos, voiceVolume);

        float baseCooldown = 4.0f + static_cast<float>(rand()) / static_cast<float>(RAND_MAX) * 3.0f;
        g_OrkVoices.nextShoutTimer = std::max(1.5f, baseCooldown / std::sqrt(static_cast<float>(livingOrks.size())));
    }
}

static void SpawnOrkBoy(SunEngine::Scene& scene, const glm::vec3& spawnPos) {
    auto enemyEntity = scene.CreateEntity();
    auto& enemyTransform = scene.GetRegistry().emplace<SunEngine::TransformComponent>(enemyEntity);

    enemyTransform.Position = spawnPos;
    enemyTransform.Position.y = 0.0f;
    enemyTransform.Scale = glm::vec3(1.0f, 1.0f, 1.0f);

    scene.GetRegistry().emplace<SunEngine::MeshComponent>(enemyEntity, 0u);
    scene.GetRegistry().emplace<SunEngine::HealthComponent>(enemyEntity, 50.0f, 50.0f);

    scene.GetRegistry().emplace<SunEngine::ColliderComponent>(enemyEntity, glm::vec3(0.35f, 0.65f, 0.35f));
    scene.GetRegistry().emplace<SunEngine::RigidBodyComponent>(enemyEntity, glm::vec3(0.0f), 0.0f, false);

    auto& enemyColor = scene.GetRegistry().emplace<SunEngine::ColorComponent>(enemyEntity);
    enemyColor.Color = glm::vec4(0.15f, 0.60f, 0.15f, 1.0f);
    enemyColor.BaseColor = enemyColor.Color;

    auto& enemyStats = scene.GetRegistry().emplace<SunEngine::UnitStatsComponent>(enemyEntity);
    enemyStats.armorSet.torso.armorRating = 2;
    enemyStats.armorSet.torso.maxDurability = 50.0f;
    enemyStats.armorSet.torso.currentDurability = 50.0f;

    auto& footstep = scene.GetRegistry().emplace<SunEngine::FootstepComponent>(enemyEntity);
    footstep.currentSurface = SunEngine::SurfaceType::Concrete;
    footstep.stepInterval = 0.50f;

    std::string skeletonsDir = FindValidDir("Skeletons");
    auto orkSkeleton = SunEngine::SkeletonLoader::LoadFromFile(skeletonsDir + "orks_boy.json");
    if (orkSkeleton) {
        auto& skelComp = scene.GetRegistry().emplace_or_replace<SunEngine::SkeletonComponent>(enemyEntity);
        skelComp.data = *orkSkeleton;
        skelComp.scale = 1.0f;
        skelComp.blockFlashTimers.assign(skelComp.data.blocks.size(), 0.0f);
    }
}

void FireWeapon(entt::registry& registry, entt::entity playerEntity, ma_engine* audioEngine, SunEngine::Scene& scene, entt::entity weaponEntity, float sfxVolume) {
    auto& weapon = registry.get<SunEngine::WeaponComponent>(playerEntity);
    auto& emitter = registry.get<SunEngine::ParticleEmitterComponent>(playerEntity);
    auto& trans = registry.get<SunEngine::TransformComponent>(playerEntity);

    auto* wState = registry.try_get<SunEngine::WeaponStateComponent>(playerEntity);
    const std::string& weaponId = registry.get<SunEngine::WeaponBlueprintId>(playerEntity).id;
    const auto* weaponBP = SunEngine::BlueprintManager::Get().GetWeapon(weaponId);

    if (weapon.cooldown > 0.0f) return;
    if (wState) {
        if (wState->isReloading || wState->isOverheating) return;
        if (weaponBP && weaponBP->magazine_size > 0 && wState->currentAmmo <= 0) {
            wState->isReloading = true;
            wState->reloadTimer = weaponBP->reload_time_sec;
            return;
        }
    }

    if (sfxVolume > 0.001f && audioEngine != nullptr) {
        if (std::filesystem::exists(weapon.shootSound)) {
            ma_engine_set_volume(audioEngine, sfxVolume);
            ma_engine_play_sound(audioEngine, weapon.shootSound.c_str(), NULL);
        }
    }

    emitter.isEmitting = true;
    weapon.cooldown = weapon.fireRate;

    if (wState && weaponBP) {
        if (weaponBP->magazine_size > 0) {
            wState->currentAmmo--;
            if (wState->currentAmmo <= 0) {
                wState->isReloading = true;
                wState->reloadTimer = weaponBP->reload_time_sec;
            }
        }
    }

    auto* inv = registry.try_get<SunEngine::InventoryComponent>(playerEntity);
    bool isLeftHandShot = (inv && inv->GetEquipStance() == SunEngine::EquipStance::HybridRangedMelee);

    glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
    rot = glm::rotate(rot, glm::radians(trans.AimPitch), glm::vec3(1.0f, 0.0f, 0.0f));
    glm::vec3 shootDir = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

    float handSideOffset = isLeftHandShot ? -0.4f : 0.4f;
    glm::vec3 muzzlePos = trans.Position + glm::vec3(handSideOffset, 1.25f, 0.0f) + shootDir * 0.5f;

    if (wState && wState->currentFireMode == "shotgun") {
        int pelletCount = 6;
        for (int i = 0; i < pelletCount; ++i) {
            float spreadX = ((rand() % 100) / 100.0f - 0.5f) * 0.12f;
            float spreadY = ((rand() % 100) / 100.0f - 0.5f) * 0.12f;
            glm::vec3 pelletDir = glm::normalize(shootDir + glm::vec3(spreadX, spreadY, spreadX));

            auto projEntity = scene.CreateEntity();
            auto& pTrans = registry.emplace<SunEngine::TransformComponent>(projEntity);
            pTrans.Position = muzzlePos;
            pTrans.Rotation = glm::vec3(trans.AimPitch, trans.AimYaw, 0.0f);
            pTrans.Scale = glm::vec3(0.10f, 0.10f, 0.30f);

            scene.GetRegistry().emplace<SunEngine::MeshComponent>(projEntity, 0u);
            auto& pColor = registry.emplace<SunEngine::ColorComponent>(projEntity);
            pColor.Color = glm::vec4(1.0f, 0.4f, 0.1f, 1.0f);
            pColor.BaseColor = pColor.Color;

            auto& proj = registry.emplace<SunEngine::ProjectileComponent>(projEntity);
            proj.velocity = pelletDir * 45.0f;
            proj.damage = 18.0f;
            proj.lifetime = 1.5f;
            proj.owner = playerEntity;
        }
        return;
    }

    bool isLaserWeapon = (weaponId.find("las") != std::string::npos || weaponId.find("plasma") != std::string::npos);

    if (isLaserWeapon) {
        RaycastHitResult hit = RaycastSceneFull(scene, muzzlePos, shootDir, 200.0f, playerEntity);
        if (hit.hasHit && !hit.isStaticObstacle && hit.hitEntity != entt::null) {
            if (registry.all_of<SunEngine::HealthComponent>(hit.hitEntity)) {
                auto& hp = registry.get<SunEngine::HealthComponent>(hit.hitEntity);
                hp.currentHealth -= 25.0f;
            }
        }

        glm::vec3 hitPoint = hit.hasHit ? hit.hitPoint : (muzzlePos + shootDir * 100.0f);
        auto beamEntity = scene.CreateEntity();
        auto& bTrans = registry.emplace<SunEngine::TransformComponent>(beamEntity);
        bTrans.Position = (muzzlePos + hitPoint) * 0.5f;
        float beamLength = glm::distance(muzzlePos, hitPoint);
        bTrans.Scale = glm::vec3(0.08f, 0.08f, beamLength);
        bTrans.Rotation = glm::vec3(trans.AimPitch, trans.AimYaw, 0.0f);

        registry.emplace<SunEngine::MeshComponent>(beamEntity, 0u);
        auto& bColor = registry.emplace<SunEngine::ColorComponent>(beamEntity);
        glm::vec4 laserColor = weaponBP ? weaponBP->primary_color : glm::vec4(1.0f, 0.2f, 0.1f, 1.0f);
        bColor.Color = laserColor;
        bColor.BaseColor = laserColor;

        auto& proj = registry.emplace<SunEngine::ProjectileComponent>(beamEntity);
        proj.velocity = glm::vec3(0.0f);
        proj.damage = 0.0f;
        proj.lifetime = 0.08f;
    }
    else {
        auto projEntity = scene.CreateEntity();
        auto& pTrans = registry.emplace<SunEngine::TransformComponent>(projEntity);
        pTrans.Position = muzzlePos;
        pTrans.Rotation = glm::vec3(trans.AimPitch, trans.AimYaw, 0.0f);
        pTrans.Scale = glm::vec3(0.20f, 0.20f, 0.80f);

        scene.GetRegistry().emplace<SunEngine::MeshComponent>(projEntity, 0u);
        auto& pColor = registry.emplace<SunEngine::ColorComponent>(projEntity);
        glm::vec4 projColor = weaponBP ? weaponBP->primary_color : glm::vec4(1.0f, 0.75f, 0.1f, 1.0f);
        pColor.Color = projColor;
        pColor.BaseColor = projColor;

        auto& proj = registry.emplace<SunEngine::ProjectileComponent>(projEntity);
        proj.velocity = shootDir * 55.0f;
        proj.damage = 25.0f;
        proj.lifetime = 3.5f;
        proj.owner = playerEntity;
        proj.isLaser = false;
    }
}

static void UpdateProjectiles(SunEngine::Scene& scene, float deltaTime) {
    auto view = scene.GetRegistry().view<SunEngine::ProjectileComponent, SunEngine::TransformComponent>();

    std::vector<entt::entity> toDestroy;

    for (auto entity : view) {
        auto& proj = view.get<SunEngine::ProjectileComponent>(entity);
        auto& trans = view.get<SunEngine::TransformComponent>(entity);

        proj.lifetime -= deltaTime;
        if (proj.lifetime <= 0.0f) {
            toDestroy.push_back(entity);
            continue;
        }

        if (glm::length(proj.velocity) > 0.001f) {
            glm::vec3 moveStep = proj.velocity * deltaTime;
            float stepDist = glm::length(moveStep);
            if (stepDist > 0.0001f) {
                glm::vec3 stepDir = moveStep / stepDist;
                RaycastHitResult hit = RaycastSceneFull(scene, trans.Position, stepDir, stepDist, proj.owner);

                if (hit.hasHit) {
                    if (!hit.isStaticObstacle && hit.hitEntity != entt::null) {
                        if (scene.GetRegistry().all_of<SunEngine::HealthComponent>(hit.hitEntity)) {
                            auto& hp = scene.GetRegistry().get<SunEngine::HealthComponent>(hit.hitEntity);
                            hp.currentHealth -= proj.damage;
                        }
                    }
                    toDestroy.push_back(entity);
                }
                else {
                    trans.Position += moveStep;
                }
            }
        }
    }

    for (auto e : toDestroy) {
        if (scene.GetRegistry().valid(e)) {
            scene.DestroyEntity(e);
        }
    }
}

void UpdateWeapons(entt::registry& registry, float deltaTime) {
    auto view = registry.view<SunEngine::WeaponComponent, SunEngine::ParticleEmitterComponent>();

    for (auto entity : view) {
        auto& weapon = view.get<SunEngine::WeaponComponent>(entity);
        auto& emitter = view.get<SunEngine::ParticleEmitterComponent>(entity);

        if (weapon.cooldown > 0.0f) weapon.cooldown -= deltaTime;

        if (emitter.isEmitting) {
            emitter.duration -= deltaTime;
            if (emitter.duration <= 0.0f) {
                emitter.isEmitting = false;
                emitter.duration = 0.05f;
            }
        }
    }
}

void SwapPlayerUnit(SunEngine::Scene& scene, entt::entity playerEntity, const std::string& newUnitId) {
    const auto* bp = SunEngine::BlueprintManager::Get().GetUnit(newUnitId);
    if (!bp) return;

    scene.GetRegistry().emplace_or_replace<SunEngine::MeshComponent>(playerEntity, 0u);
    scene.GetRegistry().get_or_emplace<SunEngine::UnitBlueprintId>(playerEntity).id = newUnitId;

    auto& stats = scene.GetRegistry().get_or_emplace<SunEngine::UnitStatsComponent>(playerEntity);
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
    stats.suppressionThreshold = bp->suppression_threshold;
    stats.visionRange = bp->vision_range;
    stats.fovDegrees = bp->fov_degrees;
    stats.weightCapacity = bp->weight_capacity;

    std::string skeletonsDir = FindValidDir("Skeletons");
    std::string skelPath = skeletonsDir + newUnitId + ".json";
    auto skeletonData = SunEngine::SkeletonLoader::LoadFromFile(skelPath);
    if (skeletonData) {
        auto& skelComp = scene.GetRegistry().emplace_or_replace<SunEngine::SkeletonComponent>(playerEntity);
        skelComp.data = *skeletonData;
        skelComp.scale = 1.0f;
        skelComp.blockFlashTimers.assign(skelComp.data.blocks.size(), 0.0f);
    }

    glm::vec4 unitColor(0.8f, 0.8f, 0.8f, 1.0f);
    if (newUnitId == "imperium_guardsman_recruit") unitColor = glm::vec4(0.65f, 0.50f, 0.35f, 1.0f);
    else if (newUnitId == "imperium_kasrkin") unitColor = glm::vec4(0.35f, 0.25f, 0.18f, 1.0f);
    else if (newUnitId == "spacemarines_neophyte") unitColor = glm::vec4(0.92f, 0.72f, 0.68f, 1.0f);
    else if (newUnitId == "spacemarines_battle_brother") unitColor = glm::vec4(0.05f, 0.25f, 0.75f, 1.0f);
    else if (newUnitId == "orks_boy") unitColor = glm::vec4(0.15f, 0.60f, 0.15f, 1.0f);
    else if (newUnitId == "orks_nob") unitColor = glm::vec4(0.08f, 0.40f, 0.08f, 1.0f);

    auto& colorComp = scene.GetRegistry().emplace_or_replace<SunEngine::ColorComponent>(playerEntity);
    colorComp.Color = unitColor;
    colorComp.BaseColor = unitColor;
}

void GameLayer::ApplyUnitVisuals(const TrainedUnit& unit) {
    SwapPlayerUnit(m_Scene, m_PlayerEntity, unit.blueprintId);

    if (m_Scene.GetRegistry().all_of<SunEngine::ColorComponent>(m_PlayerEntity)) {
        auto& color = m_Scene.GetRegistry().get<SunEngine::ColorComponent>(m_PlayerEntity);
        color.Color = unit.armorColor;
        color.BaseColor = unit.armorColor;
    }

    if (m_Scene.GetRegistry().all_of<SunEngine::UnitStatsComponent>(m_PlayerEntity)) {
        auto& stats = m_Scene.GetRegistry().get<SunEngine::UnitStatsComponent>(m_PlayerEntity);
        auto& set = stats.armorSet;

        auto applyPiece = [](const std::string& pieceId, auto& piece) {
            const auto* bp = SunEngine::BlueprintManager::Get().GetArmorPiece(pieceId);
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

    if (m_Scene.GetRegistry().all_of<SunEngine::InventoryComponent>(m_PlayerEntity)) {
        auto& inv = m_Scene.GetRegistry().get<SunEngine::InventoryComponent>(m_PlayerEntity);
        inv.primaryWeaponId = unit.primaryWeaponId;
        inv.secondaryWeaponId = unit.secondaryWeaponId;
        inv.meleeWeaponId = unit.meleeWeaponId;

        std::string activeId = inv.GetActiveWeaponId();
        if (activeId.empty()) {
            inv.currentSlot = 0;
            activeId = inv.primaryWeaponId;
        }

        m_Scene.GetRegistry().get_or_emplace<SunEngine::WeaponBlueprintId>(m_PlayerEntity).id = activeId;

        if (m_Scene.GetRegistry().all_of<SunEngine::WeaponStateComponent>(m_PlayerEntity)) {
            auto& wState = m_Scene.GetRegistry().get<SunEngine::WeaponStateComponent>(m_PlayerEntity);
            wState.isReloading = false;
            wState.reloadTimer = 0.0f;
            wState.fireCooldown = 0.0f;
            wState.currentHeat = 0.0f;
            wState.isOverheating = false;

            const auto* weaponBP = SunEngine::BlueprintManager::Get().GetWeapon(activeId);
            if (weaponBP) wState.currentAmmo = weaponBP->magazine_size;
        }
    }
}

int GameLayer::CalculateSquadPoints() const {
    int total = 0;
    for (int rosterIdx : m_SelectedSquadIndices) {
        if (rosterIdx >= 0 && rosterIdx < static_cast<int>(m_PlayerRoster.size())) {
            const auto* bp = SunEngine::BlueprintManager::Get().GetUnit(m_PlayerRoster[rosterIdx].blueprintId);
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

GameLayer::GameLayer() : Layer("Game Layer")
{
    m_PhysicsSystem.Init();

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
    auto& playerTransform = m_Scene.GetRegistry().emplace<SunEngine::TransformComponent>(m_PlayerEntity);
    m_Scene.GetRegistry().emplace<SunEngine::PlayerTag>(m_PlayerEntity);

    playerTransform.Position = glm::vec3(0.0f, 0.0f, 0.0f);
    playerTransform.Scale = glm::vec3(1.0f, 1.0f, 1.0f);

    m_Scene.GetRegistry().emplace<SunEngine::ColliderComponent>(m_PlayerEntity, glm::vec3(0.3f, 0.62f, 0.3f));
    m_Scene.GetRegistry().emplace<SunEngine::RigidBodyComponent>(m_PlayerEntity, glm::vec3(0.0f), 0.0f, false);

    m_Scene.GetRegistry().emplace<SunEngine::HealthComponent>(m_PlayerEntity, 100.0f, 100.0f);
    m_Scene.GetRegistry().emplace<SunEngine::UnitBlueprintId>(m_PlayerEntity, "imperium_guardsman_recruit");
    m_Scene.GetRegistry().emplace<SunEngine::UnitStatsComponent>(m_PlayerEntity);
    m_Scene.GetRegistry().emplace<SunEngine::MeleeStateComponent>(m_PlayerEntity);

    SwapPlayerUnit(m_Scene, m_PlayerEntity, "imperium_guardsman_recruit");

    auto& inv = m_Scene.GetRegistry().emplace<SunEngine::InventoryComponent>(m_PlayerEntity);
    inv.primaryWeaponId = "lasgun_standard";
    inv.secondaryWeaponId = "laspistol_m-g";
    inv.meleeWeaponId = "combat_knife_1h";
    inv.currentSlot = 0;

    m_Scene.GetRegistry().emplace<SunEngine::WeaponBlueprintId>(m_PlayerEntity, inv.primaryWeaponId);
    m_Scene.GetRegistry().emplace<SunEngine::WeaponStateComponent>(m_PlayerEntity);

    m_WeaponEntity = m_Scene.CreateEntity();
    m_Scene.GetRegistry().emplace<SunEngine::TransformComponent>(m_WeaponEntity);
    m_Scene.GetRegistry().emplace<SunEngine::MeshComponent>(m_WeaponEntity, 0u);
    auto& wColor = m_Scene.GetRegistry().emplace<SunEngine::ColorComponent>(m_WeaponEntity);
    wColor.Color = glm::vec4(0.2f, 0.2f, 0.25f, 1.0f);
    wColor.BaseColor = wColor.Color;

    std::string mapsDir = FindValidDir("Maps");
    if (mapsDir.empty()) mapsDir = "GameData/Maps/";

    m_CurrentMapData = SunEngine::MapLoader::LoadFromFile(m_Scene, mapsDir + "warzone_sector4.json");
    m_PhysicsSystem.RefreshBodies(m_Scene);

    if (m_CurrentMapData.navMesh) {
        m_NavQuery.Init(m_CurrentMapData.navMesh);
    }

    if (!m_CurrentMapData.playerSpawnPoints.empty()) {
        playerTransform.Position = m_CurrentMapData.playerSpawnPoints[0];
        m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, playerTransform.Position);
    }

    if (ma_engine_init(NULL, &m_AudioEngine) != MA_SUCCESS) {
        SUN_CORE_ERROR("ERRORE: Impossibile inizializzare MiniAudio!");
    }
    else {
        InitOrkAudio(&m_AudioEngine);
    }

    std::string audioDir = FindValidDir("Audio");
    if (audioDir.empty()) {
        audioDir = "GameData/Audio/";
    }

    auto& weapon = m_Scene.GetRegistry().emplace<SunEngine::WeaponComponent>(m_PlayerEntity);
    weapon.fireRate = 0.15f;
    weapon.shootSound = audioDir + "laser.wav";

    m_Scene.GetRegistry().emplace<SunEngine::ParticleEmitterComponent>(m_PlayerEntity);

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
    if (g_OrkVoices.startSoundLoaded) {
        ma_sound_uninit(&g_OrkVoices.startWaaaghSound);
    }

    for (auto& item : g_OrkVoices.voicePool) {
        if (item) {
            ma_sound_uninit(&item->sound);
        }
    }
    g_OrkVoices.voicePool.clear();

    ma_engine_uninit(&m_AudioEngine);
}

void GameLayer::OnUpdate(float deltaTime)
{
    GLFWwindow* window = glfwGetCurrentContext();

    static bool escPressedLastFrame = false;
    bool escPressed = SunEngine::Input::IsKeyPressed(GLFW_KEY_ESCAPE);
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
        if (SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) {
            glm::vec2 mouseDelta = SunEngine::Input::GetMouseDelta();
            m_EditorCameraYaw -= mouseDelta.x * 0.3f;
            m_EditorCameraPitch -= mouseDelta.y * 0.3f;
            m_EditorCameraPitch = std::clamp(m_EditorCameraPitch, -89.0f, 89.0f);
        }

        float moveSpeed = 20.0f * deltaTime;
        if (SunEngine::Input::IsKeyPressed(GLFW_KEY_LEFT_SHIFT)) moveSpeed *= 3.0f;

        glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(m_EditorCameraYaw), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::vec3 forward = glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
        glm::vec3 right = glm::vec3(rot * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));

        if (SunEngine::Input::IsKeyPressed(GLFW_KEY_W) && SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_EditorCameraTarget += forward * moveSpeed;
        if (SunEngine::Input::IsKeyPressed(GLFW_KEY_S) && SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_EditorCameraTarget -= forward * moveSpeed;
        if (SunEngine::Input::IsKeyPressed(GLFW_KEY_A) && SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_EditorCameraTarget -= right * moveSpeed;
        if (SunEngine::Input::IsKeyPressed(GLFW_KEY_D) && SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) m_EditorCameraTarget += right * moveSpeed;

        m_Camera.SetViewportSize(1280.0f, 720.0f);
        m_Camera.SetCameraOffsets(0.0f, 0.0f, m_EditorCameraDistance);
        m_Camera.SetTarget(m_EditorCameraTarget, m_EditorCameraYaw);
        m_Camera.SetPitch(m_EditorCameraPitch);
        m_Camera.Update(glm::vec3(0.0f), 1.0f);

        SunEngine::Renderer::SetViewProjection(m_Camera.GetViewProjection());

        HandleEditor3DInteraction(deltaTime);
        return;
    }

    auto& transform = m_Scene.GetRegistry().get<SunEngine::TransformComponent>(m_PlayerEntity);

    if (m_GameState == GameState::MainMenu) {
        m_GarageCameraAngle += deltaTime * 25.0f;
        if (m_GarageCameraAngle >= 360.0f) m_GarageCameraAngle -= 360.0f;

        glm::vec3 garageTargetPos = transform.Position + glm::vec3(0.0f, 0.9f, 0.0f);

        m_Camera.SetViewportSize(1280.0f, 720.0f);
        m_Camera.SetCameraOffsets(0.0f, 0.2f, m_GarageCameraDistance);
        m_Camera.SetTarget(garageTargetPos, m_GarageCameraAngle);
        m_Camera.SetPitch(-5.0f);
        m_Camera.Update(glm::vec3(0.0f), 1.0f);

        SunEngine::Renderer::SetViewProjection(m_Camera.GetViewProjection());
        return;
    }

    auto* playerHealth = m_Scene.GetRegistry().try_get<SunEngine::HealthComponent>(m_PlayerEntity);
    bool isPlayerAlive = playerHealth && (playerHealth->currentHealth > 0.0f);
    bool isAiming = false;

    // DICHIARAZIONE DELLA STANCE A SCOPE SUPERIORE
    SunEngine::EquipStance stance = SunEngine::EquipStance::RangedTwoHanded;
    if (m_Scene.GetRegistry().all_of<SunEngine::InventoryComponent>(m_PlayerEntity)) {
        stance = m_Scene.GetRegistry().get<SunEngine::InventoryComponent>(m_PlayerEntity).GetEquipStance();
    }

    if (!m_IsPaused && isPlayerAlive) {
        glm::vec2 mouseDelta = SunEngine::Input::GetMouseDelta();
        float mouseSensitivity = 0.15f;

        transform.AimYaw -= mouseDelta.x * mouseSensitivity;
        transform.AimPitch -= mouseDelta.y * mouseSensitivity;
        transform.AimPitch = std::clamp(transform.AimPitch, -85.0f, 85.0f);

        if (!SunEngine::Input::IsKeyPressed(GLFW_KEY_W) &&
            !SunEngine::Input::IsKeyPressed(GLFW_KEY_S) &&
            !SunEngine::Input::IsKeyPressed(GLFW_KEY_A) &&
            !SunEngine::Input::IsKeyPressed(GLFW_KEY_D)) {

            float diff = std::fmod(transform.AimYaw - transform.Rotation.y + 180.0f, 360.0f) - 180.0f;
            if (diff < -180.0f) diff += 360.0f;

            if (std::abs(diff) > 45.0f) {
                transform.Rotation.y = LerpAngle(transform.Rotation.y, transform.AimYaw, 5.0f * deltaTime);
            }
        }

        auto corpseView = m_Scene.GetRegistry().view<SunEngine::CorpseComponent>(entt::exclude<ProcessedCorpseTag>);
        for (auto entity : corpseView) {
            m_KillCount++;
            m_Scene.GetRegistry().emplace<ProcessedCorpseTag>(entity);
        }

        // --- GESTIONE COMBINATA: INVENTARIO, CAMBIO ARMA E INPUT POSTURA (STANCE) ---
        if (m_Scene.GetRegistry().all_of<
            SunEngine::InventoryComponent,
            SunEngine::UnitStatsComponent,
            SunEngine::WeaponStateComponent,
            SunEngine::WeaponBlueprintId
        >(m_PlayerEntity))
        {
            auto& inv = m_Scene.GetRegistry().get<SunEngine::InventoryComponent>(m_PlayerEntity);
            auto& stats = m_Scene.GetRegistry().get<SunEngine::UnitStatsComponent>(m_PlayerEntity);
            auto& melee = m_Scene.GetRegistry().get_or_emplace<SunEngine::MeleeStateComponent>(m_PlayerEntity);
            auto& wState = m_Scene.GetRegistry().get<SunEngine::WeaponStateComponent>(m_PlayerEntity);
            auto& wBlueprintId = m_Scene.GetRegistry().get<SunEngine::WeaponBlueprintId>(m_PlayerEntity);

            // 1. SELEZIONE/CAMBIO SLOT ARMA (TASTI 1 -> 7)
            int requestedSlot = -1;
            if (SunEngine::Input::IsKeyPressed(GLFW_KEY_1)) requestedSlot = 0;      // 1: Primaria
            else if (SunEngine::Input::IsKeyPressed(GLFW_KEY_2)) requestedSlot = 1; // 2: Secondaria
            else if (SunEngine::Input::IsKeyPressed(GLFW_KEY_3)) requestedSlot = 2; // 3: Melee
            else if (SunEngine::Input::IsKeyPressed(GLFW_KEY_4)) requestedSlot = 3; // 4: Granate
            else if (SunEngine::Input::IsKeyPressed(GLFW_KEY_5)) requestedSlot = 4; // 5: Tool 1
            else if (SunEngine::Input::IsKeyPressed(GLFW_KEY_6)) requestedSlot = 5; // 6: Tool 2
            else if (SunEngine::Input::IsKeyPressed(GLFW_KEY_7)) requestedSlot = 6; // 7: Tool 3

            if (requestedSlot != -1 && requestedSlot != inv.currentSlot) {
                inv.currentSlot = requestedSlot;
                std::string targetWeaponId = inv.GetActiveWeaponId();

                // RESET MELEE STATE MACHINE
                melee.currentPhase = SunEngine::MeleePhase::None;
                melee.currentAttackNode = "";
                melee.stateTimer = 0.0f;

                stance = inv.GetEquipStance();

                if (stance == SunEngine::EquipStance::HybridRangedMelee) {
                    targetWeaponId = inv.secondaryWeaponId;
                }

                if (!targetWeaponId.empty()) {
                    wBlueprintId.id = targetWeaponId;

                    wState.isReloading = false;
                    wState.reloadTimer = 0.0f;
                    wState.fireCooldown = 0.0f;
                    wState.currentHeat = 0.0f;
                    wState.isOverheating = false;

                    const auto* weaponBP = SunEngine::BlueprintManager::Get().GetWeapon(targetWeaponId);
                    if (weaponBP) {
                        wState.currentAmmo = weaponBP->magazine_size;

                        if (m_Scene.GetRegistry().all_of<SunEngine::WeaponComponent>(m_PlayerEntity)) {
                            auto& weapComp = m_Scene.GetRegistry().get<SunEngine::WeaponComponent>(m_PlayerEntity);
                            weapComp.fireRate = (weaponBP->rate_of_fire > 0.0f) ? (60.0f / weaponBP->rate_of_fire) : 0.15f;
                        }
                    }
                }
            }

            std::string activeId = inv.GetActiveWeaponId();
            if (activeId.empty() && !inv.primaryWeaponId.empty()) {
                inv.currentSlot = 0;
                activeId = inv.primaryWeaponId;
                wBlueprintId.id = activeId;
            }

            stance = inv.GetEquipStance();

            // ADS (MIRA CON CLICK DESTRO)
            if (stance == SunEngine::EquipStance::RangedTwoHanded || stance == SunEngine::EquipStance::HybridRangedMelee) {
                if (SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT) && melee.currentPhase == SunEngine::MeleePhase::None) {
                    isAiming = true;
                }
            }

            static bool mmbPressedLastFrame = false;
            bool mmbPressed = SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_MIDDLE);
            bool mmbClicked = mmbPressed && !mmbPressedLastFrame;
            mmbPressedLastFrame = mmbPressed;

            if (isAiming) {
                stats.speedModifier = 0.60f;

                const auto* currentBP = SunEngine::BlueprintManager::Get().GetWeapon(wBlueprintId.id);
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

            // GESTIONE RICARICA E SURRISCALDAMENTO
            const auto* currentBP = SunEngine::BlueprintManager::Get().GetWeapon(wBlueprintId.id);
            if (currentBP) {
                if (SunEngine::Input::IsKeyPressed(GLFW_KEY_R) && !wState.isReloading && wState.currentAmmo < currentBP->magazine_size && stance != SunEngine::EquipStance::MeleePure) {
                    wState.isReloading = true;
                    wState.reloadTimer = currentBP->reload_time_sec;
                }

                if (wState.isReloading) {
                    wState.reloadTimer -= deltaTime;
                    if (wState.reloadTimer <= 0.0f) {
                        wState.isReloading = false;
                        wState.reloadTimer = 0.0f;
                        wState.currentAmmo = currentBP->magazine_size;
                    }
                }

                if (currentBP->heat_capacity > 0.0f) {
                    if (wState.currentHeat > 0.0f) {
                        float heatDecay = currentBP->heat_capacity * 0.40f;
                        wState.currentHeat -= heatDecay * deltaTime;
                        if (wState.currentHeat <= 0.0f) {
                            wState.currentHeat = 0.0f;
                            wState.isOverheating = false;
                        }
                    }
                }
            }

            // TASTI ATTACCO E COMBO (SEPARAZIONE MOUSE E TASTIERA PER STANCE)
            if (stance == SunEngine::EquipStance::MeleePure) {
                if (SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)) {
                    melee.bufferedInput = "LMB";
                    melee.inputBufferTimer = 0.25f;

                    if (melee.currentPhase == SunEngine::MeleePhase::None) {
                        const auto* wBP = SunEngine::BlueprintManager::Get().GetWeapon(wBlueprintId.id);
                        if (wBP && wBP->hasMoveset) {
                            melee.currentAttackNode = wBP->moveset.entryNode;
                            melee.currentPhase = SunEngine::MeleePhase::Windup;
                            melee.stateTimer = 0.0f;
                        }
                    }
                }
                if (SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) {
                    melee.bufferedInput = "RMB";
                    melee.inputBufferTimer = 0.25f;
                }
            }
            else if (stance == SunEngine::EquipStance::HybridRangedMelee) {
                // Tasto Sinistro Mouse: Spara la Pistola nella mano sinistra
                if (SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)) {
                    FireWeapon(m_Scene.GetRegistry(), m_PlayerEntity, &m_AudioEngine, m_Scene, m_WeaponEntity, m_VolumeSFX);
                }

                // Tasti C e F: Attacchi Melee e Combo con la Spada nella mano destra
                if (SunEngine::Input::IsKeyPressed(GLFW_KEY_C) || SunEngine::Input::IsKeyPressed(GLFW_KEY_F)) {
                    melee.bufferedInput = "LMB";
                    melee.inputBufferTimer = 0.25f;

                    if (melee.currentPhase == SunEngine::MeleePhase::None && !inv.meleeWeaponId.empty()) {
                        const auto* meleeBP = SunEngine::BlueprintManager::Get().GetWeapon(inv.meleeWeaponId);
                        if (meleeBP && meleeBP->hasMoveset) {
                            melee.currentAttackNode = meleeBP->moveset.entryNode;
                            melee.currentPhase = SunEngine::MeleePhase::Windup;
                            melee.stateTimer = 0.0f;
                        }
                    }
                }
            }
            else {
                // Fucili / Doppie Pistole
                if (SunEngine::Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)) {
                    FireWeapon(m_Scene.GetRegistry(), m_PlayerEntity, &m_AudioEngine, m_Scene, m_WeaponEntity, m_VolumeSFX);
                }
            }

            // Tasto Q: Parata
            if (SunEngine::Input::IsKeyPressed(GLFW_KEY_Q) && melee.currentPhase == SunEngine::MeleePhase::None) {
                float parryCost = 10.0f;
                if (stats.stamina >= parryCost) {
                    stats.stamina -= parryCost;
                    melee.currentPhase = SunEngine::MeleePhase::Windup;
                    melee.stateTimer = 0.0f;
                }
            }
        }

        UpdateWeapons(m_Scene.GetRegistry(), deltaTime);
        m_MovementSystem.Update(m_Scene, m_PhysicsSystem, &m_AudioEngine, deltaTime, m_VolumeSFX);
        UpdateProjectiles(m_Scene, deltaTime);

        m_PhysicsSystem.Update(m_Scene, deltaTime);
        m_WeightSystem.Update(m_Scene, deltaTime);
        m_CombatSystem.Update(m_Scene, m_PhysicsSystem, deltaTime);

        // --- 1. GESTIONE ANIMAZIONI PROCEDURALI SCHELETRO (SUPPORTO MOVESET IBRIDO E DEDICATO) ---
        if (m_Scene.GetRegistry().all_of<SunEngine::SkeletonComponent, SunEngine::WeaponBlueprintId>(m_PlayerEntity)) {
            auto& skel = m_Scene.GetRegistry().get<SunEngine::SkeletonComponent>(m_PlayerEntity);
            auto& melee = m_Scene.GetRegistry().get_or_emplace<SunEngine::MeleeStateComponent>(m_PlayerEntity);
            auto& inv = m_Scene.GetRegistry().get<SunEngine::InventoryComponent>(m_PlayerEntity);

            const std::string& currentWeaponId = m_Scene.GetRegistry().get<SunEngine::WeaponBlueprintId>(m_PlayerEntity).id;

            // Supporto arma Melee attiva in Stance Ibrida
            std::string activeMeleeWeaponId = currentWeaponId;
            if (stance == SunEngine::EquipStance::HybridRangedMelee && !inv.meleeWeaponId.empty()) {
                activeMeleeWeaponId = inv.meleeWeaponId;
            }
            const auto* meleeBP = SunEngine::BlueprintManager::Get().GetWeapon(activeMeleeWeaponId);
            const auto* weaponBP = SunEngine::BlueprintManager::Get().GetWeapon(currentWeaponId);

            bool isTwoHanded = weaponBP ? (weaponBP->hand_type == SunEngine::HandType::TwoHanded) : true;

            float spineTwist = std::fmod(transform.AimYaw - transform.Rotation.y + 180.0f, 360.0f) - 180.0f;
            if (spineTwist < -180.0f) spineTwist += 360.0f;
            spineTwist = std::clamp(spineTwist, -75.0f, 75.0f);

            for (auto& block : skel.data.blocks) {

                // ANIMAZIONE NODO MOVESET ATTIVO
                if (meleeBP && meleeBP->hasMoveset && !melee.currentAttackNode.empty() && melee.currentPhase != SunEngine::MeleePhase::None) {
                    // Cerca il nodo nel dizionario in modo sicuro
                    auto it = meleeBP->moveset.nodes.find(melee.currentAttackNode);
                    if (it != meleeBP->moveset.nodes.end()) {
                        const auto& node = it->second;
                        if (node.pose.find(block.name) != node.pose.end()) {
                            glm::vec3 targetRot = node.pose.at(block.name);
                            block.jointRotation = glm::mix(block.jointRotation, targetRot, 15.0f * deltaTime);
                            continue;
                        }
                    }
                }

                // TORSO
                if (block.name == "Torso") {
                    float adsTorsoAngle = 0.0f;
                    if (isAiming) {
                        if (stance == SunEngine::EquipStance::HybridRangedMelee) {
                            adsTorsoAngle = 40.0f;
                        }
                        else if (isTwoHanded) {
                            adsTorsoAngle = -25.0f;
                        }
                    }
                    block.jointRotation.z = spineTwist + adsTorsoAngle;
                    block.jointRotation.x = (isAiming && isTwoHanded) ? 10.0f : 0.0f;
                }

                // TESTA
                if (block.name == "Head") {
                    if (isAiming && stance == SunEngine::EquipStance::HybridRangedMelee) {
                        block.jointRotation.x = -transform.AimPitch * 0.5f;
                        block.jointRotation.y = -35.0f;
                        block.jointRotation.z = 5.0f;
                    }
                    else {
                        block.jointRotation.x = -transform.AimPitch * 0.5f;
                        block.jointRotation.y = 0.0f;
                        block.jointRotation.z = 0.0f;
                    }
                }

                // STANDBY ARMI
                if (melee.currentPhase == SunEngine::MeleePhase::None) {
                    if (stance == SunEngine::EquipStance::RangedTwoHanded || (isTwoHanded && stance != SunEngine::EquipStance::HybridRangedMelee)) {
                        if (block.name == "Arm_R") {
                            block.jointRotation.x = isAiming ? (55.0f + transform.AimPitch * 0.8f) : (38.0f + transform.AimPitch * 0.65f);
                            block.jointRotation.y = isAiming ? -50.0f : -20.0f;
                            block.jointRotation.z = isAiming ? 20.0f : 0.0f;
                        }
                        else if (block.name == "Arm_L") {
                            block.jointRotation.x = isAiming ? (65.0f + transform.AimPitch * 0.8f) : (42.0f + transform.AimPitch * 0.65f);
                            block.jointRotation.y = isAiming ? 35.0f : 40.0f;
                            block.jointRotation.z = isAiming ? 15.0f : 22.0f;
                        }
                    }
                    else if (stance == SunEngine::EquipStance::HybridRangedMelee) {
                        if (block.name == "Arm_R") {
                            block.jointRotation.x = 20.0f;
                            block.jointRotation.y = -45.0f;
                            block.jointRotation.z = -15.0f;
                        }
                        else if (block.name == "Arm_L") {
                            block.jointRotation.x = 52.0f + transform.AimPitch * 0.75f;
                            block.jointRotation.y = 10.0f;
                            block.jointRotation.z = 0.0f;
                        }
                    }
                    else if (stance == SunEngine::EquipStance::DualRangedOneHanded) {
                        if (block.name == "Arm_R" || block.name == "Arm_L") {
                            block.jointRotation.x = 48.0f + transform.AimPitch * 0.7f;
                            block.jointRotation.y = (block.name == "Arm_R") ? -15.0f : 15.0f;
                            block.jointRotation.z = 0.0f;
                        }
                    }
                    else {
                        if (block.name == "Arm_R") {
                            block.jointRotation.x = 30.0f + transform.AimPitch * 0.5f;
                            block.jointRotation.y = -10.0f;
                            block.jointRotation.z = 0.0f;
                        }
                        else if (block.name == "Arm_L") {
                            block.jointRotation = glm::vec3(0.0f);
                        }
                    }
                }
            }
        }

        // --- 2. REGOLAZIONE TELECAMERA OVER-THE-SHOULDER GEARS OF WAR ---
        float unitHeight = 1.8f;
        float torsoWidth = 0.5f;

        if (m_Scene.GetRegistry().all_of<SunEngine::SkeletonComponent>(m_PlayerEntity)) {
            auto& skel = m_Scene.GetRegistry().get<SunEngine::SkeletonComponent>(m_PlayerEntity);
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

        if (m_Scene.GetRegistry().all_of<SunEngine::CameraRecoilComponent>(m_PlayerEntity)) {
            auto& recoil = m_Scene.GetRegistry().get<SunEngine::CameraRecoilComponent>(m_PlayerEntity);

            recoilPitch = recoil.recoilPitch;
            recoilYaw = recoil.recoilYaw;
            extraFov = recoil.fovOffset;

            if (recoil.shakeIntensity > 0.001f) {
                float shakeX = ((rand() % 100) / 100.0f - 0.5f) * recoil.shakeIntensity;
                float shakeY = ((rand() % 100) / 100.0f - 0.5f) * recoil.shakeIntensity;
                shakeOffset = glm::vec3(shakeX, shakeY, 0.0f);
            }

            recoil.recoilPitch = glm::mix(recoil.recoilPitch, 0.0f, recoil.recoverySpeed * deltaTime);
            recoil.recoilYaw = glm::mix(recoil.recoilYaw, 0.0f, recoil.recoverySpeed * deltaTime);
            recoil.shakeIntensity = glm::mix(recoil.shakeIntensity, 0.0f, 15.0f * deltaTime);
            recoil.fovOffset = glm::mix(recoil.fovOffset, 0.0f, 12.0f * deltaTime);
        }

        const auto* wBpId = m_Scene.GetRegistry().try_get<SunEngine::WeaponBlueprintId>(m_PlayerEntity);
        const auto* wSt = m_Scene.GetRegistry().try_get<SunEngine::WeaponStateComponent>(m_PlayerEntity);
        const auto* currentBP = wBpId ? SunEngine::BlueprintManager::Get().GetWeapon(wBpId->id) : nullptr;

        if (isAiming) {
            if (stance == SunEngine::EquipStance::HybridRangedMelee) {
                dynamicDistance = 2.10f;
                dynamicShoulderOffset = 0.65f;
                dynamicHeightOffset = unitHeight * 0.18f;
                extraFov -= 20.0f;
            }
            else {
                dynamicDistance = 2.05f;
                dynamicShoulderOffset = 0.52f;
                dynamicHeightOffset = unitHeight * 0.18f;
                extraFov -= 22.0f;
            }

            if (wSt && wSt->isScopeActive && currentBP && currentBP->has_scope) {
                dynamicDistance = 0.75f;
                dynamicShoulderOffset = 0.12f;
                extraFov -= currentBP->scope_zoom_fov;
            }
        }

        m_Camera.SetCameraOffsets(dynamicShoulderOffset, dynamicHeightOffset, dynamicDistance);

        glm::vec3 lookTarget = transform.Position + glm::vec3(0.0f, unitHeight * 0.65f, 0.0f);
        m_Camera.SetTarget(lookTarget, transform.AimYaw);

        m_Camera.SetPitch(transform.AimPitch + recoilPitch);
        m_Camera.SetYawOffset(recoilYaw);
        m_Camera.SetViewportSize(1280.0f, 720.0f, extraFov);
        m_Camera.Update(shakeOffset, 1.0f);

        SunEngine::Renderer::SetViewProjection(m_Camera.GetViewProjection());

        // Waaagh! Spawner & Ork Voices
        if (g_WaaaghActive) {
            g_WaaaghTimer += deltaTime;

            if (g_WaaaghTimer >= 10.0f) {
                g_WaaaghTimer = 0.0f;

                const float minSpawnDistance = 20.0f;
                const float maxSpawnDistance = 150.0f;

                for (int i = 0; i < g_WaaaghSpawnCount; ++i) {
                    float angle = ((rand() % 360) * 3.14159265f) / 180.0f;
                    float randomFactor = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
                    float distance = minSpawnDistance + randomFactor * (maxSpawnDistance - minSpawnDistance);

                    glm::vec3 spawnPos = transform.Position + glm::vec3(std::cos(angle) * distance, 0.0f, std::sin(angle) * distance);
                    SpawnOrkBoy(m_Scene, spawnPos);
                }

                g_WaaaghSpawnCount *= 2;
            }

            UpdateOrkVoices(m_Scene, m_PlayerEntity, deltaTime, m_VolumeVoice);
        }

        static float orkAnimTimer = 0.0f;
        orkAnimTimer += deltaTime * 8.0f;

        float totalTime = static_cast<float>(glfwGetTime());

        auto enemyView = m_Scene.GetRegistry().view<SunEngine::TransformComponent, SunEngine::HealthComponent>(
            entt::exclude<SunEngine::PlayerTag, SunEngine::CorpseComponent>
        );

        for (auto enemyEntity : enemyView) {
            auto& enemyTrans = enemyView.get<SunEngine::TransformComponent>(enemyEntity);

            glm::vec3 distVec = transform.Position - enemyTrans.Position;
            float distToPlayer = glm::length(distVec);

            if (distToPlayer > 1.15f) {
                float orkSeed = static_cast<float>(static_cast<uint32_t>(enemyEntity) % 100);

                std::vector<glm::vec3> path = m_NavQuery.FindPath(enemyTrans.Position, transform.Position);

                glm::vec3 targetWaypoint = transform.Position;
                if (path.size() >= 2) {
                    targetWaypoint = path[1];
                }

                glm::vec3 dirToTarget = targetWaypoint - enemyTrans.Position;
                dirToTarget.y = 0.0f;

                if (glm::length(dirToTarget) > 0.001f) {
                    glm::vec3 moveDir = glm::normalize(dirToTarget);
                    glm::vec3 sideDir = glm::cross(moveDir, glm::vec3(0.0f, 1.0f, 0.0f));

                    float swayFreq = 6.0f + std::fmod(orkSeed, 3.0f);
                    float swayAmp = (distToPlayer > 8.0f) ? 0.45f : 0.15f;
                    float swayOffset = std::sin(totalTime * swayFreq + orkSeed) * swayAmp;

                    glm::vec3 finalMoveDir = glm::normalize(moveDir + sideDir * swayOffset);

                    float baseSpeed = 3.2f + std::fmod(orkSeed, 0.8f);
                    float currentSpeed = (distToPlayer < 8.0f) ? (baseSpeed * 1.35f) : baseSpeed;

                    enemyTrans.Position.x += finalMoveDir.x * currentSpeed * deltaTime;
                    enemyTrans.Position.z += finalMoveDir.z * currentSpeed * deltaTime;

                    float targetY = GetGroundHeightAt(m_Scene, enemyTrans.Position, enemyTrans.Position.y);
                    enemyTrans.Position.y = glm::mix(enemyTrans.Position.y, targetY, 15.0f * deltaTime);
                    m_PhysicsSystem.SetBodyPosition(m_Scene, enemyEntity, enemyTrans.Position);

                    float targetYaw = glm::degrees(std::atan2(-finalMoveDir.x, -finalMoveDir.z));
                    enemyTrans.Rotation.y = LerpAngle(enemyTrans.Rotation.y, targetYaw, 10.0f * deltaTime);

                    if (m_Scene.GetRegistry().all_of<SunEngine::FootstepComponent>(enemyEntity)) {
                        auto& footstep = m_Scene.GetRegistry().get<SunEngine::FootstepComponent>(enemyEntity);

                        footstep.stepTimer += deltaTime * (currentSpeed / 3.5f);
                        if (footstep.stepTimer >= footstep.stepInterval) {
                            footstep.stepTimer = 0.0f;

                            float spatialVol = CalculateSpatialVolume(distToPlayer) * 0.6f * m_VolumeSFX;
                            if (spatialVol > 0.002f) {
                                std::string audioDir = FindValidDir("Audio");
                                if (audioDir.empty()) audioDir = "GameData/Audio/";
                                std::string stepPath = audioDir + "Footsteps/step_concrete.wav";

                                if (std::filesystem::exists(stepPath)) {
                                    ma_engine_set_volume(&m_AudioEngine, spatialVol);
                                    ma_engine_play_sound(&m_AudioEngine, stepPath.c_str(), NULL);
                                }
                            }
                        }
                    }
                }

                if (m_Scene.GetRegistry().all_of<SunEngine::SkeletonComponent>(enemyEntity)) {
                    auto& skel = m_Scene.GetRegistry().get<SunEngine::SkeletonComponent>(enemyEntity);
                    float animSpeed = (distToPlayer < 8.0f) ? 12.0f : 8.0f;
                    float legAngle = std::sin(totalTime * animSpeed + orkSeed) * 40.0f;

                    for (auto& block : skel.data.blocks) {
                        if (block.name == "Leg_L") block.jointRotation.x = legAngle;
                        else if (block.name == "Leg_R") block.jointRotation.x = -legAngle;
                        else if (block.name == "Arm_R") block.jointRotation.x = 30.0f + legAngle * 0.5f;
                        else if (block.name == "Arm_L") block.jointRotation.x = 30.0f - legAngle * 0.5f;
                    }
                }
            }
            else {
                playerHealth->currentHealth = 0.0f;
                if (m_CurrentSquadMemberIdx < static_cast<int>(m_ActiveSquad.size())) {
                    m_ActiveSquad[m_CurrentSquadMemberIdx].isAlive = false;
                }
                if (window) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                break;
            }
        }
    }

    m_ParticleSystem.Update(m_Scene, deltaTime);

    // RENDERING & ATTACHMENT DELLE ARMI SULLE MANI (BOX HITBOX PROPORZIONATE)
    if (m_Scene.GetRegistry().valid(m_PlayerEntity)) {
        static entt::entity s_LeftWeaponEntity = entt::null;
        if (!m_Scene.GetRegistry().valid(s_LeftWeaponEntity)) {
            s_LeftWeaponEntity = m_Scene.CreateEntity();
            m_Scene.GetRegistry().emplace<SunEngine::TransformComponent>(s_LeftWeaponEntity);
            m_Scene.GetRegistry().emplace<SunEngine::MeshComponent>(s_LeftWeaponEntity, 0u);
            auto& c = m_Scene.GetRegistry().emplace<SunEngine::ColorComponent>(s_LeftWeaponEntity);
            c.Color = glm::vec4(1.0f);
            c.BaseColor = glm::vec4(1.0f);
        }

        auto& inv = m_Scene.GetRegistry().get<SunEngine::InventoryComponent>(m_PlayerEntity);
        SunEngine::EquipStance currentStance = inv.GetEquipStance();

        std::string rightWeaponId = "";
        std::string leftWeaponId = "";

        if (currentStance == SunEngine::EquipStance::HybridRangedMelee) {
            rightWeaponId = inv.meleeWeaponId;     // MANO DESTRA = MELEE
            leftWeaponId = inv.secondaryWeaponId;  // MANO SINISTRA = PISTOLA
        }
        else if (currentStance == SunEngine::EquipStance::DualRangedOneHanded) {
            rightWeaponId = inv.primaryWeaponId;   // MANO DESTRA = PISTOLA 1
            leftWeaponId = inv.secondaryWeaponId;  // MANO SINISTRA = PISTOLA 2
        }
        else {
            rightWeaponId = inv.GetActiveWeaponId();
            leftWeaponId = "";
        }

        // 1. ARMA MANO DESTRA (m_WeaponEntity)
        if (m_Scene.GetRegistry().valid(m_WeaponEntity) && !rightWeaponId.empty()) {
            auto& wTransR = m_Scene.GetRegistry().get<SunEngine::TransformComponent>(m_WeaponEntity);
            const auto* wBPR = SunEngine::BlueprintManager::Get().GetWeapon(rightWeaponId);

            if (wBPR) {
                if (m_Scene.GetRegistry().all_of<SunEngine::SkeletonComponent>(m_PlayerEntity)) {
                    auto& skel = m_Scene.GetRegistry().get<SunEngine::SkeletonComponent>(m_PlayerEntity);
                    auto worldTransforms = skel.data.ComputeWorldTransforms(transform.Position, transform.Rotation, skel.scale);

                    int armRIndex = -1;
                    for (size_t i = 0; i < skel.data.blocks.size(); ++i) {
                        if (skel.data.blocks[i].name == "Arm_R") { armRIndex = static_cast<int>(i); break; }
                    }

                    if (armRIndex >= 0 && armRIndex < static_cast<int>(worldTransforms.size())) {
                        glm::mat4 armWorldMat = worldTransforms[armRIndex];
                        glm::vec3 armPos = glm::vec3(armWorldMat[3]);

                        glm::vec3 localOffset = wBPR->hold_offset;
                        glm::vec3 offsetInWorld = glm::vec3(armWorldMat * glm::vec4(localOffset, 0.0f));

                        wTransR.Position = armPos + offsetInWorld;
                        wTransR.Rotation = glm::vec3(transform.AimPitch, transform.AimYaw, 0.0f);

                        glm::vec3 scale = wBPR->weapon_scale;
                        if (scale.x == scale.y && scale.y == scale.z && scale.x > 0.3f) {
                            scale = wBPR->is_melee ? glm::vec3(0.06f, 0.15f, 0.90f) : glm::vec3(0.08f, 0.18f, 0.80f);
                        }
                        wTransR.Scale = scale;
                    }
                }

                if (m_Scene.GetRegistry().all_of<SunEngine::ColorComponent>(m_WeaponEntity)) {
                    auto& wColor = m_Scene.GetRegistry().get<SunEngine::ColorComponent>(m_WeaponEntity);
                    glm::vec4 col = wBPR->primary_color;
                    if (glm::length(glm::vec3(col)) < 0.05f) col = wBPR->is_melee ? glm::vec4(0.85f, 0.75f, 0.2f, 1.0f) : glm::vec4(0.8f, 0.2f, 0.2f, 1.0f);
                    wColor.Color = col;
                    wColor.BaseColor = col;
                }
            }
        }

        // 2. ARMA MANO SINISTRA (s_LeftWeaponEntity)
        auto& wTransL = m_Scene.GetRegistry().get<SunEngine::TransformComponent>(s_LeftWeaponEntity);
        if (!leftWeaponId.empty()) {
            const auto* wBPL = SunEngine::BlueprintManager::Get().GetWeapon(leftWeaponId);
            if (wBPL) {
                if (m_Scene.GetRegistry().all_of<SunEngine::SkeletonComponent>(m_PlayerEntity)) {
                    auto& skel = m_Scene.GetRegistry().get<SunEngine::SkeletonComponent>(m_PlayerEntity);
                    auto worldTransforms = skel.data.ComputeWorldTransforms(transform.Position, transform.Rotation, skel.scale);

                    int armLIndex = -1;
                    for (size_t i = 0; i < skel.data.blocks.size(); ++i) {
                        if (skel.data.blocks[i].name == "Arm_L") { armLIndex = static_cast<int>(i); break; }
                    }

                    if (armLIndex >= 0 && armLIndex < static_cast<int>(worldTransforms.size())) {
                        glm::mat4 armWorldMat = worldTransforms[armLIndex];
                        glm::vec3 armPos = glm::vec3(armWorldMat[3]);
                        glm::vec3 localOffset = wBPL->hold_offset;
                        localOffset.x = -localOffset.x;
                        glm::vec3 offsetInWorld = glm::vec3(armWorldMat * glm::vec4(localOffset, 0.0f));

                        wTransL.Position = armPos + offsetInWorld;
                        wTransL.Rotation = glm::vec3(transform.AimPitch, transform.AimYaw, 0.0f);

                        glm::vec3 scale = wBPL->weapon_scale;
                        if (scale.x == scale.y && scale.y == scale.z && scale.x > 0.3f) {
                            scale = glm::vec3(0.06f, 0.14f, 0.30f);
                        }
                        wTransL.Scale = scale;
                    }
                }

                if (m_Scene.GetRegistry().all_of<SunEngine::ColorComponent>(s_LeftWeaponEntity)) {
                    auto& wColorL = m_Scene.GetRegistry().get<SunEngine::ColorComponent>(s_LeftWeaponEntity);
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

// =========================================================================
// RENDER & UI METHODS
// =========================================================================

void GameLayer::OnRender()
{
    m_RenderSystem.Render(m_Scene);
    m_ParticleSystem.Render(m_Scene);

    if (m_GameState == GameState::Gameplay) {
        glm::vec2 screen = glm::vec2(1280.0f, 720.0f);
        glm::vec2 center = screen * 0.5f;

        if (m_Scene.GetRegistry().all_of<SunEngine::InventoryComponent>(m_PlayerEntity)) {
            auto& inv = m_Scene.GetRegistry().get<SunEngine::InventoryComponent>(m_PlayerEntity);

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

                SunEngine::Renderer::DrawQuadUI(pos, glm::vec2(slotSize, slotSize), boxColor, screen);

                if (isActive) {
                    SunEngine::Renderer::DrawQuadUI(pos + glm::vec2(3.0f, 3.0f), glm::vec2(slotSize - 6.0f, 4.0f), glm::vec4(1.0f, 0.9f, 0.0f, 1.0f), screen);
                }
            }
        }

        float dotSize = 4.0f;
        glm::vec2 dotPos = center - glm::vec2(dotSize * 0.5f);
        SunEngine::Renderer::DrawQuadUI(dotPos, glm::vec2(dotSize, dotSize), glm::vec4(1.0f, 1.0f, 1.0f, 0.9f), screen);

        if (m_Scene.GetRegistry().all_of<SunEngine::WeaponStateComponent, SunEngine::WeaponBlueprintId>(m_PlayerEntity)) {
            auto& weaponState = m_Scene.GetRegistry().get<SunEngine::WeaponStateComponent>(m_PlayerEntity);
            const auto* weaponBP = SunEngine::BlueprintManager::Get().GetWeapon(
                m_Scene.GetRegistry().get<SunEngine::WeaponBlueprintId>(m_PlayerEntity).id
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

                    SunEngine::Renderer::DrawQuadUI(segPos, glm::vec2(5.0f, 5.0f), segColor, screen);
                }
            }
        }

        if (m_Scene.GetRegistry().all_of<SunEngine::WeaponStateComponent, SunEngine::WeaponBlueprintId>(m_PlayerEntity)) {
            auto& weaponState = m_Scene.GetRegistry().get<SunEngine::WeaponStateComponent>(m_PlayerEntity);
            const auto* weaponBP = SunEngine::BlueprintManager::Get().GetWeapon(
                m_Scene.GetRegistry().get<SunEngine::WeaponBlueprintId>(m_PlayerEntity).id
            );

            if (weaponBP && weaponBP->heat_capacity > 0.0f) {
                float heatRatio = std::clamp(weaponState.currentHeat / weaponBP->heat_capacity, 0.0f, 1.0f);

                if (heatRatio > 0.01f || weaponState.isOverheating) {
                    float barWidth = 40.0f;
                    float barHeight = 4.0f;
                    glm::vec2 barPos = glm::vec2(center.x - (barWidth * 0.5f), center.y + 14.0f);

                    SunEngine::Renderer::DrawQuadUI(barPos, glm::vec2(barWidth, barHeight), glm::vec4(0.1f, 0.1f, 0.1f, 0.6f), screen);

                    glm::vec4 heatColor = weaponState.isOverheating ? glm::vec4(1.0f, 0.1f, 0.1f, 1.0f) :
                        glm::vec4(1.0f, 1.0f - (heatRatio * 0.8f), 0.0f, 0.9f);

                    SunEngine::Renderer::DrawQuadUI(barPos, glm::vec2(barWidth * heatRatio, barHeight), heatColor, screen);
                }
            }
        }

        if (m_Scene.GetRegistry().all_of<SunEngine::HealthComponent>(m_PlayerEntity)) {
            auto& health = m_Scene.GetRegistry().get<SunEngine::HealthComponent>(m_PlayerEntity);
            float hpRatio = std::clamp(std::max(health.currentHealth / health.maxHealth, 0.0f), 0.0f, 1.0f);

            SunEngine::Renderer::DrawQuadUI(glm::vec2(1000.0f, 30.0f), glm::vec2(250.0f, 30.0f), glm::vec4(0.3f, 0.0f, 0.0f, 0.8f), screen);
            glm::vec4 hpColor = (hpRatio > 0.4f) ? glm::vec4(0.0f, 1.0f, 0.2f, 1.0f) : glm::vec4(1.0f, 0.1f, 0.1f, 1.0f);
            SunEngine::Renderer::DrawQuadUI(glm::vec2(1000.0f, 30.0f), glm::vec2(250.0f * hpRatio, 30.0f), hpColor, screen);
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
        auto* playerHealth = m_Scene.GetRegistry().try_get<SunEngine::HealthComponent>(m_PlayerEntity);
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
                g_WaaaghActive = false;
                g_WaaaghTimer = 0.0f;
                g_WaaaghSpawnCount = 1;

                auto enemyView = m_Scene.GetRegistry().view<SunEngine::HealthComponent>(entt::exclude<SunEngine::PlayerTag>);
                for (auto e : enemyView) {
                    m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
                    m_Scene.DestroyEntity(e);
                }

                if (auto* hp = m_Scene.GetRegistry().try_get<SunEngine::HealthComponent>(m_PlayerEntity)) {
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
                ImGui::SliderFloat("##VolumeSFX", &m_VolumeSFX, 0.0f, 1.0f, "%.2f");

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
            g_WaaaghActive = false;
            g_WaaaghTimer = 0.0f;
            g_WaaaghSpawnCount = 1;

            auto enemyView = m_Scene.GetRegistry().view<SunEngine::HealthComponent>(entt::exclude<SunEngine::PlayerTag>);
            for (auto e : enemyView) {
                m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
                m_Scene.DestroyEntity(e);
            }

            if (auto* hp = m_Scene.GetRegistry().try_get<SunEngine::HealthComponent>(m_PlayerEntity)) {
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

                    if (m_Scene.GetRegistry().all_of<SunEngine::TransformComponent>(m_PlayerEntity)) {
                        auto& playerTrans = m_Scene.GetRegistry().get<SunEngine::TransformComponent>(m_PlayerEntity);
                        if (!m_CurrentMapData.playerSpawnPoints.empty()) {
                            int randomSpawnIdx = rand() % m_CurrentMapData.playerSpawnPoints.size();
                            playerTrans.Position = m_CurrentMapData.playerSpawnPoints[randomSpawnIdx];
                            m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, playerTrans.Position);
                        }
                    }

                    auto& hp = m_Scene.GetRegistry().get<SunEngine::HealthComponent>(m_PlayerEntity);
                    hp.currentHealth = hp.maxHealth;

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
        const auto* currentBp = SunEngine::BlueprintManager::Get().GetUnit(activeUnit.blueprintId);

        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 380.0f, 80.0f));
        ImGui::SetNextWindowSize(ImVec2(360, io.DisplaySize.y - 100.0f));
        ImGui::Begin("RESOCONTO UNITA'", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", activeUnit.customName.c_str());
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Classe: %s", currentBp ? currentBp->displayName.c_str() : "Sconosciuta");
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "STATISTICHE COMBATTIMENTO");
        if (currentBp) {
            ImGui::Text("Salute MAX: %.0f HP", currentBp->health);
            if (m_Scene.GetRegistry().all_of<SunEngine::UnitStatsComponent>(m_PlayerEntity)) {
                int avgArmor = m_Scene.GetRegistry().get<SunEngine::UnitStatsComponent>(m_PlayerEntity).armorSet.GetAverageRating();
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

        const auto* primBP = SunEngine::BlueprintManager::Get().GetWeapon(activeUnit.primaryWeaponId);
        ImGui::Text("Primaria: %s", primBP ? primBP->displayName.c_str() : "[Vuoto]");

        const auto* secBP = SunEngine::BlueprintManager::Get().GetWeapon(activeUnit.secondaryWeaponId);
        ImGui::Text("Secondaria: %s", secBP ? secBP->displayName.c_str() : "[Vuoto]");

        const auto* meleeBP = SunEngine::BlueprintManager::Get().GetWeapon(activeUnit.meleeWeaponId);
        ImGui::Text("Corpo a Corpo: %s", meleeBP ? meleeBP->displayName.c_str() : "[Nessuna / Disarmato]");
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
    ImGui::SetNextWindowSize(ImVec2(600, 500));

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
                    // 1. SELEZIONA ARMA PRIMARIA
                    const auto* primBP = SunEngine::BlueprintManager::Get().GetWeapon(activeUnit.primaryWeaponId);
                    std::string primName = primBP ? primBP->displayName : "Nessuna";
                    if (ImGui::BeginCombo("Seleziona Primaria", primName.c_str())) {
                        for (const auto& [wId, wBP] : SunEngine::BlueprintManager::Get().GetWeapons()) {
                            if (wBP.slot_type == SunEngine::WeaponSlotType::Primary) {
                                if (ImGui::Selectable(wBP.displayName.c_str(), activeUnit.primaryWeaponId == wId)) {
                                    activeUnit.primaryWeaponId = wId;
                                    ApplyUnitVisuals(activeUnit);
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }

                    // 2. SELEZIONA ARMA SECONDARIA
                    const auto* secBP = SunEngine::BlueprintManager::Get().GetWeapon(activeUnit.secondaryWeaponId);
                    std::string secName = secBP ? secBP->displayName : "Nessuna";
                    if (ImGui::BeginCombo("Seleziona Secondaria", secName.c_str())) {
                        for (const auto& [wId, wBP] : SunEngine::BlueprintManager::Get().GetWeapons()) {
                            if (wBP.slot_type == SunEngine::WeaponSlotType::Secondary) {
                                if (ImGui::Selectable(wBP.displayName.c_str(), activeUnit.secondaryWeaponId == wId)) {
                                    activeUnit.secondaryWeaponId = wId;
                                    ApplyUnitVisuals(activeUnit);
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }

                    // 3. SELEZIONA ARMA CORPO A CORPO (MELEE)
                    const auto* meleeBP = SunEngine::BlueprintManager::Get().GetWeapon(activeUnit.meleeWeaponId);
                    std::string meleeName = meleeBP ? meleeBP->displayName : "[Nessuna / Disarmato]";
                    if (ImGui::BeginCombo("Seleziona Arma Melee", meleeName.c_str())) {

                        if (ImGui::Selectable("[Nessuna / Disarmato]", activeUnit.meleeWeaponId.empty() || activeUnit.meleeWeaponId == "none")) {
                            activeUnit.meleeWeaponId = "";
                            ApplyUnitVisuals(activeUnit);
                        }

                        for (const auto& [wId, wBP] : SunEngine::BlueprintManager::Get().GetWeapons()) {
                            if (wBP.slot_type == SunEngine::WeaponSlotType::Melee || wBP.is_melee) {
                                if (ImGui::Selectable(wBP.displayName.c_str(), activeUnit.meleeWeaponId == wId)) {
                                    activeUnit.meleeWeaponId = wId;
                                    ApplyUnitVisuals(activeUnit);
                                }
                            }
                        }
                        ImGui::EndCombo();
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
            const auto* unitBp = SunEngine::BlueprintManager::Get().GetUnit(activeUnit.blueprintId);
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

            auto drawArmorSlotCombo = [&](const char* slotLabel, SunEngine::HitboxLocation location, std::string& currentId) {
                const auto* currentBp = SunEngine::BlueprintManager::Get().GetArmorPiece(currentId);

                ImGui::PushID(slotLabel);
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "[%s]", slotLabel);

                std::string comboName = currentBp ? currentBp->displayName + " (" + std::to_string(currentBp->pointCost) + " Pti)" : "[Scoperto / Nessuna Protezione]";

                if (ImGui::BeginCombo("##combo", comboName.c_str())) {

                    if (ImGui::Selectable("[Scoperto / Nessuna Protezione]", currentId.empty() || currentId == "none")) {
                        currentId = "none";
                        ApplyUnitVisuals(activeUnit);
                    }

                    for (const auto& [aId, aBp] : SunEngine::BlueprintManager::Get().GetArmorPieces()) {

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
                            if (isSelected) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
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

            drawArmorSlotCombo("TESTA", SunEngine::HitboxLocation::Head, activeUnit.armorHeadId);
            drawArmorSlotCombo("TORSO", SunEngine::HitboxLocation::Torso, activeUnit.armorTorsoId);
            drawArmorSlotCombo("BRACCIO SX", SunEngine::HitboxLocation::LeftArm, activeUnit.armorLeftArmId);

            ImGui::NextColumn();

            drawArmorSlotCombo("BRACCIO DX", SunEngine::HitboxLocation::RightArm, activeUnit.armorRightArmId);
            drawArmorSlotCombo("GAMBA SX", SunEngine::HitboxLocation::LeftLeg, activeUnit.armorLeftLegId);
            drawArmorSlotCombo("GAMBA DX", SunEngine::HitboxLocation::RightLeg, activeUnit.armorRightLegId);

            ImGui::Columns(1);
        }

        ImGui::SetCursorPosY(450.0f);
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

        const auto* bp = SunEngine::BlueprintManager::Get().GetUnit(m_PlayerRoster[i].blueprintId);
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
        }
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.25f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.35f, 0.15f, 1.0f));
    if (ImGui::Button("AVVIA SIMULAZIONE", ImVec2(-1, 65))) {
        if (!m_SelectedSquadIndices.empty()) {

            auto view = m_Scene.GetRegistry().view<SunEngine::ColliderComponent>(entt::exclude<SunEngine::PlayerTag>);
            for (auto e : view) {
                m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
                m_Scene.DestroyEntity(e);
            }

            std::string mapsDir = FindValidDir("Maps");
            if (mapsDir.empty()) mapsDir = "GameData/Maps/";

            m_CurrentMapData = SunEngine::MapLoader::LoadFromFile(m_Scene, mapsDir + m_SelectedMapId + ".json");
            m_PhysicsSystem.RefreshBodies(m_Scene);

            if (m_CurrentMapData.navMesh) {
                m_NavQuery.Init(m_CurrentMapData.navMesh);
            }

            m_ActiveSquad.clear();
            for (int rosterIdx : m_SelectedSquadIndices) {
                ActiveSquadMember member;
                member.rosterIndex = rosterIdx;
                member.isAlive = true;
                m_ActiveSquad.push_back(member);
            }

            m_CurrentSquadMemberIdx = 0;
            m_KillCount = 0;

            ApplyUnitVisuals(m_PlayerRoster[m_ActiveSquad[0].rosterIndex]);

            if (m_Scene.GetRegistry().all_of<SunEngine::TransformComponent>(m_PlayerEntity)) {
                auto& playerTrans = m_Scene.GetRegistry().get<SunEngine::TransformComponent>(m_PlayerEntity);
                if (!m_CurrentMapData.playerSpawnPoints.empty()) {
                    playerTrans.Position = m_CurrentMapData.playerSpawnPoints[0];
                    m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, playerTrans.Position);
                }
            }

            if (auto* hp = m_Scene.GetRegistry().try_get<SunEngine::HealthComponent>(m_PlayerEntity)) {
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