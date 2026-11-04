#pragma once
// Esperienza di partita (punti studio). I sistemi di gioco registrano qui cosa succede (danni, uccisioni,
// munizioni date, cure, strutture), con l'entita' che ne e' l'autore; GameLayer ogni frame raccoglie gli
// eventi del giocatore, li somma per l'unita' che sta controllando e a fine partita li converte in punti
// studio di quell'unita' (ed energia = meta' dei punti studio).
//
// Valori (scala x1000: un'uccisione vecchia = 1000):
//   sopravvivenza 50 al secondo | munizioni a se' 100, a un compagno 250 | danni a un nemico 250 (una volta
//   per nemico) | uccisione 500 | cura 50 per ogni compagno curato a ogni impulso (non se e' gia' pieno) |
//   costruzione 100 + 1 per ogni punto di danno che la struttura assorbe, al massimo la sua salute massima.
#include <cstdint>
#include <vector>
#include <entt/entt.hpp>

namespace SunEngine {

    enum class XpCategory : int {
        Survival = 0,
        AmmoSelf,
        AmmoAlly,
        Damage,
        Kill,
        Heal,
        Build,
        Absorb,
        Count
    };

    namespace XpValues {
        inline constexpr int64_t SurvivalPerSecond = 50;
        inline constexpr int64_t AmmoSelf = 100;
        inline constexpr int64_t AmmoAlly = 250;
        inline constexpr int64_t Damage = 250;
        inline constexpr int64_t Kill = 500;
        inline constexpr int64_t HealTick = 50;
        inline constexpr int64_t Build = 100;
        inline constexpr float StructureBulletDamage = 0.5f;   // i proiettili fanno meta' danno alle strutture
    }

    struct XpEvent {
        entt::entity source = entt::null;   // chi ha fatto la cosa
        XpCategory category = XpCategory::Survival;
        int64_t xp = 0;
        entt::entity target = entt::null;   // per i danni: il nemico (i danni contano una volta per nemico)
    };

    // Coda unica (static in una funzione inline: una sola istanza in tutto il programma)
    inline std::vector<XpEvent>& XpEventLog() {
        static std::vector<XpEvent> log;
        return log;
    }

    inline void EmitXp(entt::entity source, XpCategory category, int64_t xp, entt::entity target = entt::null) {
        if (source == entt::null || xp <= 0) return;
        XpEventLog().push_back({ source, category, xp, target });
    }

}
