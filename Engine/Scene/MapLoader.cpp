#include "MapLoader.h"
#include "../ECS/Components.h"
#include "../Core/Logger/Log.h"
#include "../Navigation/NavMeshBuilder.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <glm/gtc/quaternion.hpp>

namespace SunEngine {

    entt::entity MapLoader::CreateBlock(Scene& scene, const glm::vec3& pos, const glm::vec3& scale, const glm::vec4& color, const glm::vec3& rotEuler) {
        auto entity = scene.CreateEntity();
        auto& trans = scene.GetRegistry().emplace<TransformComponent>(entity);
        trans.Position = pos;
        trans.Scale = scale; // Scala reale 3D
        trans.Rotation = rotEuler;

        scene.GetRegistry().emplace<MeshComponent>(entity, 0u);
        // FIX DOUBLE SCALING: halfExtents unitario (0.5f), la dimensione finale e' gestita da trans.Scale
        scene.GetRegistry().emplace<ColliderComponent>(entity, glm::vec3(0.5f));
        scene.GetRegistry().emplace<RigidBodyComponent>(entity, glm::vec3(0.0f), 0.0f, false);

        auto& cComp = scene.GetRegistry().emplace<ColorComponent>(entity);
        cComp.Color = color;
        cComp.BaseColor = color;

        return entity;
    }

    entt::entity MapLoader::CreateRamp(Scene& scene, const glm::vec3& startPos, const glm::vec3& endPos, float width, const glm::vec4& color) {
        glm::vec3 dir = endPos - startPos;
        float length = glm::length(dir);
        glm::vec3 midPoint = (startPos + endPos) * 0.5f;

        float pitch = glm::degrees(std::atan2(dir.y, std::sqrt(dir.x * dir.x + dir.z * dir.z)));
        float yaw = glm::degrees(std::atan2(-dir.x, -dir.z));

        return CreateBlock(scene, midPoint, glm::vec3(width, 0.3f, length), color, glm::vec3(-pitch, yaw, 0.0f));
    }

    void MapLoader::CreateBuilding(Scene& scene, const glm::vec3& centerPos, float width, float depth, float floorHeight, const glm::vec4& color) {
        float wallThick = 0.4f;
        float doorWidth = 3.0f;
        float midH = centerPos.y + (floorHeight * 0.5f);

        CreateBlock(scene, centerPos + glm::vec3(0.0f, midH, -depth * 0.5f), glm::vec3(width, floorHeight, wallThick), color);
        CreateBlock(scene, centerPos + glm::vec3(width * 0.5f, midH, 0.0f), glm::vec3(wallThick, floorHeight, depth), color);
        CreateBlock(scene, centerPos + glm::vec3(-width * 0.5f, midH, 0.0f), glm::vec3(wallThick, floorHeight, depth), color);

        float sideWallW = (width - doorWidth) * 0.5f;
        CreateBlock(scene, centerPos + glm::vec3(-width * 0.5f + sideWallW * 0.5f, midH, depth * 0.5f), glm::vec3(sideWallW, floorHeight, wallThick), color);
        CreateBlock(scene, centerPos + glm::vec3(width * 0.5f - sideWallW * 0.5f, midH, depth * 0.5f), glm::vec3(sideWallW, floorHeight, wallThick), color);
        CreateBlock(scene, centerPos + glm::vec3(0.0f, centerPos.y + floorHeight - 0.4f, depth * 0.5f), glm::vec3(doorWidth, 0.8f, wallThick), color);

        CreateBlock(scene, centerPos + glm::vec3(0.0f, centerPos.y + floorHeight, 0.0f), glm::vec3(width, 0.3f, depth), color * 1.15f);
    }

