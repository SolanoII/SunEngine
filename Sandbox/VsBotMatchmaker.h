#pragma once
// Modalita' Vs Bot (5 contro 5): code a punteggio e generazione delle squadre.
// Nelle code conta solo il costo delle unita' ("point_cost" dei blueprint), come il tier dei carri in WoT:
// l'equipaggiamento non entra nel punteggio di battaglia.
#include <algorithm>
#include <random>
#include <string>
#include <vector>

namespace SunEngine {

    struct VsBotQueueTier {
        const char* name;
        int minExclusive;   // la squadra deve valere PIU' di questo...
        int cap;            // ...e AL MASSIMO questo
    };

    inline constexpr int kVsBotTeamSize = 5;   // giocatori per parte (5 contro 5); le squadre non hanno limite di unita'
    inline constexpr VsBotQueueTier kVsBotTiers[3] = {
        { "CODA I",   0,  20 },
        { "CODA II",  20, 100 },
        { "CODA III", 100, 300 },
    };

    struct VsBotCandidate {
        std::string unitId;
        int cost = 0;
        std::string faction;   // "imperium", "spacemarines", "orks"
        int maxCount = 5;      // massimo di questo tipo in una squadra ("max_per_squad")
    };

    inline std::string VsBotFactionOf(const std::string& unitId) {
        const auto us = unitId.find('_');
        return us == std::string::npos ? unitId : unitId.substr(0, us);
    }

    // Idoneita' della squadra del giocatore a una coda: almeno un'unita' e punteggio nell'intervallo
    // (nessun limite al numero di unita': lo frenano il cap e il massimo per tipo).
    inline bool VsBotEligible(int squadPoints, int squadSize, const VsBotQueueTier& tier) {
        return squadSize >= 1 && squadPoints > tier.minExclusive && squadPoints <= tier.cap;
    }

    // Sceglie 'slots' unita' da 'pool' (ripetizioni ammesse) con costo totale <= budget e il piu' vicino possibile
    // al budget, a caso fra le soluzioni migliori. Ritorna gli indici nel pool; vuoto se non ci stanno 'slots'
    // unita' nemmeno prendendo le piu' economiche.
    // preferredFaction: a parita' di punteggio vince la soluzione con piu' unita' di quella fazione.
    inline std::vector<int> VsBotFillTeam(const std::vector<VsBotCandidate>& pool, int slots, int budget,
                                          std::mt19937& rng, int tries = 4000, const std::string& preferredFaction = "") {
        std::vector<int> best;
        if (slots <= 0) return best;
        if (pool.empty()) return best;
        int minCost = pool[0].cost;
        for (const auto& c : pool) minCost = std::min(minCost, c.cost);
        if (minCost * slots > budget) return best;

        int bestScore = -1;
        const int perfect = budget * 16 + (preferredFaction.empty() ? 0 : slots);
        std::vector<int> allowed;
        std::vector<int> pick;
        for (int t = 0; t < tries && bestScore < perfect; ++t) {
            pick.clear();
            int remaining = budget;
            for (int k = 0; k < slots; ++k) {
                const int reserve = minCost * (slots - k - 1);   // lascia spazio per i posti che mancano
                allowed.clear();
                for (int i = 0; i < static_cast<int>(pool.size()); ++i)
                    if (pool[i].cost <= remaining - reserve) allowed.push_back(i);
                if (allowed.empty()) break;
                // L'ultimo posto prova a riempire il budget: preferisce l'unita' piu' cara che ci sta
                int chosen;
                if (k == slots - 1 && (rng() % 3) != 0) {
                    chosen = *std::max_element(allowed.begin(), allowed.end(),
                        [&](int a, int b) { return pool[a].cost < pool[b].cost; });
                }
                else {
                    chosen = allowed[rng() % allowed.size()];
                }
                pick.push_back(chosen);
                remaining -= pool[chosen].cost;
            }
            if (static_cast<int>(pick.size()) != slots) continue;
            const int sum = budget - remaining;
            int own = 0;
            if (!preferredFaction.empty()) for (int i : pick) own += (pool[i].faction == preferredFaction) ? 1 : 0;
            const int score = sum * 16 + own;   // prima il punteggio, poi la fazione preferita
            if (score > bestScore) { bestScore = score; best = pick; }
        }
        return best;
    }

    // Squadra avversaria: 5 unita' di una fazione a caso (se possibile) che si avvicinano al cap senza sforarlo
    // e superano il minimo della coda. Se nessuna fazione da sola ci riesce, mescola le fazioni.
    inline std::vector<std::string> VsBotBuildEnemyTeam(const std::vector<VsBotCandidate>& all,
                                                        const VsBotQueueTier& tier, std::mt19937& rng) {
        std::vector<std::string> factions;
        for (const auto& c : all) if (std::find(factions.begin(), factions.end(), c.faction) == factions.end()) factions.push_back(c.faction);
        std::shuffle(factions.begin(), factions.end(), rng);

        auto sumOf = [](const std::vector<VsBotCandidate>& pool, const std::vector<int>& idx) {
            int s = 0; for (int i : idx) s += pool[i].cost; return s;
        };
        for (const auto& f : factions) {
            std::vector<VsBotCandidate> pool;
            for (const auto& c : all) if (c.faction == f) pool.push_back(c);
            const auto idx = VsBotFillTeam(pool, kVsBotTeamSize, tier.cap, rng);
            if (!idx.empty() && sumOf(pool, idx) > tier.minExclusive && sumOf(pool, idx) * 4 >= tier.cap * 3) {   // almeno il 75% del cap
                std::vector<std::string> out;
                for (int i : idx) out.push_back(pool[i].unitId);
                return out;
            }
        }
        const auto idx = VsBotFillTeam(all, kVsBotTeamSize, tier.cap, rng);
        std::vector<std::string> out;
        for (int i : idx) out.push_back(all[i].unitId);
        return out;
    }

