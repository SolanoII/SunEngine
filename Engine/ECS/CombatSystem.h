#pragma once

#include "Components.h"
#include "../Scene/Scene.h"
#include "../Core/Logger/Log.h"
#include "../AssetManager/BlueprintManager.h"
#include <algorithm>
#include <cstdlib>
#include <cmath>

namespace SunEngine {

    class PhysicsSystem;

    class CombatSystem {
    public:
        // SOLO DICHIARAZIONE. L'implementazione e' nel file .cpp
        void Update(Scene& scene, PhysicsSystem& physicsSystem, float deltaTime);

        inline static void ResolveMeleeClash(Scene& scene, entt::entity attacker, entt::entity defender, HitboxLocation location) {
            auto& attackerStats = scene.GetRegistry().get<UnitStatsComponent>(attacker);
            auto& attackerMelee = scene.GetRegistry().get<MeleeStateComponent>(attacker);

            auto& defenderStats = scene.GetRegistry().get<UnitStatsComponent>(defender);
            auto& defenderMelee = scene.GetRegistry().get<MeleeStateComponent>(defender);
            auto& defenderHealth = scene.GetRegistry().get<HealthComponent>(defender);

            // PARATA PERFETTA CON Q
            if (defenderMelee.currentAttack == MeleeAttackType::Parry && defenderMelee.stateTimer <= 0.25f) {
                float staminaDamage = attackerStats.strength * 0.45f;
                defenderStats.stamina = std::max(0.0f, defenderStats.stamina - staminaDamage);

                float baseStunDuration = 2.2f;
                float stunDuration = baseStunDuration * (100.0f / (100.0f + attackerStats.toughness));

                attackerMelee.currentPhase = MeleePhase::Stunned;
                attackerMelee.stunTimer = stunDuration;
                attackerMelee.hasHyperArmor = false;

                SUN_CORE_INFO("PERFECT PARRY CON Q! Attaccante Stordito per {0}s", stunDuration);
                return;
            }

            // MATRICE SASSO - CARTA - FORBICE
            MeleeAttackType atkType = attackerMelee.currentAttack;
            MeleeAttackType defType = defenderMelee.currentAttack;

            bool attackerInterrupted = false;

            if (atkType == MeleeAttackType::Light && defType == MeleeAttackType::Normal) {
                InterruptUnit(defenderMelee);
            }
            else if (atkType == MeleeAttackType::Normal && defType == MeleeAttackType::Heavy) {
                InterruptUnit(defenderMelee);
            }
            else if (atkType == MeleeAttackType::Heavy && defType == MeleeAttackType::Light) {
                SUN_CORE_INFO("HYPER ARMOR! Colpo Leggero assorbito dall'Attacco Pesante.");
            }

            if (!attackerInterrupted) {
                float rawDamage = (attackerStats.strength * 0.5f) + 15.0f;
                if (atkType == MeleeAttackType::Heavy) rawDamage *= 2.0f;
                if (atkType == MeleeAttackType::Light) rawDamage *= 0.7f;

                ResolveHit(rawDamage, 10, location, defenderStats, defenderHealth);
                attackerMelee.hasHitInThisAttack = true;
            }
        }

        inline static void ResolveHit(float rawDamage, int weaponAP, HitboxLocation location, UnitStatsComponent& stats, HealthComponent& health) {
            ArmorPiece* armor = stats.armorSet.GetPiece(location);
            float finalHpDamage = rawDamage;

            if (armor && armor->currentDurability > 0.0f && armor->armorRating > 0) {
                float coverageRoll = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);

                if (coverageRoll <= armor->coverage) {
                    int effectiveArmor = std::max(0, armor->armorRating - weaponAP);
                    float mitigation = 100.0f / (100.0f + effectiveArmor);
                    finalHpDamage = rawDamage * mitigation;

                    armor->currentDurability -= rawDamage * 0.4f;
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

    private:
        inline static void ProcessAttackTimings(MeleeStateComponent& melee, float windupTime, float activeTime, float recoveryTime) {
            if (melee.currentPhase == MeleePhase::Windup && melee.stateTimer >= windupTime) {
                melee.currentPhase = MeleePhase::Active;
                melee.stateTimer = 0.0f;
            }
            else if (melee.currentPhase == MeleePhase::Active && melee.stateTimer >= activeTime) {
                melee.currentPhase = MeleePhase::Recovery;
                melee.stateTimer = 0.0f;
                melee.hasHyperArmor = false;
            }
            else if (melee.currentPhase == MeleePhase::Recovery && melee.stateTimer >= recoveryTime) {
                melee.currentPhase = MeleePhase::None;
                melee.currentAttack = MeleeAttackType::None;
                melee.stateTimer = 0.0f;
                melee.hasHitInThisAttack = false;
            }
        }

        inline static void InterruptUnit(MeleeStateComponent& melee) {
            melee.currentPhase = MeleePhase::Recovery;
            melee.stateTimer = 0.0f;
            melee.hasHyperArmor = false;
            melee.hasHitInThisAttack = true;
        }

        inline static void CheckMeleeHits(Scene& scene, entt::entity attacker, UnitStatsComponent& atkStats, MeleeStateComponent& atkMelee, TransformComponent& atkTrans) {
            glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(atkTrans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
            glm::vec3 fwd = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

            auto targetView = scene.GetRegistry().view<TransformComponent, HealthComponent, UnitStatsComponent, MeleeStateComponent>();

            for (auto defender : targetView) {
                if (defender == attacker) continue;

                const auto& defTrans = targetView.get<TransformComponent>(defender);
                float dist = glm::distance(atkTrans.Position, defTrans.Position);

                if (dist <= 1.8f) {
                    glm::vec3 dirToDef = glm::normalize(defTrans.Position - atkTrans.Position);
                    float dot = glm::dot(fwd, dirToDef);

                    if (dot > 0.45f) {
                        ResolveMeleeClash(scene, attacker, defender, HitboxLocation::Torso);
                        break;
                    }
                }
            }
        }

    private:
        bool m_MousePressedLastFrame = false;
    };

}