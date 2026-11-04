#pragma once
// Esplosivi: granate lanciate (es. Stikkbomb dell'ork boy), mine a pressione e l'esplosione condivisa con lo
// squig esplosivo.
//   Granata ("equipment_type": "grenade"): click sinistro la lancia nella direzione di mira; vola con la gravita',
//     rimbalza su muri e terreno e scoppia dopo fuse_time secondi.
//   Mina ("landmine"): click sinistro la posa davanti ai piedi; dopo fuse_time secondi si arma e scoppia quando un
//     nemico del padrone ci passa entro trigger_radius metri.
// L'esplosione colpisce i nemici del padrone entro explosion_radius: explosion_damage al centro, 40% al bordo,
// con la penetrazione armor_penetration dell'oggetto. Niente fuoco amico, come per le armi.
#include "Components.h"
#include "BlockModel.h"
#include "DamageResolver.h"
#include "HitUtils.h"
#include "../Scene/Scene.h"
#include "../Core/MathUtils.h"
#include "../Core/Logger/Log.h"
#include "../AssetManager/BlueprintManager.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace SunEngine {

    // Lampo dell'esplosione: cubo arancione che si gonfia e sparisce
    struct ExplosionFlashComponent {
        float life = 0.0f;
        float maxLife = 0.3f;
        float radius = 3.0f;
    };

    struct ThrownGrenadeComponent {
        entt::entity owner = entt::null;
        int team = -1;                 // squadra del padrone al lancio (-1 = nessuna)
        glm::vec3 velocity{ 0.0f };
        float fuse = 3.0f;
        float radius = 4.0f;
        float damage = 100.0f;
        int ap = 0;
        bool resting = false;
        bool explodeOnImpact = false;  // stikkbomb: scoppia appena tocca terreno, muro o un nemico
        float structureMult = 1.0f;
    };

    struct LandMineComponent {
        entt::entity owner = entt::null;
        int team = -1;
        float armTimer = 1.5f;         // >0: non ancora armata
        float triggerRadius = 0.9f;
        float radius = 3.5f;
        float damage = 150.0f;
        int ap = 0;
        float structureMult = 1.0f;
    };

    namespace Explosives {

        // Nemico di chi ha piazzato l'esplosivo: viva, colpibile, non della sua squadra. Senza squadra (sopravvivenza)
        // e' nemico chiunque tranne il padrone.
        inline bool IsTarget(entt::registry& reg, entt::entity owner, int team, entt::entity e) {
            if (e == owner || !reg.valid(e) || reg.all_of<CorpseComponent>(e)) return false;
            if (!reg.all_of<UnitStatsComponent, TransformComponent, HealthComponent>(e)) return false;
            if (reg.get<HealthComponent>(e).currentHealth <= 0.0f) return false;
            if (const auto* sq = reg.try_get<SquigComponent>(e); sq && sq->owner == owner) return false;   // il proprio squig
            if (team < 0) return true;
            const auto* t = reg.try_get<TeamComponent>(e);
            return !t || t->team != team;
        }

        inline int TeamOf(entt::registry& reg, entt::entity e) {
            const auto* t = (e != entt::null && reg.valid(e)) ? reg.try_get<TeamComponent>(e) : nullptr;
            return t ? t->team : -1;
        }

        // Esplosione: ritorna quanti bersagli ha colpito. structureMult = danno a strutture e mezzi (barricate, muri,
        // entita' riparabili "Structure"/"Construct"/"Mechanical")
        inline int Explode(Scene& scene, const glm::vec3& center, float radius, float damage, int ap, entt::entity owner, int team,
                           float structureMult = 1.0f) {
            auto& reg = scene.GetRegistry();
            radius = std::max(0.5f, radius);
            int hit = 0;
            std::vector<entt::entity> victims;
            for (auto e : reg.view<UnitStatsComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>))
                if (IsTarget(reg, owner, team, e)) victims.push_back(e);
            for (auto e : victims) {
                const glm::vec3 p = reg.get<TransformComponent>(e).Position;
                const float d = glm::length(glm::vec2(p.x - center.x, p.z - center.z));
                if (d > radius || std::abs(p.y - center.y) > radius) continue;
                auto& h = reg.get<HealthComponent>(e);
                const float before = h.currentHealth;
                DamageResolver::ResolveHit(damage * (1.0f - 0.6f * (d / radius)), ap, HitboxLocation::Torso, reg.get<UnitStatsComponent>(e), h);
                if (owner != entt::null && reg.valid(owner)) EmitHitFeedback(scene, owner, e, p + glm::vec3(0.0f, 1.0f, 0.0f), HitboxLocation::Torso, before);
                ++hit;
            }
            // Strutture costruite (barricate, muri con feritoia): si rompono a zero
            std::vector<entt::entity> broken;
            for (auto e : reg.view<StructureHealthComponent, TransformComponent>()) {
                const glm::vec3 p = reg.get<TransformComponent>(e).Position;
                float d = glm::length(glm::vec2(p.x - center.x, p.z - center.z));
                if (const auto* col = reg.try_get<ColliderComponent>(e)) {   // distanza dal bordo, non dal centro
                    const auto& tr = reg.get<TransformComponent>(e);
                    const glm::vec3 c = GetColliderWorldCenter(tr, *col), h = col->halfExtents * tr.Scale;
                    const glm::vec2 q(std::max(std::abs(center.x - c.x) - h.x, 0.0f), std::max(std::abs(center.z - c.z) - h.z, 0.0f));
                    d = glm::length(q);
                }
                if (d > radius) continue;
                if (DamageStructure(reg, e, damage * (1.0f - 0.6f * (d / radius)) * structureMult)) broken.push_back(e);
            }
            for (auto e : broken) { SUN_CORE_INFO("Struttura distrutta"); scene.DestroyEntity(e); }
            // Entita' riparabili con salute (veicoli, torrette... quando ci saranno)
            for (auto e : reg.view<RepairableComponent, HealthComponent, TransformComponent>(entt::exclude<UnitStatsComponent, CorpseComponent>)) {
                const glm::vec3 p = reg.get<TransformComponent>(e).Position;
                const float d = glm::length(glm::vec2(p.x - center.x, p.z - center.z));
                if (d > radius) continue;
                auto& h = reg.get<HealthComponent>(e);
                h.currentHealth = std::max(0.0f, h.currentHealth - damage * (1.0f - 0.6f * (d / radius)) * structureMult);
            }
            // Lampo
            const entt::entity f = scene.CreateEntity();
            auto& ft = reg.emplace<TransformComponent>(f);
            ft.Position = center + glm::vec3(0.0f, 0.4f, 0.0f);
            ft.Scale = glm::vec3(0.2f);
            reg.emplace<MeshComponent>(f, 0u);
            auto& fc = reg.emplace<ColorComponent>(f);
            fc.Color = glm::vec4(1.0f, 0.55f, 0.1f, 1.0f); fc.BaseColor = fc.Color;
            reg.emplace<ExplosionFlashComponent>(f, ExplosionFlashComponent{ 0.0f, 0.3f, radius });
            return hit;
        }

        inline entt::entity MakeVisual(Scene& scene, const glm::vec3& pos, float yaw, const std::string& modelId,
                                       const BlockModelLibrary* models, const glm::vec4& color, const glm::vec3& fallbackSize) {
            auto& reg = scene.GetRegistry();
            const entt::entity e = scene.CreateEntity();
            auto& t = reg.emplace<TransformComponent>(e);
            t.Position = pos;
            t.Rotation = glm::vec3(0.0f, yaw, 0.0f);
            reg.emplace<MeshComponent>(e, 0u);
            auto& c = reg.emplace<ColorComponent>(e);
            c.Color = color; c.BaseColor = color;
            if (const BlockModel* m = models ? models->Find(modelId) : nullptr) {
                reg.emplace<BlockModelComponent>(e, BlockModelComponent{ m });
                t.Scale = glm::vec3(1.0f);
            }
            else t.Scale = fallbackSize;
            return e;
        }

        inline void ThrowGrenade(Scene& scene, entt::entity owner, const WeaponBlueprint& bp, const glm::vec3& origin,
                                 const glm::vec3& velocity, const BlockModelLibrary* models) {
            auto& reg = scene.GetRegistry();
            const float yaw = glm::degrees(std::atan2(-velocity.x, -velocity.z));
            const entt::entity g = MakeVisual(scene, origin, yaw, bp.id, models, bp.primary_color, glm::vec3(0.07f, 0.07f, 0.30f));
            ThrownGrenadeComponent gc;
            gc.owner = owner;
            gc.team = TeamOf(reg, owner);
            gc.velocity = velocity;
            gc.fuse = std::max(0.3f, bp.fuse_time);
            gc.radius = bp.explosion_radius;
            gc.damage = bp.explosion_damage;
            gc.ap = bp.armor_penetration;
            gc.explodeOnImpact = bp.explode_on_impact;
            gc.structureMult = bp.structure_damage_multiplier;
            reg.emplace<ThrownGrenadeComponent>(g, gc);
        }

        inline void PlaceMine(Scene& scene, entt::entity owner, const WeaponBlueprint& bp, const glm::vec3& pos, float yaw,
                              const BlockModelLibrary* models) {
            auto& reg = scene.GetRegistry();
            const entt::entity m = MakeVisual(scene, pos, yaw, bp.id, models, bp.primary_color, glm::vec3(0.30f, 0.06f, 0.30f));
            LandMineComponent mc;
            mc.owner = owner;
            mc.team = TeamOf(reg, owner);
            mc.armTimer = std::max(0.0f, bp.fuse_time);
            mc.triggerRadius = bp.trigger_radius;
            mc.radius = bp.explosion_radius;
            mc.damage = bp.explosion_damage;
            mc.ap = bp.armor_penetration;
            mc.structureMult = bp.structure_damage_multiplier;
            reg.emplace<LandMineComponent>(m, mc);
        }

        // Velocita' di lancio per arrivare su 'target' con un angolo di ~40 gradi (per i bot)
        inline glm::vec3 BallisticVelocity(const glm::vec3& from, const glm::vec3& target, float maxSpeed) {
            const glm::vec3 d = target - from;
            const float horiz = std::max(0.5f, glm::length(glm::vec2(d.x, d.z)));
            const glm::vec2 dir2 = glm::vec2(d.x, d.z) / horiz;
            constexpr float g = 9.81f;
            for (float deg : { 40.0f, 50.0f, 60.0f }) {
                const float th = glm::radians(deg);
                const float denom = 2.0f * std::cos(th) * std::cos(th) * (horiz * std::tan(th) - d.y);
                if (denom <= 0.01f) continue;
                const float v = std::sqrt(g * horiz * horiz / denom);
                if (v > maxSpeed) continue;
                return glm::vec3(dir2.x * std::cos(th) * v, std::sin(th) * v, dir2.y * std::cos(th) * v);
            }
            const float th = glm::radians(40.0f);   // troppo lontano: lancio al massimo
            return glm::vec3(dir2.x * std::cos(th) * maxSpeed, std::sin(th) * maxSpeed, dir2.y * std::cos(th) * maxSpeed);
        }
    }

    class ExplosiveSystem {
    public:
        void Update(Scene& scene, float dt) {
            UpdateGrenades(scene, dt);
            UpdateMines(scene, dt);
            UpdateFlashes(scene, dt);
        }

        // Fine partita: via granate, mine e lampi rimasti
        void Clear(Scene& scene) {
            auto& reg = scene.GetRegistry();
            std::vector<entt::entity> all;
            for (auto e : reg.view<ThrownGrenadeComponent>()) all.push_back(e);
            for (auto e : reg.view<LandMineComponent>()) all.push_back(e);
            for (auto e : reg.view<ExplosionFlashComponent>()) all.push_back(e);
            for (auto e : all) if (reg.valid(e)) scene.DestroyEntity(e);
        }

    private:
        // Rimbalzo sui collider statici (muri, casse, barricate): inverte la componente con cui si e' entrati
        static bool BounceOnWalls(Scene& scene, const glm::vec3& from, glm::vec3& to, glm::vec3& vel) {
            auto& reg = scene.GetRegistry();
            auto view = reg.view<ColliderComponent, TransformComponent>(entt::exclude<PlayerTag, HealthComponent, CorpseComponent>);
            for (auto e : view) {
                const auto& col = view.get<ColliderComponent>(e);
                const auto& tr = view.get<TransformComponent>(e);
                const glm::vec3 half = col.halfExtents * tr.Scale + glm::vec3(0.04f);
                const glm::vec3 c = GetColliderWorldCenter(tr, col);
                const glm::vec3 lo = c - half, hi = c + half;
                const bool inside = to.x > lo.x && to.x < hi.x && to.y > lo.y && to.y < hi.y && to.z > lo.z && to.z < hi.z;
                if (!inside) continue;
                if (from.y >= hi.y - 0.02f) continue;   // arriva dall'alto: ci pensa il terreno
                bool bounced = false;
                if (from.x <= lo.x || from.x >= hi.x) { vel.x = -vel.x * 0.4f; bounced = true; }
                if (from.z <= lo.z || from.z >= hi.z) { vel.z = -vel.z * 0.4f; bounced = true; }
                if (!bounced) { vel.x = -vel.x * 0.4f; vel.z = -vel.z * 0.4f; }
                to.x = from.x; to.z = from.z;
                return true;
            }
            return false;
        }

        void UpdateGrenades(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            std::vector<entt::entity> boom, lost;
            for (auto e : reg.view<ThrownGrenadeComponent, TransformComponent>()) {
                auto& g = reg.get<ThrownGrenadeComponent>(e);
                auto& t = reg.get<TransformComponent>(e);
                g.fuse -= dt;
                if (g.fuse <= 0.0f) { boom.push_back(e); continue; }
                if (g.resting) continue;
                g.velocity.y -= 9.81f * dt;
                glm::vec3 next = t.Position + g.velocity * dt;
                const bool hitWall = BounceOnWalls(scene, t.Position, next, g.velocity);
                const auto ground = GetGroundHeightAt(scene, next, t.Position.y + 0.3f, 0.05f);
                const bool hitGround = ground && next.y <= *ground + 0.05f;
                if (g.explodeOnImpact) {
                    // A impatto: muro, terreno o un nemico sulla traiettoria
                    bool hitUnit = false;
                    for (auto u : reg.view<UnitStatsComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>)) {
                        if (!Explosives::IsTarget(reg, g.owner, g.team, u)) continue;
                        const glm::vec3 up = reg.get<TransformComponent>(u).Position;
                        if (glm::length(glm::vec2(up.x - next.x, up.z - next.z)) < 0.6f && next.y > up.y - 0.2f && next.y < up.y + 2.2f) { hitUnit = true; break; }
                    }
                    if (hitWall || hitGround || hitUnit) {
                        if (hitGround) next.y = *ground + 0.05f;
                        t.Position = next;
                        boom.push_back(e);
                        continue;
                    }
                }
                if (hitGround) {
                    next.y = *ground + 0.05f;
                    if (g.velocity.y < 0.0f) g.velocity.y = -g.velocity.y * 0.3f;
                    g.velocity.x *= 0.6f; g.velocity.z *= 0.6f;
                    if (glm::length(g.velocity) < 0.6f) { g.velocity = glm::vec3(0.0f); g.resting = true; }
                }
                if (next.y < -60.0f) { lost.push_back(e); continue; }   // caduta fuori dalla mappa
                t.Position = next;
                if (!g.resting) t.Rotation.x += 540.0f * dt;   // il manico gira in volo
            }
            for (auto e : boom) {
                if (!reg.valid(e)) continue;
                const auto g = reg.get<ThrownGrenadeComponent>(e);
                const int hit = Explosives::Explode(scene, reg.get<TransformComponent>(e).Position, g.radius, g.damage, g.ap, g.owner, g.team, g.structureMult);
                SUN_CORE_INFO("Granata esplosa: {0} colpiti", hit);
                scene.DestroyEntity(e);
            }
            for (auto e : lost) if (reg.valid(e)) scene.DestroyEntity(e);
        }

        void UpdateMines(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            std::vector<entt::entity> boom;
            for (auto e : reg.view<LandMineComponent, TransformComponent>()) {
                auto& m = reg.get<LandMineComponent>(e);
                if (m.armTimer > 0.0f) { m.armTimer -= dt; continue; }
                const glm::vec3 p = reg.get<TransformComponent>(e).Position;
                for (auto u : reg.view<UnitStatsComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>)) {
                    if (!Explosives::IsTarget(reg, m.owner, m.team, u)) continue;
                    const glm::vec3 up = reg.get<TransformComponent>(u).Position;
                    if (glm::length(glm::vec2(up.x - p.x, up.z - p.z)) <= m.triggerRadius && std::abs(up.y - p.y) < 1.5f) { boom.push_back(e); break; }
                }
            }
            for (auto e : boom) {
                if (!reg.valid(e)) continue;
                const auto m = reg.get<LandMineComponent>(e);
                const int hit = Explosives::Explode(scene, reg.get<TransformComponent>(e).Position, m.radius, m.damage, m.ap, m.owner, m.team, m.structureMult);
                SUN_CORE_INFO("Mina esplosa: {0} colpiti", hit);
                scene.DestroyEntity(e);
            }
        }

        void UpdateFlashes(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            std::vector<entt::entity> done;
            for (auto e : reg.view<ExplosionFlashComponent, TransformComponent>()) {
                auto& f = reg.get<ExplosionFlashComponent>(e);
                f.life += dt;
                const float k = std::clamp(f.life / f.maxLife, 0.0f, 1.0f);
                reg.get<TransformComponent>(e).Scale = glm::vec3(glm::mix(0.3f, f.radius * 1.6f, k));
                if (auto* c = reg.try_get<ColorComponent>(e)) c->Color = glm::mix(glm::vec4(1.0f, 0.85f, 0.3f, 1.0f), glm::vec4(0.6f, 0.15f, 0.05f, 1.0f), k);
                if (f.life >= f.maxLife) done.push_back(e);
            }
            for (auto e : done) scene.DestroyEntity(e);
        }
    };

}
