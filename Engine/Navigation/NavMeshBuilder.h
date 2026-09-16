#pragma once
#include <vector>
#include <memory>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Forward declaration di Recast/Detour per non inquinare gli header del motore
class dtNavMesh;
struct rcHeightfield;
struct rcCompactHeightfield;
struct rcContourSet;
struct rcPolyMesh;
struct rcPolyMeshDetail;

namespace SunEngine {

    // Deleter custom: chiama dtFreeNavMesh (API C di Detour)
    struct NavMeshDeleter {
        void operator()(dtNavMesh* mesh) const;
    };

    // Alias di ownership: usare SEMPRE questo invece di dtNavMesh*
    using NavMeshPtr = std::unique_ptr<dtNavMesh, NavMeshDeleter>;

    class NavMeshBuilder {
    public:
        NavMeshBuilder();
        ~NavMeshBuilder();

        void Reset();

        void AddPlane(const glm::vec3& center, const glm::vec3& size);

        void AddBox(const glm::vec3& center, const glm::vec3& halfExtents,
            const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

        // Restituisce ownership esclusiva della NavMesh
        NavMeshPtr Build();

    private:
        std::vector<float> m_Vertices;
        std::vector<int> m_Indices;

        glm::vec3 m_MinBounds;
        glm::vec3 m_MaxBounds;

        void AddTriangle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2);
        void AddFace(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& v3);
    };

}