#include "MeleeCombatSystem.h"
#include "DamageResolver.h"
#include "CombatVFX.h"
#include "RaycastUtils.h" 
#include <algorithm>

namespace SunEngine {

    void MeleeCombatSystem::Update(Scene& scene, float deltaTime) {
        auto view = scene.GetRegistry().view<UnitStatsComponent, MeleeStateComponent, TransformComponent>();

        for (auto entity : view) {
            auto& stats = view.get<UnitStatsComponent>(entity);
            auto& melee = view.get<MeleeStateComponent>(entity);
            auto& trans = view.get<TransformComponent>(entity);

            std::string activeMeleeId = "";
            if (scene.GetRegistry().all_of<WeaponBlueprintId>(entity)) {
                activeMeleeId = scene.GetRegistry().get<WeaponBlueprintId>(entity).id;
            }

            const auto* weaponBP = BlueprintManager::Get().GetWeapon(activeMeleeId);
            if (!weaponBP || !weaponBP->hasMoveset) {
                if (scene.GetRegistry().all_of<InventoryComponent>(entity)) {
                    const auto& inv = scene.GetRegistry().get<InventoryComponent>(entity);
                    if (!inv.meleeWeaponId.empty()) {
                        weaponBP = BlueprintManager::Get().GetWeapon(inv.meleeWeaponId);
                    }
                }
            }
            if (!weaponBP || !weaponBP->hasMoveset) {
                weaponBP = BlueprintManager::Get().GetWeapon("ork_choppa_1h");
            }

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
            }
        }
    }

    void MeleeCombatSystem::CheckMeleeHits(Scene& scene, entt::entity attacker, UnitStatsComponent& atkStats, MeleeStateComponent& atkMelee, TransformComponent& atkTrans) {
        if (atkMelee.hasHitInThisAttack) return;

        glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(atkTrans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::vec3 fwd = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

        auto targetView = scene.GetRegistry().view<TransformComponent, HealthComponent, UnitStatsComponent, MeleeStateComponent>();

        for (auto defender : targetView) {
            if (defender == attacker) continue;

            const auto& defTrans = targetView.get<TransformComponent>(defender);
            float dist = glm::distance(atkTrans.Position, defTrans.Position);

            if (dist <= 1.85f) {
                glm::vec3 dirToDef = glm::normalize(defTrans.Position - atkTrans.Position);
                float dot = glm::dot(fwd, dirToDef);

                if (dot > 0.40f) {
                    // LOS check: dal petto dell'attaccante al petto del difensore.
                    // Se un muro/ostacolo statico blocca la linea, il colpo non va a segno.
                    glm::vec3 atkChest = atkTrans.Position + glm::vec3(0.0f, 1.0f, 0.0f);
                    glm::vec3 defChest = defTrans.Position + glm::vec3(0.0f, 1.0f, 0.0f);

                    // HasLineOfSight ritorna true se la linea è libera.
                    // Passiamo "attacker" come ignoreEntity per non far collidere
                    // il raycast con il proprio stesso collider (che potrebbe
                    // essere molto vicino ai piedi).
                    if (HasLineOfSight(scene, atkChest, defChest, attacker)) {
                        DamageResolver::ResolveMeleeClash(scene, attacker, defender, HitboxLocation::Torso);
                        break;
                    }
                }
            }
        }
    }

}