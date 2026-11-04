#pragma once
// Logica delle torrette manovrate (vedi TurretCore.h): entrare/uscire (E), smontare (E tenuto 5 s dal
// proprietario), mira dentro l'arco di brandeggio, colpo caricato (click sinistro tenuto 1,5 s, spara al rilascio;
// oltre i 3 s la torretta si danneggia da sola), ricarica, e il comportamento dei bot che la manovrano.
#include "TurretCore.h"
#include "WeaponSystem.h"
#include "SunPhysicsSystem.h"
#include "AmmoSystem.h"
#include "../Core/Input.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <vector>

namespace SunEngine {

    class TurretSystem {
    public:
        // Stato per l'HUD del giocatore
        entt::entity nearTurret = entt::null;   // torretta vicina in cui puo' entrare
        bool canDismantle = false;              // e' sua: tenendo E la smonta
        float dismantleProgress = 0.0f;         // 0..1
        float exitProgress = 0.0f;              // 0..1 (dentro: E tenuto per uscire)

        void Update(Scene& scene, WeaponSystem& weapons, PhysicsSystem& physics, entt::entity player, bool playerAlive, float dt) {
            auto& reg = scene.GetRegistry();
            Cleanup(scene);
            PlayerInput(scene, physics, player, playerAlive, dt);

            auto view = reg.view<TurretComponent, TransformComponent>();
            for (auto e : view) {
                auto& t = view.get<TurretComponent>(e);
                const auto& bt = view.get<TransformComponent>(e);
                const auto* bp = BlueprintManager::Get().GetWeapon(t.itemId);
                if (!bp) continue;
                // Ricarica: un colpo dalla riserva
                if (t.loaded <= 0 && t.reserve > 0) {
                    if (t.reloadTimer <= 0.0f) t.reloadTimer = std::max(0.5f, bp->reload_time_sec);
                    t.reloadTimer -= dt;
                    if (t.reloadTimer <= 0.0f) { t.reloadTimer = 0.0f; t.loaded = 1; --t.reserve; }
                }
                bool destroyed = false;
                if (t.gunner != entt::null && reg.valid(t.gunner)) {
                    // Il tiratore resta dietro l'arma, girato dove mira
                    if (auto* gt = reg.try_get<TransformComponent>(t.gunner)) {
                        const glm::vec3 seat = Turret::SeatPos(bt, t);
                        gt->Position.x = seat.x; gt->Position.z = seat.z;
                        gt->Rotation.y = t.yaw;
                        if (reg.all_of<PlayerTag>(t.gunner)) physics.SetBodyPosition(scene, t.gunner, gt->Position);
                    }
                    if (reg.all_of<PlayerTag>(t.gunner)) destroyed = PlayerGunner(scene, weapons, e, t, bt, *bp, dt);
                    else destroyed = BotGunner(scene, weapons, e, t, bt, *bp, dt);
                }
                else t.charge = 0.0f;
                if (destroyed) { DestroyTurret(scene, e); continue; }
                // Grafica: l'arma segue la mira
                if (reg.valid(t.gun)) if (auto* g = reg.try_get<TransformComponent>(t.gun)) {
                    g->Position = Turret::PivotPos(bt);
                    g->Rotation = glm::vec3(t.pitch, t.yaw, 0.0f);
                }
            }
        }

        void Clear(Scene& scene) {
            auto& reg = scene.GetRegistry();
            std::vector<entt::entity> kill;
            for (auto e : reg.view<TurretComponent>()) kill.push_back(e);
            for (auto e : kill) DestroyTurret(scene, e);
            std::vector<entt::entity> mounted;
            for (auto e : reg.view<TurretMountComponent>()) mounted.push_back(e);
            for (auto e : mounted) reg.remove<TurretMountComponent>(e);
            nearTurret = entt::null; canDismantle = false; dismantleProgress = exitProgress = 0.0f;
        }

    private:
        float m_EHold = 0.0f;
        bool m_EWasDown = false;
        bool m_EConsumed = false;                // questa pressione di E ha gia' fatto qualcosa (entrata/uscita)
        bool m_LmbWasDown = false;

        static void DestroyTurret(Scene& scene, entt::entity base) {
            auto& reg = scene.GetRegistry();
            if (!reg.valid(base)) return;
            if (auto* t = reg.try_get<TurretComponent>(base)) {
                if (t->gunner != entt::null) Turret::Unmount(reg, t->gunner);
                if (reg.valid(t->gun)) scene.DestroyEntity(t->gun);
            }
            scene.DestroyEntity(base);
        }

