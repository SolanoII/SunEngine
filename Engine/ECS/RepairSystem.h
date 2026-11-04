#pragma once
// Riparazione con un attrezzo (es. chiave inglese, "repair_tags" nel blueprint dell'arma da mischia).
// Con l'attrezzo in mano (da solo, slot 3) il click destro tenuto ripara l'entita' riparabile davanti a se':
// deve avere RepairableComponent con almeno uno dei tag dell'attrezzo ("Structure", "Construct",
// "Mechanical": veicoli, strutture, torrette, semoventi...) e un HealthComponent non al massimo.
// Oggi si riparano barricate e muri costruiti (StructureHealthComponent); pronto per veicoli e torrette.
#include "Components.h"
#include "../Scene/Scene.h"
#include "../Core/Input.h"
#include "../AssetManager/BlueprintManager.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace SunEngine {

    class RepairSystem {
    public:
        // L'arma in mano e' un attrezzo da riparazione? (allora il click destro non fa l'attacco pesante)
        static const WeaponBlueprint* HeldRepairTool(const InventoryComponent& inv) {
            if (inv.currentSlot != 2) return nullptr;
            const auto* bp = BlueprintManager::Get().GetWeapon(inv.meleeWeaponId);
            return (bp && !bp->repair_tags.empty() && bp->repair_rate > 0.0f) ? bp : nullptr;
        }

        bool repairing = false;   // il giocatore sta riparando in questo frame (animazione della chiave inglese)

        void Update(Scene& scene, entt::entity player, float dt) {
            repairing = false;
            auto& reg = scene.GetRegistry();
            if (player == entt::null || !reg.valid(player)) return;
            const auto* inv = reg.try_get<InventoryComponent>(player);
            const auto* tool = inv ? HeldRepairTool(*inv) : nullptr;
            if (!tool || !Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) return;

            const auto& pt = reg.get<TransformComponent>(player);
            const float yaw = glm::radians(pt.AimYaw);
            const glm::vec3 fwd(-std::sin(yaw), 0.0f, -std::cos(yaw));
            // Bersaglio: entita' riparabile davanti con salute non piena (HealthComponent) o struttura costruita
            // (StructureHealthComponent: barricate, muri). Distanza misurata dal bordo del collider.
            entt::entity best = entt::null;
            float bestD = tool->repair_range;
            auto distTo = [&](entt::entity e) {
                const auto& tr = reg.get<TransformComponent>(e);
                glm::vec3 c = tr.Position, h(0.0f);
                if (const auto* col = reg.try_get<ColliderComponent>(e)) { c = GetColliderWorldCenter(tr, *col); h = col->halfExtents * tr.Scale; }
                const glm::vec2 q(std::max(std::abs(pt.Position.x - c.x) - h.x, 0.0f), std::max(std::abs(pt.Position.z - c.z) - h.z, 0.0f));
                return std::make_pair(glm::length(q), glm::vec3(c.x - pt.Position.x, 0.0f, c.z - pt.Position.z));
            };
            auto consider = [&](entt::entity e, float hp, float maxHp) {
                if (hp <= 0.0f || hp >= maxHp) return;   // distrutta o gia' integra
                if (!reg.get<RepairableComponent>(e).HasAnyTag(tool->repair_tags)) return;
                const auto [dist, toward] = distTo(e);
                if (dist > bestD) return;
                const float len = glm::length(toward);
                if (len > 0.3f && dist > 0.3f && glm::dot(fwd, toward / len) < 0.4f) return;   // deve stare davanti
                bestD = dist; best = e;
            };
            for (auto e : reg.view<RepairableComponent, HealthComponent, TransformComponent>()) {
                const auto& h = reg.get<HealthComponent>(e);
                consider(e, h.currentHealth, h.maxHealth);
            }
            for (auto e : reg.view<RepairableComponent, StructureHealthComponent, TransformComponent>()) {
                const auto& h = reg.get<StructureHealthComponent>(e);
                consider(e, h.hp, h.maxHp);
            }
            auto* ammo = reg.try_get<AmmoComponent>(player);
            if (best == entt::null) {
                if (ammo) { ammo->feedback = "Niente da riparare qui"; ammo->feedbackTimer = 0.3f; }
                return;
            }
            repairing = true;
            float cur, mx;
            if (auto* h = reg.try_get<HealthComponent>(best)) { h->currentHealth = std::min(h->maxHealth, h->currentHealth + tool->repair_rate * dt); cur = h->currentHealth; mx = h->maxHealth; }
            else { auto& sh = reg.get<StructureHealthComponent>(best); sh.hp = std::min(sh.maxHp, sh.hp + tool->repair_rate * dt); cur = sh.hp; mx = sh.maxHp; }
            if (ammo) {
                ammo->feedback = "Riparazione: " + std::to_string(static_cast<int>(cur)) + " / " + std::to_string(static_cast<int>(mx));
                ammo->feedbackTimer = 0.4f;
            }
        }
    };

}
