#pragma once

#include "../Scene/Scene.h"
#include "../Renderer/Renderer.h"
#include "../Renderer/Pipeline/DeferredRenderer.h"
#include "Components.h"

namespace SunEngine {

    class RenderSystem {
    public:
        // Vecchio renderer forward (fallback se DeferredRenderer non init).
        void Render(Scene& scene) {
            auto view = scene.GetRegistry().view<TransformComponent, MeshComponent, ColorComponent>();
            for (auto entity : view) {
                auto& transform = view.get<TransformComponent>(entity);
                auto& color = view.get<ColorComponent>(entity);

                if (scene.GetRegistry().all_of<RagdollComponent>(entity)) {
                    auto& ragdoll = scene.GetRegistry().get<RagdollComponent>(entity);
                    for (const auto& mat : ragdoll.worldTransforms) {
                        Renderer::DrawCube(mat, color.Color);
                    }
                }
                else if (scene.GetRegistry().all_of<SkeletonComponent>(entity)) {
                    auto& skel = scene.GetRegistry().get<SkeletonComponent>(entity);
                    auto worldTransforms = skel.data.ComputeWorldTransforms(
                        transform.Position, transform.Rotation, skel.scale);
                    for (size_t i = 0; i < skel.data.blocks.size(); ++i) {
                        Renderer::DrawCube(worldTransforms[i], color.Color);
                    }
                }
                else {
                    Renderer::DrawCube(transform.GetTransform(), color.Color);
                }
            }
        }

        // NUOVO: rendering PBR deferred. Da chiamare tra BeginGeometryPass()
        // ed EndGeometryPassAndPresent().
        void RenderDeferred(Scene& scene, DeferredRenderer& deferred) {
            auto view = scene.GetRegistry().view<TransformComponent, MeshComponent, ColorComponent>();
            for (auto entity : view) {
                auto& transform = view.get<TransformComponent>(entity);
                auto& color = view.get<ColorComponent>(entity);
                const glm::vec4 baseColor = color.Color;

                // 1. RAGDOLL (cadavere smontato)
                if (scene.GetRegistry().all_of<RagdollComponent>(entity)) {
                    auto& ragdoll = scene.GetRegistry().get<RagdollComponent>(entity);
                    for (const auto& mat : ragdoll.worldTransforms) {
                        // Carne/stoffa: ruvido, non metallico
                        deferred.DrawCube(mat, baseColor, 0.85f, 0.0f, 0.0f);
                    }
                }
                // 2. SKELETON VIVO (blocchi corpo)
                else if (scene.GetRegistry().all_of<SkeletonComponent>(entity)) {
                    auto& skel = scene.GetRegistry().get<SkeletonComponent>(entity);
                    auto worldTransforms = skel.data.ComputeWorldTransforms(
                        transform.Position, transform.Rotation, skel.scale);
                    for (size_t i = 0; i < worldTransforms.size() && i < skel.data.blocks.size(); ++i) {
                        // Blocchi corpo: materiale "carne/armatura" di default.
                        // In futuro leggeremo ArmorPiece per tint/rating.
                        deferred.DrawCube(worldTransforms[i], baseColor, 0.7f, 0.1f, 0.0f);
                    }
                }
                // 3. MESH STANDARD (muri, terrain, props, armi)
                else {
                    // Muri/terrain: ruvidi, opachi, non metallici
                    deferred.DrawCube(transform.GetTransform(), baseColor, 0.9f, 0.0f, 0.0f);
                }
            }
        }
    };

}