#pragma once

#include <algorithm>
#include <glm/glm.hpp>
#include "../Scene/Scene.h"
#include "../Renderer/Renderer.h"
#include "Components.h"

namespace SunEngine {

    class ParticleSystem {
    public:
        void Update(Scene& scene, float deltaTime) {
            auto view = scene.GetRegistry().view<TrailRequestComponent>();
            for (auto entity : view) {
                auto& trail = view.get<TrailRequestComponent>(entity);
                trail.currentAge += deltaTime;

                if (trail.currentAge >= trail.totalLife) {
                    scene.DestroyEntity(entity);
                }
            }
        }

        void Render(Scene& scene) {
            auto view = scene.GetRegistry().view<TrailRequestComponent>();
            for (auto entity : view) {
                auto& trail = view.get<TrailRequestComponent>(entity);

                float progress = std::clamp(trail.currentAge / trail.travelTime, 0.0f, 1.0f);
                glm::vec3 currentPos = glm::mix(trail.start, trail.end, progress);

                if (trail.isBeam) {
                    Renderer::DrawLine(trail.start, currentPos, trail.color);
                }
                else {
                    Renderer::DrawSphere(currentPos, trail.projectileWidth, trail.color);
                }
            }
        }
    };

}