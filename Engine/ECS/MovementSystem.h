#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include <string>
#include <cstdlib>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <miniaudio.h>

#include "../Scene/Scene.h"
#include "../Core/Input.h"
#include "../AssetManager/BlueprintManager.h"
#include "../Core/Logger/Log.h"
#include "Components.h"
#include "SunPhysicsSystem.h"

namespace SunEngine {

    inline float LerpAngle(float a, float b, float t) {
        float diff = std::fmod(b - a + 180.0f, 360.0f) - 180.0f;
        if (diff < -180.0f) diff += 360.0f;
        return a + diff * std::clamp(t, 0.0f, 1.0f);
    }

    class MovementSystem {
    private:
        float m_WalkAnimTimer = 0.0f;

        void PlayFootstep(ma_engine* engine, SurfaceType surface, const glm::vec3& position, bool isPlayer, float volume = 1.0f) {
            if (!engine || volume <= 0.001f) return;

            std::string soundPath = "GameData/Audio/Footsteps/";

            switch (surface) {
            case SurfaceType::Concrete:
            default:
                soundPath += "step_concrete.wav";
                break;
            case SurfaceType::Dirt:
                soundPath += "step_dirt.wav";
                break;
            case SurfaceType::Metal:
                soundPath += "step_metal.wav";
                break;
            case SurfaceType::Wood:
                soundPath += "step_wood.wav";
                break;
            }

            // CONTROLLO ESISTENZA FILE: se la cartella Footsteps non esiste, salta senza crashare
            if (!std::filesystem::exists(soundPath)) return;

            // RIPRODUZIONE FIRE-AND-FORGET SICURA (Nessuna allocazione ma_sound sullo stack)
            ma_engine_set_volume(engine, volume);
            ma_engine_play_sound(engine, soundPath.c_str(), NULL);
        }

        bool RaycastScene(Scene& scene, const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDist, glm::vec3& outHitPoint, entt::entity ignoreEntity) {
            float minTHit = maxDist;
            bool hasHit = false;

            auto view = scene.GetRegistry().view<ColliderComponent, TransformComponent>(
                entt::exclude<PlayerTag, HealthComponent>
            );

            for (auto entity : view) {
                if (entity == ignoreEntity) continue;

                const auto& col = view.get<ColliderComponent>(entity);
                const auto& trans = view.get<TransformComponent>(entity);

                glm::vec3 half = col.halfExtents * trans.Scale;
                glm::vec3 minBox = trans.Position - half;
                glm::vec3 maxBox = trans.Position + half;

                float tMin = 0.0f, tMax = maxDist;
                bool hit = true;

                for (int i = 0; i < 3; ++i) {
                    if (std::abs(rayDir[i]) < 0.0001f) {
                        if (rayOrigin[i] < minBox[i] || rayOrigin[i] > maxBox[i]) { hit = false; break; }
                    }
                    else {
                        float invD = 1.0f / rayDir[i];
                        float t0 = (minBox[i] - rayOrigin[i]) * invD;
                        float t1 = (maxBox[i] - rayOrigin[i]) * invD;
                        if (invD < 0.0f) std::swap(t0, t1);
                        tMin = std::max(tMin, t0);
                        tMax = std::min(tMax, t1);
                        if (tMax < tMin) { hit = false; break; }
                    }
                }

                if (hit && tMin < minTHit && tMin > 0.0f) {
                    minTHit = tMin;
                    hasHit = true;
                }
            }

            if (hasHit) {
                outHitPoint = rayOrigin + rayDir * minTHit;
                return true;
            }
            return false;
        }

