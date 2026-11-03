#pragma once
// Munizioni limitate e dotazione tattica.
//
// Regola: ogni arma a distanza ha un caricatore e una riserva ("colpi rimasti"). La riserva iniziale e'
//   magazine_size * spare_magazines (arma) * ammo_multiplier (unita')
// e si consuma solo ricaricando. Cambiare arma non riempie piu' il caricatore: ogni arma ricorda il suo.
// Il caricatore dell'arma in mano sta in WeaponStateComponent::currentAmmo, quelli delle altre in AmmoComponent.
//
// Dotazione tattica "ammo_box" (scatola di munizioni), tenuta in mano (tasti 4-5-6):
//   click sinistro = rifornisce se' stessi, click destro = rifornisce il compagno piu' vicino entro la portata.
//   Ogni rifornimento riporta caricatori e riserva al massimo, dura equipment_use_time secondi e consuma un uso.
//   I bot la usano tramite WeaponTriggerComponent (useEquipment / giveEquipment).
#include "Components.h"
#include "../Scene/Scene.h"
#include "../Core/Input.h"
#include "../Core/Logger/Log.h"
#include "../AssetManager/BlueprintManager.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace SunEngine {

    namespace Ammo {

        // Arma a distanza nello slot 0 (primaria) o 1 (secondaria), se c'e'
        inline const WeaponBlueprint* RangedAt(const InventoryComponent& inv, int idx) {
            if (idx < 0 || idx > 1) return nullptr;
            const auto* bp = BlueprintManager::Get().GetWeapon(idx == 0 ? inv.primaryWeaponId : inv.secondaryWeaponId);
            return (bp && !bp->is_melee && !bp->is_equipment && bp->magazine_size > 0) ? bp : nullptr;
        }

        inline int StartingReserve(const WeaponBlueprint& w, const std::string& unitId) {
            float mult = 1.0f;
            if (const auto* u = BlueprintManager::Get().GetUnit(unitId)) mult = u->ammo_multiplier;
            return std::max(0, static_cast<int>(std::lround(w.magazine_size * w.spare_magazines * mult)));
        }

        // Arma a distanza attualmente in mano (0/1), -1 se in mano c'e' altro
        inline int HeldRangedIndex(const InventoryComponent& inv) {
            if ((inv.currentSlot == 0 || inv.currentSlot == 1) && RangedAt(inv, inv.currentSlot)) return inv.currentSlot;
            return -1;
        }

        // Caricatore dell'arma idx: quello in mano o quello conservato
        inline int Magazine(const AmmoComponent& a, const WeaponStateComponent& ws, int idx) {
            return idx == a.activeIndex ? ws.currentAmmo : a.magazine[idx];
        }

        // Da chiamare ogni frame prima di sparare: se e' cambiata l'arma in mano, conserva il caricatore di
        // quella vecchia e carica quello della nuova.
        inline void SyncHeld(AmmoComponent& a, WeaponStateComponent& ws, const InventoryComponent& inv) {
            const int idx = HeldRangedIndex(inv);
            if (idx < 0 || idx == a.activeIndex) return;
            if (a.activeIndex >= 0) a.magazine[a.activeIndex] = ws.currentAmmo;
            ws.currentAmmo = a.magazine[idx];
            a.activeIndex = idx;
        }

        // Caricatori pieni, riserva piena, usi della dotazione al massimo (inizio partita / cambio unita')
        inline void Init(entt::registry& reg, entt::entity e, const std::string& unitId) {
            auto* inv = reg.try_get<InventoryComponent>(e);
            auto* ws = reg.try_get<WeaponStateComponent>(e);
            if (!inv || !ws) return;
            const bool wasInfinite = reg.all_of<AmmoComponent>(e) && reg.get<AmmoComponent>(e).infinite;
            auto& a = reg.emplace_or_replace<AmmoComponent>(e);
            a.infinite = wasInfinite;
            for (int i = 0; i < 2; ++i) {
                const auto* w = RangedAt(*inv, i);
                a.magazine[i] = w ? w->magazine_size : 0;
                a.reserve[i] = a.maxReserve[i] = w ? StartingReserve(*w, unitId) : 0;
            }
            for (int s = 4; s <= 6; ++s) {
                const auto* bp = BlueprintManager::Get().GetWeapon(inv->GetSlotItemId(s));
                a.equipmentUses[s - 4] = (bp && bp->is_equipment) ? std::max(0, bp->equipment_uses) : 0;
            }
            a.activeIndex = HeldRangedIndex(*inv);
            if (a.activeIndex >= 0) ws->currentAmmo = a.magazine[a.activeIndex];
        }

        // Colpi totali (caricatore + riserva) dell'arma idx
        inline int Total(const AmmoComponent& a, const WeaponStateComponent& ws, int idx) {
            return Magazine(a, ws, idx) + a.reserve[idx];
        }

        // L'unita' ha ancora colpi per l'arma idx? (senza componente munizioni = si', come prima)
        inline bool HasAmmo(entt::registry& reg, entt::entity e, int idx) {
            const auto* a = reg.try_get<AmmoComponent>(e);
            const auto* ws = reg.try_get<WeaponStateComponent>(e);
            if (!a || !ws) return true;
            return a->infinite || Total(*a, *ws, idx) > 0;
        }

        // Colpi mancanti rispetto al massimo (caricatori + riserve di entrambe le armi)
        inline int Missing(entt::registry& reg, entt::entity e) {
            const auto* a = reg.try_get<AmmoComponent>(e);
            const auto* ws = reg.try_get<WeaponStateComponent>(e);
            const auto* inv = reg.try_get<InventoryComponent>(e);
            if (!a || !ws || !inv) return 0;
            int missing = 0;
            for (int i = 0; i < 2; ++i) {
                const auto* w = RangedAt(*inv, i);
                if (!w) continue;
                missing += std::max(0, w->magazine_size - Magazine(*a, *ws, i));
                missing += std::max(0, a->maxReserve[i] - a->reserve[i]);
            }
            return missing;
        }

        // Frazione della riserva rimasta (0..1), per l'IA
        inline float ReserveFraction(entt::registry& reg, entt::entity e) {
            const auto* a = reg.try_get<AmmoComponent>(e);
            if (!a) return 1.0f;
            const int maxR = a->maxReserve[0] + a->maxReserve[1];
            if (maxR <= 0) return 1.0f;
            return static_cast<float>(a->reserve[0] + a->reserve[1]) / static_cast<float>(maxR);
        }

        // Riporta caricatori e riserve al massimo. Ritorna i colpi recuperati.
        inline int Refill(entt::registry& reg, entt::entity e) {
            auto* a = reg.try_get<AmmoComponent>(e);
            auto* ws = reg.try_get<WeaponStateComponent>(e);
            const auto* inv = reg.try_get<InventoryComponent>(e);
            if (!a || !ws || !inv) return 0;
            const int recovered = Missing(reg, e);
            for (int i = 0; i < 2; ++i) {
                const auto* w = RangedAt(*inv, i);
                if (!w) continue;
                if (i == a->activeIndex) ws->currentAmmo = w->magazine_size;
                else a->magazine[i] = w->magazine_size;
                a->reserve[i] = a->maxReserve[i];
            }
            return recovered;
        }

        // Ricarica completata: sposta nel caricatore i colpi che servono, presi dalla riserva
        inline void FinishReload(AmmoComponent& a, WeaponStateComponent& ws, const WeaponBlueprint& w) {
            const int need = std::max(0, w.magazine_size - ws.currentAmmo);
            if (a.activeIndex < 0 || a.infinite) { ws.currentAmmo += need; return; }
            const int take = std::min(need, a.reserve[a.activeIndex]);
            a.reserve[a.activeIndex] -= take;
            ws.currentAmmo += take;
        }

        inline bool CanReload(const AmmoComponent& a) {
            return a.infinite || a.activeIndex < 0 || a.reserve[a.activeIndex] > 0;
        }

    }

    // Uso della dotazione tattica tenuta in mano (giocatore: mouse; bot: WeaponTriggerComponent).
    class EquipmentSystem {
    public:
        void Update(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            auto view = reg.view<AmmoComponent, InventoryComponent, TransformComponent>(entt::exclude<CorpseComponent>);
            for (auto e : view) {
                auto& a = view.get<AmmoComponent>(e);
                const auto& inv = view.get<InventoryComponent>(e);
                if (a.feedbackTimer > 0.0f) a.feedbackTimer -= dt;

                const bool isPlayer = reg.all_of<PlayerTag>(e);
                auto* trigger = reg.try_get<WeaponTriggerComponent>(e);
                bool wantSelf = false, wantGive = false;
                entt::entity giveTarget = entt::null;
                if (isPlayer) {
                    const bool lmb = Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
                    const bool rmb = Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT);
                    wantSelf = lmb && !m_PlayerLmbWasDown;
                    wantGive = rmb && !m_PlayerRmbWasDown;
                    m_PlayerLmbWasDown = lmb;
                    m_PlayerRmbWasDown = rmb;
                }
                else if (trigger) {
                    wantSelf = trigger->useEquipment;
                    wantGive = trigger->giveEquipment;
                    giveTarget = trigger->giveTarget;
                    trigger->useEquipment = trigger->giveEquipment = false;
                }

                const auto* hp = reg.try_get<HealthComponent>(e);
                const WeaponBlueprint* held = BlueprintManager::Get().GetWeapon(inv.GetActiveWeaponId());
                const bool holding = held && held->is_equipment && inv.currentSlot >= 4 && inv.currentSlot <= 6 && hp && hp->currentHealth > 0.0f;
                if (!holding) { a.useTimer = 0.0f; a.useSlot = -1; continue; }   // posata la scatola: annullato

                const int slotIdx = inv.currentSlot - 4;

                // Rifornimento in corso
                if (a.useTimer > 0.0f) {
                    if (a.useSlot != inv.currentSlot || !Valid(reg, a.useTarget) ||
                        (a.useTarget != e && Dist(reg, e, a.useTarget) > held->equipment_range + 0.5f)) {
                        a.useTimer = 0.0f; a.useSlot = -1;
                        Feedback(a, "Rifornimento annullato");
                        continue;
                    }
                    a.useTimer -= dt;
                    if (a.useTimer <= 0.0f) {
                        a.useTimer = 0.0f; a.useSlot = -1;
                        const int got = Ammo::Refill(reg, a.useTarget);
                        a.equipmentUses[slotIdx] = std::max(0, a.equipmentUses[slotIdx] - 1);
                        if (a.useTarget == e) Feedback(a, "Munizioni recuperate: +" + std::to_string(got));
                        else {
                            Feedback(a, "Munizioni date a " + NameOf(reg, a.useTarget) + ": +" + std::to_string(got));
                            if (auto* ta = reg.try_get<AmmoComponent>(a.useTarget)) {
                                ta->feedback = "Munizioni ricevute: +" + std::to_string(got);
                                ta->feedbackTimer = 2.5f;
                            }
                        }
                        if (isPlayer) SUN_CORE_INFO("Scatola di munizioni: +{0} colpi, usi rimasti {1}", got, a.equipmentUses[slotIdx]);
                    }
                    continue;
                }

                if (!wantSelf && !wantGive) continue;
                if (held->equipment_type != "ammo_box") continue;
                if (a.equipmentUses[slotIdx] <= 0) { Feedback(a, "Scatola di munizioni vuota"); continue; }

                entt::entity target = entt::null;
                if (wantSelf) {
                    if (Ammo::Missing(reg, e) <= 0) { Feedback(a, "Munizioni gia' al massimo"); continue; }
                    target = e;
                }
                else {
                    target = (giveTarget != entt::null && Valid(reg, giveTarget) && Dist(reg, e, giveTarget) <= held->equipment_range)
                        ? giveTarget : FindAlly(reg, e, held->equipment_range);
                    if (target == entt::null) {
                        Feedback(a, "Nessun compagno entro " + std::to_string(static_cast<int>(held->equipment_range)) + " m");
                        continue;
                    }
                    if (Ammo::Missing(reg, target) <= 0) { Feedback(a, NameOf(reg, target) + " ha gia' tutte le munizioni"); continue; }
                }
                a.useTarget = target;
                a.useSlot = inv.currentSlot;
                a.useDuration = std::max(0.05f, held->equipment_use_time);
                a.useTimer = a.useDuration;
            }
        }

    private:
        bool m_PlayerLmbWasDown = true;   // true: il click che ha avviato la partita non conta
        bool m_PlayerRmbWasDown = true;

        static bool Valid(entt::registry& reg, entt::entity e) {
            if (e == entt::null || !reg.valid(e) || reg.all_of<CorpseComponent>(e)) return false;
            const auto* h = reg.try_get<HealthComponent>(e);
            return h && h->currentHealth > 0.0f && reg.all_of<AmmoComponent>(e);
        }
        static float Dist(entt::registry& reg, entt::entity a, entt::entity b) {
            const glm::vec3 d = reg.get<TransformComponent>(a).Position - reg.get<TransformComponent>(b).Position;
            return glm::length(glm::vec2(d.x, d.z));
        }
        static void Feedback(AmmoComponent& a, const std::string& text) { a.feedback = text; a.feedbackTimer = 2.0f; }
        static std::string NameOf(entt::registry& reg, entt::entity e) {
            if (const auto* b = reg.try_get<BotBrainComponent>(e)) if (!b->displayName.empty()) return b->displayName;
            return "il compagno";
        }

        // Compagno di squadra entro la portata a cui servono munizioni: preferisce quello verso cui si guarda
        static entt::entity FindAlly(entt::registry& reg, entt::entity self, float range) {
            const auto& st = reg.get<TransformComponent>(self);
            const float yaw = glm::radians(st.AimYaw);
            const glm::vec3 fwd(-std::sin(yaw), 0.0f, -std::cos(yaw));
            entt::entity best = entt::null;
            float bestScore = -1e9f;
            auto view = reg.view<AmmoComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>);
            for (auto o : view) {
                if (o == self || !SameTeam(reg, self, o) || view.get<HealthComponent>(o).currentHealth <= 0.0f) continue;
                glm::vec3 d = view.get<TransformComponent>(o).Position - st.Position; d.y = 0.0f;
                const float dist = glm::length(d);
                if (dist > range) continue;
                const float facing = dist > 1e-3f ? glm::dot(fwd, d / dist) : 1.0f;
                float score = facing * 2.0f - dist * 0.2f;
                if (Ammo::Missing(reg, o) > 0) score += 3.0f;   // meglio chi ne ha bisogno
                if (score > bestScore) { bestScore = score; best = o; }
            }
            return best;
        }
    };

}
