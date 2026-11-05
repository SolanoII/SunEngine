#include "BotAISystem.h"
#include "DamageResolver.h"
#include "RaycastUtils.h"
#include "AmmoSystem.h"
#include "../Core/MathUtils.h"
#include "../Core/Random.h"
#include "../AssetManager/BlueprintManager.h"
#include <algorithm>
#include <cmath>

namespace SunEngine {

    namespace {

        constexpr float kThinkMin = 0.22f;        // intervallo fra due decisioni (s), con un po' di casualita'
        constexpr float kThinkMax = 0.38f;
        constexpr float kMemorySeconds = 6.0f;    // per quanto ricorda dove ha visto il bersaglio
        constexpr float kCloseAwareness = 9.0f;   // entro questa distanza si accorge di un nemico anche alle spalle

        bool Alive(entt::registry& reg, entt::entity e) {
            if (e == entt::null || !reg.valid(e) || reg.all_of<CorpseComponent>(e)) return false;
            const auto* h = reg.try_get<HealthComponent>(e);
            return h && h->currentHealth > 0.0f;
        }

        // Piedi e altezza dal collider (Position non e' sempre ai piedi: vedi lo scheletro del guardsman)
        void FootAndHeight(entt::registry& reg, entt::entity e, float& footY, float& height) {
            const auto& t = reg.get<TransformComponent>(e);
            footY = t.Position.y; height = 1.8f;
            if (const auto* c = reg.try_get<ColliderComponent>(e)) {
                height = std::max(0.6f, c->halfExtents.y * 2.0f * t.Scale.y);
                footY = t.Position.y + (c->localCenterOffset.y - c->halfExtents.y) * t.Scale.y;
            }
        }
        glm::vec3 PointAt(entt::registry& reg, entt::entity e, float frac) {
            float footY, h; FootAndHeight(reg, e, footY, h);
            const auto& p = reg.get<TransformComponent>(e).Position;
            return glm::vec3(p.x, footY + h * frac, p.z);
        }
        glm::vec3 Chest(entt::registry& reg, entt::entity e) { return PointAt(reg, e, 0.62f); }
        glm::vec3 Eye(entt::registry& reg, entt::entity e) { return PointAt(reg, e, 0.88f); }

        float HorizontalDist(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); }
        glm::vec3 YawForward(float yawDeg) { const float y = glm::radians(yawDeg); return glm::vec3(-std::sin(y), 0.0f, -std::cos(y)); }
        float YawTowards(const glm::vec3& from, const glm::vec3& to) {
            const glm::vec3 d = to - from;
            return glm::degrees(std::atan2(-d.x, -d.z));
        }
        float AngleDiff(float a, float b) {
            float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
            return std::abs(d);
        }

        int TeamOf(entt::registry& reg, entt::entity e) {
            const auto* t = reg.try_get<TeamComponent>(e);
            return t ? t->team : -1;
        }

        BotRole RoleOf(entt::registry& reg, entt::entity e) {
            if (const auto* b = reg.try_get<BotBrainComponent>(e)) return b->role;
            if (const auto* uid = reg.try_get<UnitBlueprintId>(e)) {
                if (const auto* bp = BlueprintManager::Get().GetUnit(uid->id)) {
                    if (bp->ai_role == "ranged") return BotRole::Ranged;
                    if (bp->ai_role == "melee") return BotRole::Melee;
                }
            }
            return BotRole::Versatile;
        }

        // Resistenza effettiva: salute moltiplicata per la riduzione della robustezza (stessa formula del danno)
        float EffectiveHealth(entt::registry& reg, entt::entity e) {
            const auto* h = reg.try_get<HealthComponent>(e);
            const auto* s = reg.try_get<UnitStatsComponent>(e);
            const float hp = h ? h->currentHealth : 100.0f;
            const float tough = s ? s->toughness : 15.0f;
            return hp * (1.0f + tough / 100.0f);
        }
        float MeleeDamage(entt::registry& reg, entt::entity e) {
            const auto* s = reg.try_get<UnitStatsComponent>(e);
            const auto* w = DamageResolver::ResolveMeleeWeapon(reg, e);
            return (s ? s->strength * 0.4f : 10.0f) + (w ? w->base_damage : 20.0f);
        }
        // Arma a distanza da usare: la primaria se ha ancora colpi, altrimenti la secondaria (es. il gretchin
        // non ha primaria e spara con la Grot Blasta). -1 = nessuna arma da fuoco o munizioni finite.
        int RangedSlot(entt::registry& reg, entt::entity e) {
            const auto* inv = reg.try_get<InventoryComponent>(e);
            if (!inv) return -1;
            for (int idx = 0; idx < 2; ++idx)
                if (Ammo::RangedAt(*inv, idx) && Ammo::HasAmmo(reg, e, idx)) return idx;
            return -1;
        }

        const WeaponBlueprint* PrimaryWeapon(entt::registry& reg, entt::entity e) {
            const int slot = RangedSlot(reg, e);
            const auto* inv = reg.try_get<InventoryComponent>(e);
            return (slot < 0 || !inv) ? nullptr : Ammo::RangedAt(*inv, slot);
        }

        float RangedPower(entt::registry& reg, entt::entity e) {
            const auto* w = PrimaryWeapon(reg, e);
            if (!w) return 0.0f;
            const float rof = std::min(w->rate_of_fire > 0.0f ? w->rate_of_fire : 300.0f, 450.0f);
            return w->base_damage * std::max(1, w->pellet_count) * rof / 60.0f;
        }
        float Power(entt::registry& reg, entt::entity e) {
            return EffectiveHealth(reg, e) * (MeleeDamage(reg, e) + RangedPower(reg, e) * 0.5f);
        }

