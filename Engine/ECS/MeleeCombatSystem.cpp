#include "MeleeCombatSystem.h"
#include "DamageResolver.h"
#include "CombatVFX.h"
#include "RaycastUtils.h"
#include "HitUtils.h"
#include <cmath>
#include <string>
#include <algorithm>

namespace SunEngine {

    void MeleeCombatSystem::Update(Scene& scene, float deltaTime) {
        auto view = scene.GetRegistry().view<UnitStatsComponent, MeleeStateComponent, TransformComponent>();

        for (auto entity : view) {
            auto& stats = view.get<UnitStatsComponent>(entity);
            auto& melee = view.get<MeleeStateComponent>(entity);
            auto& trans = view.get<TransformComponent>(entity);

            if (melee.parrySuccessTimer > 0.0f) melee.parrySuccessTimer = std::max(0.0f, melee.parrySuccessTimer - deltaTime);

            // Arma da mischia in uso (stessa regola dell'input e del calcolo del danno)
            const auto* weaponBP = DamageResolver::ResolveMeleeWeapon(scene.GetRegistry(), entity);

            if (melee.inputBufferTimer > 0.0f) {
                melee.inputBufferTimer -= deltaTime;
                if (melee.inputBufferTimer <= 0.0f) {
                    melee.bufferedInput = "";
                }
            }

            if (melee.currentPhase == MeleePhase::None) {
                stats.stamina = std::min(stats.maxStamina, stats.stamina + stats.staminaRegen * deltaTime);
            }

            if (melee.currentPhase == MeleePhase::Stunned) {
                melee.stunTimer -= deltaTime;
                if (melee.stunTimer <= 0.0f) {
                    melee.stunTimer = 0.0f;
                    melee.currentPhase = MeleePhase::None;
                    melee.currentAttackNode = "";
                    melee.currentAttack = MeleeAttackType::None;
                }
                continue;
            }

            if (melee.currentAttack == MeleeAttackType::Parry) {
                melee.stateTimer += deltaTime;
                if (melee.currentPhase == MeleePhase::Windup && melee.stateTimer >= 0.08f) {
                    melee.currentPhase = MeleePhase::Active;
                }
                else if (melee.currentPhase == MeleePhase::Active && melee.stateTimer >= 0.28f) {
                    melee.currentPhase = MeleePhase::Recovery;
                }
                else if (melee.currentPhase == MeleePhase::Recovery && melee.stateTimer >= 0.48f) {
                    melee.currentPhase = MeleePhase::None;
                    melee.currentAttack = MeleeAttackType::None;
                    melee.stateTimer = 0.0f;
                }
                continue;
            }

            if (weaponBP && weaponBP->hasMoveset && !melee.currentAttackNode.empty()) {
                auto it = weaponBP->moveset.nodes.find(melee.currentAttackNode);
                if (it != weaponBP->moveset.nodes.end()) {
                    const auto& node = it->second;
                    melee.stateTimer += deltaTime;

                    if (melee.stateTimer >= node.comboWindow.start && melee.stateTimer <= node.comboWindow.end) {
                        if (!melee.bufferedInput.empty() && node.transitions.find(melee.bufferedInput) != node.transitions.end()) {
                            std::string nextNodeKey = node.transitions.at(melee.bufferedInput);

                            auto nextIt = weaponBP->moveset.nodes.find(nextNodeKey);
                            if (nextIt != weaponBP->moveset.nodes.end()) {
                                const auto& nextNode = nextIt->second;
                                if (stats.stamina >= nextNode.staminaCost) {
                                    stats.stamina -= nextNode.staminaCost;
                                    melee.currentAttackNode = nextNodeKey;
                                    melee.currentPhase = MeleePhase::Windup;
                                    melee.stateTimer = 0.0f;
                                    melee.bufferedInput = "";
                                    melee.hasHitInThisAttack = false;
                                    continue;
                                }
                            }
                        }
                    }

                    if (melee.currentPhase == MeleePhase::Windup && melee.stateTimer >= node.timings.windup) {
                        melee.currentPhase = MeleePhase::Active;
                        melee.stateTimer = 0.0f;
                        CombatVFX::SpawnSlashTrail(scene, entity, trans, melee, &node);
                    }
                    else if (melee.currentPhase == MeleePhase::Active && melee.stateTimer >= node.timings.active) {
                        melee.currentPhase = MeleePhase::Recovery;
                        melee.stateTimer = 0.0f;
                    }
                    else if (melee.currentPhase == MeleePhase::Recovery && melee.stateTimer >= node.timings.recovery) {
                        melee.currentPhase = MeleePhase::None;
                        melee.currentAttackNode = "";
                        melee.stateTimer = 0.0f;
                        melee.hasHitInThisAttack = false;
                    }

                    if (melee.currentPhase == MeleePhase::Active && !melee.hasHitInThisAttack) {
                        CheckMeleeHits(scene, entity, stats, melee, trans);
                    }
                }
                else {
                    // Nodo inesistente nel moveset (es. arma cambiata a meta' colpo): annulla l'attacco invece di
                    // restare bloccati in Windup (che impedirebbe anche la parata, possibile solo da fermi).
                    melee.currentPhase = MeleePhase::None;
                    melee.currentAttackNode = "";
                    melee.currentAttack = MeleeAttackType::None;
                    melee.stateTimer = 0.0f;
                }
            }
        }
    }

