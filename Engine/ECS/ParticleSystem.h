#pragma once

#include <algorithm>
#include <glm/glm.hpp>
#include "../Scene/Scene.h"
#include "../Renderer/Renderer.h"
#include "Components.h"

namespace SunEngine {

    class ParticleSystem {
    private:
        // Parallelepipedo orientato lungo dir (asse Z locale), centrato in center
        static glm::mat4 Oriented(const glm::vec3& center, const glm::vec3& dir, const glm::vec3& size) {
            const glm::vec3 z = dir;
            const glm::vec3 up = std::abs(z.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 x = glm::normalize(glm::cross(up, z));
            const glm::vec3 y = glm::cross(z, x);
            glm::mat4 m(1.0f);
            m[0] = glm::vec4(x * size.x, 0.0f);
            m[1] = glm::vec4(y * size.y, 0.0f);
            m[2] = glm::vec4(z * size.z, 0.0f);
            m[3] = glm::vec4(center, 1.0f);
            return m;
        }

        // Proiettile classico: tracciante sottile e lungo, giallo-bianco, velocissimo
        static void RenderBullet(const TrailRequestComponent& t, const glm::vec3& pos) {
            const glm::vec3 d = t.end - t.start;
            const float total = glm::length(d);
            if (total < 1e-3f || t.currentAge > t.travelTime) return;
            const glm::vec3 dir = d / total;
            const float travelled = glm::length(pos - t.start);
            const float len = std::min(0.9f, travelled);
            if (len < 0.05f) return;
            const float w = std::max(0.012f, std::min(t.projectileWidth, 0.03f));
            Renderer::DrawCube(Oriented(pos - dir * (len * 0.5f), dir, glm::vec3(w, w, len)), glm::vec4(1.0f, 0.92f, 0.65f, 1.0f));
        }

        // Bolt: cartuccia grossa (piu' di un calibro 50) con razzo acceso e scia di fumo; arrivata, scoppia
        static void RenderBolt(const TrailRequestComponent& t, const glm::vec3& pos) {
            const glm::vec3 d = t.end - t.start;
            const float total = glm::length(d);
            if (total < 1e-3f) return;
            const glm::vec3 dir = d / total;
            if (t.currentAge <= t.travelTime) {
                const float travelled = glm::length(pos - t.start);
                const float smoke = std::min(3.5f, travelled);
                if (smoke > 0.1f) Renderer::DrawCube(Oriented(pos - dir * (smoke * 0.5f + 0.1f), dir, glm::vec3(0.025f, 0.025f, smoke)),
                                                    glm::vec4(0.55f, 0.52f, 0.48f, 1.0f));   // scia di fumo
                Renderer::DrawCube(Oriented(pos, dir, glm::vec3(0.05f, 0.05f, 0.15f)), glm::vec4(0.62f, 0.48f, 0.22f, 1.0f));   // cartuccia
                Renderer::DrawCube(Oriented(pos + dir * 0.085f, dir, glm::vec3(0.035f, 0.035f, 0.03f)), glm::vec4(0.25f, 0.22f, 0.2f, 1.0f));   // ogiva
                Renderer::DrawSphere(pos - dir * 0.1f, 0.045f, glm::vec4(1.0f, 0.6f, 0.15f, 1.0f));   // razzo acceso
                return;
            }
            // Scoppio della carica esplosiva nel punto colpito
            const float k = std::clamp((t.currentAge - t.travelTime) / std::max(0.01f, t.totalLife - t.travelTime), 0.0f, 1.0f);
            if (k >= 1.0f || t.totalLife - t.travelTime < 0.1f) return;
            Renderer::DrawSphere(t.end, 0.08f + 0.32f * k, glm::mix(glm::vec4(1.0f, 0.85f, 0.4f, 1.0f), glm::vec4(0.35f, 0.3f, 0.28f, 1.0f), k));
            if (k < 0.4f) Renderer::DrawSphere(t.end, 0.12f * (1.0f - k), glm::vec4(1.0f, 1.0f, 0.85f, 1.0f));
        }

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

                if (trail.style == 1) { RenderBullet(trail, currentPos); continue; }
                if (trail.style == 2) { RenderBolt(trail, currentPos); continue; }

                if (trail.isBeam) {
                    Renderer::DrawLine(trail.start, currentPos, trail.color);

                    if (trail.projectileWidth > 0.08f) {
                        int steps = 8;
                        for (int i = 0; i <= steps; ++i) {
                            float t = static_cast<float>(i) / static_cast<float>(steps);
                            glm::vec3 p = glm::mix(trail.start, currentPos, t);
                            Renderer::DrawSphere(p, trail.projectileWidth * 0.45f, trail.color);
                        }
                    }
                }
                else {
                    Renderer::DrawSphere(currentPos, trail.projectileWidth, trail.color);
                }
            }
        }
    };

}