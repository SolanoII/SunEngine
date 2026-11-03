#pragma once
// Riparazione con un attrezzo (es. chiave inglese, "repair_tags" nel blueprint dell'arma da mischia).
// Con l'attrezzo in mano (da solo, slot 3) il click destro tenuto ripara l'entita' riparabile davanti a se':
// deve avere RepairableComponent con almeno uno dei tag dell'attrezzo ("Structure", "Construct",
// "Mechanical": veicoli, strutture, torrette, semoventi...) e un HealthComponent non al massimo.
// Per ora nel gioco non ci sono entita' riparabili: il sistema e' pronto per veicoli e torrette.
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

        void Update(Scene& scene, entt::entity player, float dt) {
            auto& reg = scene.GetRegistry();
            if (player == entt::null || !reg.valid(player)) return;
            const auto* inv = reg.try_get<InventoryComponent>(player);
            const auto* tool = inv ? HeldRepairTool(*inv) : nullptr;
            if (!tool || !Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) return;

            const auto& pt = reg.get<TransformComponent>(player);
            const float yaw = glm::radians(pt.AimYaw);
            const glm::vec3 fwd(-std::sin(yaw), 0.0f, -std::cos(yaw));
            entt::entity best = entt::null;
            float bestD = tool->repair_range;
            auto view = reg.view<RepairableComponent, HealthComponent, TransformComponent>();
            for (auto e : view) {
                const auto& h = view.get<HealthComponent>(e);
                if (h.currentHealth <= 0.0f || h.currentHealth >= h.maxHealth) continue;   // distrutta o gia' integra
                if (!view.get<RepairableComponent>(e).HasAnyTag(tool->repair_tags)) continue;
                glm::vec3 d = view.get<TransformComponent>(e).Position - pt.Position; d.y = 0.0f;
                const float dist = glm::length(d);
                if (dist > bestD) continue;
                if (dist > 0.3f && glm::dot(fwd, d / dist) < 0.4f) continue;   // deve stare davanti
                bestD = dist; best = e;
            }
            auto* ammo = reg.try_get<AmmoComponent>(player);
            if (best == entt::null) {
                if (ammo) { ammo->feedback = "Niente da riparare qui"; ammo->feedbackTimer = 0.3f; }
                return;
            }
            auto& h = reg.get<HealthComponent>(best);
            h.currentHealth = std::min(h.maxHealth, h.currentHealth + tool->repair_rate * dt);
            if (ammo) {
                ammo->feedback = "Riparazione: " + std::to_string(static_cast<int>(h.currentHealth)) + " / " + std::to_string(static_cast<int>(h.maxHealth));
                ammo->feedbackTimer = 0.4f;
            }
        }
    };

}
