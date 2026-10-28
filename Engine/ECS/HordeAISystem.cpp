#include "../Core/Random.h"
#include "../Core/Logger/Log.h"
#include "HordeAISystem.h"
#include "CombatVFX.h"
#include "RaycastUtils.h"          // <-- AGGIUNGI
#include <algorithm>
#include <cstdlib>
#include <cmath>

namespace SunEngine {

    void HordeAISystem::Update(Scene& scene, NavMeshQuery& navQuery, float deltaTime) {
        entt::entity playerEnt = entt::null;
        glm::vec3 playerPos(0.0f);
        glm::vec3 playerVelocity(0.0f);

        auto playerView = scene.GetRegistry().view<PlayerTag, TransformComponent>();
        for (auto p : playerView) {
            playerEnt = p;
            playerPos = playerView.get<TransformComponent>(p).Position;

            if (scene.GetRegistry().all_of<RigidBodyComponent>(p)) {
                playerVelocity = scene.GetRegistry().get<RigidBodyComponent>(p).velocity;
            }
            break;
        }

        if (playerEnt == entt::null) return;

        bool activeSlots[4] = { false, false, false, false };
        int activeAttackersCount = 0;

        auto hordeView = scene.GetRegistry().view<HordeAgentComponent, TransformComponent, HealthComponent>(
            entt::exclude<PlayerTag, CorpseComponent>
        );

        for (auto ork : hordeView) {
            auto& horde = hordeView.get<HordeAgentComponent>(ork);
            if (horde.state == HordeState::Approaching || horde.state == HordeState::Attacking) {
                if (horde.assignedSlotIndex >= 0 && horde.assignedSlotIndex < 4) {
                    activeSlots[horde.assignedSlotIndex] = true;
                }
                activeAttackersCount++;
            }
        }

        int totalOrksInHorde = 0;
        for (auto ork : hordeView) totalOrksInHorde++;

        int orkIndex = 0;
        for (auto ork : hordeView) {
            auto& horde = hordeView.get<HordeAgentComponent>(ork);
            auto& trans = hordeView.get<TransformComponent>(ork);
            auto& melee = scene.GetRegistry().get_or_emplace<MeleeStateComponent>(ork);

            float distToPlayer = glm::distance(trans.Position, playerPos);

            // ------------------------------------------------------------------
            // LINE OF SIGHT: raycast periodico dagli occhi al petto del player.
            // Se libero -> aggiorna lastKnownPlayerPos. Se bloccato -> l'ork
            // va verso la memoria (l'ultima posizione dove ha VISTO il player).
            // ------------------------------------------------------------------
            horde.losCheckTimer -= deltaTime;
            if (horde.losCheckTimer <= 0.0f) {
                horde.losCheckTimer = horde.losCheckInterval;

                glm::vec3 eyePos = trans.Position + glm::vec3(0.0f, 1.20f, 0.0f);
                glm::vec3 targetPos = playerPos + glm::vec3(0.0f, 1.00f, 0.0f);

                horde.hasLOS = HasLineOfSight(scene, eyePos, targetPos, ork);
                if (horde.hasLOS) {
                    horde.lastKnownPlayerPos = playerPos;
                    horde.hasEverSeenPlayer = true;   // <-- AGGIUNGI QUESTA RIGA
                }
            }

            // Posizione "di riferimento" per l'AI: se vede il player uso la posizione
            // reale, altrimenti quella memorizzata (o, se non ha mai visto il player,
            // resta in attesa in cerchio attorno al player per non impazzire).
            glm::vec3 effectivePlayerPos = horde.hasLOS
                ? playerPos
                : (horde.hasEverSeenPlayer ? horde.lastKnownPlayerPos : playerPos);

            // Se non ha LOS e non ha mai visto il player, non deve caricare:
            // resta in cerchio (Waiting) finche' non acquisisce visibilità.
            if (!horde.hasLOS && !horde.hasEverSeenPlayer) {
                if (horde.state == HordeState::Approaching || horde.state == HordeState::Attacking) {
                    horde.state = HordeState::Waiting;
                    horde.assignedSlotIndex = -1;
                    horde.currentPath.clear();
                }
            }

            // 1. MIRA PREDITTIVA (solo se ha LOS: sparare a un ricordo non ha senso)
            if (horde.hasLOS && distToPlayer <= 8.0f) {
                float leadTime = 0.28f;
                glm::vec3 predictedPlayerPos = effectivePlayerPos + (playerVelocity * leadTime);
                glm::vec3 dirToP = predictedPlayerPos - trans.Position;
                dirToP.y = 0.0f;

                if (glm::length(dirToP) > 0.001f) {
                    float targetYaw = glm::degrees(std::atan2(-dirToP.x, -dirToP.z));
                    trans.AimYaw = targetYaw;
                    trans.Rotation.y = LerpAngle(trans.Rotation.y, targetYaw, 14.0f * deltaTime);
                }
            }
            else if (!horde.hasLOS) {
                // Senza LOS: l'Ork si guarda attorno verso l'ultima posizione nota
                glm::vec3 dirToMem = effectivePlayerPos - trans.Position;
                dirToMem.y = 0.0f;
                if (glm::length(dirToMem) > 0.5f) {
                    float targetYaw = glm::degrees(std::atan2(-dirToMem.x, -dirToMem.z));
                    trans.AimYaw = targetYaw;
                    trans.Rotation.y = LerpAngle(trans.Rotation.y, targetYaw, 6.0f * deltaTime);
                }
            }

            // CALCOLO TARGET E VELOCITA' IN BASE ALLO STATO
            glm::vec3 targetPos = trans.Position;
            float moveSpeed = 0.0f;
            bool needsToMove = false;

            if (horde.state == HordeState::Cooldown) {
                horde.cooldownTimer -= deltaTime;

                if (distToPlayer < 4.5f) {
                    glm::vec3 backDir = glm::normalize(trans.Position - effectivePlayerPos);
                    if (glm::length(backDir) < 0.001f) backDir = glm::vec3(0.0f, 0.0f, 1.0f);
                    backDir.y = 0.0f;
                    targetPos = navQuery.GetNearestValidPoint(trans.Position + backDir * 3.5f);
                    moveSpeed = 2.8f;
                    needsToMove = true;
                }

                if (horde.cooldownTimer <= 0.0f) {
                    horde.state = HordeState::Waiting;
                }
            }
            else if (horde.state == HordeState::Waiting) {
                float angle = (360.0f / (std::max)(1, totalOrksInHorde)) * orkIndex;
                horde.circleAngle = angle;

                targetPos = effectivePlayerPos + glm::vec3(
                    std::cos(glm::radians(angle)) * 5.5f,
                    0.0f,
                    std::sin(glm::radians(angle)) * 5.5f
                );
                if (glm::distance(trans.Position, targetPos) > 0.4f) {
                    moveSpeed = 3.5f;
                    needsToMove = true;
                }

                if (activeAttackersCount < 4) {
                    for (int s = 0; s < 4; ++s) {
                        if (!activeSlots[s]) {
                            activeSlots[s] = true;
                            horde.assignedSlotIndex = s;
                            horde.state = HordeState::Approaching;
                            horde.attackDelay = Random::Range(0.4f, 1.2f);
                            horde.attacksToPerform = Random::Chance(0.5f) ? 1 : 2;
                            horde.currentAttackCount = 0;
                            activeAttackersCount++;
                            break;
                        }
                    }
                }
            }
            else if (horde.state == HordeState::Approaching) {
                float slotAngles[4] = { 0.0f, 90.0f, 180.0f, 270.0f };
                float slotAngle = slotAngles[horde.assignedSlotIndex];

                targetPos = effectivePlayerPos + glm::vec3(
                    std::cos(glm::radians(slotAngle)) * 1.45f,
                    0.0f,
                    std::sin(glm::radians(slotAngle)) * 1.45f
                );

                moveSpeed = 4.2f;
                needsToMove = true;
                if (distToPlayer <= 1.65f && horde.hasLOS) {
                    horde.state = HordeState::Attacking;
                    horde.attackTimer = 0.0f;
                }
            }

            // ESECUZIONE NAVMESH PATHFINDING E MOVIMENTO
            if (needsToMove) {
                horde.pathUpdateTimer -= deltaTime;

                if (horde.pathUpdateTimer <= 0.0f || horde.currentPath.empty()) {
                    horde.currentPath = navQuery.FindPath(trans.Position, targetPos);
                    horde.pathUpdateTimer = Random::Range(0.4f, 0.6f);

                    // Diagnostica: un ork che "cammina" ma non si muove ha quasi sempre un percorso vuoto
                    // (partenza o destinazione fuori dalla navmesh). Solo le prime segnalazioni.
                    static int s_EmptyPathLogs = 0;
                    if (horde.currentPath.empty() && s_EmptyPathLogs < 6) {
                        ++s_EmptyPathLogs;
                        SUN_CORE_WARN("Horde: percorso VUOTO da ({0:.1f},{1:.1f},{2:.1f}) a ({3:.1f},{4:.1f},{5:.1f}), stato {6}",
                            trans.Position.x, trans.Position.y, trans.Position.z,
                            targetPos.x, targetPos.y, targetPos.z, static_cast<int>(horde.state));
                    }
                }

                if (!horde.currentPath.empty()) {
                    glm::vec3 nextWaypoint = horde.currentPath.front();
                    glm::vec3 dirToWaypoint = nextWaypoint - trans.Position;
                    dirToWaypoint.y = 0.0f;

                    float distToWay = glm::length(dirToWaypoint);

                    if (distToWay < 0.25f) {
                        horde.currentPath.erase(horde.currentPath.begin());
                    }
                    else {
                        trans.Position += glm::normalize(dirToWaypoint) * moveSpeed * deltaTime;
                    }
                }
            }
            else {
                horde.currentPath.clear();
            }

            if (horde.state == HordeState::Attacking) {
                horde.attackTimer += deltaTime;

                if (horde.attackTimer >= horde.attackDelay && melee.currentPhase == MeleePhase::None && horde.currentAttackCount == 0) {
                    melee.currentPhase = MeleePhase::Windup;
                    melee.currentAttack = MeleeAttackType::Normal;
                    melee.currentAttackNode = "normal_1";
                    melee.stateTimer = 0.0f;
                    melee.hasHitInThisAttack = false;
                    horde.currentAttackCount = 1;
                }

                if (melee.currentPhase == MeleePhase::Windup) {
                    glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
                    glm::vec3 fwd = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
                    trans.Position += fwd * 3.6f * deltaTime;
                }

                if (horde.attacksToPerform == 2 && horde.currentAttackCount == 1) {
                    if (melee.currentAttackNode == "normal_1" && melee.currentPhase == MeleePhase::Active) {
                        melee.bufferedInput = "F_CLICK";
                        horde.currentAttackCount = 2;
                    }
                }

                if (horde.currentAttackCount >= horde.attacksToPerform && melee.currentPhase == MeleePhase::None) {
                    horde.state = HordeState::Cooldown;

                    float baseCooldown = Random::Range(2.5f, 4.0f);
                    float hordeReductionFactor = std::clamp(1.0f - ((totalOrksInHorde - 1) * 0.08f), 0.40f, 1.0f);
                    horde.cooldownTimer = std::max(0.7f, baseCooldown * hordeReductionFactor);

                    if (horde.assignedSlotIndex >= 0 && horde.assignedSlotIndex < 4) {
                        activeSlots[horde.assignedSlotIndex] = false;
                    }
                    horde.assignedSlotIndex = -1;
                }
            }

            orkIndex++;
        }
    }

}