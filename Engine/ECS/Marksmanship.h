#pragma once

#include <algorithm>
#include <cmath>
#include "Components.h"

namespace SunEngine {
    // Capacita' di tiro (accuracy_skill, 0-100) applicata ai colpi.
    // I colpi non vanno piu' a caso: escono dove punta il mirino (piu' il rinculo gia' accumulato). La capacita'
    // di tiro decide invece:
    //  - quanto calcia l'arma a ogni colpo (100 = quasi niente, 30 = molto) e su quale arco verso l'alto si
    //    sparpaglia la raffica (100 = dritto in su, 30 = ventaglio di 60 gradi, raggio scelto a caso);
    //  - quanto spesso si prende il nemico "di striscio" (danno ridotto);
    //  - quanto ci vuole a "preparare" il tiro: fermi (meglio se accovacciati e mirando) la mira si assesta e,
    //    assestata, il colpo va esattamente sul mirino. Chi spara dal fianco o muovendosi ha ancora un po' di
    //    deviazione casuale, che sparisce man mano che la mira si assesta.
    namespace Marksmanship {
        inline float Skill01(const UnitStatsComponent& s) { return std::clamp(s.accuracySkill / 100.0f, 0.0f, 1.0f); }

        // Moltiplicatore del rinculo dell'arma: 100 -> 0,03 (un pixel), 75 -> ~0,6, 50 -> ~1,3, 30 -> ~1,9
        inline float KickMult(float s) { return 0.03f + 2.9f * std::pow(1.0f - s, 1.2f); }

        // Ampiezza dell'arco (gradi) entro cui ogni colpo spinge la mira verso l'alto: 30 -> 60, 100 -> 0
        inline float SprayArcDeg(float s) { return std::clamp(60.0f * (1.0f - s) / 0.7f, 0.0f, 80.0f); }

        // Secondi per assestare la mira da zero (fermi): 100 -> 0,25 s, 30 -> ~0,9 s, 0 -> 1,2 s
        inline float SettleTime(float s) { return 1.2f + (0.25f - 1.2f) * s; }

        // Probabilita' di colpire di striscio: 30 -> ~37% (mira non assestata) / ~19% (assestata), 75 -> 12% / 6%, 100 -> 0
        inline float GrazeChance(float s, float steady) { return 0.55f * std::pow(1.0f - s, 1.1f) * (1.0f - 0.5f * steady); }
        inline constexpr float kGrazeDamage = 0.35f;   // di striscio passa solo il 35% del danno

        // Deviazione casuale (radianti) di un colpo con la mira non assestata: si annulla a mira assestata
        inline float UnsteadyCone(float s, float weaponSpread, float steady) {
            return weaponSpread * (0.5f + (1.0f - s)) * (1.0f - steady);
        }

        inline constexpr float kSteadyLossPerShot = 0.12f;   // ogni colpo scompone un po' la posizione
        inline constexpr float kMaxRecoilPitch = 30.0f;      // la raffica non sale oltre (gradi)
    }
}