    // Alleati che completano la squadra del giocatore fino a 5: stessa fazione del capo squadra se possibile,
    // budget = cap - punti della squadra. Se il budget non basta per tutti i posti, mette quelli che ci stanno.
    inline std::vector<std::string> VsBotBuildAllies(const std::vector<VsBotCandidate>& all, const std::string& faction,
                                                     int squadPoints, int squadSize, const VsBotQueueTier& tier,
                                                     std::mt19937& rng) {
        const int budget = tier.cap - squadPoints;
        std::vector<VsBotCandidate> own;
        for (const auto& c : all) if (c.faction == faction) own.push_back(c);
        for (int slots = kVsBotTeamSize - squadSize; slots > 0; --slots) {
            // 1) solo la fazione del capo squadra, se arriva almeno all'85% del budget
            const auto ownIdx = VsBotFillTeam(own, slots, budget, rng, 4000);
            int ownSum = 0; for (int i : ownIdx) ownSum += own[i].cost;
            if (!ownIdx.empty() && ownSum * 100 >= budget * 85) {
                std::vector<std::string> out;
                for (int i : ownIdx) out.push_back(own[i].unitId);
                return out;
            }
            // 2) fazioni miste, preferendo comunque quella del capo squadra
            const auto idx = VsBotFillTeam(all, slots, budget, rng, 6000, faction);
            if (!idx.empty()) {
                std::vector<std::string> out;
                for (int i : idx) out.push_back(all[i].unitId);
                return out;
            }
        }
        return {};
    }

    // Squadra di un giocatore IA: quante piu' unita' possibile con somma <= cap della coda (e sopra il minimo),
    // senza superare il massimo per tipo ("max_per_squad" del blueprint, es. 5 gretchin, 4 boyz). Non c'e' un
    // limite al numero di unita': ogni unita' e' un rientro in partita, quindi gli eserciti d'orda (tante unita'
    // economiche) hanno piu' rientri di chi porta poche unita' d'elite.
    // Riempimento casuale ripetuto: ogni tentativo aggiunge unita' a caso finche' ne entra almeno una; vince il
    // totale piu' vicino al cap, a parita' quello con piu' unita'. Prima solo la fazione indicata; se
    // strictFaction e' falso e la fazione da sola non arriva sopra il minimo, mescola le fazioni.
    inline std::vector<std::string> VsBotBuildSquad(const std::vector<VsBotCandidate>& all, const std::string& faction,
                                                    const VsBotQueueTier& tier, std::mt19937& rng, bool strictFaction,
                                                    int tries = 400) {
        std::vector<VsBotCandidate> own;
        for (const auto& c : all) if (c.faction == faction) own.push_back(c);
        auto attempt = [&](const std::vector<VsBotCandidate>& pool) -> std::vector<std::string> {
            std::vector<int> best;
            int bestSum = -1;
            std::vector<int> pick, count(pool.size()), allowed;
            for (int t = 0; t < tries; ++t) {
                pick.clear();
                std::fill(count.begin(), count.end(), 0);
                int remaining = tier.cap;
                for (;;) {
                    allowed.clear();
                    for (int i = 0; i < static_cast<int>(pool.size()); ++i)
                        if (pool[i].cost > 0 && pool[i].cost <= remaining && count[i] < std::max(1, pool[i].maxCount)) allowed.push_back(i);
                    if (allowed.empty()) break;
                    const int c = allowed[rng() % allowed.size()];
                    pick.push_back(c); ++count[c]; remaining -= pool[c].cost;
                }
                const int sum = tier.cap - remaining;
                if (sum <= tier.minExclusive) continue;
                if (sum > bestSum || (sum == bestSum && pick.size() > best.size())) { bestSum = sum; best = pick; }
            }
            std::vector<std::string> out;
            for (int i : best) out.push_back(pool[i].unitId);
            return out;
        };
        auto out = attempt(own);
        if (out.empty() && !strictFaction) out = attempt(all);
        return out;
    }

    // Nomi dei giocatori IA, per fazione (senza ripetizioni nella stessa partita).
    inline std::string VsBotRandomName(const std::string& faction, std::vector<std::string>& used, std::mt19937& rng) {
        static const std::vector<std::string> orks = { "Zogrot", "Krugdakka", "Urgnash", "Skabblog", "Morgrim", "Drakka",
            "Nazgrub", "Grimtoof", "Uzbad", "Wazzgob", "Snagrik", "Bogrukk" };
        static const std::vector<std::string> imperium = { "Sgt. Varro", "Cpl. Hessin", "Kael", "Ostrava", "Brandt", "Mirelle",
            "Taveck", "Ilsen", "Corvin", "Lt. Drusa", "Okonkwo", "Reiss" };
        static const std::vector<std::string> marines = { "Fratello Aurex", "Fratello Taddeus", "Fratello Lucan", "Fratello Varn",
            "Fratello Castor", "Fratello Severin", "Fratello Ignus", "Fratello Halvar", "Fratello Remus", "Fratello Cassian" };
        const auto& list = faction == "orks" ? orks : (faction == "spacemarines" ? marines : imperium);
        std::vector<std::string> free;
        for (const auto& n : list) if (std::find(used.begin(), used.end(), n) == used.end()) free.push_back(n);
        std::string name = free.empty() ? list[rng() % list.size()] + " " + std::to_string(used.size() + 1) : free[rng() % free.size()];
        used.push_back(name);
        return name;
    }

}
