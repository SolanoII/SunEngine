#pragma once

#include "../Scene/Scene.h"
#include "../Renderer/Renderer.h"
#include "../Renderer/Pipeline/DeferredRenderer.h"
#include "Components.h"
#include "Tags.h"
#include "BlockModel.h"

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
            auto view = scene.GetRegistry().view<TransformComponent, ColorComponent>();

            for (auto entity : view) {
                auto& transform = view.get<TransformComponent>(entity);
                auto& color = view.get<ColorComponent>(entity);
                const glm::vec4 baseColor = color.Color;

                // 0. MODELLO SKINNATO FBX (nuova priorità più alta)
                if (auto* skinnedInst = scene.GetRegistry().try_get<SkinnedModelInstanceComponent>(entity)) {
                    if (skinnedInst->sourceModel && skinnedInst->boneMatrices) {
                        const glm::mat4 modelMat = transform.GetTransform();
                        const auto& bones = *skinnedInst->boneMatrices;

                        for (const auto& submesh : skinnedInst->sourceModel->meshes) {
                            deferred.DrawSkinnedMesh(
                                submesh,
                                modelMat,
                                bones,
                                skinnedInst->albedo,
                                skinnedInst->roughness,
                                skinnedInst->metallic,
                                skinnedInst->emissive);
                        }
                        continue;   // non disegnare i cubi procedurali per questa entità
                    }
                }

                // 1. RAGDOLL
                if (scene.GetRegistry().all_of<RagdollComponent>(entity)) {
                    auto& ragdoll = scene.GetRegistry().get<RagdollComponent>(entity);
                    for (const auto& mat : ragdoll.worldTransforms) {
                        deferred.DrawCube(mat, baseColor, 0.85f, 0.0f, 0.0f);
                    }
                }
                // 2. SKELETON PROCEDURALE (cubi corpo)
                else if (scene.GetRegistry().all_of<SkeletonComponent>(entity)) {
                    // Con HideProceduralVisualTag i cubi restano come hitbox ma non vengono disegnati
                    // (il giocatore e' mostrato col modello animato).
                    if (!scene.GetRegistry().all_of<HideProceduralVisualTag>(entity)) {
                        auto& skel = scene.GetRegistry().get<SkeletonComponent>(entity);
                        auto worldTransforms = skel.data.ComputeWorldTransforms(
                            transform.Position, transform.Rotation, skel.scale);
                        for (size_t i = 0; i < worldTransforms.size() && i < skel.data.blocks.size(); ++i) {
                            deferred.DrawCube(worldTransforms[i], baseColor, 0.7f, 0.1f, 0.0f);
                        }
                    }
                }
                // 3. MESH/CUBO STANDARD
                else if (scene.GetRegistry().all_of<MeshComponent>(entity)) {
                    // Modello a blocchi (es. arma): disegna i singoli blocchi coi loro materiali
                    const auto* blockModel = scene.GetRegistry().try_get<BlockModelComponent>(entity);
                    if (blockModel && blockModel->model) {
                        const glm::mat4 entityMat = transform.GetTransform();
                        for (const auto& part : blockModel->model->parts) {
                            deferred.DrawCube(BlockPartMatrix(entityMat, part), part.color,
                                part.roughness, part.metallic, part.emissive);
                        }
                    }
                    else {
                        deferred.DrawCube(transform.GetTransform(), baseColor, 0.9f, 0.0f, 0.0f);
                    }
                }
            }
        }
    };

}