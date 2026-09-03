#include "NavMeshQuery.h"
#include "../Core/Logger/Log.h"
#include <DetourNavMeshQuery.h>

namespace SunEngine {

    NavMeshQuery::NavMeshQuery() {
    }

    NavMeshQuery::~NavMeshQuery() {
        Destroy();
    }

    bool NavMeshQuery::Init(dtNavMesh* navMesh) {
        Destroy();
        if (!navMesh) return false;

        m_NavMesh = navMesh;
        m_NavQuery = dtAllocNavMeshQuery();

        // Inizializziamo permettendo di gestire percorsi composti fino a 2048 nodi
        dtStatus status = m_NavQuery->init(m_NavMesh, 2048);
        if (dtStatusFailed(status)) {
            SUN_CORE_ERROR("NavMeshQuery: Inizializzazione fallita.");
            return false;
        }

        return true;
    }

    void NavMeshQuery::Destroy() {
        if (m_NavQuery) {
            dtFreeNavMeshQuery(m_NavQuery);
            m_NavQuery = nullptr;
        }
        m_NavMesh = nullptr;
    }

    glm::vec3 NavMeshQuery::GetNearestValidPoint(const glm::vec3& point) {
        if (!m_NavQuery) return point;

        dtQueryFilter filter;
        filter.setIncludeFlags(0xFFFF);
        filter.setExcludeFlags(0);

        // Raggio di ricerca per agganciare un punto fluttuante alla mesh
        float extents[3] = { 2.0f, 4.0f, 2.0f };
        float nearestPt[3];
        dtPolyRef nearestRef;

        m_NavQuery->findNearestPoly(&point.x, extents, &filter, &nearestRef, nearestPt);

        if (nearestRef != 0) {
            return glm::vec3(nearestPt[0], nearestPt[1], nearestPt[2]);
        }
        return point;
    }

    std::vector<glm::vec3> NavMeshQuery::FindPath(const glm::vec3& start, const glm::vec3& end) {
        std::vector<glm::vec3> waypoints;
        if (!m_NavQuery) return waypoints;

        dtQueryFilter filter;
        filter.setIncludeFlags(0xFFFF);
        filter.setExcludeFlags(0);

        float extents[3] = { 2.0f, 4.0f, 2.0f };

        dtPolyRef startRef, endRef;
        float startPt[3], endPt[3];

        m_NavQuery->findNearestPoly(&start.x, extents, &filter, &startRef, startPt);
        m_NavQuery->findNearestPoly(&end.x, extents, &filter, &endRef, endPt);

        if (startRef == 0 || endRef == 0) {
            return waypoints; // Punti inesistenti o fuori mappa
        }

        dtPolyRef pathPolys[256];
        int pathCount = 0;

        // 1. Calcola il percorso a blocchi (Poligoni)
        m_NavQuery->findPath(startRef, endRef, startPt, endPt, &filter, pathPolys, &pathCount, 256);

        if (pathCount > 0) {
            float straightPath[256 * 3];
            unsigned char straightPathFlags[256];
            dtPolyRef straightPathPolys[256];
            int straightPathCount = 0;

            // 2. "Tira la corda" (String Pulling) per tracciare linee rette tra i corner della mesh
            m_NavQuery->findStraightPath(startPt, endPt, pathPolys, pathCount,
                straightPath, straightPathFlags, straightPathPolys, &straightPathCount, 256);

            for (int i = 0; i < straightPathCount; ++i) {
                waypoints.push_back(glm::vec3(
                    straightPath[i * 3],
                    straightPath[i * 3 + 1],
                    straightPath[i * 3 + 2]
                ));
            }
        }

        return waypoints;
    }

}