        // VERIFICA OSTACOLI: ALTEZZA GAMBE + CAPACITA' DI SALTO (FORZA FISICA)
        bool CheckVaultObstacle(Scene& scene, entt::entity entity, glm::vec3& outPeakPos, glm::vec3& outTargetPos, bool& outIsClimbing, float& outDuration) {
            const auto& trans = scene.GetRegistry().get<TransformComponent>(entity);
            const auto& stats = scene.GetRegistry().get<UnitStatsComponent>(entity);

            float legHeight = 0.9f; // Altezza standard gambe
            if (scene.GetRegistry().all_of<ColliderComponent>(entity)) {
                const auto& col = scene.GetRegistry().get<ColliderComponent>(entity);
                legHeight = col.halfExtents.y * trans.Scale.y; // Metà altezza corpo = gambe
            }

            // Calcolo capacità di salto ed estensione basato su Forza * Modificatore (Buff/Nerf)
            float vaultMod = (stats.vaultModifier > 0.01f) ? stats.vaultModifier : 1.0f;
            float jumpReach = (stats.strength * 0.032f) * vaultMod;
            float maxClimbHeight = legHeight + jumpReach; // Altezza massima scalabile (Gambe + Salto)

            glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
            glm::vec3 forwardDir = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

            glm::vec3 forwardRayStart = trans.Position + glm::vec3(0.0f, 0.4f, 0.0f);
            glm::vec3 forwardHitPoint;
            float checkDistance = 1.3f;

            if (RaycastScene(scene, forwardRayStart, forwardDir, checkDistance, forwardHitPoint, entity)) {
                float wallDist = glm::distance(glm::vec2(trans.Position.x, trans.Position.z), glm::vec2(forwardHitPoint.x, forwardHitPoint.z));

                glm::vec3 downRayStart = trans.Position + forwardDir * (wallDist + 0.35f) + glm::vec3(0.0f, maxClimbHeight + 0.5f, 0.0f);
                glm::vec3 downHitPoint;

                if (RaycastScene(scene, downRayStart, glm::vec3(0.0f, -1.0f, 0.0f), maxClimbHeight + 1.0f, downHitPoint, entity)) {
                    float obstacleHeight = downHitPoint.y - trans.Position.y;

                    if (obstacleHeight > 0.25f && obstacleHeight <= maxClimbHeight) {
                        outPeakPos = downHitPoint + glm::vec3(0.0f, 0.2f, 0.0f);
                        outTargetPos = downHitPoint + forwardDir * 0.85f;
                        outTargetPos.y = downHitPoint.y - obstacleHeight;

                        if (obstacleHeight <= legHeight) {
                            // 1. SCAVALCO RAPIDO (Ostacolo <= Altezza Gambe)
                            outIsClimbing = false;
                            outDuration = 0.35f;
                        }
                        else {
                            // 2. ARRAMPICATA LENTA (Ostacolo > Gambe: Salto + Aggrapparsi e sollevarsi)
                            outIsClimbing = true;
                            outDuration = 0.80f;
                        }
                        return true;
                    }
                }
            }
            return false;
        }

        void ProcessVaulting(Scene& scene, PhysicsSystem& physicsSystem, entt::entity entity, float deltaTime) {
            auto& vault = scene.GetRegistry().get<VaultComponent>(entity);
            auto& trans = scene.GetRegistry().get<TransformComponent>(entity);

            vault.vaultTimer += deltaTime;
            float progress = std::clamp(vault.vaultTimer / vault.vaultDuration, 0.0f, 1.0f);

            glm::vec3 currentPos;
            if (progress < 0.5f) {
                float t = progress * 2.0f;
                currentPos = glm::mix(vault.startPos, vault.peakPos, t);
            }
            else {
                float t = (progress - 0.5f) * 2.0f;
                currentPos = glm::mix(vault.peakPos, vault.targetPos, t);
            }

            trans.Position = currentPos;
            physicsSystem.SetBodyPosition(scene, entity, currentPos);

            // ANIMAZIONE PROCEDURALE
            if (scene.GetRegistry().all_of<SkeletonComponent>(entity)) {
                auto& skel = scene.GetRegistry().get<SkeletonComponent>(entity);

                if (vault.isClimbing) {
                    // ANIMAZIONE ARRAMPICATA LENTA (Mani che si aggrappano in alto)
                    float grabPhase = std::sin(progress * 3.14159f);
                    for (auto& block : skel.data.blocks) {
                        if (block.name == "Arm_R" || block.name == "Arm_L") {
                            block.jointRotation.x = -85.0f + (progress * 60.0f);
                        }
                        if (block.name == "Leg_L" || block.name == "Leg_R") {
                            block.jointRotation.x = grabPhase * 45.0f;
                        }
                        if (block.name == "Torso") {
                            block.jointRotation.x = grabPhase * 25.0f;
                        }
                    }
                }
                else {
                    // ANIMAZIONE SCAVALCO RAPIDO
                    float legPitch = std::sin(progress * 3.14159f) * 55.0f;
                    for (auto& block : skel.data.blocks) {
                        if (block.name == "Leg_L") block.jointRotation.x = legPitch;
                        if (block.name == "Leg_R") block.jointRotation.x = legPitch * 0.4f;
                        if (block.name == "Arm_R" || block.name == "Arm_L") block.jointRotation.x = -25.0f;
                    }
                }
            }

            if (progress >= 1.0f) {
                vault.isVaulting = false;
                vault.vaultTimer = 0.0f;
                scene.GetRegistry().remove<VaultComponent>(entity);
            }
        }

