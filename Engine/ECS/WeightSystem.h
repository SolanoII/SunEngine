#pragma once

#include "../Scene/Scene.h"
#include "Components.h"
#include "Tags.h"

namespace SunEngine {
    class WeightSystem {
    public:
        void Update(Scene& scene, float deltaTime) {
            auto view = scene.GetRegistry().view<TransformComponent, UnitStatsComponent>();
            for (auto entity : view) {
                auto& transform = view.get<TransformComponent>(entity);
                auto& stats = view.get<UnitStatsComponent>(entity);

                // Il giocatore: speedModifier e' il rallentamento della mira (lo mette GameLayer) e il carico lo
                // applica MovementSystem; prima qui veniva sovrascritto e il peso del giocatore non contava mai.
                if (!scene.GetRegistry().all_of<PlayerTag>(entity)) stats.speedModifier = CarrySpeedFactor(stats);

                if (stats.currentWeight > stats.weightCapacity) {
                    stats.stamina -= 20.0f * deltaTime;
                }
            }
        }
    };
}