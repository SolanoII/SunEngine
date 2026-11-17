#pragma once
// Morale delle unita'.
//
// Statistica: ogni unita' ha il suo "morale" da 0 a 100 (JSON dell'unita'), come le altre statistiche.
//   100 = incrollabile: niente lo scalfisce.   0 = creatura amorale (verranno piu' avanti; per ora niente la scalfisce).
// Punti: in partita il morale e' un numero di punti. Si parte dal valore di partenza (la statistica + i bonus flat degli
//   oggetti che lo alzano) e i colpi al morale o gli aiuti sono punti flat. La percentuale si calcola sempre rispetto alla
//   statistica dell'unita'. Il tetto (cap) e' la statistica + i bonus flat di equipaggiamento e abilita': non si supera mai.
//
// Zone (percentuale = punti / statistica):
//   ALTO     dal 100% al 60%   statistiche di combattimento invariate
//   MEDIO    dal 59% al 40%    -5% alle statistiche di combattimento
//   BASSO    dal 39% al 20%    -15%
//   IN ROTTA sotto il 20%      -50%; +30% di velocita' se ci si allontana da un nemico (i bot scappano)
//   Statistiche di combattimento: capacita' di tiro, abilita' in mischia, forza (danni e fatica in mischia).
//
// Cosa lo abbassa (punti flat):
//   - vedere cadere un compagno (entro 35 m, in vista): -10 per compagno
//   - posizione soppressa da armi pesanti (suppression_value >= 8: mitragliatrici pesanti, armi a proiettili pesanti
//     rinforzate, lanciarazzi, lanciagranate...): un colpo che passa entro 3 m tiene sotto il fuoco per 0,7 s, e
//     sotto il fuoco si perdono suppression_value x 0,35 punti al secondo (mitragliatrice pesante da 14: ~5 al secondo)
//   - artiglieria ed esplosioni: -10 dentro il raggio, -6 fino al doppio del raggio
//   - essere ferito: 25 punti per tutta la salute massima persa (un colpo da un quinto della salute: -5)
// Cosa lo rialza, fino al valore di partenza:
//   - fuori dal combattimento (5 s senza colpi subiti o sparati e nessun nemico in vista): +1,5 al secondo
//   - curarsi: 15 punti per tutta la salute massima recuperata
//   - vedere cadere un nemico (entro 35 m, in vista): +4
// Aiuti oltre il valore di partenza, fino al tetto: l'insegna imperiale vicina alza il tetto di morale_bonus x 100 e
//   rialza di 2 punti al secondo.
// Oggetti (armature e dotazione, nei JSON): "morale_cap_bonus" alza il tetto, "morale_start_bonus" il valore di partenza.
//
// In rete ogni computer calcola il morale solo di cio' che simula (il suo giocatore e, se e' l'host, i bot): i colpi
// degli altri arrivano come scie (NetShotQueue) e anche quelli tengono sotto il fuoco.
#include "Components.h"
#include "RaycastUtils.h"
#include "../Scene/Scene.h"
#include "../AssetManager/BlueprintManager.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace SunEngine {

    // Colpi che tengono sotto il fuoco (armi pesanti) ed esplosioni: chi spara/esplode non pesa sulla sua squadra
    struct MoraleShock {
        glm::vec3 a{ 0.0f }, b{ 0.0f };   // traiettoria (o centro dell'esplosione in a)
        entt::entity shooter = entt::null;
        int team = -1;                    // squadra di chi ha sparato, se non c'e' piu' (-1 = nessuna)
        float value = 0.0f;               // suppression_value dell'arma
        bool explosion = false;
        float radius = 0.0f;
    };
    inline std::vector<MoraleShock>& MoraleShockLog() { static std::vector<MoraleShock> log; return log; }
    inline void EmitMoraleShock(const MoraleShock& s) { if (MoraleShockLog().size() < 2048) MoraleShockLog().push_back(s); }

    namespace Morale {
        inline constexpr float kHeavyWeapon = 8.0f;        // suppression_value da cui un'arma tiene sotto il fuoco
        inline constexpr float kSuppressReach = 3.0f;      // metri dalla traiettoria
        inline constexpr float kSuppressTime = 0.7f;
        inline constexpr float kSuppressPerValue = 0.35f;  // punti al secondo per punto di suppression_value
        inline constexpr float kAllyDeath = 10.0f;
        inline constexpr float kEnemyDeath = 4.0f;
        inline constexpr float kSeeDeathRange = 35.0f;
        inline constexpr float kExplosionHit = 10.0f, kExplosionNear = 6.0f;
        inline constexpr float kWoundScale = 25.0f;        // per tutta la salute massima persa
        inline constexpr float kHealScale = 15.0f;         // per tutta la salute massima recuperata
        inline constexpr float kRestDelay = 5.0f;          // secondi fuori dal combattimento
        inline constexpr float kRestPerSecond = 1.5f;
        inline constexpr float kAuraPerSecond = 2.0f;
        inline constexpr float kRoutFleeSpeed = 1.30f;

        inline float Percent(const MoraleComponent& m) { return m.base > 0.0f ? m.current / m.base : 1.0f; }
        inline MoraleZone ZoneFor(const MoraleComponent& m) {
            if (m.unbreakable || m.amoral) return MoraleZone::High;
            const float p = Percent(m);
            if (p >= 0.60f) return MoraleZone::High;
            if (p >= 0.40f) return MoraleZone::Medium;
            if (p >= 0.20f) return MoraleZone::Low;
            return MoraleZone::Rout;
        }
        inline float CombatMult(MoraleZone z) {
            switch (z) {
            case MoraleZone::Medium: return 0.95f;
            case MoraleZone::Low: return 0.85f;
            case MoraleZone::Rout: return 0.50f;
            default: return 1.0f;
            }
        }
        inline const char* ZoneName(MoraleZone z) {
            switch (z) {
            case MoraleZone::Medium: return "MEDIO";
            case MoraleZone::Low: return "BASSO";
            case MoraleZone::Rout: return "IN ROTTA";
            default: return "ALTO";
            }
        }
        // In rotta e ci si sta allontanando dal nemico visto piu' vicino: +30% di velocita'
        inline float FleeSpeedMult(entt::registry& reg, entt::entity e, const glm::vec3& moveDirWorld) {
            const auto* m = reg.try_get<MoraleComponent>(e);
            if (!m || m->zone != MoraleZone::Rout || !m->hasThreat) return 1.0f;
            glm::vec3 away = reg.get<TransformComponent>(e).Position - m->threatPos; away.y = 0.0f;
            glm::vec3 d = moveDirWorld; d.y = 0.0f;
            if (glm::length(away) < 1e-3f || glm::length(d) < 1e-3f) return 1.0f;
            return glm::dot(glm::normalize(away), glm::normalize(d)) > 0.25f ? kRoutFleeSpeed : 1.0f;
        }
    }

    class MoraleSystem {
    public:
        // Fine o inizio partita: si riparte dal valore di partenza
        void Clear(Scene& scene) {
            m_Tracked.clear(); MoraleShockLog().clear();
            auto& reg = scene.GetRegistry();
            std::vector<entt::entity> all;
            for (auto e : reg.view<MoraleComponent>()) all.push_back(e);
            for (auto e : all) reg.remove<MoraleComponent>(e);
            for (auto e : reg.view<UnitStatsComponent>()) reg.get<UnitStatsComponent>(e).moraleCombatMult = 1.0f;
        }

        void Update(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            if (dt <= 0.0f) { MoraleShockLog().clear(); return; }

            // 1) Chi ha il morale: unita' simulate qui (non le copie di rete, non manichini e squig)
            auto units = reg.view<UnitStatsComponent, HealthComponent, TransformComponent>(entt::exclude<TrainingDummyComponent, SquigComponent>);
            for (auto e : units) {
                if (reg.all_of<NetGhostComponent>(e)) { if (reg.all_of<MoraleComponent>(e)) reg.remove<MoraleComponent>(e); continue; }
                const auto& st = units.get<UnitStatsComponent>(e);
                const auto& hp = units.get<HealthComponent>(e);
                const bool alive = hp.currentHealth > 0.0f && !reg.all_of<CorpseComponent>(e);
                auto* m = reg.try_get<MoraleComponent>(e);
                if (!m) { if (!alive) continue; m = &reg.emplace<MoraleComponent>(e); Init(reg, e, *m); }
                else if (alive && (m->wasDead || std::fabs(m->base - std::clamp(st.morale, 0.0f, 100.0f)) > 0.01f)) Init(reg, e, *m);   // rientro o altra unita'
                m->wasDead = !alive;
            }

            // 2) Morti viste: compagni (giu') e nemici (su). Si seguono tutte le unita', anche le copie di rete.
            std::vector<std::pair<glm::vec3, int>> deaths;
            for (auto e : units) {
                const auto& hp = units.get<HealthComponent>(e);
                const bool alive = hp.currentHealth > 0.0f && !reg.all_of<CorpseComponent>(e);
                auto it = m_Tracked.find(e);
                if (alive) { m_Tracked[e] = TeamOf(reg, e); continue; }
                if (it != m_Tracked.end()) { deaths.push_back({ units.get<TransformComponent>(e).Position, it->second }); m_Tracked.erase(it); }
            }
            for (auto it = m_Tracked.begin(); it != m_Tracked.end();) { if (!reg.valid(it->first)) it = m_Tracked.erase(it); else ++it; }

            auto observers = reg.view<MoraleComponent, UnitStatsComponent, HealthComponent, TransformComponent>(entt::exclude<CorpseComponent>);
            for (const auto& [pos, team] : deaths) {
                for (auto o : observers) {
                    auto& m = observers.get<MoraleComponent>(o);
                    if (observers.get<HealthComponent>(o).currentHealth <= 0.0f || m.unbreakable || m.amoral) continue;
                    const glm::vec3 op = observers.get<TransformComponent>(o).Position;
                    if (glm::distance(op, pos) > Morale::kSeeDeathRange) continue;
                    if (!HasLineOfSight(scene, Eye(reg, o), pos + glm::vec3(0.0f, 0.9f, 0.0f), o)) continue;
                    if (team == TeamOf(reg, o)) Lose(m, Morale::kAllyDeath, "Compagno caduto");
                    else Gain(m, Morale::kEnemyDeath, m.start, "Nemico abbattuto");
                }
            }

            // 3) Sotto il fuoco di armi pesanti ed esplosioni
            for (const MoraleShock& s : MoraleShockLog()) {
                for (auto o : observers) {
                    auto& m = observers.get<MoraleComponent>(o);
                    if (observers.get<HealthComponent>(o).currentHealth <= 0.0f || m.unbreakable || m.amoral) continue;
                    if (o == s.shooter) continue;
                    const int shooterTeam = (s.shooter != entt::null && reg.valid(s.shooter)) ? TeamOf(reg, s.shooter) : s.team;
                    if (TeamOf(reg, o) == shooterTeam) continue;   // la propria squadra non spaventa
                    const glm::vec3 chest = observers.get<TransformComponent>(o).Position + glm::vec3(0.0f, 0.6f, 0.0f);
                    if (s.explosion) {
                        const float d = glm::distance(chest, s.a);
                        const float r = std::max(0.5f, s.radius);
                        if (d <= r) Lose(m, Morale::kExplosionHit, "Esplosione");
                        else if (d <= 2.0f * r) Lose(m, Morale::kExplosionNear, "Esplosione vicina");
                    }
                    else if (DistToSegment(chest, s.a, s.b) <= Morale::kSuppressReach) {
                        m.suppressTimer = Morale::kSuppressTime;
                        m.suppressRate = std::max(m.suppressRate, s.value * Morale::kSuppressPerValue);
                    }
                }
            }
            MoraleShockLog().clear();

            // 4) Ferite, cure, fuoco, riposo, insegne; zona ed effetti
            for (auto o : observers) {
                auto& m = observers.get<MoraleComponent>(o);
                auto& st = observers.get<UnitStatsComponent>(o);
                const auto& hp = observers.get<HealthComponent>(o);
                if (hp.currentHealth <= 0.0f) continue;
                m.causeTimer = std::max(0.0f, m.causeTimer - dt);
                m.zoneTimer = std::max(0.0f, m.zoneTimer - dt);
                ScanThreat(scene, o, m, st, dt);
                if (m.unbreakable || m.amoral) {
                    m.current = m.start; m.zone = MoraleZone::High; st.moraleCombatMult = 1.0f;
                    m.prevHp = hp.currentHealth; m.suppressTimer = 0.0f;
                    continue;
                }
                const float maxHp = std::max(1.0f, hp.maxHealth);
                if (m.prevHp >= 0.0f) {
                    const float d = hp.currentHealth - m.prevHp;
                    if (d < -0.01f) Lose(m, Morale::kWoundScale * (-d) / maxHp, "Ferito");
                    else if (d > 0.01f) Gain(m, Morale::kHealScale * d / maxHp, m.start, "Cure");
                }
                m.prevHp = hp.currentHealth;
                if (m.suppressTimer > 0.0f) {
                    m.suppressTimer -= dt;
                    Lose(m, m.suppressRate * dt, "Sotto il fuoco pesante");
                    if (m.suppressTimer <= 0.0f) m.suppressRate = 0.0f;
                }
                // Aiuto: insegna vicina (tetto in piu' finche' ci si sta vicini)
                m.auraCap = 0.0f;
                if (const auto* aura = reg.try_get<MoraleAuraComponent>(o)) {
                    m.auraCap = std::max(0.0f, aura->bonus * 100.0f);
                    Gain(m, Morale::kAuraPerSecond * dt, m.cap + m.auraCap, "Insegna");
                }
                // Fuori dal combattimento: piano piano fino al valore di partenza
                if (st.outOfCombatTime >= Morale::kRestDelay && m.suppressTimer <= 0.0f && !m.enemyInSight)
                    Gain(m, Morale::kRestPerSecond * dt, m.start, "Riposo");
                m.current = std::clamp(m.current, 0.0f, m.cap + m.auraCap);
                const MoraleZone z = Morale::ZoneFor(m);
                if (z != m.zone) { m.zone = z; m.zoneTimer = 3.0f; }
                st.moraleCombatMult = Morale::CombatMult(m.zone);
            }
        }

        // Valori iniziali: statistica, bonus flat degli oggetti indossati e in dotazione
        static void Init(entt::registry& reg, entt::entity e, MoraleComponent& m) {
            const auto& st = reg.get<UnitStatsComponent>(e);
            float capBonus = 0.0f, startBonus = 0.0f;
            const auto& bm = BlueprintManager::Get();
            const ArmorSet& as = st.armorSet;
            for (const ArmorPiece* p : { &as.head, &as.torso, &as.leftArm, &as.rightArm, &as.leftLeg, &as.rightLeg }) {
                if (p->id.empty() || p->id == "none") continue;
                if (const auto* a = bm.GetArmorPiece(p->id)) { capBonus += a->morale_cap_bonus; startBonus += a->morale_start_bonus; }
            }
            if (const auto* inv = reg.try_get<InventoryComponent>(e)) {
                for (int s = 0; s <= 6; ++s) {
                    const std::string id = inv->GetSlotItemId(s);
                    if (id.empty()) continue;
                    if (const auto* w = bm.GetWeapon(id)) { capBonus += w->morale_cap_bonus; startBonus += w->morale_start_bonus; }
                }
            }
            m = MoraleComponent{};
            m.base = std::clamp(st.morale, 0.0f, 100.0f);
            m.unbreakable = m.base >= 100.0f;
            m.amoral = m.base <= 0.0f;
            m.cap = m.base + std::max(0.0f, capBonus);
            m.start = std::min(m.cap, m.base + std::max(0.0f, startBonus));
            m.current = m.start;
            if (const auto* hp = reg.try_get<HealthComponent>(e)) m.prevHp = hp->currentHealth;
        }

    private:
        std::unordered_map<entt::entity, int> m_Tracked;   // unita' vive -> squadra (per accorgersi di chi cade)

    public:
        // Squadra per il morale: quella dell'unita'; l'orda fa squadra a se'; chi non ne ha sta con il giocatore
        static int TeamOf(entt::registry& reg, entt::entity e) {
            if (const auto* t = reg.try_get<TeamComponent>(e)) return t->team;
            if (reg.all_of<HordeAgentComponent>(e)) return -2;
            return 0;
        }
    private:
        static glm::vec3 Eye(entt::registry& reg, entt::entity e) {
            const auto& t = reg.get<TransformComponent>(e);
            float h = 1.8f, foot = t.Position.y;
            if (const auto* c = reg.try_get<ColliderComponent>(e)) {
                h = std::max(0.6f, c->halfExtents.y * 2.0f * t.Scale.y);
                foot = t.Position.y + (c->localCenterOffset.y - c->halfExtents.y) * t.Scale.y;
            }
            return glm::vec3(t.Position.x, foot + h * 0.88f, t.Position.z);
        }
        static float DistToSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
            const glm::vec3 ab = b - a;
            const float l2 = glm::dot(ab, ab);
            const float t = l2 > 1e-6f ? std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
            return glm::distance(p, a + ab * t);
        }
        static void Lose(MoraleComponent& m, float pts, const char* why) {
            if (pts <= 0.0f) return;
            m.current = std::max(0.0f, m.current - pts);
            Note(m, -pts, why);
        }
        // Rialza fino a 'limit' (valore di partenza; il tetto per gli aiuti)
        static void Gain(MoraleComponent& m, float pts, float limit, const char* why) {
            if (pts <= 0.0f || m.current >= limit) return;
            const float before = m.current;
            m.current = std::min(limit, m.current + pts);
            Note(m, m.current - before, why);
        }
        static void Note(MoraleComponent& m, float delta, const char* why) {
            if (m.causeTimer > 0.0f && m.lastCause == why) m.lastDelta += delta;
            else { m.lastCause = why; m.lastDelta = delta; }
            m.causeTimer = 2.5f;
        }
        // Nemico vivo piu' vicino in vista (ogni 0,3 s): per sapere se si e' in combattimento e da dove scappare
        void ScanThreat(Scene& scene, entt::entity o, MoraleComponent& m, const UnitStatsComponent& st, float dt) {
            m.scanTimer -= dt;
            if (m.scanTimer > 0.0f) return;
            m.scanTimer = 0.3f;
            auto& reg = scene.GetRegistry();
            const int myTeam = TeamOf(reg, o);
            const glm::vec3 me = reg.get<TransformComponent>(o).Position;
            const float range = std::clamp(st.visionRange, 20.0f, 120.0f);
            entt::entity best = entt::null; float bestD = range;
            auto view = reg.view<UnitStatsComponent, HealthComponent, TransformComponent>(entt::exclude<CorpseComponent, TrainingDummyComponent>);
            for (auto e : view) {
                if (e == o || view.get<HealthComponent>(e).currentHealth <= 0.0f) continue;
                const int t = TeamOf(reg, e);
                if (t == myTeam) continue;
                if (const auto* sq = reg.try_get<SquigComponent>(e); sq) continue;
                const float d = glm::distance(me, view.get<TransformComponent>(e).Position);
                if (d < bestD) { bestD = d; best = e; }
            }
            m.enemyInSight = false;
            if (best == entt::null) { m.hasThreat = false; return; }
            const glm::vec3 bp = reg.get<TransformComponent>(best).Position;
            if (HasLineOfSight(scene, Eye(reg, o), bp + glm::vec3(0.0f, 1.0f, 0.0f), o)) {
                m.enemyInSight = bestD < 30.0f;
                m.hasThreat = true;
                m.threatPos = bp;
            }
            else if (bestD > 12.0f) m.hasThreat = false;   // non lo vede: smette di scappare da li' (vicinissimo lo sente)
        }
    };
}