    MapData MapLoader::LoadFromFile(Scene& scene, const std::string& filepath) {
        MapData data;
        std::ifstream file(filepath);
        if (!file.is_open()) {
            SUN_CORE_ERROR("Impossibile caricare la mappa dal file: {0}", filepath);
            return data;
        }

        nlohmann::json j;
        file >> j;

        data.id = j.value("id", "map_default");
        data.name = j.value("name", "Scenario Ignoto");

        NavMeshBuilder navBuilder;

        // Terreno Principale
        if (j.contains("terrain")) {
            auto t = j["terrain"];
            glm::vec3 size = { t["size"][0], t["size"][1], t["size"][2] };
            glm::vec4 color = { t["color"][0], t["color"][1], t["color"][2], t["color"][3] };

            CreateBlock(scene, glm::vec3(0.0f, -0.5f, 0.0f), size, color);
            navBuilder.AddPlane(glm::vec3(0.0f, 0.0f, 0.0f), size);
        }

        // Punti Spawn Giocatore
        if (j.contains("player_spawn_points")) {
            for (const auto& sp : j["player_spawn_points"]) {
                data.playerSpawnPoints.push_back(glm::vec3(sp[0], sp[1], sp[2]));
            }
        }

        // Punti Spawn Nemici
        if (j.contains("enemy_spawn_points")) {
            for (const auto& sp : j["enemy_spawn_points"]) {
                data.enemySpawnPoints.push_back(glm::vec3(sp[0], sp[1], sp[2]));
            }
        }

        // Strutture ed Edifici
        if (j.contains("structures")) {
            for (const auto& s : j["structures"]) {
                std::string type = s.value("type", "block");
                glm::vec4 color = { s["color"][0], s["color"][1], s["color"][2], s["color"][3] };

                if (type == "building") {
                    glm::vec3 pos = { s["position"][0], s["position"][1], s["position"][2] };
                    float w = s.value("width", 10.0f);
                    float d = s.value("depth", 10.0f);
                    float h = s.value("floor_height", 3.5f);

                    CreateBuilding(scene, pos, w, d, h, color);

                    navBuilder.AddBox(pos + glm::vec3(0.0f, h * 0.5f, -d * 0.5f), glm::vec3(w, h, 0.4f) * 0.5f);
                    navBuilder.AddBox(pos + glm::vec3(w * 0.5f, h * 0.5f, 0.0f), glm::vec3(0.4f, h, d) * 0.5f);
                    navBuilder.AddBox(pos + glm::vec3(-w * 0.5f, h * 0.5f, 0.0f), glm::vec3(0.4f, h, d) * 0.5f);
                    navBuilder.AddBox(pos + glm::vec3(0.0f, h, 0.0f), glm::vec3(w, 0.3f, d) * 0.5f);

                    if (s.value("has_ramp", false)) {
                        glm::vec3 rStart = { s["ramp_start"][0], s["ramp_start"][1], s["ramp_start"][2] };
                        glm::vec3 rEnd = { s["ramp_end"][0], s["ramp_end"][1], s["ramp_end"][2] };
                        float rW = s.value("ramp_width", 3.0f);
                        CreateRamp(scene, rStart, rEnd, rW, color * 1.1f);

                        glm::vec3 dir = rEnd - rStart;
                        float length = glm::length(dir);
                        glm::vec3 mid = (rStart + rEnd) * 0.5f;
                        float pitch = std::atan2(dir.y, std::sqrt(dir.x * dir.x + dir.z * dir.z));
                        float yaw = std::atan2(-dir.x, -dir.z);
                        glm::quat rot = glm::quat(glm::vec3(-pitch, yaw, 0.0f));

                        navBuilder.AddBox(mid, glm::vec3(rW, 0.3f, length) * 0.5f, rot);
                    }
                }
                else if (type == "block") {
                    glm::vec3 pos = { s["position"][0], s["position"][1], s["position"][2] };
                    glm::vec3 scale = { s["scale"][0], s["scale"][1], s["scale"][2] };
                    CreateBlock(scene, pos, scale, color);
                    navBuilder.AddBox(pos, scale * 0.5f);
                }
            }
        }

        data.navMesh = navBuilder.Build();

        SUN_CORE_INFO("Mappa caricata con successo: {0} ({1} Punti Spawn Giocatore)", data.name, data.playerSpawnPoints.size());
        return data;
    }

}