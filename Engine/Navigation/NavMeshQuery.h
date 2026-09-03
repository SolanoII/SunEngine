#pragma once
#include <vector>
#include <glm/glm.hpp>

class dtNavMesh;
class dtNavMeshQuery;

namespace SunEngine {

    class NavMeshQuery {
    public:
        NavMeshQuery();
        ~NavMeshQuery();

        // Inizializza la query agganciandosi ad una NavMesh precedentemente creata
        bool Init(dtNavMesh* navMesh);

        // Dealloca la memoria
        void Destroy();

        // Trova il percorso più breve lungo il terreno esplorabile e le rampe
        std::vector<glm::vec3> FindPath(const glm::vec3& start, const glm::vec3& end);

        // Trova il punto calpestabile valido più vicino ad una posizione data
        glm::vec3 GetNearestValidPoint(const glm::vec3& point);

    private:
        dtNavMesh* m_NavMesh = nullptr;
        dtNavMeshQuery* m_NavQuery = nullptr;
    };

}