#include "MapLoader.h"
#include "../ECS/Components.h"
#include "../Core/Logger/Log.h"
#include "../Navigation/NavMeshBuilder.h"
#include "../Renderer/SurfaceMaterials.h"
#include <cmath>
#include <cstdint>
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
        auto& col = scene.GetRegistry().emplace<ColliderComponent>(entity, glm::vec3(0.5f));
        // FIX #1: Position e' gia' il CENTRO del cubo (mesh unitario centrato, navmesh e raycast
        // AABB lo trattano cosi'). L'offset (0,0.5,0) spostava SOLO il corpo Jolt di 0.5*Scale.y
        // verso l'alto rispetto alla geometria visibile -> ragdoll/proiettili fisici sfasati.
        col.localCenterOffset = glm::vec3(0.0f);
        scene.GetRegistry().emplace<RigidBodyComponent>(entity, glm::vec3(0.0f), 0.0f, false);

        auto& cComp = scene.GetRegistry().emplace<ColorComponent>(entity);
        cComp.Color = color;
        cComp.BaseColor = color;
        // materiale procedurale dedotto da colore e forma (il chiamante puo' cambiarlo, es. terreno o "material" nel JSON)
        scene.GetRegistry().emplace<SurfaceMaterialComponent>(entity, SurfaceMaterialComponent{ GuessSurfaceMaterial(color, scale, false) });

        return entity;
    }

    // Detriti sparsi: macerie e assi ai piedi di muri, bunker e trincee, sassi, lamiere e casse sul terreno.
    // Solo da vedere: niente collisioni, niente navmesh. Sempre gli stessi per la stessa mappa (seme dal nome).
    void MapLoader::ScatterDecor(Scene& scene, const std::string& mapId, const glm::vec3& terrainSize, const std::vector<DecorSource>& blocks) {
        uint32_t seed = 2166136261u;
        for (char c : mapId) { seed ^= static_cast<uint8_t>(c); seed *= 16777619u; }
        auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFFFF) / 16777215.0f; };
        auto insideBlock = [&](const glm::vec3& p, float margin) {
            for (const auto& b : blocks) {
                const glm::vec3 d = glm::abs(p - b.center) - (b.scale * 0.5f + glm::vec3(margin));
                if (d.x < 0.0f && d.z < 0.0f && p.y < b.center.y + b.scale.y * 0.5f) return true;
            }
            return false;
        };
        int count = 0;
        const int maxDecor = 260;
        auto spawn = [&](const glm::vec3& footPos, const glm::vec3& size, const glm::vec3& rot, const glm::vec4& color, int material) {
            if (count >= maxDecor) return;
            ++count;
            auto e = scene.CreateEntity();
            auto& t = scene.GetRegistry().emplace<TransformComponent>(e);
            t.Position = footPos + glm::vec3(0.0f, size.y * 0.5f - 0.015f, 0.0f);
            t.Scale = size;
            t.Rotation = rot;
            scene.GetRegistry().emplace<MeshComponent>(e, 0u);
            auto& c = scene.GetRegistry().emplace<ColorComponent>(e);
            c.Color = color; c.BaseColor = color;
            scene.GetRegistry().emplace<SurfaceMaterialComponent>(e, SurfaceMaterialComponent{ material });
            scene.GetRegistry().emplace<MapDecorTag>(e);
        };
        const glm::vec4 grey(0.34f, 0.33f, 0.31f, 1.0f), wood(0.18f, 0.13f, 0.09f, 1.0f), rust(0.22f, 0.13f, 0.08f, 1.0f), sand(0.30f, 0.26f, 0.19f, 1.0f);
        // 1) ai piedi delle strutture
        for (const auto& b : blocks) {
            if (b.scale.y < 0.7f || b.center.y - b.scale.y * 0.5f > 0.3f) continue;   // solo cose appoggiate a terra
            if (b.material != Surface_Concrete && b.material != Surface_Metal && b.material != Surface_Earthworks && b.material != Surface_Sandbags) continue;
            const float perim = 2.0f * (b.scale.x + b.scale.z);
            const int n = std::min(8, static_cast<int>(perim * 0.22f) + 1);
            for (int i = 0; i < n; ++i) {
                // punto sul perimetro, appena fuori
                float u = rnd() * perim;
                glm::vec3 p = b.center; p.y = 0.0f;
                const float hx = b.scale.x * 0.5f, hz = b.scale.z * 0.5f, out = 0.15f + rnd() * 0.5f;
                if (u < b.scale.x) p += glm::vec3(-hx + u, 0.0f, hz + out);
                else if ((u -= b.scale.x) < b.scale.z) p += glm::vec3(hx + out, 0.0f, -hz + u);
                else if ((u -= b.scale.z) < b.scale.x) p += glm::vec3(hx - u, 0.0f, -hz - out);
                else { u -= b.scale.x; p += glm::vec3(-hx - out, 0.0f, hz - u); }
                if (insideBlock(p, 0.05f)) continue;
                const glm::vec3 rot(rnd() * 30.0f - 15.0f, rnd() * 360.0f, rnd() * 30.0f - 15.0f);
                if (b.material == Surface_Sandbags && rnd() < 0.5f) spawn(p, glm::vec3(0.5f, 0.17f, 0.3f), glm::vec3(0.0f, rnd() * 360.0f, rnd() * 10.0f), sand, Surface_Sandbags);   // sacco caduto
                else if (b.material == Surface_Earthworks && rnd() < 0.5f) spawn(p, glm::vec3(0.9f + rnd() * 0.6f, 0.04f, 0.16f), glm::vec3(0.0f, rnd() * 360.0f, 0.0f), wood, Surface_Wood);   // asse
                else { const float s = 0.08f + rnd() * rnd() * 0.32f; spawn(p, glm::vec3(s * (0.8f + rnd() * 0.8f), s * (0.5f + rnd() * 0.6f), s * (0.8f + rnd() * 0.8f)), rot, grey, Surface_Rubble); }
            }
        }
        // 2) sul terreno
        const float area = terrainSize.x * terrainSize.z;
        const int n = std::min(maxDecor - count, static_cast<int>(area / 140.0f));
        for (int i = 0; i < n * 2 && count < maxDecor; ++i) {
            const glm::vec3 p((rnd() - 0.5f) * terrainSize.x * 0.94f, 0.0f, (rnd() - 0.5f) * terrainSize.z * 0.94f);
            if (insideBlock(p, 0.4f)) continue;
            const float k = rnd();
            if (k < 0.5f) { const float s = 0.06f + rnd() * rnd() * 0.3f; spawn(p, glm::vec3(s, s * 0.6f, s * 1.2f), glm::vec3(rnd() * 40.0f - 20.0f, rnd() * 360.0f, rnd() * 40.0f - 20.0f), grey, Surface_Rubble); }   // sassi e macerie
            else if (k < 0.72f) spawn(p, glm::vec3(1.0f + rnd() * 0.8f, 0.035f, 0.15f), glm::vec3(rnd() * 6.0f, rnd() * 360.0f, rnd() * 6.0f), wood, Surface_Wood);   // assi
            else if (k < 0.88f) spawn(p, glm::vec3(0.5f + rnd() * 0.6f, 0.02f, 0.4f + rnd() * 0.5f), glm::vec3(rnd() * 12.0f - 6.0f, rnd() * 360.0f, rnd() * 12.0f - 6.0f), rust, Surface_Metal);   // lamiere
            else spawn(p, glm::vec3(0.6f, 0.36f, 0.4f), glm::vec3(0.0f, rnd() * 360.0f, 0.0f), wood, Surface_Wood);   // cassa di munizioni
        }
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
        glm::vec3 terrainSize(0.0f);
        std::vector<DecorSource> decorBlocks;

        // Terreno Principale
        if (j.contains("terrain")) {
            auto t = j["terrain"];
            glm::vec3 size = { t["size"][0], t["size"][1], t["size"][2] };
            glm::vec4 color = { t["color"][0], t["color"][1], t["color"][2], t["color"][3] };

            const entt::entity terrain = CreateBlock(scene, glm::vec3(0.0f, -0.5f, 0.0f), size, color);
            scene.GetRegistry().get<SurfaceMaterialComponent>(terrain).id = Surface_Mud;   // fango, crateri, pozzanghere
            navBuilder.AddPlane(glm::vec3(0.0f, 0.0f, 0.0f), size);
            terrainSize = size;
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

        // Manichini del campo da tiro (opzionale)
        if (j.contains("training_dummies") && j["training_dummies"].is_array()) {
            for (const auto& d : j["training_dummies"]) {
                TrainingDummySpawn spawn;
                spawn.unitId = d.value("unit", std::string());
                if (d.contains("position") && d["position"].is_array() && d["position"].size() >= 3) {
                    spawn.position = glm::vec3(d["position"][0], d["position"][1], d["position"][2]);
                }
                spawn.yawDegrees = d.value("yaw", 0.0f);
                if (!spawn.unitId.empty()) data.trainingDummies.push_back(spawn);
            }
        }

        // Strutture ed Edifici
        if (j.contains("structures")) {
            for (const auto& s : j["structures"]) {
                std::string type = s.value("type", "block");
                glm::vec4 color = { s["color"][0], s["color"][1], s["color"][2], s["color"][3] };
                const int forcedMaterial = SurfaceMaterialFromName(s.value("material", std::string()));   // facoltativo

                if (type == "building") {
                    glm::vec3 pos = { s["position"][0], s["position"][1], s["position"][2] };
                    float w = s.value("width", 10.0f);
                    float d = s.value("depth", 10.0f);
                    float h = s.value("floor_height", 3.5f);

                    CreateBuilding(scene, pos, w, d, h, color);
                    (void)forcedMaterial;   // gli edifici prendono il materiale dedotto dal colore

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
                    // FIX #1: nei JSON "block" position.y e' la BASE del blocco (barricata y=0, h=0.4
                    // poggia sul terreno). Le entity (mesh centrata, AABB, navmesh, Jolt) usano invece
                    // Position = CENTRO: l'editor lo fa gia' con localPos.y = scale.y*0.5. Qui mancava la
                    // conversione, quindi i blocchi affondavano di scale.y/2 nel pavimento.
                    // I JSON restano invariati; CreateBlock continua a prendere un CENTRO (come per
                    // terreno, rampe ed edifici).
                    const glm::vec3 center = pos + glm::vec3(0.0f, scale.y * 0.5f, 0.0f);
                    const entt::entity blk = CreateBlock(scene, center, scale, color);
                    auto& surf = scene.GetRegistry().get<SurfaceMaterialComponent>(blk);
                    if (forcedMaterial >= 0) surf.id = forcedMaterial;
                    decorBlocks.push_back({ center, scale, surf.id });
                    navBuilder.AddBox(center, scale * 0.5f);
                }
            }
        }

        if (terrainSize.x > 0.0f && j.value("decor", true)) ScatterDecor(scene, data.id, terrainSize, decorBlocks);   // "decor": false per spegnerli

        data.navMesh = navBuilder.Build();

        SUN_CORE_INFO("Mappa caricata con successo: {0} ({1} Punti Spawn Giocatore)", data.name, data.playerSpawnPoints.size());
        return data;
    }

}