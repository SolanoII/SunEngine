#pragma once

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

#include "../Scene/Scene.h"
#include "Components.h"
#include "Tags.h"

namespace SunEngine {

    // Interpolazione angolare con wraparound [-180, 180].
    // Gestisce il caso in cui a=170 e b=-170: il percorso corto e' 20 gradi, non 340.
    // t viene clampato in [0, 1].
    inline float LerpAngle(float a, float b, float t) {
        float diff = std::fmod(b - a + 180.0f, 360.0f) - 180.0f;
        if (diff < -180.0f) diff += 360.0f;
        return a + diff * std::clamp(t, 0.0f, 1.0f);
    }

    // Ritorna l'altezza del "terreno" sotto una data posizione.
    // Considera solo collider statici (esclude PlayerTag e HealthComponent,
    // cosi' esclude anche i cadaveri che conservano l'HealthComponent).
    //
    // position      : posizione XZ+Y di riferimento (di solito piedi del player)
    // currentY      : altezza attuale dell'entita'; il collider e' valido solo
    //                 se il suo top sta entro currentY + 0.6 (evita di "salire"
    //                 su piattaforme lontane verticalmente)
    // playerRadius  : tolleranza orizzontale attorno alla posizione (default 0.35m)
    //
    // Ritorna 0.0f se non trova nulla di calpestabile sotto.
    inline float GetGroundHeightAt(Scene& scene, const glm::vec3& position,
        float currentY = 0.0f, float playerRadius = 0.35f) {
        float maxGroundY = 0.0f;

        auto view = scene.GetRegistry().view<ColliderComponent, TransformComponent>(
            entt::exclude<PlayerTag, HealthComponent>
        );

        for (auto entity : view) {
            const auto& col = view.get<ColliderComponent>(entity);
            const auto& trans = view.get<TransformComponent>(entity);

            glm::vec3 halfExtents = col.halfExtents * trans.Scale;
            glm::vec3 minBox = trans.Position - halfExtents;
            glm::vec3 maxBox = trans.Position + halfExtents;

            if (position.x + playerRadius >= minBox.x && position.x - playerRadius <= maxBox.x &&
                position.z + playerRadius >= minBox.z && position.z - playerRadius <= maxBox.z) {

                if (maxBox.y > maxGroundY && maxBox.y <= currentY + 0.6f) {
                    maxGroundY = maxBox.y;
                }
            }
        }
        return maxGroundY;
    }

}