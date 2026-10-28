#pragma once

#include <vector>
#include <entt/entt.hpp>

#include "Components.h"
#include "Tags.h"
#include "SunPhysicsSystem.h"
#include "../Scene/Scene.h"

namespace SunEngine {

    // FIX #2: ciclo di vita dei cadaveri.
    // CorpseComponent::maxLifetime esisteva ma nessun sistema lo leggeva: i ragdoll (body Jolt +
    // constraint + entity) si accumulavano per sempre e la fisica rallentava nelle partite lunghe.
    //
    // Regole:
    //  - timeSinceDeath avanza di deltaTime per ogni cadavere.
    //  - Superato maxLifetime l'entity viene distrutta.
    //  - Il PLAYER non viene mai distrutto qui: m_PlayerEntity e' persistente e il suo cadavere
    //    viene ripulito da GameLayer (respawn / ritorno al garage).
    //  - Si chiama PRIMA DestroyJoltBody, che rimuove JoltBodyComponent/RagdollComponent e fa
    //    scattare gli observer di cleanup dei body Jolt; solo dopo si distrugge l'entity.
    //  - La lista da distruggere viene raccolta prima: niente distruzioni durante l'iterazione.
    class CorpseSystem {
    public:
        static void Update(Scene& scene, PhysicsSystem& physics, float deltaTime) {
            auto& reg = scene.GetRegistry();

            std::vector<entt::entity> expired;
            auto view = reg.view<CorpseComponent>(entt::exclude<PlayerTag>);
            for (auto e : view) {
                auto& corpse = view.get<CorpseComponent>(e);
                corpse.timeSinceDeath += deltaTime;
                if (corpse.timeSinceDeath > corpse.maxLifetime) {
                    expired.push_back(e);
                }
            }

            for (auto e : expired) {
                if (!reg.valid(e)) continue;
                physics.DestroyJoltBody(scene, e);   // prima: fa scattare gli observer Jolt
                scene.DestroyEntity(e);              // l'observer HordeAgent pulisce l'arma figlia
            }
        }
    };

}
