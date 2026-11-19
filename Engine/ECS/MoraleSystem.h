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
//   - vedere cadere un compagno (entro 35 m, in vista): 10 punti x il peso del suo grado rispetto al proprio
//     (2^(grado del caduto - proprio grado), da 0,1 a 4): tre Pighcib che cadono pesano poco su una Vanguard Armor,
//     una Vanguard Armor che cade pesa molto su chi le sta attorno. Lo stesso peso vale per i nemici che si vedono cadere.
//   - soppressione: ogni colpo che passa entro 3 m (o ti colpisce) aggiunge il suppression_value dell'arma; la soppressione
//     cala da sola (20 al secondo + il 25%). Oltre la resistenza dell'unita' (suppression_threshold) si e' soppressi:
//     piu' lenti del suppression_slow dell'arma piu' pesante che ti spara addosso (nerfa le cariche in mischia, come chi
//     sbarcava sotto le mitragliatrici) e il morale cala di 1 + 8 x rallentamento punti al secondo.
//     Esplosioni: +25 di soppressione nel raggio (+12 fino al doppio), rallentamento del 40%, e -10 / -6 di morale.
//   - ferite: conta quanta salute si perde e quanto in fretta. La salute persa negli ultimi ~2,5 s (frazione della
//     massima) sotto il 6% non pesa nulla (un Pighcib che graffia una Vanguard Armor); oltre, ogni ferita toglie
//     30 x frazione persa x gravita', con gravita' da 0 (6%) a 1 (31% in poco tempo) fino a 1,6.
// Cosa lo rialza, fino al valore di partenza:
//   - fuori dal combattimento (5 s senza colpi subiti o sparati e nessun nemico in vista): +1,5 al secondo
//   - curarsi: 15 punti per tutta la salute massima recuperata
//   - vedere cadere un nemico (entro 35 m, in vista): +4
// Aiuti oltre il valore di partenza, fino al tetto: l'insegna del Kingship vicina alza il tetto di morale_bonus x 100 e
//   rialza di 2 punti al secondo.
// Oggetti (armature e dotazione, nei JSON): "morale_cap_bonus" alza il tetto, "morale_start_bonus" il valore di partenza.
//
// In rete ogni computer calcola il morale solo di cio' che simula (il suo giocatore e, se e' l'host, i bot): i colpi
// degli altri arrivano come scie (NetShotQueue) e anche quelli sopprimono. Chi e' in rotta lo dice a tutti (triangolo giallo).
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

    // Colpi che passano vicini (soppressione) ed esplosioni: chi spara/esplode non pesa sulla sua squadra
    struct MoraleShock {
        glm::vec3 a{ 0.0f }, b{ 0.0f };   // traiettoria (o centro dell'esplosione in a)
        entt::entity shooter = entt::null;
        int team = -1;                    // squadra di chi ha sparato, se non c'e' piu' (-1 = nessuna)
        float value = 0.0f;               // soppressione portata (suppression_value dell'arma)
        float slow = 0.0f;                // rallentamento di chi e' soppresso (suppression_slow dell'arma)
        bool explosion = false;
        float radius = 0.0f;
    };
    inline std::vector<MoraleShock>& MoraleShockLog() { static std::vector<MoraleShock> log; return log; }
    inline void EmitMoraleShock(const MoraleShock& s) { if (MoraleShockLog().size() < 4096) MoraleShockLog().push_back(s); }

    namespace Morale {
        inline constexpr float kSuppressReach = 3.0f;      // metri dalla traiettoria
        inline constexpr float kSuppressDecayFlat = 20.0f, kSuppressDecayPct = 0.25f;   // al secondo
        inline constexpr float kSlowHold = 1.5f;           // il rallentamento dell'arma resta per 1,5 s dall'ultimo colpo
        inline constexpr float kSuppressMoraleBase = 1.0f, kSuppressMoraleSlow = 8.0f;  // morale al secondo da soppressi
        inline constexpr float kExplosionSuppress = 25.0f, kExplosionSuppressNear = 12.0f, kExplosionSlow = 0.40f;
        inline constexpr float kAllyDeath = 10.0f;
        inline constexpr float kEnemyDeath = 5.0f;
        inline constexpr float kSeeDeathRange = 35.0f;
        inline constexpr float kExplosionHit = 10.0f, kExplosionNear = 6.0f;
        inline constexpr float kWoundScale = 30.0f;        // per tutta la salute massima persa, a gravita' 1
        inline constexpr float kWoundFree = 0.06f;         // salute persa di recente sotto cui la ferita non pesa
        inline constexpr float kWoundFull = 0.25f;         // ...oltre la soglia: gravita' 1 dopo un altro 25%
        inline constexpr float kWoundMaxSeverity = 1.6f;
        inline constexpr float kWoundMemory = 2.5f;        // secondi (smaltimento della salute persa di recente)
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
        // Peso di una morte vista: grado del caduto rispetto al proprio
        inline float TierWeight(int deadTier, int myTier) { return std::clamp(std::pow(2.0f, static_cast<float>(deadTier - myTier)), 0.1f, 4.0f); }
        // In rotta e ci si sta allontanando dal nemico visto piu' vicino: +30% di velocita'
        inline float FleeSpeedMult(entt::registry& reg, entt::entity e, const glm::vec3& moveDirWorld) {
            const auto* m = reg.try_get<MoraleComponent>(e);
            if (!m || m->zone != MoraleZone::Rout || !m->hasThreat) return 1.0f;
            glm::vec3 away = reg.get<TransformComponent>(e).Position - m->threatPos; away.y = 0.0f;
            glm::vec3 d = moveDirWorld; d.y = 0.0f;
            if (glm::length(away) < 1e-3f || glm::length(d) < 1e-3f) return 1.0f;
            return glm::dot(glm::normalize(away), glm::normalize(d)) > 0.25f ? kRoutFleeSpeed : 1.0f;
        }
        // Velocita' per il morale: soppressione (piu' lenti) e fuga in rotta (piu' veloci allontanandosi)
        inline float MoveMult(entt::registry& reg, entt::entity e, const glm::vec3& moveDirWorld) {
            const auto* m = reg.try_get<MoraleComponent>(e);
            if (!m) return 1.0f;
            return (1.0f - (m->suppressed ? m->slow : 0.0f)) * FleeSpeedMult(reg, e, moveDirWorld);
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
            all.clear();
            for (auto e : reg.view<RoutIndicatorTag>()) all.push_back(e);
            for (auto e : all) reg.remove<RoutIndicatorTag>(e);
            for (auto e : reg.view<UnitStatsComponent>()) reg.get<UnitStatsComponent>(e).moraleCombatMult = 1.0f;
        }

        void Update(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            if (dt <= 0.0f) { MoraleShockLog().clear(); return; }

            // 1) Chi ha il morale: unita' simulate qui (non le copie di rete, non manichini e grugn)
            auto units = reg.view<UnitStatsComponent, HealthComponent, TransformComponent>(entt::exclude<TrainingDummyComponent, GrugnComponent>);
            for (auto e : units) {
                if (reg.all_of<NetGhostComponent>(e)) { if (reg.all_of<MoraleComponent>(e)) reg.remove<MoraleComponent>(e); continue; }
                const auto& st = units.get<UnitStatsComponent>(e);
                const auto& hp = units.get<HealthComponent>(e);
                const bool alive = hp.currentHealth > 0.0f && !reg.all_of<CorpseComponent>(e);
                auto* m = reg.try_get<MoraleComponent>(e);
                if (!m) { if (!alive) continue; m = &reg.emplace<MoraleComponent>(e); Init(reg, e, *m); }
                else if (alive && (m->wasDead || std::fabs(m->base - std::clamp(st.morale, 0.0f, 100.0f)) > 0.01f || m->tier != TierOf(reg, e))) Init(reg, e, *m);   // rientro o altra unita'
                m->wasDead = !alive;
            }

            // 2) Morti viste: compagni (giu') e nemici (su), pesate col grado. Si seguono tutte le unita', anche le copie di rete.
            struct Death { glm::vec3 pos; int team, tier; };
            std::vector<Death> deaths;
            for (auto e : units) {
                const auto& hp = units.get<HealthComponent>(e);
                const bool alive = hp.currentHealth > 0.0f && !reg.all_of<CorpseComponent>(e);
                auto it = m_Tracked.find(e);
                if (alive) { if (it == m_Tracked.end()) m_Tracked[e] = { TeamOf(reg, e), TierOf(reg, e) }; else it->second.team = TeamOf(reg, e); continue; }
                if (it != m_Tracked.end()) { deaths.push_back({ units.get<TransformComponent>(e).Position, it->second.team, it->second.tier }); m_Tracked.erase(it); }
            }
            for (auto it = m_Tracked.begin(); it != m_Tracked.end();) { if (!reg.valid(it->first)) it = m_Tracked.erase(it); else ++it; }

            auto observers = reg.view<MoraleComponent, UnitStatsComponent, HealthComponent, TransformComponent>(entt::exclude<CorpseComponent>);
            for (const Death& dd : deaths) {
                for (auto o : observers) {
                    auto& m = observers.get<MoraleComponent>(o);
                    if (observers.get<HealthComponent>(o).currentHealth <= 0.0f || m.unbreakable || m.amoral) continue;
                    const glm::vec3 op = observers.get<TransformComponent>(o).Position;
                    if (glm::distance(op, dd.pos) > Morale::kSeeDeathRange) continue;
                    if (!HasLineOfSight(scene, Eye(reg, o), dd.pos + glm::vec3(0.0f, 0.9f, 0.0f), o)) continue;
                    const float w = Morale::TierWeight(dd.tier, m.tier);
                    if (dd.team == TeamOf(reg, o)) Lose(m, Morale::kAllyDeath * w, "Compagno caduto");
                    else Gain(m, Morale::kEnemyDeath * w, m.start, "Nemico abbattuto");
                }
            }

            // 3) Soppressione (colpi che passano vicini) ed esplosioni. Anche le unita' incrollabili rallentano:
            //    il morale non cala, ma la soppressione dipende dalla resistenza, non dal morale.
            for (const MoraleShock& s : MoraleShockLog()) {
                for (auto o : observers) {
                    auto& m = observers.get<MoraleComponent>(o);
                    if (observers.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
                    if (o == s.shooter) continue;
                    const int shooterTeam = (s.shooter != entt::null && reg.valid(s.shooter)) ? TeamOf(reg, s.shooter) : s.team;
                    if (TeamOf(reg, o) == shooterTeam) continue;   // la propria squadra non spaventa
                    const glm::vec3 chest = observers.get<TransformComponent>(o).Position + glm::vec3(0.0f, 0.6f, 0.0f);
                    auto addSlow = [&](float sl) { if (sl >= m.pendingSlow || m.slowTimer <= 0.0f) m.pendingSlow = sl; m.slowTimer = Morale::kSlowHold; };
                    if (s.explosion) {
                        const float d = glm::distance(chest, s.a);
                        const float r = std::max(0.5f, s.radius);
                        if (d <= r) { m.suppression += Morale::kExplosionSuppress; addSlow(std::max(s.slow, Morale::kExplosionSlow)); if (!m.unbreakable && !m.amoral) Lose(m, Morale::kExplosionHit, "Esplosione"); }
                        else if (d <= 2.0f * r) { m.suppression += Morale::kExplosionSuppressNear; addSlow(std::max(s.slow, Morale::kExplosionSlow)); if (!m.unbreakable && !m.amoral) Lose(m, Morale::kExplosionNear, "Esplosione vicina"); }
                    }
                    else if (s.value > 0.0f && DistToSegment(chest, s.a, s.b) <= Morale::kSuppressReach) {
                        m.suppression += s.value;
                        addSlow(s.slow);
                    }
                }
            }
            MoraleShockLog().clear();

            // 4) Ferite, cure, soppressione, riposo, insegne; zona ed effetti
            for (auto o : observers) {
                auto& m = observers.get<MoraleComponent>(o);
                auto& st = observers.get<UnitStatsComponent>(o);
                const auto& hp = observers.get<HealthComponent>(o);
                if (hp.currentHealth <= 0.0f) continue;
                m.causeTimer = std::max(0.0f, m.causeTimer - dt);
                m.zoneTimer = std::max(0.0f, m.zoneTimer - dt);
                ScanThreat(scene, o, m, st, dt);
                // Soppressione: cala da sola; oltre la resistenza si e' soppressi
                const float threshold = std::max(1.0f, st.suppressionThreshold);
                m.suppression = std::clamp(m.suppression - (Morale::kSuppressDecayFlat + Morale::kSuppressDecayPct * m.suppression) * dt, 0.0f, threshold * 2.5f);
                m.slowTimer = std::max(0.0f, m.slowTimer - dt);
                if (m.slowTimer <= 0.0f && m.suppression <= threshold) m.pendingSlow = 0.0f;
                m.suppressed = m.suppressed ? m.suppression > threshold * 0.75f : m.suppression > threshold;   // si esce sotto il 75%: niente sfarfallio
                m.slow = m.suppressed ? m.pendingSlow : 0.0f;
                SetRoutTag(reg, o, false);
                if (m.unbreakable || m.amoral) {
                    m.current = m.start; m.zone = MoraleZone::High; st.moraleCombatMult = 1.0f;
                    m.prevHp = hp.currentHealth; m.recentDamage = 0.0f;
                    continue;
                }
                // Ferite: quanto si perde e quanto in fretta
                const float maxHp = std::max(1.0f, hp.maxHealth);
                m.recentDamage *= std::exp(-dt / Morale::kWoundMemory);
                if (m.prevHp >= 0.0f) {
                    const float d = hp.currentHealth - m.prevHp;
                    if (d < -0.01f) {
                        const float frac = (-d) / maxHp;
                        m.recentDamage += frac;
                        const float severity = std::clamp((m.recentDamage - Morale::kWoundFree) / Morale::kWoundFull, 0.0f, Morale::kWoundMaxSeverity);
                        Lose(m, Morale::kWoundScale * frac * severity, "Ferito");
                    }
                    else if (d > 0.01f) Gain(m, Morale::kHealScale * d / maxHp, m.start, "Cure");
                }
                m.prevHp = hp.currentHealth;
                if (m.suppressed) Lose(m, (Morale::kSuppressMoraleBase + Morale::kSuppressMoraleSlow * m.slow) * dt, "Soppresso");
                // Aiuto: insegna vicina (tetto in piu' finche' ci si sta vicini)
                m.auraCap = 0.0f;
                if (const auto* aura = reg.try_get<MoraleAuraComponent>(o)) {
                    m.auraCap = std::max(0.0f, aura->bonus * 100.0f);
                    Gain(m, Morale::kAuraPerSecond * dt, m.cap + m.auraCap, "Insegna");
                }
                // Fuori dal combattimento: piano piano fino al valore di partenza
                if (st.outOfCombatTime >= Morale::kRestDelay && m.suppression <= 0.0f && !m.enemyInSight)
                    Gain(m, Morale::kRestPerSecond * dt, m.start, "Riposo");
                m.current = std::clamp(m.current, 0.0f, m.cap + m.auraCap);
                const MoraleZone z = Morale::ZoneFor(m);
                if (z != m.zone) { m.zone = z; m.zoneTimer = 3.0f; }
                st.moraleCombatMult = Morale::CombatMult(m.zone);
                SetRoutTag(reg, o, m.zone == MoraleZone::Rout && (reg.all_of<BotBrainComponent>(o) || reg.all_of<PlayerTag>(o)));
            }
        }

        // Gradi delle unita' (albero di ricerca): id del blueprint -> grado
        void SetUnitTiers(std::unordered_map<std::string, int> tiers) { m_Tiers = std::move(tiers); }
        int TierOf(entt::registry& reg, entt::entity e) const {
            if (const auto* id = reg.try_get<UnitBlueprintId>(e)) {
                auto it = m_Tiers.find(id->id);
                if (it != m_Tiers.end()) return it->second;
                if (const auto* bp = BlueprintManager::Get().GetUnit(id->id)) {   // fuori dall'albero: dal costo in punti
                    const int pc = bp->point_cost;
                    return pc <= 5 ? 1 : pc <= 12 ? 2 : pc <= 25 ? 3 : pc <= 45 ? 4 : pc <= 70 ? 5 : 6;
                }
            }
            return reg.all_of<HordeAgentComponent>(e) ? 3 : 2;
        }

        // Valori iniziali: statistica, bonus flat degli oggetti indossati e in dotazione
        void Init(entt::registry& reg, entt::entity e, MoraleComponent& m) const {
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
            m.tier = TierOf(reg, e);
            if (const auto* hp = reg.try_get<HealthComponent>(e)) m.prevHp = hp->currentHealth;
        }

    private:
        struct Tracked { int team = 0, tier = 1; };
        std::unordered_map<entt::entity, Tracked> m_Tracked;   // unita' vive -> squadra e grado (per accorgersi di chi cade)
        std::unordered_map<std::string, int> m_Tiers;
        static void SetRoutTag(entt::registry& reg, entt::entity e, bool on) {
            const bool has = reg.all_of<RoutIndicatorTag>(e);
            if (on && !has) reg.emplace<RoutIndicatorTag>(e);
            else if (!on && has) reg.remove<RoutIndicatorTag>(e);
        }

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
                if (const auto* sq = reg.try_get<GrugnComponent>(e); sq) continue;
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
