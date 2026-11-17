#pragma once
// Slancio e carica (Feral Pig): chi ha "momentum_bonus" nel blueprint prende velocita' andando dritto.
//
//   - Slancio (0..1): cresce correndo in linea retta (pieno in "momentum_time" secondi), cala girando e si
//     perde in fretta fermandosi. La velocita' sale fino a +momentum_bonus (es. +75%). Sostituisce la corsa
//     con SHIFT: non consuma stamina.
//   - Carica: sopra "charge_threshold" di slancio, il primo nemico che si trova davanti a contatto prende le
//     zanne: "charge_damage" (piu' forte a slancio pieno), viene spinto indietro e barcolla. Lo slancio si
//     azzera e serve un attimo prima di poter caricare di nuovo.
//   - A quattro zampe ("quadruped_run"): la posa la fanno GameLayer / PoseLib leggendo lo slancio da qui.
//
// Lo slancio lo aggiornano MovementSystem (giocatore) e BotAISystem (bot) quando muovono l'unita'; la carica la
// risolve UpdateFeralCharges una volta per fotogramma.
#include "Components.h"
#include "DamageResolver.h"
#include "HitUtils.h"
#include "VfxEvents.h"
#include "Tags.h"
#include "../Scene/Scene.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace SunEngine {

    namespace FeralCharge {

        inline constexpr float kGoreReach = 1.25f;       // metri davanti al muso
        inline constexpr float kGoreCooldown = 1.4f;     // secondi fra una carica e l'altra
        inline constexpr float kGoreStun = 0.7f;         // il colpito barcolla
        inline constexpr float kGoreKnockback = 0.9f;    // metri

        // Da chiamare ogni fotogramma in cui l'unita' viene mossa. moveDir = direzione nel mondo (qualsiasi
        // lunghezza), straight = sta andando "in avanti" (per il giocatore: W senza S). Ritorna il moltiplicatore
        // di velocita' (1 = nessuno slancio).
        inline float TickMomentum(entt::registry& reg, entt::entity e, const UnitStatsComponent& stats, glm::vec3 moveDir, bool straight, float dt) {
            if (stats.momentumBonus <= 0.0f) return 1.0f;
            auto& m = reg.get_or_emplace<MomentumComponent>(e);
            moveDir.y = 0.0f;
            const float len = glm::length(moveDir);
            if (len < 1e-4f || !straight) {
                m.value = std::max(0.0f, m.value - dt * (len < 1e-4f ? 2.5f : 1.5f));   // fermo o di lato: lo slancio si perde
            }
            else {
                const glm::vec2 d(moveDir.x / len, moveDir.z / len);
                const float align = glm::dot(m.dir, m.dir) > 1e-6f ? glm::dot(glm::normalize(m.dir), d) : 1.0f;
                if (align > 0.94f) m.value = std::min(1.0f, m.value + dt / std::max(0.2f, stats.momentumTime));   // dritto (entro ~20 gradi)
                else m.value = std::max(0.0f, m.value - (1.0f - align) * dt * 7.0f);                             // curva: frena
                m.dir = glm::mix(m.dir, d, std::min(1.0f, dt * 5.0f));
            }
            const float mult = 1.0f + stats.momentumBonus * m.value;
            m.gallop += std::max(1.0f, stats.baseSpeed) * mult * dt * 1.9f * (len > 1e-4f ? 1.0f : 0.0f);   // ciclo del galoppo
            if (m.gallop > 6283.0f) m.gallop -= 6283.0f;
            return mult;
        }

        // Nemici: squadre diverse se entrambi ne hanno una, altrimenti l'orda contro chi non e' dell'orda
        inline bool Hostile(entt::registry& reg, entt::entity a, entt::entity b) {
            if (reg.all_of<TeamComponent>(a) && reg.all_of<TeamComponent>(b)) return !SameTeam(reg, a, b);
            return reg.all_of<HordeAgentComponent>(a) != reg.all_of<HordeAgentComponent>(b);
        }

        // Cariche con le zanne: chi ha abbastanza slancio e un nemico a contatto davanti a se'
        inline void UpdateFeralCharges(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            auto view = reg.view<MomentumComponent, TransformComponent, UnitStatsComponent, HealthComponent>(entt::exclude<CorpseComponent, NetGhostComponent>);
            for (auto e : view) {
                auto& m = view.get<MomentumComponent>(e);
                const auto& stats = view.get<UnitStatsComponent>(e);
                m.cooldown = std::max(0.0f, m.cooldown - dt);
                m.sinceGore += dt;
                if (stats.chargeDamage <= 0.0f || m.cooldown > 0.0f || m.value < stats.chargeThreshold) continue;
                if (view.get<HealthComponent>(e).currentHealth <= 0.0f || glm::dot(m.dir, m.dir) < 1e-6f) continue;
                const glm::vec3 pos = view.get<TransformComponent>(e).Position;
                const glm::vec2 fwd = glm::normalize(m.dir);

                entt::entity best = entt::null; float bestDist = kGoreReach;
                auto targets = reg.view<TransformComponent, HealthComponent, UnitStatsComponent>(entt::exclude<CorpseComponent>);
                for (auto o : targets) {
                    if (o == e || !Hostile(reg, e, o)) continue;
                    if (targets.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
                    const glm::vec3 op = targets.get<TransformComponent>(o).Position;
                    if (std::fabs(op.y - pos.y) > 1.4f) continue;
                    const glm::vec2 d(op.x - pos.x, op.z - pos.z);
                    const float dist = glm::length(d);
                    if (dist > bestDist || dist < 1e-4f || glm::dot(d / dist, fwd) < 0.55f) continue;
                    best = o; bestDist = dist;
                }
                if (best == entt::null) continue;

                auto& defHealth = reg.get<HealthComponent>(best);
                auto& defStats = reg.get<UnitStatsComponent>(best);
                auto& defT = reg.get<TransformComponent>(best);
                const float before = defHealth.currentHealth;
                const float raw = stats.chargeDamage * (0.6f + 0.4f * m.value) * (0.7f + stats.strength * stats.moraleCombatMult / 100.0f);
                DamageResolver::ResolveHit(raw, 2, HitboxLocation::Torso, defStats, defHealth);
                const glm::vec3 push(fwd.x, 0.0f, fwd.y);
                // Il colpito barcolla e arretra (gli attacchi pesanti in corso si interrompono)
                if (auto* dm = reg.try_get<MeleeStateComponent>(best)) {
                    dm->currentPhase = MeleePhase::Stunned;
                    dm->currentAttack = MeleeAttackType::None;
                    dm->currentAttackNode = "";
                    dm->stunTimer = kGoreStun;
                    if (reg.all_of<PlayerTag>(best)) {             // giocatore: la spinta usa lo scatto, che si ferma contro i muri
                        dm->lungeTimer = 0.18f;
                        dm->lungeVelocity = push * (kGoreKnockback / 0.18f);
                    }
                }
                if (!reg.all_of<PlayerTag>(best)) defT.Position += push * (kGoreKnockback * 0.5f);   // bot e orda: mezzo passo indietro

                const glm::vec3 point = defT.Position + glm::vec3(0.0f, 0.9f, 0.0f) - push * 0.3f;
                EmitHitFeedback(scene, e, best, point, HitboxLocation::Torso, before);
                VfxEvent v; v.kind = VfxKind::MeleeHit; v.pos = point; v.dir = push; v.variant = 2; v.pos2 = defT.Position;
                EmitVfx(v);
                FeelEvent fe; fe.kind = FeelKind::MeleeHit; fe.pos = point; fe.dir = push; fe.variant = 2;
                fe.source = static_cast<std::uint32_t>(e); fe.target = static_cast<std::uint32_t>(best);
                fe.killed = before > 0.0f && defHealth.currentHealth <= 0.0f;
                EmitFeel(fe);

                m.value = 0.1f;            // l'urto ferma la corsa
                m.cooldown = kGoreCooldown;
                m.sinceGore = 0.0f;
            }
        }
    }

}
