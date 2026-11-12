#pragma once
// Compagni squig dei gretchin (dotazione tattica, slot 4-6: non serve tenerli in mano).
//   "squig"      : segue il padrone; quando il padrone spara (anche mancando il bersaglio) corre verso il nemico
//                  piu' vicino alla linea di tiro e lo morde finche' l'ordine non scade.
//   "squig_bomb" : carica il nemico piu' vicino che vede; se non ne vede, quello a cui il padrone spara.
//                  Arrivato a contatto esplode (danno ad area ai nemici del padrone). Uno solo per partita.
// Lo squig ha uno scheletro a cubi (Skeletons/squig.json) che fa da modello e da hitbox: si puo' colpire e
// uccidere (poca salute, ma schiva il 30% dei colpi). Ucciso, per il padrone e' perso fino a fine partita
// (la morte la gestisce GameLayer::CheckUnitDeaths, con il ragdoll). Sparisce anche se il padrone muore,
// se toglie lo squig dalla dotazione o se si esce dalla partita.
#include "Components.h"
#include "DamageResolver.h"
#include "RaycastUtils.h"
#include "AmmoSystem.h"
#include "ExplosiveSystem.h"
#include "../Scene/Scene.h"
#include "../Navigation/NavMeshQuery.h"
#include "../Core/Logger/Log.h"
#include "../Core/Random.h"
#include "../AssetManager/BlueprintManager.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace SunEngine {

    class SquigSystem {
    public:
        static constexpr float kFollowDistance = 1.4f;
        static constexpr float kVisionRange = 25.0f;     // squig esplosivo: quanto lontano vede i nemici
        static constexpr float kOrderSeconds = 6.0f;     // per quanto insegue il bersaglio indicato dal padrone

        void Update(Scene& scene, NavMeshQuery& nav, float dt, const SkeletonData* skeleton) {
            auto& reg = scene.GetRegistry();
            SyncSquigs(scene, skeleton);

            std::vector<entt::entity> squigs;
            for (auto e : reg.view<SquigComponent, TransformComponent>()) squigs.push_back(e);
            for (auto e : squigs) {
                if (!reg.valid(e) || !Alive(reg, e)) continue;   // squig morto: ci pensa CheckUnitDeaths
                UpdateSquig(scene, nav, e, dt);
            }
        }

        // Elimina tutti gli squig (uscita dalla partita, menu)
        void Clear(Scene& scene) {
            auto& reg = scene.GetRegistry();
            std::vector<entt::entity> toDestroy;
            for (auto e : reg.view<SquigComponent>()) toDestroy.push_back(e);
            for (auto e : toDestroy) if (reg.valid(e)) scene.DestroyEntity(e);
        }

    private:
        static bool Alive(entt::registry& reg, entt::entity e) {
            if (e == entt::null || !reg.valid(e) || reg.all_of<CorpseComponent>(e)) return false;
            const auto* h = reg.try_get<HealthComponent>(e);
            return h && h->currentHealth > 0.0f;
        }

        // Nemico del padrone: vivo, danneggiabile, non della sua squadra
        static bool IsEnemy(entt::registry& reg, entt::entity owner, entt::entity e) {
            return e != owner && Alive(reg, e) && reg.all_of<UnitStatsComponent, TransformComponent>(e) && !SameTeam(reg, owner, e);
        }

        static float FootY(entt::registry& reg, entt::entity e) {
            const auto& t = reg.get<TransformComponent>(e);
            if (const auto* c = reg.try_get<ColliderComponent>(e)) return t.Position.y + (c->localCenterOffset.y - c->halfExtents.y) * t.Scale.y;
            return t.Position.y;
        }
        static glm::vec3 Chest(entt::registry& reg, entt::entity e) {
            const auto& t = reg.get<TransformComponent>(e);
            float h = 1.8f;
            if (const auto* c = reg.try_get<ColliderComponent>(e)) h = std::max(0.6f, c->halfExtents.y * 2.0f * t.Scale.y);
            return glm::vec3(t.Position.x, FootY(reg, e) + h * 0.6f, t.Position.z);
        }
        static float HDist(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); }

        static const WeaponBlueprint* SquigItem(const InventoryComponent& inv, int slot) {
            const auto* bp = BlueprintManager::Get().GetWeapon(inv.GetSlotItemId(slot));
            if (!bp || !bp->is_equipment) return nullptr;
            return (bp->equipment_type == "squig" || bp->equipment_type == "squig_bomb") ? bp : nullptr;
        }

        // Crea gli squig mancanti ed elimina quelli senza piu' padrone
        void SyncSquigs(Scene& scene, const SkeletonData* skeleton) {
            auto& reg = scene.GetRegistry();
            std::vector<std::pair<entt::entity, int>> present;
            std::vector<entt::entity> toDestroy;
            for (auto e : reg.view<SquigComponent>()) {
                const auto& sq = reg.get<SquigComponent>(e);
                bool ok = Alive(reg, sq.owner) && reg.all_of<InventoryComponent, AmmoComponent>(sq.owner);
                if (ok) {
                    const auto& inv = reg.get<InventoryComponent>(sq.owner);
                    ok = inv.GetSlotItemId(sq.ownerSlot) == sq.itemId;
                }
                if (ok) present.push_back({ sq.owner, sq.ownerSlot });
                else toDestroy.push_back(e);
            }
            for (auto e : toDestroy) scene.DestroyEntity(e);

            std::vector<std::pair<entt::entity, int>> toSpawn;
            auto owners = reg.view<InventoryComponent, AmmoComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>);
            for (auto o : owners) {
                if (owners.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
                const auto& inv = owners.get<InventoryComponent>(o);
                const auto& ammo = owners.get<AmmoComponent>(o);
                for (int s = 4; s <= 6; ++s) {
                    const auto* bp = SquigItem(inv, s);
                    if (!bp) continue;
                    if (ammo.equipmentUses[s - 4] <= 0) continue;   // esploso o ucciso: perso fino a fine partita
                    if (std::find(present.begin(), present.end(), std::make_pair(o, s)) != present.end()) continue;
                    toSpawn.push_back({ o, s });
                }
            }
            for (const auto& [o, s] : toSpawn) {
                const auto* bp = SquigItem(reg.get<InventoryComponent>(o), s);
                Spawn(scene, o, s, *bp, skeleton);
            }
        }

        void Spawn(Scene& scene, entt::entity owner, int slot, const WeaponBlueprint& bp, const SkeletonData* skeleton) {
            auto& reg = scene.GetRegistry();
            const auto& ot = reg.get<TransformComponent>(owner);
            const float yaw = glm::radians(ot.Rotation.y);
            const glm::vec3 back(std::sin(yaw), 0.0f, std::cos(yaw));   // dietro al padrone
            const entt::entity e = scene.CreateEntity();
            auto& t = reg.emplace<TransformComponent>(e);
            t.Position = glm::vec3(ot.Position.x, FootY(reg, owner), ot.Position.z) + back * kFollowDistance;
            t.Rotation = glm::vec3(0.0f, ot.Rotation.y, 0.0f);
            reg.emplace<MeshComponent>(e, 0u);
            auto& c = reg.emplace<ColorComponent>(e);
            c.Color = bp.primary_color; c.BaseColor = c.Color;
            t.Scale = glm::vec3(1.0f);
            if (skeleton) {
                // Modello e hitbox: lo scheletro a cubi (corpo, testa, mascella, piedi)
                auto& skel = reg.emplace<SkeletonComponent>(e);
                skel.data = *skeleton;
                skel.scale = 1.0f;
            }
            else {
                t.Scale = glm::vec3(0.40f, 0.35f, 0.45f);   // senza scheletro: un cubo (colpibile col box di riserva dei colpi)
            }
            // Colpibile: salute, statistiche (niente armatura), squadra del padrone (niente fuoco amico), evasione
            reg.emplace<HealthComponent>(e, bp.companion_health, bp.companion_health);
            auto& st = reg.emplace<UnitStatsComponent>(e);
            st.maxHealth = st.currentHealth = bp.companion_health;
            st.toughness = 5.0f;
            st.baseSpeed = bp.companion_speed;
            if (const auto* team = reg.try_get<TeamComponent>(owner)) reg.emplace<TeamComponent>(e, *team);
            reg.emplace<EvasionComponent>(e, EvasionComponent{ std::clamp(bp.companion_evasion, 0.0f, 0.95f) });
            auto& sq = reg.emplace<SquigComponent>(e);
            sq.owner = owner;
            sq.ownerSlot = slot;
            sq.itemId = bp.id;
            sq.kind = bp.equipment_type == "squig_bomb" ? SquigKind::Bomb : SquigKind::Biter;
            if (const auto* ws = reg.try_get<WeaponStateComponent>(owner)) sq.prevOwnerFireCooldown = ws->fireCooldown;
        }

        // Il nemico piu' vicino alla linea di tiro del padrone (anche se il colpo lo ha mancato)
        static entt::entity ShotTarget(entt::registry& reg, entt::entity owner) {
            glm::vec3 origin, dir;
            const auto* aim = reg.try_get<AimRayComponent>(owner);
            if (aim && aim->valid && reg.all_of<PlayerTag>(owner)) { origin = aim->origin; dir = glm::normalize(aim->dir); }
            else {
                const auto& t = reg.get<TransformComponent>(owner);
                origin = Chest(reg, owner);
                const float y = glm::radians(t.AimYaw), p = glm::radians(t.AimPitch);
                dir = glm::normalize(glm::vec3(-std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)));
            }
            entt::entity best = entt::null;
            float bestScore = 1e9f;
            for (auto e : reg.view<UnitStatsComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>)) {
                if (!IsEnemy(reg, owner, e)) continue;
                const glm::vec3 v = Chest(reg, e) - origin;
                const float along = glm::dot(v, dir);
                if (along < 0.0f || along > 60.0f) continue;
                const float perp = glm::length(v - dir * along);
                const float tol = 1.5f + along * 0.06f;
                if (perp > tol) continue;
                const float score = perp / tol + along * 0.005f;
                if (score < bestScore) { bestScore = score; best = e; }
            }
            return best;
        }

        // Squig esplosivo: nemico piu' vicino visibile dallo squig
        static entt::entity NearestVisibleEnemy(Scene& scene, entt::entity squig, entt::entity owner) {
            auto& reg = scene.GetRegistry();
            const glm::vec3 me = reg.get<TransformComponent>(squig).Position + glm::vec3(0.0f, 0.35f, 0.0f);
            entt::entity best = entt::null;
            float bestD = kVisionRange;
            for (auto e : reg.view<UnitStatsComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>)) {
                if (!IsEnemy(reg, owner, e)) continue;
                const glm::vec3 c = Chest(reg, e);
                const float d = HDist(me, c);
                if (d >= bestD) continue;
                if (!HasLineOfSight(scene, me, c, owner)) continue;
                bestD = d; best = e;
            }
            return best;
        }

        void UpdateSquig(Scene& scene, NavMeshQuery& nav, entt::entity e, float dt) {
            auto& reg = scene.GetRegistry();
            auto& sq = reg.get<SquigComponent>(e);
            auto& t = reg.get<TransformComponent>(e);
            const auto* bp = BlueprintManager::Get().GetWeapon(sq.itemId);
            if (!bp || !Alive(reg, sq.owner)) return;
            const auto& ot = reg.get<TransformComponent>(sq.owner);
            sq.attackCooldown -= dt;
            sq.pathTimer -= dt;
            sq.targetTimer -= dt;

            // Il padrone ha sparato? (il cooldown di fuoco e' appena risalito) -> ordine di attacco
            if (const auto* ws = reg.try_get<WeaponStateComponent>(sq.owner)) {
                const bool shot = ws->fireCooldown > sq.prevOwnerFireCooldown + 0.0001f;
                sq.prevOwnerFireCooldown = ws->fireCooldown;
                if (shot) {
                    const entt::entity tgt = ShotTarget(reg, sq.owner);
                    if (tgt != entt::null && (sq.kind == SquigKind::Biter || sq.target == entt::null)) {
                        sq.target = tgt;
                        sq.targetTimer = kOrderSeconds;
                    }
                }
            }

            if (sq.kind == SquigKind::Bomb) {
                // Prima il nemico che vede lui, poi quello indicato dal padrone; una volta scelto non molla
                if (!IsEnemy(reg, sq.owner, sq.target)) sq.target = entt::null;
                if (sq.target == entt::null) sq.target = NearestVisibleEnemy(scene, e, sq.owner);
                sq.state = sq.target != entt::null ? SquigState::Charge : SquigState::Follow;
            }
            else {
                if (!IsEnemy(reg, sq.owner, sq.target) || sq.targetTimer <= 0.0f) sq.target = entt::null;
                sq.state = sq.target != entt::null ? SquigState::Attack : SquigState::Follow;
            }

            // Destinazione
            glm::vec3 goal;
            float speed = bp->companion_speed;
            float stopDist = 0.5f;
            if (sq.state == SquigState::Follow) {
                const float yaw = glm::radians(ot.Rotation.y);
                const glm::vec3 back(std::sin(yaw), 0.0f, std::cos(yaw)), right(std::cos(yaw), 0.0f, -std::sin(yaw));
                goal = glm::vec3(ot.Position.x, FootY(reg, sq.owner), ot.Position.z) + back * kFollowDistance + right * 0.6f;
                const float far = HDist(t.Position, goal);
                if (far > 30.0f) {   // rimasto indietro (es. il padrone ha scavalcato): lo raggiunge subito
                    t.Position = goal;
                    sq.path.clear();
                    return;
                }
                if (far > 5.0f) speed *= 1.35f;
                stopDist = 0.6f;
            }
            else {
                goal = reg.get<TransformComponent>(sq.target).Position;
                goal.y = FootY(reg, sq.target);
                if (sq.state == SquigState::Charge) speed *= 1.2f;
                stopDist = 0.9f;
            }

            // Attacco a contatto
            if (sq.target != entt::null) {
                const float d = HDist(t.Position, goal);
                if (sq.kind == SquigKind::Bomb && d <= 1.4f) { Explode(scene, e, *bp); return; }
                if (sq.kind == SquigKind::Biter && d <= 1.3f && sq.attackCooldown <= 0.0f) {
                    sq.attackCooldown = std::max(0.2f, bp->companion_attack_interval);
                    auto& ts = reg.get<UnitStatsComponent>(sq.target);
                    auto& th = reg.get<HealthComponent>(sq.target);
                    const HitboxLocation loc = Random::Chance(0.5f) ? HitboxLocation::LeftLeg : HitboxLocation::RightLeg;
                    const float before = th.currentHealth;
                    DamageResolver::ResolveHit(bp->base_damage, bp->armor_penetration, loc, ts, th);
                    // Feedback ed esperienza al padrone (i danni dello squig contano come suoi)
                    EmitHitFeedback(scene, sq.owner, sq.target, reg.get<TransformComponent>(sq.target).Position + glm::vec3(0.0f, 0.4f, 0.0f), loc, before);
                    if (reg.all_of<PlayerTag>(sq.owner)) SUN_CORE_INFO("Morso di squig! Salute bersaglio: {0}", th.currentHealth);
                }
            }

            // Movimento sulla navmesh (percorso ricalcolato spesso: il bersaglio si muove)
            if (sq.pathTimer <= 0.0f) {
                sq.pathTimer = sq.state == SquigState::Follow ? 0.5f : 0.3f;
                sq.path = nav.FindPath(t.Position, goal);
                if (!sq.path.empty() && HDist(sq.path.front(), t.Position) < 0.3f) sq.path.erase(sq.path.begin());
            }
            glm::vec3 next = sq.path.empty() ? goal : sq.path.front();
            glm::vec3 dir = next - t.Position; dir.y = 0.0f;
            float dist = glm::length(dir);
            if (!sq.path.empty() && dist < 0.3f) {
                sq.path.erase(sq.path.begin());
                next = sq.path.empty() ? goal : sq.path.front();
                dir = next - t.Position; dir.y = 0.0f;
                dist = glm::length(dir);
            }
            const float toGoal = HDist(t.Position, goal);
            const bool moving = toGoal > stopDist && dist > 1e-3f;
            Animate(reg, e, sq, moving, dt);
            if (moving) {
                const float step = std::min(dist, speed * dt);
                t.Position += dir / dist * step;
                t.Position.y = glm::mix(t.Position.y, next.y, std::min(1.0f, 10.0f * dt));
                t.Rotation.y = glm::degrees(std::atan2(-dir.x, -dir.z));   // il modello guarda verso -Z
            }
            else if (sq.target != entt::null) {
                const glm::vec3 d = goal - t.Position;
                if (glm::length(glm::vec2(d.x, d.z)) > 1e-3f) t.Rotation.y = glm::degrees(std::atan2(-d.x, -d.z));
            }
        }

        // Animazione minima sullo scheletro: piedi che si alternano e corpo che ondeggia quando corre,
        // mascella che si apre quando morde
        static void Animate(entt::registry& reg, entt::entity e, SquigComponent& sq, bool moving, float dt) {
            auto* skel = reg.try_get<SkeletonComponent>(e);
            if (!skel) return;
            sq.hopPhase = moving ? sq.hopPhase + dt * 14.0f : 0.0f;
            const float swing = moving ? std::sin(sq.hopPhase) * 30.0f : 0.0f;
            const bool biting = sq.kind == SquigKind::Biter && sq.attackCooldown > 0.6f;
            for (auto& b : skel->data.blocks) {
                if (b.name == "Leg_L") b.jointRotation.x = swing;
                else if (b.name == "Leg_R") b.jointRotation.x = -swing;
                else if (b.name == "Torso") b.jointRotation.x = moving ? std::abs(std::sin(sq.hopPhase)) * -8.0f : 0.0f;
                else if (b.name == "Jaw") b.jointRotation.x = (biting || sq.state == SquigState::Charge) ? -28.0f : 0.0f;   // bocca spalancata (in giu')
            }
        }

        // Esplosione: stessa regola di granate e mine (nemici del padrone, pieno al centro, 40% al bordo)
        void Explode(Scene& scene, entt::entity squig, const WeaponBlueprint& bp) {
            auto& reg = scene.GetRegistry();
            const auto sq = reg.get<SquigComponent>(squig);
            const glm::vec3 center = reg.get<TransformComponent>(squig).Position;
            const int hit = Explosives::Explode(scene, center, bp.explosion_radius, bp.explosion_damage, bp.armor_penetration,
                                                sq.owner, Explosives::TeamOf(reg, sq.owner), bp.structure_damage_multiplier);
            if (auto* ammo = reg.try_get<AmmoComponent>(sq.owner)) {
                ammo->equipmentUses[sq.ownerSlot - 4] = 0;   // uno solo per partita
                ammo->feedback = "Grugn esplosivo: BOOM! (" + std::to_string(hit) + " colpiti)";
                ammo->feedbackTimer = 2.5f;
            }
            SUN_CORE_INFO("Squig esplosivo detonato: {0} nemici nel raggio", hit);
            scene.DestroyEntity(squig);
        }
    };

}