        int CountTeamNear(entt::registry& reg, int team, const glm::vec3& p, float radius, entt::entity except) {
            int n = 0;
            auto view = reg.view<TeamComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent, SquigComponent>);
            for (auto e : view) {
                if (e == except || view.get<TeamComponent>(e).team != team) continue;
                if (view.get<HealthComponent>(e).currentHealth <= 0.0f) continue;
                if (HorizontalDist(view.get<TransformComponent>(e).Position, p) <= radius) ++n;
            }
            return n;
        }

        float MeleeReach(entt::registry& reg, entt::entity e) {
            const auto* w = DamageResolver::ResolveMeleeWeapon(reg, e);
            return (w && w->melee_reach > 0.3f) ? w->melee_reach : 1.85f;
        }

        void SetActiveSlot(entt::registry& reg, entt::entity e, int slot) {
            auto* inv = reg.try_get<InventoryComponent>(e);
            if (!inv || inv->currentSlot == slot) return;
            if (!inv->HasSlotItem(slot)) return;
            const std::string id = inv->GetSlotItemId(slot);
            inv->currentSlot = slot;
            reg.get_or_emplace<WeaponBlueprintId>(e).id = id;
            if (auto* ws = reg.try_get<WeaponStateComponent>(e)) {
                ws->wasFiringLastFrame = false;
                ws->burstRemaining = 0;
            }
        }

        // Sceglie un punto della navmesh in cui spostarsi. mode 0 = ritirata lontano dalle minacce, 1 = distanza
        // ideale dal bersaglio, 2 = avvicinamento all'ultima posizione nota. Premia i punti coperti dalle minacce
        // (linea di vista bloccata) e, tranne in ritirata, quelli da cui si vede il bersaglio per sparare.
        glm::vec3 PickPoint(Scene& scene, NavMeshQuery& nav, entt::entity self, const std::vector<entt::entity>& threats,
                            const glm::vec3& targetPos, float desiredDist, int mode) {
            auto& reg = scene.GetRegistry();
            const glm::vec3 me = reg.get<TransformComponent>(self).Position;
            float footY, h; FootAndHeight(reg, self, footY, h);
            const float yOff = me.y - footY;   // da piedi a Position

            glm::vec3 threatCenter = targetPos;
            if (!threats.empty()) {
                threatCenter = glm::vec3(0.0f);
                for (auto t : threats) threatCenter += reg.get<TransformComponent>(t).Position;
                threatCenter /= static_cast<float>(threats.size());
            }
            glm::vec3 away = me - threatCenter; away.y = 0.0f;
            if (glm::length(away) < 0.1f) away = glm::vec3(Random::Range(-1.0f, 1.0f), 0.0f, Random::Range(-1.0f, 1.0f));
            away = glm::normalize(away);

            glm::vec3 center;
            if (mode == 0) center = me + away * 7.0f;
            else if (mode == 1) {
                glm::vec3 fromT = me - targetPos; fromT.y = 0.0f;
                if (glm::length(fromT) < 0.1f) fromT = away;
                center = targetPos + glm::normalize(fromT) * desiredDist;
            }
            else center = me + (targetPos - me) * 0.6f;

            glm::vec3 best = center; float bestScore = -1e9f; bool found = false;
            const float radii[3] = { 0.0f, 4.0f, 8.0f };
            for (float r : radii) {
                const int steps = r < 0.1f ? 1 : 8;
                for (int i = 0; i < steps; ++i) {
                    const float a = 6.2831853f * (i + Random::Float01() * 0.5f) / steps;
                    glm::vec3 cand = center + glm::vec3(std::cos(a) * r, 0.0f, std::sin(a) * r);
                    glm::vec3 p = nav.GetNearestValidPoint(cand);
                    if (HorizontalDist(p, cand) > 2.5f) continue;
                    p.y = me.y;   // terreno piano: resta alla quota dell'unita'
                    const glm::vec3 chestHere(p.x, footY + h * 0.62f, p.z);

                    float score = 0.0f;
                    int checked = 0;
                    for (auto t : threats) {
                        if (++checked > 3) break;
                        if (!HasLineOfSight(scene, Chest(reg, t), chestHere, self)) score += (mode == 0) ? 3.5f : 2.0f;
                    }
                    if (mode != 0) {
                        const glm::vec3 eyeHere(p.x, footY + h * 0.88f, p.z);
                        if (HasLineOfSight(scene, eyeHere, targetPos + glm::vec3(0.0f, 1.0f, 0.0f), self)) score += 2.5f;
                    }
                    const float dT = HorizontalDist(p, targetPos);
                    if (mode == 0) score += std::min(dT, desiredDist) * 0.12f;
                    else score -= std::abs(dT - desiredDist) * 0.15f;
                    score -= HorizontalDist(p, me) * 0.10f;
                    if (score > bestScore) { bestScore = score; best = p; found = true; }
                }
            }
            (void)yOff;
            return found ? best : nav.GetNearestValidPoint(center);
        }

    }

    void BotAISystem::Update(Scene& scene, NavMeshQuery& navQuery, float deltaTime) {
        auto& reg = scene.GetRegistry();
        auto view = reg.view<BotBrainComponent, TransformComponent, HealthComponent, TeamComponent>(entt::exclude<CorpseComponent>);
        for (auto self : view) {
            auto& brain = view.get<BotBrainComponent>(self);
            auto& health = view.get<HealthComponent>(self);
            if (health.currentHealth <= 0.0f) continue;

            // Danno ricevuto di recente (decade in ~2.5 s): rende i bot reattivi quando vengono colpiti
            const float dmg = std::max(0.0f, brain.healthLastThink - health.currentHealth);
            brain.healthLastThink = health.currentHealth;
            brain.recentDamage = brain.recentDamage * std::exp(-deltaTime / 2.5f) + dmg;

            brain.meleeCooldown -= deltaTime;
            brain.parryCooldown -= deltaTime;
            brain.memoryTimer += deltaTime;

            brain.thinkTimer -= deltaTime;
            if (brain.thinkTimer <= 0.0f) {
                brain.thinkTimer = Random::Range(kThinkMin, kThinkMax);
                Think(scene, navQuery, self, brain);
            }

            // Bersaglio ancora vivo? (puo' morire fra due decisioni)
            if (brain.target != entt::null && !Alive(reg, brain.target)) {
                brain.target = entt::null;
                brain.targetVisible = false;
                brain.thinkTimer = 0.0f;
            }
            if (brain.target != entt::null && brain.targetVisible) {
                brain.lastKnownTargetPos = reg.get<TransformComponent>(brain.target).Position;
            }

            // Dentro una torretta (es. lascannon): mira e spara TurretSystem, qui niente movimento/armi/mischia
            if (reg.all_of<TurretMountComponent>(self)) {
                auto& trig = reg.get_or_emplace<WeaponTriggerComponent>(self);
                trig.fire = false; trig.reload = false; trig.holdEquipment = false;
                brain.isMoving = false; brain.deploying = false; brain.holdStill = false; brain.resupplySlot = -1;
                continue;
            }
            Resupply(scene, self, brain, deltaTime);
            TryGrenade(scene, self, brain, deltaTime);
            TryDeploy(scene, self, brain, deltaTime);
            if (brain.equipReleaseTimer > 0.0f) {          // granata/mina appena usata: resta in mano un attimo
                brain.equipReleaseTimer -= deltaTime;
                brain.resupplySlot = brain.equipReleaseTimer > 0.0f ? brain.equipReleaseSlot : -1;
            }
            TryParry(scene, self, brain);
            Move(scene, navQuery, self, brain, deltaTime);
            AimAndFire(scene, self, brain, deltaTime);
            DoMelee(scene, self, brain, deltaTime);
        }
    }

    void BotAISystem::Think(Scene& scene, NavMeshQuery& navQuery, entt::entity self, BotBrainComponent& brain) {
        auto& reg = scene.GetRegistry();
        auto& t = reg.get<TransformComponent>(self);
        const auto& stats = reg.get<UnitStatsComponent>(self);
        const auto& health = reg.get<HealthComponent>(self);
        const int myTeam = TeamOf(reg, self);
        const float hpFrac = health.maxHealth > 0.0f ? health.currentHealth / health.maxHealth : 1.0f;
        const glm::vec3 eye = Eye(reg, self);
        const glm::vec3 fwd = YawForward(t.AimYaw);

        // ---------------- PERCEZIONE: solo cio' che vede dalla sua posizione ----------------
        struct Seen { entt::entity e; float dist; };
        std::vector<Seen> seen;
        auto consider = [&](entt::entity o) {
            if (o == self || reg.get<HealthComponent>(o).currentHealth <= 0.0f) return;
            const glm::vec3 op = reg.get<TransformComponent>(o).Position;
            const float d = HorizontalDist(op, t.Position);
            if (const auto* ck = reg.try_get<CloakComponent>(o); ck && ck->amount > 0.5f && d > 6.0f) return;   // mimetizzato: si vede solo da vicino
            if (d > std::max(20.0f, stats.visionRange)) return;
            glm::vec3 dir = op - t.Position; dir.y = 0.0f;
            const bool inFov = d < kCloseAwareness || (glm::length(dir) > 1e-3f &&
                glm::degrees(std::acos(std::clamp(glm::dot(fwd, glm::normalize(dir)), -1.0f, 1.0f))) <= stats.fovDegrees * 0.5f);
            const bool alerted = brain.recentDamage > 1.0f && d < 45.0f;   // colpito: si guarda attorno
            if (!inFov && !alerted) return;
            if (!HasLineOfSight(scene, eye, Chest(reg, o), self)) return;
            seen.push_back({ o, d });
        };
        auto others = reg.view<TeamComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>);
        for (auto o : others) if (others.get<TeamComponent>(o).team != myTeam) consider(o);
        // Rinforzi evocati fuori dal Vs Bot (modalita' orda): i nemici dell'orda non hanno squadra
        if (reg.all_of<SummonedComponent>(self)) {
            auto horde = reg.view<HordeAgentComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent, TeamComponent>);
            for (auto o : horde) consider(o);
        }

        // ---------------- BERSAGLIO ----------------
        const auto* primary = PrimaryWeapon(reg, self);
        const float range = primary ? std::clamp(primary->range_effective, 15.0f, 80.0f) : 20.0f;
        BotRole role = brain.role;
        if (!primary && role != BotRole::Melee) role = BotRole::Melee;   // niente arma da fuoco: mischia

        auto scoreOf = [&](const Seen& s) {
            const auto& h = reg.get<HealthComponent>(s.e);
            const float frac = h.maxHealth > 0.0f ? h.currentHealth / h.maxHealth : 1.0f;
            const glm::vec3 theirFwd = YawForward(reg.get<TransformComponent>(s.e).AimYaw);
            glm::vec3 toMe = t.Position - reg.get<TransformComponent>(s.e).Position; toMe.y = 0.0f;
            const bool facingMe = glm::length(toMe) > 1e-3f && glm::dot(theirFwd, glm::normalize(toMe)) > 0.5f;
            const float squigPenalty = reg.all_of<SquigComponent>(s.e) ? 35.0f : 0.0f;   // prima le unita', poi gli squig
            switch (role) {
            case BotRole::Ranged:    return squigPenalty + s.dist * 0.5f + frac * 25.0f + (facingMe ? 6.0f : 0.0f) + (s.dist > range ? 30.0f : 0.0f);
            case BotRole::Versatile: return squigPenalty + s.dist * 1.0f + frac * 15.0f;
            default:                 return squigPenalty + s.dist + frac * 4.0f;
            }
        };
        if (!seen.empty()) {
            const Seen* best = &seen[0];
            for (const auto& s : seen) if (scoreOf(s) < scoreOf(*best)) best = &s;
            // isteresi: tiene il bersaglio attuale se e' ancora visibile e non molto peggiore
            for (const auto& s : seen) {
                if (s.e == brain.target && scoreOf(s) < scoreOf(*best) * 1.2f + 2.0f) { best = &s; break; }
            }
            brain.target = best->e;
            brain.targetVisible = true;
            brain.memoryTimer = 0.0f;
            brain.lastKnownTargetPos = reg.get<TransformComponent>(best->e).Position;
        }
        else {
            brain.targetVisible = false;
            if (brain.target != entt::null && (!Alive(reg, brain.target) || brain.memoryTimer > kMemorySeconds)) brain.target = entt::null;
        }

        // ---------------- MINACCIA (dalla sua prospettiva) ----------------
        const float myPower = std::max(1.0f, Power(reg, self));
        float threat = 0.0f, nearestMeleeEnemy = 1e9f;
        std::vector<entt::entity> threats;
        for (const auto& s : seen) {
            threats.push_back(s.e);
            const float rel = Power(reg, s.e) / myPower;
            threat += rel * std::clamp(1.0f - s.dist / 45.0f, 0.15f, 1.0f);
            if (RoleOf(reg, s.e) == BotRole::Melee || RoleOf(reg, s.e) == BotRole::Versatile) {
                nearestMeleeEnemy = std::min(nearestMeleeEnemy, s.dist);
                if (s.dist < 10.0f) threat += 0.6f;
            }
        }
        threat += (brain.recentDamage / std::max(1.0f, health.maxHealth)) * 4.0f;
        if (hpFrac < 0.35f) threat += 0.6f;
        brain.threat = threat;

        // ---------------- STALLO (spara senza ferire) ----------------
        if (brain.target != entt::null && brain.targetVisible && brain.intent == BotIntent::Engage) {
            const float thp = reg.get<HealthComponent>(brain.target).currentHealth;
            if (brain.stalemateTarget != brain.target || thp < brain.stalemateTargetHp - 0.5f) {
                brain.stalemateTarget = brain.target; brain.stalemateTargetHp = thp; brain.stalemateTimer = 0.0f;
            }
            else brain.stalemateTimer += kThinkMax * 0.8f;
        }
        else if (brain.intent != BotIntent::Reposition) {
            brain.stalemateTimer = 0.0f;
        }
        if (brain.stalemateTimer > 14.0f) { brain.stalemateTimer = 0.0f; brain.stalemateTargetHp = -1.0f; }

        // ---------------- DECISIONE ----------------
        const BotIntent prevIntent = brain.intent;

        // Rinforzo evocato (vox caster): senza nemici segue il padrone, un paio di metri dietro e di lato; in
        // combattimento non si allontana troppo da lui (vedi in fondo).
        const auto* summon = reg.try_get<SummonedComponent>(self);
        const bool followOwner = summon && Alive(reg, summon->owner);
        glm::vec3 ownerPos(0.0f);
        float ownerDist = 0.0f;
        if (followOwner) {
            const auto& ot = reg.get<TransformComponent>(summon->owner);
            ownerPos = ot.Position;
            ownerDist = HorizontalDist(ownerPos, t.Position);
            if (brain.target == entt::null) {
                brain.intent = BotIntent::Advance;
                brain.searching = false;
                const glm::vec3 of = YawForward(ot.AimYaw);
                const glm::vec3 right(-of.z, 0.0f, of.x);
                const glm::vec3 slot = ownerPos - of * 2.2f + right * (summon->side * 1.8f);
                brain.moveSpeedFactor = ownerDist > 4.5f ? 1.35f : 1.0f;   // rimasto indietro: corre per raggiungerlo
                if (ownerDist > 3.5f || HorizontalDist(slot, t.Position) > 3.0f) {
                    if (!brain.hasMoveGoal || HorizontalDist(slot, brain.moveGoal) > 1.5f) {
                        brain.moveGoal = navQuery.GetNearestValidPoint(slot);
                        brain.hasMoveGoal = true;
                        brain.pathTimer = 0.0f;
                    }
                }
                else {
                    brain.hasMoveGoal = false;
                    t.AimYaw = ot.AimYaw;   // fermo accanto al padrone: guarda dove guarda lui
                }
                return;
            }
        }
        const glm::vec3 myBase = m_Base[myTeam == 1 ? 1 : 0];
        const glm::vec3 enemyBase = m_Base[myTeam == 1 ? 0 : 1];

        if (brain.target == entt::null) {
            // Nessun nemico noto: avanza verso il centro (con uno spostamento laterale proprio) e poi verso la base nemica
            brain.intent = BotIntent::Advance;
            brain.moveSpeedFactor = 1.0f;
            glm::vec3 axis = enemyBase - myBase; axis.y = 0.0f;
            const glm::vec3 lateral = glm::normalize(glm::vec3(-axis.z, 0.0f, axis.x));
            const glm::vec3 mid = (myBase + enemyBase) * 0.5f + lateral * (brain.advanceSide * 16.0f) + glm::normalize(axis) * 4.0f;
            const bool pastMid = glm::dot(t.Position - mid, glm::normalize(axis)) > -6.0f;
            const glm::vec3 enemyGoal = enemyBase + lateral * (brain.advanceSide * 8.0f);
            const bool atEnemyBase = HorizontalDist(t.Position, enemyGoal) < 6.0f;
            if (atEnemyBase || (prevIntent == BotIntent::Advance && brain.searching)) {
                // Gia' arrivato senza trovare nessuno: perlustra la mappa a caso (meta' avversaria preferita)
                brain.searching = true;
                if (!brain.hasMoveGoal || HorizontalDist(t.Position, brain.moveGoal) < 2.0f || brain.stuckTimer > 1.0f) {
                    const glm::vec3 c = (myBase + enemyBase) * 0.5f + glm::normalize(axis) * Random::Range(-10.0f, 30.0f);
                    const glm::vec3 r = c + lateral * Random::Range(-30.0f, 30.0f);
                    brain.moveGoal = navQuery.GetNearestValidPoint(r);
                    brain.hasMoveGoal = true;
                    brain.pathTimer = 0.0f;
                }
                return;
            }
            const glm::vec3 goal = pastMid ? enemyGoal : mid;
            if (!brain.hasMoveGoal || prevIntent != BotIntent::Advance || HorizontalDist(goal, brain.moveGoal) > 3.0f) {
                brain.moveGoal = navQuery.GetNearestValidPoint(goal);
                brain.hasMoveGoal = true;
                brain.pathTimer = 0.0f;
            }
            return;
        }

        const glm::vec3 targetPos = brain.targetVisible ? reg.get<TransformComponent>(brain.target).Position : brain.lastKnownTargetPos;
        const float d = HorizontalDist(targetPos, t.Position);
        const float reach = MeleeReach(reg, self);

        auto goMelee = [&]() {
            const float targetRadius = [&] {
                const auto* c = reg.try_get<ColliderComponent>(brain.target);
                return c ? std::max(c->halfExtents.x, c->halfExtents.z) : 0.4f;
            }();
            brain.intent = (brain.targetVisible && d <= reach + targetRadius * 0.6f) ? BotIntent::Melee : BotIntent::Charge;
            brain.moveSpeedFactor = 1.2f;   // carica di corsa
            brain.moveGoal = targetPos;
            brain.hasMoveGoal = brain.intent == BotIntent::Charge;
            brain.pathTimer = std::min(brain.pathTimer, 0.15f);
        };

        auto rangedLogic = [&](float bandMinF, float bandMaxF, float threatLimit) {
            // Distanza ideale: entro la portata e, se l'arma perde danno con la distanza, prima del calo
            float effective = range;
            if (primary && primary->damage_falloff_start > 1.0f) effective = std::min(range, primary->damage_falloff_start / bandMaxF);
            float bandMax = std::max(10.0f, effective * bandMaxF);
            float bandMin = std::max(5.0f, effective * bandMinF);
            // Stallo: se da un po' spara senza ferire il bersaglio, accorcia la distanza
            if (brain.stalemateTimer > 7.0f) { bandMax = std::max(8.0f, d * 0.6f); bandMin = std::max(4.0f, bandMax * 0.5f); }
            if (bandMax < bandMin + 4.0f) bandMax = bandMin + 4.0f;
            const bool threatened = threat > threatLimit || nearestMeleeEnemy < 9.0f
                || brain.recentDamage > 0.18f * health.maxHealth;
            brain.moveSpeedFactor = 1.0f;

            const auto& th = reg.get<HealthComponent>(brain.target);
            const float targetFrac = th.maxHealth > 0.0f ? th.currentHealth / th.maxHealth : 1.0f;
            glm::vec3 toMe = t.Position - targetPos; toMe.y = 0.0f;
            const bool targetFacingMe = glm::length(toMe) > 1e-3f &&
                glm::dot(YawForward(reg.get<TransformComponent>(brain.target).AimYaw), glm::normalize(toMe)) > 0.6f;
            const bool easyTarget = brain.targetVisible && d <= range &&
                (targetFrac < 0.5f || !targetFacingMe || Power(reg, brain.target) < myPower * 0.8f);

            // Mentre si riposiziona tiene la destinazione finche' non ci arriva (niente avanti e indietro)
            const bool keepGoal = prevIntent == BotIntent::Reposition && brain.hasMoveGoal &&
                HorizontalDist(t.Position, brain.moveGoal) > 1.2f && brain.stuckTimer < 1.2f;

            if (!brain.targetVisible) {
                brain.intent = BotIntent::Reposition;
                if (!keepGoal) { brain.moveGoal = PickPoint(scene, navQuery, self, threats, targetPos, bandMin, 2); brain.hasMoveGoal = true; brain.pathTimer = 0.0f; }
            }
            else if (threatened) {
                brain.intent = BotIntent::Reposition;
                if (!keepGoal || prevIntent != BotIntent::Reposition) {
                    brain.moveGoal = PickPoint(scene, navQuery, self, threats, targetPos, bandMax, 0);
                    brain.hasMoveGoal = true; brain.pathTimer = 0.0f;
                }
            }
            else if ((easyTarget && brain.stalemateTimer <= 7.0f && d <= bandMax * 1.4f) || (d >= bandMin && d <= bandMax)) {
                brain.intent = BotIntent::Engage;
                brain.hasMoveGoal = false;
            }
            else {
                brain.intent = BotIntent::Reposition;
                if (!keepGoal) {
                    brain.moveGoal = PickPoint(scene, navQuery, self, threats, targetPos, (bandMin + bandMax) * 0.5f, 1);
                    brain.hasMoveGoal = true; brain.pathTimer = 0.0f;
                }
            }
        };

        switch (role) {
        case BotRole::Melee:
            goMelee();   // costi quel che costi
            break;
        case BotRole::Versatile: {
            // Vincerebbe il corpo a corpo? Confronta forze in mischia e salute, contando chi sta vicino a ciascuno
            const int alliesNearMe = CountTeamNear(reg, myTeam, t.Position, 8.0f, self);
            const int enemiesNearTarget = CountTeamNear(reg, TeamOf(reg, brain.target), targetPos, 7.0f, brain.target);
            const float mine = EffectiveHealth(reg, self) * MeleeDamage(reg, self) * std::sqrt(1.0f + alliesNearMe);
            const float theirs = EffectiveHealth(reg, brain.target) * MeleeDamage(reg, brain.target) * (1.0f + 0.7f * enemiesNearTarget);
            const float winChance = mine / std::max(1.0f, mine + theirs);
            const bool committed = prevIntent == BotIntent::Charge || prevIntent == BotIntent::Melee;
            if ((brain.targetVisible && winChance > 0.58f && d < 20.0f) || (committed && winChance > 0.42f && hpFrac > 0.3f)) goMelee();
            else rangedLogic(0.15f, 0.45f, 1.4f);
            break;
        }
        default:
            rangedLogic(0.35f, 0.70f, 1.0f);
            break;
        }

        // Rinforzo evocato: non si allontana piu' di 14 m dal padrone (tranne a contatto in mischia)
        if (followOwner && ownerDist > 14.0f && brain.intent != BotIntent::Melee) {
            brain.intent = BotIntent::Reposition;
            brain.moveSpeedFactor = 1.2f;
            if (!brain.hasMoveGoal || HorizontalDist(brain.moveGoal, ownerPos) > 6.0f) {
                brain.moveGoal = navQuery.GetNearestValidPoint(ownerPos + glm::normalize(glm::vec3(t.Position.x - ownerPos.x, 0.0f, t.Position.z - ownerPos.z) + glm::vec3(1e-3f)) * 4.0f);
                brain.hasMoveGoal = true;
                brain.pathTimer = 0.0f;
            }
        }
    }

    void BotAISystem::Move(Scene& scene, NavMeshQuery& navQuery, entt::entity self, BotBrainComponent& brain, float dt) {
        auto& reg = scene.GetRegistry();
        auto& t = reg.get<TransformComponent>(self);
        auto& stats = reg.get<UnitStatsComponent>(self);
        stats.sprinting = false;   // lo rimette TickSprint se corre davvero

        const bool wantsMove = !brain.holdStill && brain.hasMoveGoal && (brain.intent == BotIntent::Advance || brain.intent == BotIntent::Reposition || brain.intent == BotIntent::Charge);
        brain.isMoving = false;
        if (wantsMove) {
            if (brain.intent == BotIntent::Charge && brain.target != entt::null && brain.targetVisible) {
                brain.moveGoal = reg.get<TransformComponent>(brain.target).Position;   // insegue il bersaglio che si muove
            }
            brain.pathTimer -= dt;
            if (brain.pathTimer <= 0.0f || brain.path.empty()) {
                brain.path = navQuery.FindPath(t.Position, brain.moveGoal);
                brain.pathTimer = brain.intent == BotIntent::Charge ? 0.35f : Random::Range(0.6f, 0.9f);
                if (!brain.path.empty() && HorizontalDist(brain.path.front(), t.Position) < 0.3f) brain.path.erase(brain.path.begin());
            }
            if (!brain.path.empty()) {
                glm::vec3 wp = brain.path.front();
                glm::vec3 dir = wp - t.Position; dir.y = 0.0f;
                const float dist = glm::length(dir);
                if (dist < 0.3f) brain.path.erase(brain.path.begin());
                else {
                    // Corsa (carica, rincorsa del padrone): consuma stamina come per il giocatore; senza fiato si cammina
                    float runFactor = brain.moveSpeedFactor;
                    if (!TickSprint(stats, runFactor > 1.05f, dt)) runFactor = std::min(runFactor, 1.0f);
                    float speed = std::max(1.0f, stats.baseSpeed * stats.speedModifier) * runFactor;
                    if (const auto* inv = reg.try_get<InventoryComponent>(self)) speed *= inv->HeldSpeedMultiplier();   // es. coltello del gretchin
                    if (const auto* buff = reg.try_get<FactionBuffComponent>(self)) speed *= buff->moveSpeedMult;      // buff di fazione
                    if (const auto* boost = reg.try_get<SpeedBoostComponent>(self)) speed *= boost->mult;             // zaino energetico
                    t.Position += dir / dist * std::min(dist, speed * dt);
                    brain.isMoving = true;
                }
            }
            if (HorizontalDist(t.Position, brain.moveGoal) < 0.8f) {
                brain.hasMoveGoal = brain.intent == BotIntent::Charge;   // arrivato
                brain.path.clear();
            }
        }
        else {
            brain.path.clear();
        }

        // Bloccato? (percorso che non avanza): ricalcola presto con una destinazione leggermente diversa
        brain.stuckTimer = (brain.isMoving && HorizontalDist(t.Position, brain.lastPos) < 0.6f * dt) ? brain.stuckTimer + dt : 0.0f;
        if (brain.stuckTimer > 1.5f) {
            brain.moveGoal = navQuery.GetNearestValidPoint(brain.moveGoal + glm::vec3(Random::Range(-3.0f, 3.0f), 0.0f, Random::Range(-3.0f, 3.0f)));
            brain.path.clear();
            brain.stuckTimer = 0.0f;
        }
        brain.lastPos = t.Position;

        // Separazione: non si compenetrano con le altre unita'
        auto units = reg.view<TransformComponent, HealthComponent, UnitStatsComponent>(entt::exclude<CorpseComponent>);
        for (auto o : units) {
            if (o == self) continue;
            const glm::vec3& op = units.get<TransformComponent>(o).Position;
            glm::vec2 d(t.Position.x - op.x, t.Position.z - op.z);
            const float len = glm::length(d);
            if (len < 0.85f && len > 1e-4f) {
                const glm::vec2 push = d / len * (0.85f - len) * 0.5f;
                t.Position.x += push.x; t.Position.z += push.y;
            }
        }
    }

    void BotAISystem::AimAndFire(Scene& scene, entt::entity self, BotBrainComponent& brain, float dt) {
        auto& reg = scene.GetRegistry();
        auto& t = reg.get<TransformComponent>(self);
        const auto& stats = reg.get<UnitStatsComponent>(self);
        auto& trigger = reg.get_or_emplace<WeaponTriggerComponent>(self);
        trigger.fire = false;
        trigger.reload = false;

        const bool resupplying = brain.resupplySlot >= 0;
        const int rangedSlot = RangedSlot(reg, self);
        const bool meleeMode = !resupplying && (brain.intent == BotIntent::Charge || brain.intent == BotIntent::Melee ||
                                                brain.role == BotRole::Melee || rangedSlot < 0);
        SetActiveSlot(reg, self, resupplying ? brain.resupplySlot : (meleeMode ? 2 : rangedSlot));

        // Errore di mira: dipende da accuracy_skill, aumenta muovendosi e quando si viene colpiti
        brain.aimErrorTimer -= dt;
        if (brain.aimErrorTimer <= 0.0f) {
            brain.aimErrorTimer = Random::Range(0.25f, 0.5f);
            const float skill = std::clamp(stats.accuracySkill / 100.0f, 0.0f, 1.0f);
            float spread = glm::mix(6.5f, 0.6f, skill);
            if (brain.isMoving) spread *= 1.6f;
            spread *= 1.0f + std::min(1.5f, brain.recentDamage / std::max(1.0f, stats.maxHealth) * 3.0f);
            brain.aimError = glm::vec2(Random::Range(-1.0f, 1.0f), Random::Range(-1.0f, 1.0f)) * spread;
        }

        float desiredYaw = t.AimYaw, desiredPitch = 0.0f;
        const bool haveTarget = brain.target != entt::null && reg.valid(brain.target);
        if (haveTarget && (brain.targetVisible || meleeMode)) {
            const glm::vec3 from = Chest(reg, self);
            const glm::vec3 to = brain.targetVisible ? Chest(reg, brain.target) : brain.lastKnownTargetPos + glm::vec3(0.0f, 1.0f, 0.0f);
            desiredYaw = YawTowards(from, to);
            const float horiz = HorizontalDist(from, to);
            desiredPitch = glm::degrees(std::atan2(to.y - from.y, std::max(0.1f, horiz)));
            if (!meleeMode) { desiredYaw += brain.aimError.x; desiredPitch += brain.aimError.y * 0.6f; }
        }
        else if (brain.isMoving && !brain.path.empty()) {
            desiredYaw = YawTowards(t.Position, brain.path.front());
        }
        else if (!brain.targetVisible && brain.target != entt::null) {
            desiredYaw = YawTowards(t.Position, brain.lastKnownTargetPos);   // guarda dove l'ha visto l'ultima volta
        }
        t.AimYaw = LerpAngle(t.AimYaw, desiredYaw, std::min(1.0f, 9.0f * dt));
        t.AimPitch = glm::mix(t.AimPitch, std::clamp(desiredPitch, -60.0f, 60.0f), std::min(1.0f, 9.0f * dt));
        t.Rotation.y = LerpAngle(t.Rotation.y, t.AimYaw, std::min(1.0f, 7.0f * dt));
        brain.isAiming = !meleeMode && !resupplying && haveTarget && brain.targetVisible;
        if (resupplying) return;   // in mano c'e' la scatola di munizioni

        // ---------------- FUOCO ----------------
        brain.pauseTimer -= dt;
        const auto* w = PrimaryWeapon(reg, self);
        auto* ws = reg.try_get<WeaponStateComponent>(self);
        if (meleeMode || !w || !ws) return;

        const float dist = haveTarget ? HorizontalDist(t.Position, reg.get<TransformComponent>(brain.target).Position) : 1e9f;
        const bool aligned = AngleDiff(t.AimYaw, desiredYaw) < 7.0f;
        // Lanciafiamme: solo quando il bersaglio e' dentro il cono (oltre e' fiato sprecato)
        const bool flamer = w->projectile_type == ProjectileType::Flamer;
        const float shootRange = flamer ? w->range_effective : std::max(15.0f, w->range_effective * 1.1f);
        const bool canShoot = brain.isAiming && aligned && dist <= shootRange && !ws->isReloading;

        if (canShoot) {
            const std::string mode = w->fire_modes.empty() ? std::string("auto") : w->fire_modes[0];
            if (mode == "semi" || mode == "single" || mode == "burst") {
                if (brain.pauseTimer <= 0.0f) {
                    trigger.fire = true;   // un frame premuto: fronte di salita = un colpo (o una raffica)
                    const float skill = std::clamp(stats.accuracySkill / 100.0f, 0.0f, 1.0f);
                    const float minInterval = w->rate_of_fire > 0.0f ? 60.0f / w->rate_of_fire : 0.2f;
                    brain.pauseTimer = std::max(minInterval, glm::mix(0.6f, 0.22f, skill)) * Random::Range(0.9f, 1.3f);
                }
            }
            else {
                if (brain.burstTimer > 0.0f) {
                    trigger.fire = true;
                    brain.burstTimer -= dt;
                    if (brain.burstTimer <= 0.0f) brain.pauseTimer = flamer ? Random::Range(0.1f, 0.25f) : Random::Range(0.25f, 0.7f);
                }
                else if (brain.pauseTimer <= 0.0f) {
                    brain.burstTimer = flamer ? Random::Range(1.2f, 2.2f) : Random::Range(0.25f, 0.65f);   // fiammate lunghe
                }
            }
        }
        else {
            brain.burstTimer = 0.0f;
        }

        // Ricarica quando ha pochi colpi e non sta sparando a un bersaglio in vista
        const auto* ammo = reg.try_get<AmmoComponent>(self);
        if (ws->currentAmmo <= std::max(1, w->magazine_size / 4) && !ws->isReloading && (!ammo || Ammo::CanReload(*ammo)) &&
            (!brain.targetVisible || brain.intent == BotIntent::Reposition || ws->currentAmmo == 0)) {
            trigger.reload = true;
        }
    }

    // Curare o attaccare? (solo per chi ha un oggetto che cura, es. zaino di cibo dello yoof: cura tutti nel
    // raggio, lui compreso)
    //  - in mischia (nemico entro 3 m) non cura mai: posare il cibo adesso vorrebbe dire prendere colpi gratis;
    //  - un compagno nel raggio sotto il 50%: cura quasi sempre (90%);
    //  - compagni sotto il 75%: confronta il valore della cura (vita mancante x forza dei feriti, cioe' il loro
    //    costo in punti) con quello di continuare l'attacco (forza del bersaglio, piu' alta se e' gia' ferito e
    //    a tiro): cura se la cura vale di piu'.
    bool BotAISystem::ShouldHeal(Scene& scene, entt::entity self, BotBrainComponent& brain, const WeaponBlueprint& item) {
        auto& reg = scene.GetRegistry();
        const glm::vec3 me = reg.get<TransformComponent>(self).Position;
        auto unitValue = [&](entt::entity u) {
            const auto* uid = reg.try_get<UnitBlueprintId>(u);
            const auto* ubp = uid ? BlueprintManager::Get().GetUnit(uid->id) : nullptr;
            return ubp ? static_cast<float>(std::max(1, ubp->point_cost)) : 10.0f;
        };
        // In mischia con qualcuno?
        auto all = reg.view<TeamComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent, SquigComponent>);
        for (auto o : all) {
            if (o == self || SameTeam(reg, self, o) || all.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
            if (HorizontalDist(all.get<TransformComponent>(o).Position, me) <= 3.0f) return false;
        }
        const float radius = std::max(1.0f, item.equipment_range * 0.85f);
        float healValue = 0.0f;
        bool critical = false, anyHurt = false;
        for (auto o : all) {
            if (!SameTeam(reg, self, o) && o != self) continue;
            const auto& h = all.get<HealthComponent>(o);
            if (h.currentHealth <= 0.0f || h.maxHealth <= 0.0f) continue;
            if (HorizontalDist(all.get<TransformComponent>(o).Position, me) > radius) continue;
            const float frac = h.currentHealth / h.maxHealth;
            if (frac >= 0.75f) continue;
            anyHurt = true;
            if (frac < 0.5f) critical = true;
            healValue += (1.0f - frac) * unitValue(o);
        }
        if (!anyHurt) return false;
        if (critical) return Random::Chance(0.9f);
        // Valore dell'attacco in corso: bersaglio in vista, vale di piu' se gia' ferito e vicino (lo si puo' finire)
        float pushValue = 0.0f;
        if (brain.target != entt::null && Alive(reg, brain.target) && brain.targetVisible) {
            const auto& th = reg.get<HealthComponent>(brain.target);
            const float tFrac = th.maxHealth > 0.0f ? th.currentHealth / th.maxHealth : 1.0f;
            const float d = HorizontalDist(reg.get<TransformComponent>(brain.target).Position, me);
            const float reach = d < 12.0f ? 1.0f : (d < 25.0f ? 0.6f : 0.3f);
            pushValue = unitValue(brain.target) * (0.5f + (1.0f - tFrac)) * reach;
        }
        return healValue >= pushValue;
    }

    // Scatola di munizioni: quando non sta combattendo, la prende in mano e rifornisce se' stesso (riserva quasi
    // finita) o il compagno entro la portata che ne ha piu' bisogno. Appena vede un nemico torna alle armi.
    void BotAISystem::Resupply(Scene& scene, entt::entity self, BotBrainComponent& brain, float dt) {
        auto& reg = scene.GetRegistry();
        const auto* inv = reg.try_get<InventoryComponent>(self);
        const auto* ammo = reg.try_get<AmmoComponent>(self);
        if (!inv || !ammo) { brain.resupplySlot = -1; return; }
        if (ammo->useTimer > 0.0f) return;   // rifornimento in corso: tiene la scatola in mano

        // Vox caster: appena c'e' un nemico noto e nessuno entro 8 m chiama il rinforzo (1 s di chiamata)
        for (int s = 4; s <= 6; ++s) {
            const auto* bp = BlueprintManager::Get().GetWeapon(inv->GetSlotItemId(s));
            if (!bp || bp->equipment_type != "vox_caster" || ammo->equipmentUses[s - 4] <= 0) continue;
            if (brain.resupplySlot != s) {
                if (brain.target == entt::null) break;
                const glm::vec3 me = reg.get<TransformComponent>(self).Position;
                bool enemyNear = false;
                auto all = reg.view<TeamComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent, SquigComponent>);
                for (auto o : all) {
                    if (o == self || SameTeam(reg, self, o) || all.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
                    if (HorizontalDist(all.get<TransformComponent>(o).Position, me) <= 8.0f) { enemyNear = true; break; }
                }
                if (enemyNear) break;
                brain.resupplySlot = s;
            }
            if (inv->currentSlot == s) reg.get_or_emplace<WeaponTriggerComponent>(self).useEquipment = true;
            return;
        }

        // Zaino energetico: spinta quando carica in mischia o si riposiziona sotto pressione (se e' carico)
        for (int s = 4; s <= 6; ++s) {
            const auto* bp = BlueprintManager::Get().GetWeapon(inv->GetSlotItemId(s));
            if (!bp || bp->equipment_type != "power_pack" || ammo->equipmentCooldown[s - 4] > 0.0f) continue;
            if (brain.resupplySlot != s) {
                const bool rush = brain.intent == BotIntent::Charge || (brain.intent == BotIntent::Reposition && brain.recentDamage > 10.0f);
                if (!rush || reg.all_of<SpeedBoostComponent>(self)) break;
                brain.resupplySlot = s;
            }
            if (inv->currentSlot == s) {
                reg.get_or_emplace<WeaponTriggerComponent>(self).useEquipment = true;
                brain.equipReleaseSlot = s; brain.equipReleaseTimer = 0.2f;
            }
            return;
        }

        // Mantello mimetico: lo attiva quando sa dov'e' un nemico ma non lo vede (per avvicinarsi o aggirarlo) o
        // quando e' ferito sotto il 40% (per sganciarsi)
        for (int s = 4; s <= 6; ++s) {
            const auto* bp = BlueprintManager::Get().GetWeapon(inv->GetSlotItemId(s));
            if (!bp || bp->equipment_type != "camo_cloak" || ammo->equipmentUses[s - 4] <= 0) continue;
            if (brain.resupplySlot != s) {
                if (reg.all_of<CloakComponent>(self)) break;
                const auto& h = reg.get<HealthComponent>(self);
                const bool hurt = h.maxHealth > 0.0f && h.currentHealth < h.maxHealth * 0.4f && brain.target != entt::null;
                const bool stalking = brain.target != entt::null && !brain.targetVisible;
                if (!hurt && !stalking) break;
                brain.resupplySlot = s;
            }
            if (inv->currentSlot == s) {
                reg.get_or_emplace<WeaponTriggerComponent>(self).useEquipment = true;
                brain.equipReleaseSlot = s; brain.equipReleaseTimer = 0.3f;
            }
            return;
        }

        // Cassa di munizioni: la posa quando lui o un compagno vicino ha la riserva bassa e nessun nemico e' a 10 m
        // (poi ci passa accanto e la usa, vedi EquipmentSystem::UpdateCrates)
        for (int s = 4; s <= 6; ++s) {
            const auto* bp = BlueprintManager::Get().GetWeapon(inv->GetSlotItemId(s));
            if (!bp || bp->equipment_type != "ammo_crate" || ammo->equipmentUses[s - 4] <= 0) continue;
            if (brain.resupplySlot != s) {
                const glm::vec3 me = reg.get<TransformComponent>(self).Position;
                bool need = Ammo::ReserveFraction(reg, self) <= 0.5f;
                auto mates = reg.view<AmmoComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>);
                for (auto o : mates) {
                    if (need) break;
                    if (o == self || !SameTeam(reg, self, o) || mates.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
                    if (HorizontalDist(mates.get<TransformComponent>(o).Position, me) <= 6.0f && Ammo::ReserveFraction(reg, o) <= 0.4f) need = true;
                }
                if (!need) break;
                bool enemyNear = false;
                auto all = reg.view<TeamComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent, SquigComponent>);
                for (auto o : all) {
                    if (o == self || SameTeam(reg, self, o) || all.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
                    if (HorizontalDist(all.get<TransformComponent>(o).Position, me) <= 10.0f) { enemyNear = true; break; }
                }
                if (enemyNear) break;
                brain.resupplySlot = s;
            }
            if (inv->currentSlot == s) reg.get_or_emplace<WeaponTriggerComponent>(self).useEquipment = true;
            return;
        }

        // Medikit (cura se stesso in 3 s): sotto il 50% di salute e senza nemici entro 10 m, perche' per tutta la
        // medicazione non spara. Una volta deciso lo tiene in mano finche' la medicazione non finisce.
        for (int s = 4; s <= 6; ++s) {
            const auto* bp = BlueprintManager::Get().GetWeapon(inv->GetSlotItemId(s));
            if (!bp || bp->heal_amount <= 0.0f || ammo->equipmentUses[s - 4] <= 0) continue;
            const auto& h = reg.get<HealthComponent>(self);
            if (brain.resupplySlot != s) {
                if (h.maxHealth <= 0.0f || h.currentHealth >= h.maxHealth * 0.5f) break;
                const glm::vec3 me = reg.get<TransformComponent>(self).Position;
                bool enemyNear = false;
                auto all = reg.view<TeamComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent, SquigComponent>);
                for (auto o : all) {
                    if (o == self || SameTeam(reg, self, o) || all.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
                    if (HorizontalDist(all.get<TransformComponent>(o).Position, me) <= 10.0f) { enemyNear = true; break; }
                }
                if (enemyNear) break;
                brain.resupplySlot = s;
            }
            if (inv->currentSlot == s) reg.get_or_emplace<WeaponTriggerComponent>(self).useEquipment = true;
            return;
        }

        // Cura (zaino di cibo e altri oggetti che curano): ogni ~0,6 s valuta se curare o continuare l'attacco
        for (int s = 4; s <= 6; ++s) {
            const auto* bp = BlueprintManager::Get().GetWeapon(inv->GetSlotItemId(s));
            if (!bp || bp->heal_per_second <= 0.0f || ammo->equipmentUses[s - 4] <= 0) continue;
            if (brain.resupplySlot == s) {   // gia' deciso: la tiene in mano e la usa appena e' in mano
                if (inv->currentSlot == s) reg.get_or_emplace<WeaponTriggerComponent>(self).useEquipment = true;
                return;
            }
            brain.healCheck -= dt;
            if (brain.healCheck > 0.0f) break;
            brain.healCheck = 0.6f;
            if (ShouldHeal(scene, self, brain, *bp)) {
                brain.resupplySlot = s;
                if (inv->currentSlot == s) reg.get_or_emplace<WeaponTriggerComponent>(self).useEquipment = true;
                return;
            }
            break;
        }

        int boxSlot = -1;
        float boxRange = 5.0f;
        for (int s = 4; s <= 6; ++s) {
            const auto* bp = BlueprintManager::Get().GetWeapon(inv->GetSlotItemId(s));
            if (bp && bp->is_equipment && bp->equipment_type == "ammo_box" && ammo->equipmentUses[s - 4] > 0) {
                boxSlot = s; boxRange = bp->equipment_range; break;
            }
        }
        const bool engaged = brain.targetVisible || brain.intent == BotIntent::Charge || brain.intent == BotIntent::Melee;
        if (boxSlot < 0 || engaged) { brain.resupplySlot = -1; return; }

        brain.resupplyCheck -= dt;
        if (brain.resupplySlot < 0 && brain.resupplyCheck > 0.0f) return;
        brain.resupplyCheck = 0.8f;

        const bool selfOut = RangedSlot(reg, self) < 0 && (Ammo::RangedAt(*inv, 0) || Ammo::RangedAt(*inv, 1));
        const bool selfNeeds = Ammo::Missing(reg, self) > 0 && (selfOut || Ammo::ReserveFraction(reg, self) <= 0.35f);

        entt::entity ally = entt::null;
        float worst = 0.5f;   // solo compagni sotto meta' riserva
        if (!selfNeeds) {
            const glm::vec3 me = reg.get<TransformComponent>(self).Position;
            auto view = reg.view<AmmoComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>);
            for (auto o : view) {
                if (o == self || !SameTeam(reg, self, o) || view.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
                if (HorizontalDist(view.get<TransformComponent>(o).Position, me) > boxRange) continue;
                if (Ammo::Missing(reg, o) <= 0) continue;
                const float frac = Ammo::ReserveFraction(reg, o);
                if (frac < worst) { worst = frac; ally = o; }
            }
        }
        if (!selfNeeds && ally == entt::null) { brain.resupplySlot = -1; return; }

        brain.resupplySlot = boxSlot;
        if (inv->currentSlot != boxSlot) return;   // la prende in mano in AimAndFire, la usa al prossimo giro
        auto& trigger = reg.get_or_emplace<WeaponTriggerComponent>(self);
        if (selfNeeds) trigger.useEquipment = true;
        else { trigger.giveEquipment = true; trigger.giveTarget = ally; }
    }

    // Granata: se vede il bersaglio fra 7 e 22 metri, senza compagni vicino al bersaglio, ogni tanto prende la
    // granata e gliela tira (parabola calcolata da EquipmentSystem su giveTarget)
    void BotAISystem::TryGrenade(Scene& scene, entt::entity self, BotBrainComponent& brain, float dt) {
        auto& reg = scene.GetRegistry();
        brain.grenadeCooldown -= dt;
        if (brain.deploying) return;   // sta piazzando una struttura/arma: niente granate a meta' lavoro
        const auto* inv = reg.try_get<InventoryComponent>(self);
        const auto* ammo = reg.try_get<AmmoComponent>(self);
        if (!inv || !ammo) return;
        int slot = -1;
        for (int s = 4; s <= 6; ++s) {
            const auto* bp = BlueprintManager::Get().GetWeapon(inv->GetSlotItemId(s));
            if (bp && bp->equipment_type == "grenade" && ammo->equipmentUses[s - 4] > 0) { slot = s; break; }
        }
        const bool targetOk = brain.target != entt::null && Alive(reg, brain.target) && brain.targetVisible;
        if (brain.throwingGrenade) {
            if (slot < 0 || !targetOk) { brain.throwingGrenade = false; if (brain.resupplySlot == slot || slot < 0) brain.resupplySlot = -1; return; }
            brain.resupplySlot = slot;
            if (inv->currentSlot == slot) {
                auto& trigger = reg.get_or_emplace<WeaponTriggerComponent>(self);
                trigger.useEquipment = true;
                trigger.giveTarget = brain.target;
                brain.throwingGrenade = false;
                brain.equipReleaseSlot = slot;      // la tiene in mano finche' l'EquipmentSystem non la lancia
                brain.equipReleaseTimer = 0.3f;
                brain.grenadeCooldown = Random::Range(10.0f, 18.0f);
            }
            return;
        }
        if (slot < 0 || brain.grenadeCooldown > 0.0f || !targetOk) return;
        const glm::vec3 me = reg.get<TransformComponent>(self).Position;
        const glm::vec3 tp = reg.get<TransformComponent>(brain.target).Position;
        const float d = HorizontalDist(me, tp);
        if (d < 7.0f || d > 22.0f) return;
        if (CountTeamNear(reg, TeamOf(reg, self), tp, 4.5f, self) > 0) { brain.grenadeCooldown = 1.5f; return; }   // compagni vicino al bersaglio
        if (!Random::Chance(0.5f)) { brain.grenadeCooldown = 2.5f; return; }
        brain.throwingGrenade = true;
        brain.resupplySlot = slot;
    }

    // Mine (boy), barricate di scrap (yoof) e muri con feritoia (loota).
    // Mina: con un nemico noto fra 8 e 25 m la posa davanti a se', sulla strada che fara' chi lo carica.
    // Barricata: sotto tiro da un nemico in vista fra 10 e 35 m si ferma, la costruisce (3 s) fra se' e lui e ci
    // combatte dietro. Muro con feritoia: appena vede un nemico fra 14 e 70 m. Se il nemico arriva a 6 m o parte
    // la mischia, annulla.
    void BotAISystem::TryDeploy(Scene& scene, entt::entity self, BotBrainComponent& brain, float dt) {
        auto& reg = scene.GetRegistry();
        brain.deployCooldown -= dt;
        const auto* inv = reg.try_get<InventoryComponent>(self);
        const auto* ammo = reg.try_get<AmmoComponent>(self);
        if (!inv || !ammo) return;
        int slot = -1;
        const WeaponBlueprint* bp = nullptr;
        for (int s = 4; s <= 6 && slot < 0; ++s) {
            const auto* b = BlueprintManager::Get().GetWeapon(inv->GetSlotItemId(s));
            if (!b || ammo->equipmentUses[s - 4] <= 0) continue;
            if (b->equipment_type == "landmine" || b->equipment_type == "scrap_barricade" || b->equipment_type == "slit_wall" ||
                b->equipment_type == "turret") { slot = s; bp = b; }
        }
        auto& trigger = reg.get_or_emplace<WeaponTriggerComponent>(self);
        const bool isMine = bp && bp->equipment_type == "landmine";
        const bool meleeFight = brain.intent == BotIntent::Charge || brain.intent == BotIntent::Melee;
        const glm::vec3 me = reg.get<TransformComponent>(self).Position;
        const bool hasTarget = brain.target != entt::null && Alive(reg, brain.target);
        const float d = hasTarget ? HorizontalDist(me, reg.get<TransformComponent>(brain.target).Position) : 1e9f;

        if (brain.deploying) {
            const bool abort = slot < 0 || brain.intent == BotIntent::Melee || (!isMine && d < 6.0f);
            if (abort) {
                brain.deploying = false; brain.holdStill = false; trigger.holdEquipment = false;
                if (brain.resupplySlot >= 4) brain.resupplySlot = -1;
                brain.deployCooldown = Random::Range(4.0f, 7.0f);
                return;
            }
            brain.resupplySlot = slot;               // AimAndFire la prende in mano
            if (inv->currentSlot != slot) return;
            if (isMine) {
                trigger.useEquipment = true;
                brain.deploying = false;
                brain.equipReleaseSlot = slot;
                brain.equipReleaseTimer = 0.3f;
                brain.deployCooldown = Random::Range(12.0f, 20.0f);
                return;
            }
            brain.holdStill = true;
            trigger.holdEquipment = true;
            brain.deployTimer += dt;
            const bool built = ammo->equipmentUses[slot - 4] < brain.deployUsesBefore;
            if (built || brain.deployTimer > bp->equipment_use_time * 2.0f + 2.0f) {   // margine: una spinta dei compagni fa ripartire la canalizzazione
                brain.deploying = false; brain.holdStill = false; trigger.holdEquipment = false;
                brain.resupplySlot = -1;
                brain.deployCooldown = built ? Random::Range(20.0f, 35.0f) : Random::Range(5.0f, 8.0f);
            }
            return;
        }
        trigger.holdEquipment = false;
        brain.holdStill = false;
        if (slot < 0 || brain.deployCooldown > 0.0f || brain.throwingGrenade || brain.resupplySlot >= 0 || brain.equipReleaseTimer > 0.0f) return;
        const int myTeam = TeamOf(reg, self) == 1 ? 1 : 0;
        if (isMine) {
            bool ok = hasTarget && !meleeFight && d >= 8.0f && d <= 25.0f;   // nemico in arrivo: mina sulla sua strada
            if (!ok && !brain.targetVisible) {
                // Avanzando senza nemici in vista: mina a meta' campo (fra il 30% e il 60% della strada verso la
                // base nemica), dove passera' chi attacca
                const glm::vec3 lane = m_Base[1 - myTeam] - m_Base[myTeam];
                const float len2 = glm::dot(lane, lane);
                const float progress = len2 > 1.0f ? glm::dot(me - m_Base[myTeam], lane) / len2 : 0.0f;
                ok = progress >= 0.3f && progress <= 0.6f;
            }
            if (!ok) return;
            if (CountTeamNear(reg, myTeam, me, 2.5f, self) > 0) { brain.deployCooldown = 2.0f; return; }   // non in mezzo ai compagni
            if (!Random::Chance(0.35f)) { brain.deployCooldown = 3.0f; return; }
        }
        else {
            // Il loota (muro con feritoia, tiratore a lunga distanza) costruisce appena vede il nemico lontano
            // Arma pesante (lascannon): come il muro, appena vede un nemico lontano (poi ci entra e spara)
            const bool farBuild = bp->equipment_type == "slit_wall" || bp->equipment_type == "turret";
            const float maxD = farBuild ? 70.0f : 35.0f;
            const float minD = farBuild ? 14.0f : 10.0f;
            if (!hasTarget || !brain.targetVisible || d < minD || d > maxD) return;
            // In mischia o in carica (es. yoof) si ferma a costruire solo se lo stanno colpendo da lontano
            if (meleeFight && (brain.intent == BotIntent::Melee || d < 15.0f || brain.recentDamage < 5.0f)) return;
            if (!farBuild && brain.recentDamage < 5.0f && !Random::Chance(0.15f)) { brain.deployCooldown = 3.0f; return; }   // la barricata meglio se sotto tiro
            if (!Random::Chance(0.5f)) { brain.deployCooldown = 4.0f; return; }
        }
        brain.deploying = true;
        brain.deployTimer = 0.0f;
        brain.deployUsesBefore = ammo->equipmentUses[slot - 4];
        brain.resupplySlot = slot;
    }

    void BotAISystem::DoMelee(Scene& scene, entt::entity self, BotBrainComponent& brain, float dt) {
        auto& reg = scene.GetRegistry();
        auto* melee = reg.try_get<MeleeStateComponent>(self);
        if (!melee) return;
        auto& t = reg.get<TransformComponent>(self);

        // Affondo durante la carica del colpo (come l'orda)
        if (melee->currentPhase == MeleePhase::Windup && melee->currentAttack != MeleeAttackType::Parry &&
            brain.target != entt::null && reg.valid(brain.target)) {
            if (HorizontalDist(t.Position, reg.get<TransformComponent>(brain.target).Position) > 1.1f) {
                t.Position += YawForward(t.AimYaw) * 3.0f * dt;
            }
        }

        if (brain.intent != BotIntent::Melee || brain.target == entt::null || !Alive(reg, brain.target)) return;
        if (melee->currentPhase != MeleePhase::None || brain.meleeCooldown > 0.0f) {
            // Combo: durante la fase attiva prova a concatenare il colpo successivo (piu' probabile se bravo in mischia)
            if (melee->currentPhase == MeleePhase::Active && melee->bufferedInput.empty() && melee->currentAttack != MeleeAttackType::Parry) {
                const auto& stats = reg.get<UnitStatsComponent>(self);
                if (Random::Chance(std::clamp(stats.meleeSkill / 130.0f, 0.0f, 0.8f) * dt * 6.0f)) {
                    melee->bufferedInput = "F_CLICK";
                    melee->inputBufferTimer = 0.3f;
                }
            }
            return;
        }
        if (AngleDiff(t.AimYaw, YawTowards(t.Position, reg.get<TransformComponent>(brain.target).Position)) > 25.0f) return;

        const auto* weapon = DamageResolver::ResolveMeleeWeapon(reg, self);
        auto has = [&](const char* n) { return weapon && weapon->hasMoveset && weapon->moveset.nodes.count(n) > 0; };
        std::string node = (weapon && !weapon->moveset.entryNode.empty()) ? weapon->moveset.entryNode : "normal_1";
        MeleeAttackType type = MeleeAttackType::Normal;
        const float roll = Random::Float01();
        if (roll < 0.2f && has("heavy_1")) { node = "heavy_1"; type = MeleeAttackType::Heavy; }
        else if (roll < 0.35f && has("low_1")) { node = "low_1"; type = MeleeAttackType::Light; }
        if (!has(node.c_str())) return;

        auto& stats = reg.get<UnitStatsComponent>(self);
        const auto& nodeData = weapon->moveset.nodes.at(node);
        if (stats.stamina < nodeData.staminaCost) return;
        stats.stamina -= nodeData.staminaCost;

        melee->currentAttackNode = node;
        melee->currentAttack = type;
        melee->currentPhase = MeleePhase::Windup;
        melee->stateTimer = 0.0f;
        melee->hasHitInThisAttack = false;
        brain.meleeCooldown = glm::mix(1.6f, 0.6f, std::clamp(stats.meleeSkill / 100.0f, 0.0f, 1.0f));
    }

    void BotAISystem::TryParry(Scene& scene, entt::entity self, BotBrainComponent& brain) {
        if (brain.parryCooldown > 0.0f) return;
        auto& reg = scene.GetRegistry();
        auto* melee = reg.try_get<MeleeStateComponent>(self);
        if (!melee || melee->currentPhase != MeleePhase::None) return;
        const auto& t = reg.get<TransformComponent>(self);
        const int myTeam = TeamOf(reg, self);

        auto attackers = reg.view<MeleeStateComponent, TransformComponent, TeamComponent>(entt::exclude<CorpseComponent>);
        for (auto a : attackers) {
            if (a == self || attackers.get<TeamComponent>(a).team == myTeam) continue;
            const auto& am = attackers.get<MeleeStateComponent>(a);
            if (am.currentPhase != MeleePhase::Windup || am.currentAttack == MeleeAttackType::Parry) continue;
            const auto& at = attackers.get<TransformComponent>(a);
            if (HorizontalDist(at.Position, t.Position) > 2.6f) continue;
            glm::vec3 toMe = t.Position - at.Position; toMe.y = 0.0f;
            if (glm::length(toMe) < 1e-3f || glm::dot(YawForward(at.AimYaw), glm::normalize(toMe)) < 0.6f) continue;

            // Un solo tentativo per attacco: riuscita in base all'abilita' in mischia
            brain.parryCooldown = 1.4f;
            auto& stats = reg.get<UnitStatsComponent>(self);
            if (Random::Chance(std::clamp(stats.meleeSkill / 150.0f, 0.05f, 0.65f)) && stats.stamina >= 10.0f) {
                stats.stamina -= 10.0f;
                melee->currentPhase = MeleePhase::Windup;
                melee->currentAttack = MeleeAttackType::Parry;
                melee->stateTimer = 0.0f;
            }
            return;
        }
    }

}
