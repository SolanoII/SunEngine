#pragma once

#include "../Scene/Scene.h"
#include "Components.h"

namespace SunEngine {
    class WeightSystem {
    public:
        void Update(Scene& scene, float deltaTime) {
            auto view = scene.GetRegistry().view<TransformComponent, UnitStatsComponent>();
            for (auto entity : view) {
                auto& transform = view.get<TransformComponent>(entity);
                auto& stats = view.get<UnitStatsComponent>(entity);

                float weightRatio = stats.currentWeight / stats.weightCapacity;
                float speedPenalty = 1.0f - (weightRatio * 0.5f);
                stats.speedModifier = glm::clamp(speedPenalty, 0.5f, 1.0f);

                if (stats.currentWeight > stats.weightCapacity) {
                    stats.stamina -= 20.0f * deltaTime;
                }
            }
        }
    };
}