    // Colpo in mischia: un ventaglio di raggi orizzontali (+-50 gradi attorno alla mira) a una o piu' altezze,
    // lungo quanto la portata dell'arma, contro i BLOCCHI dello scheletro dei bersagli (le stesse hitbox delle
    // armi a distanza, con la zona colpita). Prima: distanza fra i piedi <= 1.85 m + cono, sempre "Torso", e
    // solo contro unita' con stato di mischia, quindi i manichini del campo da tiro non erano colpibili.
    void MeleeCombatSystem::CheckMeleeHits(Scene& scene, entt::entity attacker, UnitStatsComponent& atkStats, MeleeStateComponent& atkMelee, TransformComponent& atkTrans) {
        if (atkMelee.hasHitInThisAttack) return;
        auto& reg = scene.GetRegistry();

        const auto* weaponBP = DamageResolver::ResolveMeleeWeapon(reg, attacker);
        const float reach = (weaponBP && weaponBP->melee_reach > 0.3f) ? weaponBP->melee_reach : 1.85f;

        const float yaw = glm::radians(atkTrans.AimYaw);
        const glm::vec3 fwd(-std::sin(yaw), 0.0f, -std::cos(yaw));   // = glm::rotate(AimYaw) * (0,0,-1)
        const glm::vec3 up(0.0f, 1.0f, 0.0f);

        // Piedi e altezza dell'attaccante dal collider (che avvolge lo scheletro: Position non e' ai piedi,
        // per il guardsman le gambe scendono 35 cm sotto l'origine). Altezze del colpo in base al tipo di attacco.
        float footY = atkTrans.Position.y, height = 1.8f;
        if (const auto* col = reg.try_get<ColliderComponent>(attacker)) {
            height = std::max(0.8f, col->halfExtents.y * 2.0f * atkTrans.Scale.y);
            footY = atkTrans.Position.y + (col->localCenterOffset.y - col->halfExtents.y) * atkTrans.Scale.y;
        }
        std::string nodeKey = atkMelee.currentAttackNode;
        std::transform(nodeKey.begin(), nodeKey.end(), nodeKey.begin(), ::tolower);
        float heights[3] = { 0.62f, 0.78f, 0.48f };                                   // normale: petto, testa, pancia
        if (nodeKey.find("low") != std::string::npos || atkMelee.currentAttack == MeleeAttackType::Light) {
            heights[0] = 0.30f; heights[1] = 0.45f; heights[2] = 0.15f;              // basso: gambe
        }
        else if (nodeKey.find("heavy") != std::string::npos || atkMelee.currentAttack == MeleeAttackType::Heavy) {
            heights[0] = 0.70f; heights[1] = 0.86f; heights[2] = 0.50f;              // pesante: dall'alto
        }
        constexpr int kRays = 9;
        constexpr float kHalfFan = 50.0f;

        struct Best { entt::entity e = entt::null; float score = 1e9f; glm::vec3 point{ 0.0f }; HitboxLocation loc = HitboxLocation::Torso; };
        Best best;

        auto targetView = reg.view<TransformComponent, HealthComponent, UnitStatsComponent>(entt::exclude<CorpseComponent>);
        for (auto defender : targetView) {
            if (defender == attacker) continue;
            const auto& defTrans = targetView.get<TransformComponent>(defender);
            const auto& defHealth = targetView.get<HealthComponent>(defender);
            if (defHealth.currentHealth <= 0.0f) continue;

            glm::vec3 flat = defTrans.Position - atkTrans.Position; flat.y = 0.0f;
            if (glm::length(flat) > reach + 2.5f) continue;   // scarto veloce

            if (const auto* skel = reg.try_get<SkeletonComponent>(defender); skel && !skel->data.blocks.empty()) {
                const auto mats = skel->data.ComputeWorldTransforms(defTrans.Position, defTrans.Rotation, skel->scale);
                for (int hi = 0; hi < 3; ++hi) {
                    const glm::vec3 origin(atkTrans.Position.x, footY + heights[hi] * height, atkTrans.Position.z);
                    float bestT = reach; int bestBlock = -1; glm::vec3 bestDir = fwd;
                    for (int r = 0; r < kRays; ++r) {
                        const float a = glm::radians(-kHalfFan + 2.0f * kHalfFan * r / (kRays - 1));
                        const glm::vec3 dir = fwd * std::cos(a) + glm::cross(up, fwd) * std::sin(a);
                        for (size_t b = 0; b < mats.size() && b < skel->data.blocks.size(); ++b) {
                            float t;
                            if (IntersectSkeletonBlock(mats[b], origin, dir, bestT, t) && t < bestT) {
                                bestT = t; bestBlock = static_cast<int>(b); bestDir = dir;
                            }
                        }
                    }
                    if (bestBlock >= 0) {
                        const float score = hi * 100.0f + bestT;   // preferisce l'altezza principale del colpo
                        if (score < best.score) {
                            best.e = defender; best.score = score;
                            best.point = origin + bestDir * bestT;
                            best.loc = LocationFromBlockName(skel->data.blocks[bestBlock].name);
                        }
                        break;
                    }
                }
                continue;
            }

            // Senza scheletro: distanza dal bordo del collider + cono davanti all'attaccante
            float radius = 0.35f;
            if (const auto* col = reg.try_get<ColliderComponent>(defender)) {
                radius = std::max(col->halfExtents.x * defTrans.Scale.x, col->halfExtents.z * defTrans.Scale.z);
            }
            const float dist = glm::length(flat);
            if (dist - radius > reach || dist < 1e-4f) continue;
            if (glm::dot(fwd, flat / dist) < 0.40f) continue;
            const float score = 50.0f + dist;
            if (score < best.score) {
                best.e = defender; best.score = score;
                best.point = glm::vec3(defTrans.Position.x, footY + 0.6f * height, defTrans.Position.z);
                best.loc = HitboxLocation::Torso;
            }
        }

        if (best.e == entt::null) return;

        // LOS: un muro fra attaccante e punto colpito blocca il colpo
        const glm::vec3 atkChest(atkTrans.Position.x, footY + 0.6f * height, atkTrans.Position.z);
        if (!HasLineOfSight(scene, atkChest, best.point, attacker)) return;

        const float healthBefore = reg.get<HealthComponent>(best.e).currentHealth;
        const auto result = DamageResolver::ResolveMeleeClash(scene, attacker, best.e, best.loc);
        if (result == DamageResolver::MeleeClashResult::Hit) {
            EmitHitFeedback(scene, attacker, best.e, best.point, best.loc, healthBefore);
        }
        (void)atkStats;
    }

}