        // Torretta distrutta da colpi o esplosioni (entity sparita): via l'arma e fuori il tiratore.
        // Tiratore morto: la torretta resta libera.
        void Cleanup(Scene& scene) {
            auto& reg = scene.GetRegistry();
            std::vector<entt::entity> guns;
            for (auto g : reg.view<TurretGunComponent>()) {
                const auto base = reg.get<TurretGunComponent>(g).base;
                if (!reg.valid(base) || !reg.all_of<TurretComponent>(base)) guns.push_back(g);
            }
            for (auto g : guns) scene.DestroyEntity(g);
            std::vector<entt::entity> out;
            for (auto u : reg.view<TurretMountComponent>()) {
                const auto turret = reg.get<TurretMountComponent>(u).turret;
                const auto* hp = reg.try_get<HealthComponent>(u);
                if (!reg.valid(turret) || !reg.all_of<TurretComponent>(turret) || !hp || hp->currentHealth <= 0.0f || reg.all_of<CorpseComponent>(u)) out.push_back(u);
            }
            for (auto u : out) {
                if (reg.valid(reg.get<TurretMountComponent>(u).turret))
                    if (auto* t = reg.try_get<TurretComponent>(reg.get<TurretMountComponent>(u).turret)) { t->gunner = entt::null; t->charge = 0.0f; }
                reg.remove<TurretMountComponent>(u);
            }
        }

        // E: dentro -> tenuto 1,5 s esce. Fuori, vicino a una torretta libera della sua squadra -> premuto e
        // rilasciato entra; tenuto 5 s (solo il proprietario) la smonta e torna l'uso della dotazione.
        void PlayerInput(Scene& scene, PhysicsSystem& physics, entt::entity player, bool alive, float dt) {
            auto& reg = scene.GetRegistry();
            nearTurret = entt::null; canDismantle = false; dismantleProgress = 0.0f; exitProgress = 0.0f;
            const bool eDown = Input::IsKeyPressed(GLFW_KEY_E);
            const bool pressed = eDown && !m_EWasDown;
            const bool released = !eDown && m_EWasDown;
            m_EWasDown = eDown;
            if (pressed) { m_EHold = 0.0f; m_EConsumed = false; }
            if (eDown) m_EHold += dt;
            if (!alive || !reg.valid(player)) return;

            if (reg.all_of<TurretMountComponent>(player)) {
                if (eDown && !m_EConsumed) {
                    exitProgress = std::clamp(m_EHold / 1.5f, 0.0f, 1.0f);
                    if (m_EHold >= 1.5f) { Turret::Unmount(reg, player); m_EConsumed = true; }
                }
                return;
            }
            const auto& pt = reg.get<TransformComponent>(player);
            const int team = reg.all_of<TeamComponent>(player) ? reg.get<TeamComponent>(player).team : 0;
            float best = 1.9f;
            for (auto e : reg.view<TurretComponent, TransformComponent>()) {
                const auto& t = reg.get<TurretComponent>(e);
                if (t.gunner != entt::null || (t.team != team && t.owner != player)) continue;
                const auto& bt = reg.get<TransformComponent>(e);
                const glm::vec3 seat = Turret::SeatPos(bt, t);
                const float d = glm::length(glm::vec2(pt.Position.x - seat.x, pt.Position.z - seat.z));
                const float d2 = glm::length(glm::vec2(pt.Position.x - bt.Position.x, pt.Position.z - bt.Position.z));
                if (std::min(d, d2) < best) { best = std::min(d, d2); nearTurret = e; }
            }
            if (nearTurret == entt::null) return;
            auto& t = reg.get<TurretComponent>(nearTurret);
            canDismantle = t.owner == player;
            if (eDown && !m_EConsumed && canDismantle && m_EHold > 0.35f) {
                dismantleProgress = std::clamp((m_EHold - 0.35f) / 5.0f, 0.0f, 1.0f);
                if (dismantleProgress >= 1.0f) {
                    // Smontata: l'uso torna nello slot da cui era stata piazzata
                    if (auto* a = reg.try_get<AmmoComponent>(player); a && t.ownerSlot >= 4 && t.ownerSlot <= 6) a->equipmentUses[t.ownerSlot - 4] += 1;
                    DestroyTurret(scene, nearTurret);
                    nearTurret = entt::null;
                    m_EConsumed = true;
                }
                return;
            }
            // Rilascio rapido di E: entra
            if (released && m_EHold <= 0.35f && !m_EConsumed) {
                Turret::Mount(reg, nearTurret, player);
                m_EConsumed = true;
                (void)physics;
            }
        }

