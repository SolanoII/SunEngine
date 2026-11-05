#pragma once

#include <algorithm>
#include "../Scene/Scene.h"
#include "Components.h"
#include "Tags.h"

namespace SunEngine {
    // Rigenerazione passiva della vita fuori dal combattimento.
    // Dopo 7 secondi senza subire danni, senza sparare e senza colpi corpo a corpo, la vita torna da sola in
    // proporzione alla costituzione (toughness): 100 = 4 hp/s, 50 = 2 hp/s, 25 = 1 hp/s (toughness / 25).
    // Valgono giocatore, bot e orda; esclusi manichini e cadaveri.
    class HealthRegenSystem {
    public:
        static constexpr float kOutOfCombatDelay = 7.0f;

        static float RegenPerSecond(const UnitStatsComponent& s) { return std::max(0.0f, s.toughness) / 25.0f; }
        // Quanto manca alla rigenerazione (0 = sta rigenerando)
        static float TimeToRegen(const UnitStatsComponent& s) { return std::max(0.0f, kOutOfCombatDelay - s.outOfCombatTime); }

        void Update(Scene& scene, float deltaTime) {
            if (deltaTime <= 0.0f) return;
            auto& reg = scene.GetRegistry();
            auto view = reg.view<UnitStatsComponent, HealthComponent>(entt::exclude<CorpseComponent, TrainingDummyComponent>);
            for (auto e : view) {
                auto& st = view.get<UnitStatsComponent>(e);
                auto& hp = view.get<HealthComponent>(e);
                if (hp.currentHealth <= 0.0f) { st.outOfCombatTime = 0.0f; st.lastHealthSeen = hp.currentHealth; continue; }

                bool inCombat = false;
                if (st.lastHealthSeen >= 0.0f && hp.currentHealth < st.lastHealthSeen - 0.01f) inCombat = true;   // colpito
                if (const auto* ws = reg.try_get<WeaponStateComponent>(e)) {
                    if (st.lastAmmoSeen >= 0 && ws->currentAmmo < st.lastAmmoSeen) inCombat = true;            // ha sparato
                    st.lastAmmoSeen = ws->currentAmmo;
                }
                if (const auto* m = reg.try_get<MeleeStateComponent>(e)) {
                    if (m->currentPhase == MeleePhase::Windup || m->currentPhase == MeleePhase::Active || m->currentPhase == MeleePhase::Recovery)
                        inCombat = true;                                                                      // corpo a corpo
                }
                st.outOfCombatTime = inCombat ? 0.0f : st.outOfCombatTime + deltaTime;

                if (st.outOfCombatTime >= kOutOfCombatDelay && hp.currentHealth < hp.maxHealth)
                    hp.currentHealth = std::min(hp.maxHealth, hp.currentHealth + RegenPerSecond(st) * deltaTime);
                st.lastHealthSeen = hp.currentHealth;
            }
        }
    };
}
