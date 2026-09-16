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

        // IMPORTANTE: navMesh è NON-OWNING. Il chiamante garantisce che
        // la NavMesh sopravviva alla query. Preferibilmente usa NavMeshPtr
        // e chiama Destroy() PRIMA di far morire la mesh.
        bool Init(dtNavMesh* navMesh);

        // Libera la query e scollega dalla mesh (non libera la mesh)
        void Destroy();

        std::vector<glm::vec3> FindPath(const glm::vec3& start, const glm::vec3& end);
        glm::vec3 GetNearestValidPoint(const glm::vec3& point);

    private:
        dtNavMesh* m_NavMesh = nullptr;         // non-owning
        dtNavMeshQuery* m_NavQuery = nullptr;   // owned
    };

}