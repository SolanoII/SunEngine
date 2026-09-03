#include "CombatSystem.h"

namespace SunEngine {

    void CombatSystem::Update(Scene& scene, PhysicsSystem& physicsSystem, float deltaTime) {
        auto view = scene.GetRegistry().view<UnitStatsComponent, MeleeStateComponent, TransformComponent, WeaponBlueprintId>();

        for (auto entity : view) {
            auto& stats = view.get<UnitStatsComponent>(entity);
            auto& melee = view.get<MeleeStateComponent>(entity);
            auto& trans = view.get<TransformComponent>(entity);
            const auto& wId = view.get<WeaponBlueprintId>(entity).id;

            const auto* weaponBP = BlueprintManager::Get().GetWeapon(wId);

            // Gestione timer buffer input
            if (melee.inputBufferTimer > 0.0f) {
                melee.inputBufferTimer -= deltaTime;
                if (melee.inputBufferTimer <= 0.0f) {
                    melee.bufferedInput = "";
                }
            }

            // Rigenerazione Stamina fuori dal combattimento
            if (melee.currentPhase == MeleePhase::None) {
                stats.stamina = std::min(stats.maxStamina, stats.stamina + stats.staminaRegen * deltaTime);
            }

            // Gestione Stun da Parata
            if (melee.currentPhase == MeleePhase::Stunned) {
                melee.stunTimer -= deltaTime;
                if (melee.stunTimer <= 0.0f) {
                    melee.stunTimer = 0.0f;
                    melee.currentPhase = MeleePhase::None;
                    melee.currentAttackNode = "";
                }
                continue;
            }

            // ESECUZIONE MOVESET DINAMICO DA JSON
            if (weaponBP && weaponBP->hasMoveset && !melee.currentAttackNode.empty()) {
                auto it = weaponBP->moveset.nodes.find(melee.currentAttackNode);
                if (it != weaponBP->moveset.nodes.end()) {
                    const auto& node = it->second;
                    melee.stateTimer += deltaTime;

                    // FINISTRA DI COMBO (CANCEL/TRANSITION)
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

                    // TIMINGS DEL NODO ATTUALE
                    if (melee.currentPhase == MeleePhase::Windup && melee.stateTimer >= node.timings.windup) {
                        melee.currentPhase = MeleePhase::Active;
                        melee.stateTimer = 0.0f;
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

                    // HITBOX DAMAGE CHECK
                    if (melee.currentPhase == MeleePhase::Active && !melee.hasHitInThisAttack) {
                        CheckMeleeHits(scene, entity, stats, melee, trans);
                    }
                }
            }
        }
    }

}