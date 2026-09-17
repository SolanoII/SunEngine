#pragma once

#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include <algorithm>
#include <cmath>

#include "Components.h"
#include "Tags.h"
#include "../Scene/Scene.h"

namespace SunEngine {

    // Ray-AABB slab test. Ritorna true se il raggio colpisce il box entro maxDist.
    // outT riceve la distanza del primo hit.
    inline bool IntersectAABB(const glm::vec3& origin, const glm::vec3& dir,
        const glm::vec3& minB, const glm::vec3& maxB,
        float maxDist, float& outT) {
        float tMin = 0.0f, tMax = maxDist;
        for (int a = 0; a < 3; ++a) {
            if (std::abs(dir[a]) < 0.0001f) {
                if (origin[a] < minB[a] || origin[a] > maxB[a]) return false;
            }
            else {
                float invD = 1.0f / dir[a];
                float t0 = (minB[a] - origin[a]) * invD;
                float t1 = (maxB[a] - origin[a]) * invD;
                if (invD < 0.0f) std::swap(t0, t1);
                tMin = (std::max)(tMin, t0);
                tMax = (std::min)(tMax, t1);
                if (tMax < tMin) return false;
            }
        }
        outT = tMin;
        return tMin > 0.0f;
    }

    // Test LOS contro i soli ostacoli statici (muri, props).
    // Ritorna true se la linea da `from` a `to` è libera.
    // ignoreEntity: entità da ignorare (di solito il caller stesso).
    inline bool HasLineOfSight(Scene& scene,
        const glm::vec3& from,
        const glm::vec3& to,
        entt::entity ignoreEntity = entt::null) {
        glm::vec3 delta = to - from;
        float dist = glm::length(delta);
        if (dist < 0.001f) return true;

        glm::vec3 dir = delta / dist;

        auto& reg = scene.GetRegistry();
        auto staticView = reg.view<TransformComponent, ColliderComponent>(
            entt::exclude<PlayerTag, HealthComponent, CorpseComponent>   // <-- AGGIUNTO
        );

        for (auto e : staticView) {
            if (e == ignoreEntity) continue;

            const auto& trans = staticView.get<TransformComponent>(e);
            const auto& col = staticView.get<ColliderComponent>(e);

            glm::vec3 half = col.halfExtents * trans.Scale;
            glm::vec3 minB = trans.Position - half;
            glm::vec3 maxB = trans.Position + half;

            float t;
            // Sottraggo un epsilon per non considerare il bersaglio stesso come ostruzione
            if (IntersectAABB(from, dir, minB, maxB, dist - 0.05f, t) && t > 0.01f) {
                return false;
            }
        }
        return true;
    }

}