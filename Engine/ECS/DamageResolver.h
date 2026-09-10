#pragma once

#include "Components.h"
#include "../Scene/Scene.h"
#include "../AssetManager/BlueprintManager.h"
#include "../Core/Logger/Log.h"
#include <algorithm>
#include <cstdlib>

namespace SunEngine {

    class DamageResolver {
    public:
        inline static void ResolveHit(float rawDamage, int weaponAP, HitboxLocation location, UnitStatsComponent& stats, HealthComponent& health) {
            ArmorPiece* armor = stats.armorSet.GetPiece(location);
            float finalHpDamage = rawDamage;

            if (armor && armor->currentDurability > 0.0f && armor->armorRating > 0) {
                float coverageRoll = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);

                if (coverageRoll <= armor->coverage) {
                    int effectiveArmor = std::max(0, armor->armorRating - weaponAP);
                    float mitigation = 100.0f / (100.0f + static_cast<float>(effectiveArmor));
                    finalHpDamage = rawDamage * mitigation;

                    armor->currentDurability -= rawDamage * 0.2f;
                    if (armor->currentDurability < 0.0f) {
                        armor->currentDurability = 0.0f;
                    }
                }
            }

            float toughnessMitigation = 100.0f / (100.0f + stats.toughness);
            finalHpDamage *= toughnessMitigation;

            if (location == HitboxLocation::Head) {
                finalHpDamage *= 1.75f;
            }
            else if (location == HitboxLocation::LeftLeg || location == HitboxLocation::RightLeg ||
                location == HitboxLocation::LeftArm || location == HitboxLocation::RightArm) {
                finalHpDamage *= 0.8f;
            }

            health.currentHealth -= finalHpDamage;
            if (health.currentHealth < 0.0f) health.currentHealth = 0.0f;
        }

        inline static void InterruptUnit(MeleeStateComponent& melee) {
            melee.currentPhase = MeleePhase::Recovery;
            melee.stateTimer = 0.0f;
            melee.hasHyperArmor = false;
            melee.hasHitInThisAttack = true;
        }

        inline static void ResolveMeleeClash(Scene& scene, entt::entity attacker, entt::entity defender, HitboxLocation location) {
            auto& attackerStats = scene.GetRegistry().get<UnitStatsComponent>(attacker);
            auto& attackerMelee = scene.GetRegistry().get<MeleeStateComponent>(attacker);

            auto& defenderStats = scene.GetRegistry().get<UnitStatsComponent>(defender);
            auto& defenderMelee = scene.GetRegistry().get<MeleeStateComponent>(defender);
            auto& defenderHealth = scene.GetRegistry().get<HealthComponent>(defender);

            // PARATA PERFETTA CON Q
            if (defenderMelee.currentAttack == MeleeAttackType::Parry && defenderMelee.stateTimer <= 0.28f) {
                float staminaDamage = attackerStats.strength * 0.45f;
                defenderStats.stamina = std::max(0.0f, defenderStats.stamina - staminaDamage);

                float baseStunDuration = 2.2f;
                float stunDuration = baseStunDuration * (100.0f / (100.0f + attackerStats.toughness));

                attackerMelee.currentPhase = MeleePhase::Stunned;
                attackerMelee.stunTimer = stunDuration;
                attackerMelee.hasHyperArmor = false;

                SUN_CORE_INFO("PERFECT PARRY! Attaccante Stordito per {0}s", stunDuration);
                return;
            }

            // REGOLE DI COUNTER MELEE & INTERRUZIONE AI
            MeleeAttackType atkType = attackerMelee.currentAttack;
            MeleeAttackType defType = defenderMelee.currentAttack;

            if (defType == MeleeAttackType::Normal && atkType == MeleeAttackType::Light) {
                InterruptUnit(defenderMelee);
                if (scene.GetRegistry().all_of<HordeAgentComponent>(defender)) {
                    auto& horde = scene.GetRegistry().get<HordeAgentComponent>(defender);
                    horde.state = HordeState::Cooldown;
                    horde.cooldownTimer = 2.5f;
                    horde.assignedSlotIndex = -1;
                }
                SUN_CORE_INFO("COUNTER BASSO! Attacco Orco interrotto!");
                return;
            }

            // CALCOLO DANNO REALE (Strength + BaseDamage * Multiplier)
            std::string activeWeaponId = "";
            if (scene.GetRegistry().all_of<WeaponBlueprintId>(attacker)) {
                activeWeaponId = scene.GetRegistry().get<WeaponBlueprintId>(attacker).id;
            }

            const auto* weaponBP = BlueprintManager::Get().GetWeapon(activeWeaponId);
            if (!weaponBP && scene.GetRegistry().all_of<InventoryComponent>(attacker)) {
                const auto& inv = scene.GetRegistry().get<InventoryComponent>(attacker);
                weaponBP = BlueprintManager::Get().GetWeapon(inv.meleeWeaponId);
            }

            float weaponBaseDamage = weaponBP ? weaponBP->base_damage : 20.0f;
            float nodeMult = 1.0f;

            if (weaponBP && weaponBP->hasMoveset && !attackerMelee.currentAttackNode.empty()) {
                auto it = weaponBP->moveset.nodes.find(attackerMelee.currentAttackNode);
                if (it != weaponBP->moveset.nodes.end()) {
                    nodeMult = it->second.damageMultiplier;
                }
            }

            float rawDamage = (attackerStats.strength * 0.4f) + (weaponBaseDamage * nodeMult);
            int ap = weaponBP ? weaponBP->armor_penetration : 10;

            ResolveHit(rawDamage, ap, location, defenderStats, defenderHealth);
            attackerMelee.hasHitInThisAttack = true;
        }
    };

}