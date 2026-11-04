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

            // Mimetismo: per tutta l'entita' i disegni vanno nella coda del liquido trasparente
            struct CloakScope {
                DeferredRenderer& d; bool on;
                CloakScope(DeferredRenderer& r, float amount) : d(r), on(amount > 0.001f) { if (on) d.BeginCloak(amount); }
                ~CloakScope() { if (on) d.EndCloak(); }
            };
            for (auto entity : view) {
                auto& transform = view.get<TransformComponent>(entity);
                auto& color = view.get<ColorComponent>(entity);
                const glm::vec4 baseColor = color.Color;
                const auto* cloak = scene.GetRegistry().try_get<CloakComponent>(entity);
                const bool cloakable = cloak && !scene.GetRegistry().all_of<RagdollComponent>(entity);
                CloakScope cloakScope(deferred, cloakable ? cloak->amount : 0.0f);

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
                        // Vestiti e armature: stesse ossa del corpo, quindi seguono ogni animazione
                        for (const auto& g : skinnedInst->garments) {
                            if (!g.mesh) continue;
                            deferred.DrawSkinnedMesh(*g.mesh, modelMat, bones, g.color, g.roughness, g.metallic, 0.0f,
                                g.texture, g.texScale, g.alphaCut);
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
                        const auto* skin = scene.GetRegistry().try_get<SkinComponent>(entity);   // vesti / armatura sul campo
                        auto worldTransforms = skel.data.ComputeWorldTransforms(
                            transform.Position, transform.Rotation, skel.scale);
                        for (size_t i = 0; i < worldTransforms.size() && i < skel.data.blocks.size(); ++i) {
                            // Colore del blocco: quello della skin (es. vesti marroni sul torso) o quello dell'unita'
                            glm::vec4 blockColor = baseColor;
                            if (skin && i < skin->blockColors.size() && skin->blockColors[i].a > 0.0f) blockColor = skin->blockColors[i];
                            // Blocco colpito da poco: si illumina col colore di efficacia del colpo
                            float flash = (i < skel.blockFlashTimers.size()) ? skel.blockFlashTimers[i] : 0.0f;
                            if (flash > 0.0f && i < skel.blockFlashColors.size()) {
                                const float k = flash / 0.18f;   // 1 appena colpito -> 0
                                const glm::vec4 col = glm::mix(blockColor, glm::vec4(skel.blockFlashColors[i], blockColor.a), 0.75f * k);
                                deferred.DrawCube(worldTransforms[i], col, 0.7f, 0.1f, k);   // emissivo: canale 0..1 del GBuffer
                            }
                            else {
                                deferred.DrawCube(worldTransforms[i], blockColor, 0.7f, 0.1f, 0.0f);
                            }
                        }
                        // Pezzi aggiunti dalla skin (es. elmetto): cubi agganciati al loro blocco
                        if (skin) {
                            for (const auto& part : skin->parts) {
                                if (part.block < 0 || part.block >= static_cast<int>(worldTransforms.size())) continue;
                                glm::mat4 m = worldTransforms[part.block];
                                for (int c = 0; c < 3; ++c) {   // tolgo la scala del blocco, tengo posizione e rotazione
                                    const float len = glm::length(glm::vec3(m[c]));
                                    if (len > 1e-6f) m[c] /= len;
                                }
                                m = glm::translate(m, part.offset * skel.scale);
                                m = glm::scale(m, part.size * skel.scale);
                                deferred.DrawCube(m, part.color, part.roughness, part.metallic, 0.0f);
                            }
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