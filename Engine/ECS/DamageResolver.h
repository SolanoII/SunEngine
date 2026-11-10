#pragma once

#include "Components.h"
#include "../Scene/Scene.h"
#include "../AssetManager/BlueprintManager.h"
#include "../Core/Logger/Log.h"
#include "../Core/Random.h"
#include "VfxEvents.h"
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

            bool helmetHeld = false;   // elmetto che ha retto il colpo: niente moltiplicatore della testa
            if (armor && armor->currentDurability > 0.0f && armor->armorRating > 0) {
                if (Random::Chance(armor->coverage)) {
                    helmetHeld = armor->blocksHeadshotBonus;
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

            if (location == HitboxLocation::Head && !helmetHeld) {
                finalHpDamage *= 1.75f;
            }
            else if (location == HitboxLocation::LeftLeg || location == HitboxLocation::RightLeg ||
                location == HitboxLocation::LeftArm || location == HitboxLocation::RightArm) {
                finalHpDamage *= 0.8f;
            }

            finalHpDamage *= stats.damageTakenMult;   // es. ordine "Take Cover"
            health.currentHealth -= finalHpDamage;
            if (health.currentHealth < 0.0f) health.currentHealth = 0.0f;
        }

        inline static void InterruptUnit(MeleeStateComponent& melee) {
            melee.currentPhase = MeleePhase::Recovery;
            melee.stateTimer = 0.0f;
            melee.hasHyperArmor = false;
            melee.hasHitInThisAttack = true;
        }

        // ---------- Ritmo della mischia (carta-sasso-forbice) ----------
        //   normale / basso  batte  pesante  (lo colpisce mentre carica: interrotto e barcollante)
        //   pesante          batte  parata   (la parata non lo ferma: guardia rotta, stordito)
        //   parata           batte  normale / basso (attaccante stordito)
        // Perche' si possa leggere e rispondere, ogni tipo di colpo ha una carica minima visibile (con l'anello che
        // si stringe fino al momento del colpo) e un recupero minimo; un colpo a vuoto lascia scoperti di piu'.
        // Colpire chi sta recuperando fa piu' danno (punizione).
        static constexpr float kPunishDamage = 1.25f;      // colpo su chi e' in recupero
        static constexpr float kHeavyStagger = 0.55f;      // pesante interrotto: barcolla
        static constexpr float kGuardBreakStun = 1.0f;     // parata rotta da un pesante
        static constexpr float kWhiffNormal = 0.2f, kWhiffHeavy = 0.3f;
        // Punto della fase attiva in cui l'arma tocca il bersaglio (0 = inizio, 1 = fine): li' si applica il colpo
        static constexpr float kMeleeContactPoint = 0.45f;

        inline static bool IsComboFollowUp(const std::string& node) {
            return node.find("_2") != std::string::npos || node.find("_3") != std::string::npos;
        }
        // Tempi effettivi di un colpo: quelli dell'arma, con i minimi del suo tipo
        inline static MeleeTiming EffectiveTiming(const MeleeNode& node, MeleeAttackType type, const std::string& nodeKey) {
            MeleeTiming t = node.timings;
            const bool follow = IsComboFollowUp(nodeKey);
            float minWind = 0.24f, minRec = 0.25f;
            if (type == MeleeAttackType::Heavy || nodeKey.find("heavy") != std::string::npos) { minWind = follow ? 0.40f : 0.50f; minRec = 0.45f; }
            else if (type == MeleeAttackType::Light || nodeKey.find("low") != std::string::npos) { minWind = follow ? 0.16f : 0.20f; minRec = 0.22f; }
            else if (follow) minWind = 0.18f;
            t.windup = std::max(t.windup, minWind);
            t.active = std::max(t.active, 0.10f);
            t.recovery = std::max(t.recovery, minRec);
            return t;
        }

        // ---------- Parata (Q) ----------
        // Finestra in cui la parata ferma un colpo: windup + fase attiva (0 .. 0.28 s dall'inizio).
        static constexpr float kParryWindowEnd = 0.28f;
        // Fine della parata: riuscita si torna in guardia presto (per punire), a vuoto si resta scoperti
        inline static float ParryEnd(const MeleeStateComponent& m) { return m.parrySucceeded ? 0.40f : 0.62f; }

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
            // Unita' senza armi da mischia (es. loota): la sua arma di riserva
            if (const auto* uid = reg.try_get<UnitBlueprintId>(e)) {
                if (const auto* ubp = bpm.GetUnit(uid->id); ubp && !ubp->melee_fallback.empty()) {
                    const auto* fb = bpm.GetWeapon(ubp->melee_fallback);
                    if (fb && fb->hasMoveset) return fb;
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

        // L'attaccante e' alle spalle del difensore? (dietro al busto: fuori dai 240 gradi davanti, cioe' nel
        // cono di 120 gradi alle spalle)
        inline static bool IsBehind(entt::registry& reg, entt::entity attacker, entt::entity defender) {
            const auto* ta = reg.try_get<TransformComponent>(attacker);
            const auto* td = reg.try_get<TransformComponent>(defender);
            if (!ta || !td) return false;
            const float yaw = glm::radians(td->Rotation.y);   // orientamento del corpo
            const glm::vec3 fwd(-std::sin(yaw), 0.0f, -std::cos(yaw));
            glm::vec3 toAttacker = ta->Position - td->Position; toAttacker.y = 0.0f;
            if (glm::length(toAttacker) < 1e-4f) return false;
            return glm::dot(fwd, glm::normalize(toAttacker)) < -0.5f;
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

            const auto* defTrans = reg.try_get<TransformComponent>(defender);
            const glm::vec3 fxPos = defTrans ? defTrans->Position + glm::vec3(0.0f, 1.1f, 0.0f) : glm::vec3(0.0f);
            bool guardBroken = false;

            // PESANTE CONTRO PARATA: la parata non regge, guardia rotta (stordito) e il colpo passa
            if (defenderMelee && IsParryActive(*defenderMelee) && attackerMelee.currentAttack == MeleeAttackType::Heavy) {
                defenderMelee->currentPhase = MeleePhase::Stunned;
                defenderMelee->currentAttack = MeleeAttackType::None;
                defenderMelee->currentAttackNode = "";
                defenderMelee->stunTimer = kGuardBreakStun;
                defenderMelee->parrySucceeded = false;
                defenderStats.stamina = std::max(0.0f, defenderStats.stamina - attackerStats.strength * 0.6f);
                guardBroken = true;
                { VfxEvent e; e.kind = VfxKind::GuardBreak; e.pos = fxPos; EmitVfx(e); }
                EmitVfxText(fxPos + glm::vec3(0.0f, 0.7f, 0.0f), "GUARDIA ROTTA!", glm::vec4(1.0f, 0.35f, 0.2f, 1.0f));
                SUN_CORE_INFO("GUARDIA ROTTA dal colpo pesante!");
            }
            // PARATA PERFETTA CON Q (blocca i colpi normali e bassi)
            else if (defenderMelee && IsParryActive(*defenderMelee)) {
                defenderMelee->parrySucceeded = true;
                { VfxEvent e; e.kind = VfxKind::Parry; e.pos = fxPos; EmitVfx(e); }
                EmitVfxText(fxPos + glm::vec3(0.0f, 0.7f, 0.0f), "PARATA!", glm::vec4(1.0f, 0.9f, 0.5f, 1.0f));
                float staminaDamage = attackerStats.strength * 0.45f;
                defenderStats.stamina = std::max(0.0f, defenderStats.stamina - staminaDamage);
                defenderMelee->parrySuccessTimer = 0.25f;

                float baseStunDuration = 2.2f;
                float stunDuration = baseStunDuration * (100.0f / (100.0f + attackerStats.toughness));
                // Immune agli stordimenti (ordine "Take Cover"): il colpo e' parato ma l'attaccante non resta stordito
                if (const auto* buff = reg.try_get<FactionBuffComponent>(attacker); buff && buff->stunImmuneTimer > 0.0f) {
                    attackerMelee.hasHitInThisAttack = true;
                    return MeleeClashResult::Parried;
                }

                attackerMelee.currentPhase = MeleePhase::Stunned;
                attackerMelee.stunTimer = stunDuration;
                attackerMelee.hasHyperArmor = false;
                attackerMelee.hasHitInThisAttack = true;

                SUN_CORE_INFO("PERFECT PARRY! Attaccante Stordito per {0}s", stunDuration);
                return MeleeClashResult::Parried;
            }

            // EVASIONE (es. squig): il colpo va a vuoto, uno solo per fendente
            if (const auto* ev = reg.try_get<EvasionComponent>(defender); ev && ev->chance > 0.0f && Random::Chance(ev->chance)) {
                attackerMelee.hasHitInThisAttack = true;
                SUN_CORE_INFO("Colpo schivato!");
                return MeleeClashResult::Countered;
            }

            // Stato del difensore al momento del colpo
            MeleeAttackType defType = defenderMelee ? defenderMelee->currentAttack : MeleeAttackType::None;
            const MeleePhase defPhase = defenderMelee ? defenderMelee->currentPhase : MeleePhase::None;
            (void)guardBroken;

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
            if (const auto* buff = reg.try_get<FactionBuffComponent>(attacker)) rawDamage *= buff->meleeDamageMult;   // WAAAGH!
            int ap = weaponBP ? weaponBP->armor_penetration : 10;

            // Colpo alle spalle: armi con "backstab_multiplier" (es. coltello del gretchin x2)
            if (weaponBP && weaponBP->backstab_multiplier != 1.0f && IsBehind(reg, attacker, defender)) {
                rawDamage *= weaponBP->backstab_multiplier;
                SUN_CORE_INFO("COLPO ALLE SPALLE! Danno x{0}", weaponBP->backstab_multiplier);
            }

            // Punizione: chi sta recuperando da un suo colpo prende piu' danno
            if (defenderMelee && defPhase == MeleePhase::Recovery && defType != MeleeAttackType::Parry) {
                rawDamage *= kPunishDamage;
                EmitVfxText(fxPos + glm::vec3(0.0f, 0.7f, 0.0f), "PUNITO!", glm::vec4(1.0f, 0.6f, 0.2f, 1.0f));
            }

            ResolveHit(rawDamage, ap, location, defenderStats, defenderHealth);
            attackerMelee.hasHitInThisAttack = true;
            // Fermo sul colpo: un istante di pausa per entrambi (piu' lungo per i pesanti), si sente l'impatto
            const float stop = attackerMelee.currentAttack == MeleeAttackType::Heavy ? 0.12f : 0.06f;
            attackerMelee.hitStop = stop;
            if (defenderMelee && defPhase != MeleePhase::Stunned) defenderMelee->hitStop = stop;

            // Interruzione: chi viene colpito mentre carica perde il colpo (il pesante resta anche barcollante).
            // Durante la fase attiva il colpo e' gia' partito: si scambiano i colpi.
            if (defenderMelee && defPhase == MeleePhase::Windup && defType != MeleeAttackType::Parry) {
                if (defType == MeleeAttackType::Heavy) {
                    defenderMelee->currentPhase = MeleePhase::Stunned;
                    defenderMelee->stunTimer = kHeavyStagger;
                    defenderMelee->currentAttackNode = "";
                    defenderMelee->currentAttack = MeleeAttackType::None;
                    EmitVfxText(fxPos + glm::vec3(0.0f, 0.7f, 0.0f), "INTERROTTO!", glm::vec4(0.55f, 0.85f, 1.0f, 1.0f));
                }
                else {
                    InterruptUnit(*defenderMelee);
                    EmitVfxText(fxPos + glm::vec3(0.0f, 0.7f, 0.0f), "INTERROTTO", glm::vec4(0.55f, 0.85f, 1.0f, 0.9f));
                }
                { VfxEvent e; e.kind = VfxKind::Interrupt; e.pos = fxPos; EmitVfx(e); }
                if (reg.all_of<HordeAgentComponent>(defender)) {
                    auto& horde = reg.get<HordeAgentComponent>(defender);
                    horde.state = HordeState::Cooldown;
                    horde.cooldownTimer = 2.5f;
                    horde.assignedSlotIndex = -1;
                }
            }
            return MeleeClashResult::Hit;
        }
    };

}