        // Giocatore: mira col mouse (limitata all'arco), click sinistro tenuto per caricare il colpo
        bool PlayerGunner(Scene& scene, WeaponSystem& weapons, entt::entity base, TurretComponent& t, const TransformComponent& bt,
                          const WeaponBlueprint& bp, float dt) {
            auto& reg = scene.GetRegistry();
            auto& pt = reg.get<TransformComponent>(t.gunner);
            const float rel = std::clamp(Turret::WrapDeg(pt.AimYaw - t.baseYaw), -Turret::kYawArc, Turret::kYawArc);
            pt.AimYaw = t.baseYaw + rel;
            pt.AimPitch = std::clamp(pt.AimPitch, Turret::kPitchMin, Turret::kPitchMax);
            t.yaw = pt.AimYaw; t.pitch = pt.AimPitch;

            const bool lmb = Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
            const bool released = !lmb && m_LmbWasDown;
            m_LmbWasDown = lmb;
            if (t.loaded <= 0) { t.charge = 0.0f; return false; }
            if (lmb) {
                t.charge += dt;
                // Tenuto troppo: l'arma si surriscalda e si danneggia (1 punto ogni 50 ms)
                if (t.charge > bp.overcharge_time) {
                    auto& sh = reg.get<StructureHealthComponent>(base);
                    sh.hp -= bp.overcharge_damage * dt;
                    if (sh.hp <= 0.0f) return true;
                }
                return false;
            }
            if (released && t.charge >= bp.charge_time) {
                // Verso il punto inquadrato dal mirino
                glm::vec3 dir = Turret::Forward(t.yaw);
                const glm::vec3 muzzle = Turret::MuzzlePos(bt, t);
                if (const auto* aim = reg.try_get<AimRayComponent>(t.gunner); aim && aim->valid) {
                    const auto hit = weapons.RaycastAll(scene, aim->origin, glm::normalize(aim->dir), bp.range_effective + 20.0f, t.gunner, base);
                    const glm::vec3 target = hit.hit ? hit.point : aim->origin + glm::normalize(aim->dir) * (bp.range_effective + 20.0f);
                    if (glm::length(target - muzzle) > 0.5f) dir = glm::normalize(target - muzzle);
                }
                weapons.FireHeavyBeam(scene, t.gunner, base, muzzle, dir, bp);
                t.loaded -= 1;
            }
            t.charge = 0.0f;
            return false;
        }

        // Bot: mira al suo bersaglio (o al nemico piu' vicino in vista dentro l'arco), carica 1,5 s e spara.
        // Esce se un nemico gli arriva a 6 m, se finisce i colpi o dopo 20 s senza bersagli.
        bool BotGunner(Scene& scene, WeaponSystem& weapons, entt::entity base, TurretComponent& t, const TransformComponent& bt,
                       const WeaponBlueprint& bp, float dt) {
            auto& reg = scene.GetRegistry();
            auto& m = reg.get<TurretMountComponent>(t.gunner);
            const glm::vec3 pivot = Turret::PivotPos(bt);
            entt::entity target = entt::null;
            float bestD = bp.range_effective;
            bool threat = false;
            auto units = reg.view<TeamComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent, SquigComponent>);
            for (auto o : units) {
                if (units.get<TeamComponent>(o).team == t.team || units.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
                const glm::vec3 p = units.get<TransformComponent>(o).Position;
                const float d = glm::length(glm::vec2(p.x - pivot.x, p.z - pivot.z));
                if (d < 6.0f) threat = true;
                const float yawTo = glm::degrees(std::atan2(-(p.x - pivot.x), -(p.z - pivot.z)));
                if (std::abs(Turret::WrapDeg(yawTo - t.baseYaw)) > Turret::kYawArc) continue;
                if (d >= bestD) continue;
                const auto los = weapons.RaycastAll(scene, pivot, glm::normalize(p + glm::vec3(0, 1.0f, 0) - pivot), d + 2.0f, t.gunner, base);
                if (los.hitUnit && los.entity == o) { bestD = d; target = o; }
            }
            if (threat || (t.loaded <= 0 && t.reserve <= 0)) { Turret::Unmount(reg, t.gunner); return false; }
            if (target == entt::null) {
                m.idle += dt; t.charge = 0.0f;
                if (m.idle > 20.0f) Turret::Unmount(reg, t.gunner);
                return false;
            }
            m.idle = 0.0f;
            const glm::vec3 aimPt = reg.get<TransformComponent>(target).Position + glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 to = aimPt - pivot;
            const float wantYaw = glm::degrees(std::atan2(-to.x, -to.z));
            const float wantPitch = std::clamp(glm::degrees(std::atan2(to.y, glm::length(glm::vec2(to.x, to.z)))), Turret::kPitchMin, Turret::kPitchMax);
            const float turn = 70.0f * dt;   // brandeggio a 70 gradi al secondo
            t.yaw += std::clamp(Turret::WrapDeg(wantYaw - t.yaw), -turn, turn);
            t.pitch += std::clamp(wantPitch - t.pitch, -turn, turn);
            if (auto* gt = reg.try_get<TransformComponent>(t.gunner)) { gt->AimYaw = t.yaw; gt->AimPitch = t.pitch; }
            if (t.loaded <= 0) { t.charge = 0.0f; return false; }
            const bool onTarget = std::abs(Turret::WrapDeg(wantYaw - t.yaw)) < 3.0f;
            t.charge = onTarget ? t.charge + dt : std::max(0.0f, t.charge - dt);
            if (t.charge >= bp.charge_time) {
                const glm::vec3 muzzle = Turret::MuzzlePos(bt, t);
                glm::vec3 dir = glm::normalize(aimPt - muzzle);
                // Un po' di imprecisione: i bot non centrano sempre
                dir = glm::normalize(dir + glm::vec3(Random::Range(-0.015f, 0.015f), Random::Range(-0.01f, 0.01f), Random::Range(-0.015f, 0.015f)));
                weapons.FireHeavyBeam(scene, t.gunner, base, muzzle, dir, bp);
                t.loaded -= 1;
                t.charge = 0.0f;
            }
            return false;
        }
    };
}
