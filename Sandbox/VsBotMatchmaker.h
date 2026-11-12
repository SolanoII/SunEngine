#pragma once
// Modalita' Vs Bot (5 contro 5): code a punteggio e generazione delle squadre.
// Nelle code conta solo il costo delle unita' ("point_cost" dei blueprint), come il tier dei carri in WoT:
// l'equipaggiamento non entra nel punteggio di battaglia.
#include <algorithm>
#include <cmath>
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

    // Squadra mista di un giocatore IA: ogni bot pesca uno "stile" diverso, cosi' nella stessa parte convivono schemi
    // diversi e le fazioni si mescolano (es. un gretchin e un aspirant; due gretchin e una guardia; un "grot spammer"
    // con quattro gretchin). Sempre entro il cap della coda e il massimo per tipo; quel che avanza del cap va
    // all'equipaggiamento (AddBotExtras).
    //   0 misto       tutte le fazioni, scelta uniforme
    //   1 orda        tante unita' economiche (peso 1/costo)
    //   2 elite       poche unita' care (peso costo^1.5)
    //   3 due fazioni due fazioni a caso
    //   4 una fazione una fazione a caso
    //   5 spammer     un tipo economico ripetuto fino al massimo, il resto a caso
    inline const char* VsBotStyleName(int style) {
        static const char* names[] = { "misto", "orda", "elite", "due fazioni", "una fazione", "spammer" };
        return names[(style >= 0 && style < 6) ? style : 0];
    }
    inline std::vector<std::string> VsBotBuildMixedSquad(const std::vector<VsBotCandidate>& all, const VsBotQueueTier& tier,
                                                         std::mt19937& rng, int* styleOut = nullptr) {
        std::vector<std::string> factions;
        for (const auto& c : all) if (std::find(factions.begin(), factions.end(), c.faction) == factions.end()) factions.push_back(c.faction);
        std::shuffle(factions.begin(), factions.end(), rng);
        auto pickUnits = [&](int style) -> std::vector<std::string> {
            std::vector<VsBotCandidate> pool;
            std::string spam;   // spammer: il tipo ripetuto
            for (const auto& c : all) {
                if (c.cost <= 0 || c.cost > tier.cap) continue;
                if (style == 3 && factions.size() >= 2 && c.faction != factions[0] && c.faction != factions[1]) continue;
                if (style == 4 && !factions.empty() && c.faction != factions[0]) continue;
                pool.push_back(c);
            }
            if (pool.empty()) return {};
            if (style == 5) {   // il piu' economico fra alcuni a caso, che ci stia almeno due volte
                std::vector<const VsBotCandidate*> cheap;
                for (const auto& c : pool) if (c.cost * 2 <= tier.cap) cheap.push_back(&c);
                if (cheap.empty()) return {};
                std::shuffle(cheap.begin(), cheap.end(), rng);
                const VsBotCandidate* best = cheap[0];
                for (size_t i = 1; i < cheap.size() && i < 3; ++i) if (cheap[i]->cost < best->cost) best = cheap[i];
                spam = best->unitId;
            }
            const double expo = style == 1 ? -1.0 : (style == 2 ? 1.5 : 0.0);
            std::vector<std::vector<std::string>> good;
            std::vector<std::string> fallback; int fallbackSum = -1;
            std::vector<int> count(pool.size());
            for (int t = 0; t < 40; ++t) {
                std::fill(count.begin(), count.end(), 0);
                std::vector<std::string> pick;
                int remaining = tier.cap;
                if (!spam.empty()) {
                    for (size_t i = 0; i < pool.size(); ++i) if (pool[i].unitId == spam) {
                        const int n = std::min(std::max(1, pool[i].maxCount), 2 + static_cast<int>(rng() % 4));
                        for (int k = 0; k < n && pool[i].cost <= remaining; ++k) { pick.push_back(spam); ++count[i]; remaining -= pool[i].cost; }
                    }
                }
                for (;;) {
                    std::vector<double> w(pool.size(), 0.0);
                    double tot = 0.0;
                    for (size_t i = 0; i < pool.size(); ++i) {
                        if (pool[i].cost > remaining || count[i] >= std::max(1, pool[i].maxCount)) continue;
                        w[i] = std::pow(static_cast<double>(pool[i].cost), expo);
                        tot += w[i];
                    }
                    if (tot <= 0.0) break;
                    // a volte un bot si ferma prima (squadre piccole ma con piu' equipaggiamento)
                    if (!pick.empty() && (rng() % 100) < 12) break;
                    double r = std::uniform_real_distribution<double>(0.0, tot)(rng);
                    size_t c = 0;
                    for (; c + 1 < pool.size(); ++c) { if (w[c] > 0.0 && r < w[c]) break; r -= w[c]; }
                    while (w[c] <= 0.0 && c > 0) --c;
                    pick.push_back(pool[c].unitId); ++count[c]; remaining -= pool[c].cost;
                }
                const int sum = tier.cap - remaining;
                if (pick.empty() || sum <= tier.minExclusive) continue;
                if (sum * 10 >= tier.cap * 6) good.push_back(pick);   // almeno il 60% del cap in unita'
                if (sum > fallbackSum) { fallbackSum = sum; fallback = pick; }
            }
            if (!good.empty()) return good[rng() % good.size()];
            return fallback;
        };
        int style = static_cast<int>(rng() % 6);
        auto out = pickUnits(style);
        if (out.empty()) { style = 0; out = pickUnits(0); }
        if (styleOut) *styleOut = style;
        return out;
    }

    // Nomi dei giocatori IA, per fazione (senza ripetizioni nella stessa partita).
    inline std::string VsBotRandomName(const std::string& faction, std::vector<std::string>& used, std::mt19937& rng) {
        static const std::vector<std::string> orks = { "Grunf", "Zannor", "Brakk", "Murgan", "Ruggzanna", "Krungh",
            "Vrokk", "Mazzuco", "Dorgun", "Skarn", "Bogrim", "Tusk" };
        static const std::vector<std::string> imperium = { "Sgt. Varro", "Cpl. Hessin", "Kael", "Ostrava", "Brandt", "Mirelle",
            "Taveck", "Ilsen", "Corvin", "Lt. Drusa", "Okonkwo", "Reiss" };
        static const std::vector<std::string> marines = { "Aurelian", "Cassar", "Valerius", "Severan", "Marcell",
            "Dorian", "Lucent", "Hadrian", "Corvin", "Octavian" };
        const auto& list = faction == "orks" ? orks : (faction == "spacemarines" ? marines : imperium);
        std::vector<std::string> free;
        for (const auto& n : list) if (std::find(used.begin(), used.end(), n) == used.end()) free.push_back(n);
        std::string name = free.empty() ? list[rng() % list.size()] + " " + std::to_string(used.size() + 1) : free[rng() % free.size()];
        used.push_back(name);
        return name;
    }

}
