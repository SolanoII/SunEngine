#pragma once
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Forward declarations di Recast/Detour per non inquinare gli header del motore
class dtNavMesh;
struct rcHeightfield;
struct rcCompactHeightfield;
struct rcContourSet;
struct rcPolyMesh;
struct rcPolyMeshDetail;

namespace SunEngine {

    class NavMeshBuilder {
    public:
        NavMeshBuilder();
        ~NavMeshBuilder();

        // Pulisce la geometria accumulata
        void Reset();

        // Aggiunge un piano (es. il terreno base)
        void AddPlane(const glm::vec3& center, const glm::vec3& size);

        // Aggiunge un blocco, un muretto o un edificio (supporta rotazioni per le rampe!)
        void AddBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

        // Esegue il processo di Voxelizzazione di Recast e restituisce la NavMesh finita
        dtNavMesh* Build();

    private:
        std::vector<float> m_Vertices;
        std::vector<int> m_Indices;

        glm::vec3 m_MinBounds;
        glm::vec3 m_MaxBounds;

        void AddTriangle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2);
        void AddFace(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& v3);
    };

}