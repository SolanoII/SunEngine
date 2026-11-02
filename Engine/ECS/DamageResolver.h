#pragma once

#include "Components.h"
#include "../Scene/Scene.h"
#include "../AssetManager/BlueprintManager.h"
#include "../Core/Logger/Log.h"
#include "../Core/Random.h"
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <glm/glm.hpp>

namespace SunEngine {

    class DamageResolver {
    public:
        inline static void ResolveHit(float rawDamage, int weaponAP, HitboxLocation location, UnitStatsComponent& stats, HealthComponent& health) {
            ArmorPiece* armor = stats.armorSet.GetPiece(location);
            float finalHpDamage = rawDamage;

            if (armor && armor->currentDurability > 0.0f && armor->armorRating > 0) {
                if (Random::Chance(armor->coverage)) {
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

        // ---------- Parata (Q) ----------
        // Finestra in cui la parata ferma un colpo: windup + fase attiva (0 .. 0.28 s dall'inizio).
        static constexpr float kParryWindowEnd = 0.28f;

        inline static bool IsParryActive(const MeleeStateComponent& m) {
            return m.currentAttack == MeleeAttackType::Parry
                && m.currentPhase != MeleePhase::None && m.currentPhase != MeleePhase::Stunned
                && m.stateTimer <= kParryWindowEnd;
        }

        // Arma da mischia in uso: quella impugnata se ha un moveset, altrimenti quella dell'inventario,
        // altrimenti la spada a catena (giocatore) o la choppa (nemici). La stessa regola vale per l'input
        // del giocatore, per il sistema di mischia e per il calcolo del danno.
        inline static const WeaponBlueprint* ResolveMeleeWeapon(entt::registry& reg, entt::entity e) {
            auto& bpm = BlueprintManager::Get();
            if (const auto* wid = reg.try_get<WeaponBlueprintId>(e)) {
                const auto* bp = bpm.GetWeapon(wid->id);
                if (bp && bp->hasMoveset) return bp;
            }
            if (const auto* inv = reg.try_get<InventoryComponent>(e)) {
                if (!inv->meleeWeaponId.empty() && inv->meleeWeaponId != "none") {
                    const auto* bp = bpm.GetWeapon(inv->meleeWeaponId);
                    if (bp && bp->hasMoveset) return bp;
                }
            }
            const auto* fallback = bpm.GetWeapon(reg.all_of<PlayerTag>(e) ? "standard_chainsword_1h" : "ork_choppa_1h");
            if (fallback && fallback->hasMoveset) return fallback;
            return bpm.GetWeapon("ork_choppa_1h");
        }

        // La parata del difensore puo' fermare anche i colpi a distanza? Si' con uno scudo, con un'abilita'
        // (parryBlocksRanged) o con un'arma da mischia che lo dichiara ("parry_blocks_ranged": true).
        inline static bool CanParryRanged(entt::registry& reg, entt::entity defender) {
            const auto* melee = reg.try_get<MeleeStateComponent>(defender);
            if (!melee) return false;
            if (melee->isShieldEquipped || melee->parryBlocksRanged) return true;
            const auto* bp = ResolveMeleeWeapon(reg, defender);
            return bp && bp->parry_blocks_ranged;
        }

        // Colpo a distanza contro un'unita' in parata: bloccato se la parata e' nella finestra attiva, se puo'
        // fermare i colpi a distanza e se il colpo arriva da davanti (entro ~80 gradi dalla direzione di mira).
        // Ritorna true se il colpo e' stato parato (niente danno).
        inline static bool TryParryRanged(Scene& scene, entt::entity defender, const glm::vec3& shotDir) {
            auto& reg = scene.GetRegistry();
            auto* melee = reg.try_get<MeleeStateComponent>(defender);
            const auto* trans = reg.try_get<TransformComponent>(defender);
            if (!melee || !trans || !IsParryActive(*melee) || !CanParryRanged(reg, defender)) return false;
            const float yaw = glm::radians(trans->AimYaw);
            const glm::vec3 fwd(-std::sin(yaw), 0.0f, -std::cos(yaw));   // come glm::rotate(AimYaw) * (0,0,-1)
            glm::vec3 incoming(-shotDir.x, 0.0f, -shotDir.z);              // verso il tiratore
            if (glm::length(incoming) < 1e-4f) return false;
            if (glm::dot(fwd, glm::normalize(incoming)) < 0.17f) return false;
            melee->parrySuccessTimer = 0.25f;
            SUN_CORE_INFO("PARATA A DISTANZA: colpo deviato");
            return true;
        }

        enum class MeleeClashResult { Parried, Countered, Hit };

        // Risolve un colpo in mischia gia' andato a segno sulla zona 'location'. Il difensore puo' non avere
        // stato di mischia (manichini del campo da tiro, unita' senza arma bianca): in quel caso niente
        // parata ne' contrattacco, solo danno.
        inline static MeleeClashResult ResolveMeleeClash(Scene& scene, entt::entity attacker, entt::entity defender, HitboxLocation location) {
            auto& reg = scene.GetRegistry();
            auto& attackerStats = reg.get<UnitStatsComponent>(attacker);
            auto& attackerMelee = reg.get<MeleeStateComponent>(attacker);

            auto& defenderStats = reg.get<UnitStatsComponent>(defender);
            auto& defenderHealth = reg.get<HealthComponent>(defender);
            auto* defenderMelee = reg.try_get<MeleeStateComponent>(defender);

            // PARATA PERFETTA CON Q (blocca sempre i colpi in mischia)
            if (defenderMelee && IsParryActive(*defenderMelee)) {
                float staminaDamage = attackerStats.strength * 0.45f;
                defenderStats.stamina = std::max(0.0f, defenderStats.stamina - staminaDamage);
                defenderMelee->parrySuccessTimer = 0.25f;

                float baseStunDuration = 2.2f;
                float stunDuration = baseStunDuration * (100.0f / (100.0f + attackerStats.toughness));

                attackerMelee.currentPhase = MeleePhase::Stunned;
                attackerMelee.stunTimer = stunDuration;
                attackerMelee.hasHyperArmor = false;
                attackerMelee.hasHitInThisAttack = true;

                SUN_CORE_INFO("PERFECT PARRY! Attaccante Stordito per {0}s", stunDuration);
                return MeleeClashResult::Parried;
            }

            // REGOLE DI COUNTER MELEE & INTERRUZIONE AI
            MeleeAttackType atkType = attackerMelee.currentAttack;
            MeleeAttackType defType = defenderMelee ? defenderMelee->currentAttack : MeleeAttackType::None;

            if (defenderMelee && defType == MeleeAttackType::Normal && atkType == MeleeAttackType::Light) {
                InterruptUnit(*defenderMelee);
                if (reg.all_of<HordeAgentComponent>(defender)) {
                    auto& horde = reg.get<HordeAgentComponent>(defender);
                    horde.state = HordeState::Cooldown;
                    horde.cooldownTimer = 2.5f;
                    horde.assignedSlotIndex = -1;
                }
                attackerMelee.hasHitInThisAttack = true;
                SUN_CORE_INFO("COUNTER BASSO! Attacco Orco interrotto!");
                return MeleeClashResult::Countered;
            }

            // CALCOLO DANNO REALE (Strength + BaseDamage * Multiplier) con l'arma da mischia vera:
            // prima si usava l'arma impugnata, cioe' il fucile, col suo danno e senza moltiplicatore del nodo.
            const auto* weaponBP = ResolveMeleeWeapon(reg, attacker);

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
            return MeleeClashResult::Hit;
        }
    };

}