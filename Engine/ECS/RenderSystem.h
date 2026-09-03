#pragma once

#include "../Scene/Scene.h"
#include "../Renderer/Renderer.h"
#include "Components.h"

namespace SunEngine {

    class RenderSystem {
    public:
        void Render(Scene& scene) {
            auto view = scene.GetRegistry().view<TransformComponent, MeshComponent, ColorComponent>();
            for (auto entity : view) {
                auto& transform = view.get<TransformComponent>(entity);
                auto& color = view.get<ColorComponent>(entity);

                // 1. RAGDOLL ARTICOLATO (GTA / Gears style)
                if (scene.GetRegistry().all_of<RagdollComponent>(entity)) {
                    auto& ragdoll = scene.GetRegistry().get<RagdollComponent>(entity);
                    for (const auto& mat : ragdoll.worldTransforms) {
                        Renderer::DrawCube(mat, color.Color);
                    }
                }
                // 2. UNITA' VIVA ANIMATA
                else if (scene.GetRegistry().all_of<SkeletonComponent>(entity)) {
                    auto& skel = scene.GetRegistry().get<SkeletonComponent>(entity);
                    auto worldTransforms = skel.data.ComputeWorldTransforms(transform.Position, transform.Rotation, skel.scale);

                    for (size_t i = 0; i < skel.data.blocks.size(); ++i) {
                        Renderer::DrawCube(worldTransforms[i], color.Color);
                    }
                }
                // 3. MESH STANDARD
                else {
                    Renderer::DrawCube(transform.GetTransform(), color.Color);
                }
            }
        }
    };

}