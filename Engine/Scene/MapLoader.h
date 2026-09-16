#pragma once
#include "Scene.h"
#include "../Navigation/NavMeshBuilder.h"   // <-- ASSICURATI che ci sia
#include <string>
#include <vector>
#include <glm/glm.hpp>

class dtNavMesh; // Forward declaration per Recast/Detour

namespace SunEngine {

    struct MapData {
        std::string id;
        std::string name;
        std::vector<glm::vec3> playerSpawnPoints;
        std::vector<glm::vec3> enemySpawnPoints;
        // Ownership esclusiva: quando MapData muore, la mesh viene liberata
        NavMeshPtr navMesh;
    };

    class MapLoader {
    public:
        static MapData LoadFromFile(Scene& scene, const std::string& filepath);

    private:
        static entt::entity CreateBlock(Scene& scene, const glm::vec3& pos, const glm::vec3& scale,
            const glm::vec4& color,
            const glm::vec3& rotEuler = glm::vec3(0.0f));
        static entt::entity CreateRamp(Scene& scene, const glm::vec3& startPos, const glm::vec3& endPos,
            float width, const glm::vec4& color);
        static void CreateBuilding(Scene& scene, const glm::vec3& centerPos, float width, float depth,
            float floorHeight, const glm::vec4& color);
    };

}