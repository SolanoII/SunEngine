#include "NavMeshBuilder.h"
#include "../Core/Logger/Log.h"

#include <Recast.h>
#include <DetourNavMesh.h> 
#include <DetourNavMeshBuilder.h>
#include <algorithm>
#include <cstring>

namespace SunEngine {
    void NavMeshDeleter::operator()(dtNavMesh* mesh) const {
        if (mesh) {
            dtFreeNavMesh(mesh);
        }
    }
    // Context minimale per i log di Recast
    class SunRecastContext : public rcContext {
    protected:
        virtual void doLog(const rcLogCategory category, const char* msg, const int len) override {
            if (category == RC_LOG_ERROR) SUN_CORE_ERROR("Recast Error: {0}", msg);
            else if (category == RC_LOG_WARNING) SUN_CORE_WARN("Recast Warning: {0}", msg);
        }
    };

    NavMeshBuilder::NavMeshBuilder() {
        Reset();
    }

    NavMeshBuilder::~NavMeshBuilder() {
        Reset();
    }

    void NavMeshBuilder::Reset() {
        m_Vertices.clear();
        m_Indices.clear();
        m_MinBounds = glm::vec3(99999.0f);
        m_MaxBounds = glm::vec3(-99999.0f);
    }

    void NavMeshBuilder::AddTriangle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2) {
        int startIndex = static_cast<int>(m_Vertices.size()) / 3;

        m_Vertices.insert(m_Vertices.end(), { v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z });
        m_Indices.insert(m_Indices.end(), { startIndex, startIndex + 1, startIndex + 2 });

        for (const auto& v : { v0, v1, v2 }) {
            m_MinBounds = glm::min(m_MinBounds, v);
            m_MaxBounds = glm::max(m_MaxBounds, v);
        }
    }

    void NavMeshBuilder::AddFace(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& v3) {
        AddTriangle(v0, v1, v2);
        AddTriangle(v0, v2, v3);
    }

    void NavMeshBuilder::AddPlane(const glm::vec3& center, const glm::vec3& size) {
        glm::vec3 hExt = size * 0.5f;
        glm::vec3 v0 = center + glm::vec3(-hExt.x, 0.0f, -hExt.z);
        glm::vec3 v1 = center + glm::vec3(hExt.x, 0.0f, -hExt.z);
        glm::vec3 v2 = center + glm::vec3(hExt.x, 0.0f, hExt.z);
        glm::vec3 v3 = center + glm::vec3(-hExt.x, 0.0f, hExt.z);
        AddFace(v0, v3, v2, v1); // Orientato verso l'alto (Y up)
    }

    void NavMeshBuilder::AddBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rot) {
        glm::vec3 corners[8] = {
            glm::vec3(-halfExtents.x, -halfExtents.y, -halfExtents.z),
            glm::vec3(halfExtents.x, -halfExtents.y, -halfExtents.z),
            glm::vec3(halfExtents.x, -halfExtents.y,  halfExtents.z),
            glm::vec3(-halfExtents.x, -halfExtents.y,  halfExtents.z),
            glm::vec3(-halfExtents.x,  halfExtents.y, -halfExtents.z),
            glm::vec3(halfExtents.x,  halfExtents.y, -halfExtents.z),
            glm::vec3(halfExtents.x,  halfExtents.y,  halfExtents.z),
            glm::vec3(-halfExtents.x,  halfExtents.y,  halfExtents.z)
        };

        for (int i = 0; i < 8; ++i) {
            corners[i] = center + (rot * corners[i]);
        }

        AddFace(corners[0], corners[3], corners[2], corners[1]); // Bottom
        AddFace(corners[4], corners[5], corners[6], corners[7]); // Top
        AddFace(corners[0], corners[1], corners[5], corners[4]); // Front
        AddFace(corners[2], corners[3], corners[7], corners[6]); // Back
        AddFace(corners[1], corners[2], corners[6], corners[5]); // Right
        AddFace(corners[3], corners[0], corners[4], corners[7]); // Left
    }

    NavMeshPtr NavMeshBuilder::Build() {
        if (m_Vertices.empty() || m_Indices.empty()) {
            SUN_CORE_ERROR("NavMeshBuilder: Nessuna geometria fornita per costruire la NavMesh.");
            return nullptr;
        }

        SunRecastContext ctx;
        rcConfig cfg;
        memset(&cfg, 0, sizeof(cfg));

        // --- CONFIGURAZIONE RECAST (Tarata per soldati/orchi umanoidi) ---
        cfg.cs = 0.2f;              // Cell Size (accuratezza X/Z)
        cfg.ch = 0.2f;              // Cell Height (accuratezza Y)
        cfg.walkableSlopeAngle = 45.0f; // Max pendenza rampa
        cfg.walkableHeight = (int)ceilf(1.8f / cfg.ch); // Altezza agente (1.8m)
        cfg.walkableClimb = (int)floorf(0.5f / cfg.ch); // Max gradino (0.5m)
        cfg.walkableRadius = (int)ceilf(0.4f / cfg.cs); // Raggio agente (0.4m)
        cfg.maxEdgeLen = (int)(12.0f / cfg.cs);
        cfg.maxSimplificationError = 1.3f;
        cfg.minRegionArea = (int)rcSqr(8.0f);
        cfg.mergeRegionArea = (int)rcSqr(20.0f);
        cfg.maxVertsPerPoly = 6;
        cfg.detailSampleDist = 6.0f < 0.9f ? 0 : cfg.cs * 6.0f;
        cfg.detailSampleMaxError = 1.0f;

        rcVcopy(cfg.bmin, &m_MinBounds.x);
        rcVcopy(cfg.bmax, &m_MaxBounds.x);
        rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

        // 1. Voxelizzazione (Rasterization)
        rcHeightfield* solid = rcAllocHeightfield();
        rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch);

        int ntris = static_cast<int>(m_Indices.size()) / 3;
        std::vector<unsigned char> triAreas(ntris, RC_WALKABLE_AREA);

        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, m_Vertices.data(), static_cast<int>(m_Vertices.size()) / 3, m_Indices.data(), ntris, triAreas.data());
        rcRasterizeTriangles(&ctx, m_Vertices.data(), static_cast<int>(m_Vertices.size()) / 3, m_Indices.data(), triAreas.data(), ntris, *solid, cfg.walkableClimb);

        // 2. Filtri
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

        // 3. Compact Heightfield
        rcCompactHeightfield* chf = rcAllocCompactHeightfield();
        rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf);
        rcFreeHeightField(solid);

        rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf);
        rcBuildDistanceField(&ctx, *chf);
        rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea);

        // 4. Mesh dei Contorni
        rcContourSet* cset = rcAllocContourSet();
        rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset);

        // 5. Creazione della Poly Mesh 
        rcPolyMesh* pmesh = rcAllocPolyMesh();
        rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh);

        // 6. Mesh di Dettaglio
        rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
        rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh);

        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);

        // 7. Preparazione dati per Detour
        dtNavMeshCreateParams params;
        memset(&params, 0, sizeof(params));
        params.verts = pmesh->verts;
        params.vertCount = pmesh->nverts;
        params.polys = pmesh->polys;
        params.polyAreas = pmesh->areas;
        params.polyFlags = pmesh->flags;
        params.polyCount = pmesh->npolys;
        params.nvp = pmesh->nvp;
        params.detailMeshes = dmesh->meshes;
        params.detailVerts = dmesh->verts;
        params.detailVertsCount = dmesh->nverts;
        params.detailTris = dmesh->tris;
        params.detailTriCount = dmesh->ntris;
        rcVcopy(params.bmin, pmesh->bmin);
        rcVcopy(params.bmax, pmesh->bmax);
        params.cs = cfg.cs;
        params.ch = cfg.ch;
        params.buildBvTree = true;

        unsigned char* navData = nullptr;
        int navDataSize = 0;
        if (!dtCreateNavMeshData(&params, &navData, &navDataSize)) {
            SUN_CORE_ERROR("NavMeshBuilder: Fallita la creazione dei NavMesh Data.");
            return nullptr;
        }

        // --- OWNERSHIP: unique_ptr che si auto-libera ---
        NavMeshPtr navMesh(dtAllocNavMesh());
        if (!navMesh) {
            SUN_CORE_ERROR("NavMeshBuilder: dtAllocNavMesh fallita.");
            dtFree(navData);
            return nullptr;
        }

        if (dtStatusFailed(navMesh->init(navData, navDataSize, DT_TILE_FREE_DATA))) {
            SUN_CORE_ERROR("NavMeshBuilder: Inizializzazione dtNavMesh fallita.");
            dtFree(navData);
            // navMesh esce di scope e chiama dtFreeNavMesh automaticamente
            return nullptr;
        }

        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);

        SUN_CORE_INFO("NavMesh creata con successo! (Poligoni: {0})", params.polyCount);
        return navMesh;
    }

}