        float GetGroundHeightAt(Scene& scene, const glm::vec3& position, float currentY, float playerRadius = 0.35f) {
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

        bool CheckWallCollision(Scene& scene, const glm::vec3& targetPos, float currentY, float playerRadius = 0.35f) {
            auto view = scene.GetRegistry().view<ColliderComponent, TransformComponent>(
                entt::exclude<PlayerTag, HealthComponent>
            );

            for (auto entity : view) {
                const auto& col = view.get<ColliderComponent>(entity);
                const auto& trans = view.get<TransformComponent>(entity);

                glm::vec3 halfExtents = col.halfExtents * trans.Scale;
                glm::vec3 minBox = trans.Position - halfExtents;
                glm::vec3 maxBox = trans.Position + halfExtents;

                if (maxBox.y > currentY + 0.6f) {
                    if (targetPos.x + playerRadius > minBox.x && targetPos.x - playerRadius < maxBox.x &&
                        targetPos.z + playerRadius > minBox.z && targetPos.z - playerRadius < maxBox.z &&
                        targetPos.y < maxBox.y && (targetPos.y + 1.8f) > minBox.y) {
                        return true;
                    }
                }
            }
            return false;
        }

    public:
        void Update(Scene& scene, PhysicsSystem& physicsSystem, ma_engine* audioEngine, float deltaTime, float volumeSFX = 1.0f) {
            auto view = scene.GetRegistry().view<PlayerTag, TransformComponent, UnitStatsComponent, WeaponBlueprintId>();
            for (auto entity : view) {
                auto& transform = view.get<TransformComponent>(entity);
                auto& stats = view.get<UnitStatsComponent>(entity);

                auto& footstep = scene.GetRegistry().get_or_emplace<FootstepComponent>(entity);

                if (scene.GetRegistry().all_of<VaultComponent>(entity)) {
                    ProcessVaulting(scene, physicsSystem, entity, deltaTime);
                    continue;
                }

                if (Input::IsKeyPressed(GLFW_KEY_SPACE) && Input::IsKeyPressed(GLFW_KEY_W)) {
                    glm::vec3 peakPos, targetPos;
                    bool isClimbing = false;
                    float duration = 0.35f;

                    if (CheckVaultObstacle(scene, entity, peakPos, targetPos, isClimbing, duration)) {
                        auto& vault = scene.GetRegistry().emplace<VaultComponent>(entity);
                        vault.isVaulting = true;
                        vault.vaultTimer = 0.0f;
                        vault.vaultDuration = duration;
                        vault.isClimbing = isClimbing;
                        vault.startPos = transform.Position;
                        vault.peakPos = peakPos;
                        vault.targetPos = targetPos;

                        SUN_CORE_INFO(isClimbing ? "ARRAMPICATA AVVIATA!" : "SCAVALCO RAPIDO AVVIATO!");
                        continue;
                    }
                }

                float moveSpeed = (stats.baseSpeed > 0.1f) ? (stats.baseSpeed * stats.speedModifier) : 5.0f;

                glm::vec3 moveDir{ 0.0f };
                if (Input::IsKeyPressed(GLFW_KEY_W)) moveDir.z -= 1.0f;
                if (Input::IsKeyPressed(GLFW_KEY_S)) moveDir.z += 1.0f;
                if (Input::IsKeyPressed(GLFW_KEY_A)) moveDir.x -= 1.0f;
                if (Input::IsKeyPressed(GLFW_KEY_D)) moveDir.x += 1.0f;

                bool isMoving = glm::length(moveDir) > 0.001f;

                transform.Rotation.x = 0.0f;
                transform.Rotation.z = 0.0f;

                if (isMoving) {
                    moveDir = glm::normalize(moveDir);

                    glm::mat4 rotMat = glm::rotate(glm::mat4(1.0f), glm::radians(transform.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
                    glm::vec3 worldMove = glm::vec3(rotMat * glm::vec4(moveDir, 0.0f));

                    glm::vec3 nextPosX = transform.Position;
                    nextPosX.x += worldMove.x * moveSpeed * deltaTime;
                    if (!CheckWallCollision(scene, nextPosX, transform.Position.y)) {
                        transform.Position.x = nextPosX.x;
                    }

                    glm::vec3 nextPosZ = transform.Position;
                    nextPosZ.z += worldMove.z * moveSpeed * deltaTime;
                    if (!CheckWallCollision(scene, nextPosZ, transform.Position.y)) {
                        transform.Position.z = nextPosZ.z;
                    }

                    transform.Rotation.y = LerpAngle(transform.Rotation.y, transform.AimYaw, 15.0f * deltaTime);
                    m_WalkAnimTimer += deltaTime * moveSpeed * 2.5f;

                    footstep.stepTimer += deltaTime * (moveSpeed / 4.0f);
                    if (footstep.stepTimer >= footstep.stepInterval) {
                        footstep.stepTimer = 0.0f;
                        PlayFootstep(audioEngine, footstep.currentSurface, transform.Position, true, 0.85f * volumeSFX);
                    }
                }
                else {
                    m_WalkAnimTimer = glm::mix(m_WalkAnimTimer, 0.0f, 10.0f * deltaTime);
                    transform.Rotation.y = LerpAngle(transform.Rotation.y, transform.AimYaw, 15.0f * deltaTime);
                    footstep.stepTimer = footstep.stepInterval * 0.8f;
                }

                float targetY = GetGroundHeightAt(scene, transform.Position, transform.Position.y);
                transform.Position.y = glm::mix(transform.Position.y, targetY, 20.0f * deltaTime);

                physicsSystem.SetBodyPosition(scene, entity, transform.Position);

                auto treeView = scene.GetRegistry().view<TreeObstacleTag, TransformComponent>();
                for (auto treeEntity : treeView) {
                    const auto& treeTrans = treeView.get<TransformComponent>(treeEntity);
                    glm::vec2 playerXZ(transform.Position.x, transform.Position.z);
                    glm::vec2 treeXZ(treeTrans.Position.x, treeTrans.Position.z);

                    float minDist = 0.75f;
                    float dist = glm::distance(playerXZ, treeXZ);

                    if (dist < minDist && dist > 0.0001f) {
                        glm::vec2 pushDir = glm::normalize(playerXZ - treeXZ);
                        glm::vec2 correctedPos = treeXZ + pushDir * minDist;
                        transform.Position.x = correctedPos.x;
                        transform.Position.z = correctedPos.y;
                        physicsSystem.SetBodyPosition(scene, entity, transform.Position);
                    }
                }

                if (scene.GetRegistry().all_of<SkeletonComponent>(entity)) {
                    auto& skel = scene.GetRegistry().get<SkeletonComponent>(entity);
                    float legAngle = isMoving ? std::sin(m_WalkAnimTimer) * 30.0f : 0.0f;

                    for (auto& block : skel.data.blocks) {
                        if (block.name == "Leg_L") {
                            block.jointRotation.x = legAngle;
                        }
                        else if (block.name == "Leg_R") {
                            block.jointRotation.x = -legAngle;
                        }
                        else if (block.name == "Arm_R" || block.name == "Arm_L") {
                            float armSwing = isMoving ? (block.name == "Arm_R" ? legAngle * 0.3f : -legAngle * 0.3f) : 0.0f;
                            block.jointRotation.x = -transform.AimPitch + armSwing;
                        }
                        else if (block.name == "Head") {
                            block.jointRotation.x = -transform.AimPitch * 0.5f;
                        }
                    }
                }
            }
        }
    };

}