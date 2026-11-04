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
//
// Scrap ("scrap_barricade"): fermo, click sinistro tenuto per equipment_use_time secondi -> barricata davanti a
//   se' (collider statico: ferma colpi, linea di vista e movimento del giocatore). Muoversi azzera la canalizzazione.
// Zaino di cibo ("food_pack"): click sinistro lo posa ai piedi; per effect_duration secondi cura heal_per_second
//   ai compagni (stessa squadra) entro equipment_range metri.
// Vox caster ("vox_caster"): click sinistro e equipment_use_time secondi di chiamata (ci si puo' muovere), poi
//   dopo summon_delay secondi arriva un rinforzo summon_unit che segue chi l'ha chiamato (lo crea GameLayer).
#include "Components.h"
#include "../Scene/Scene.h"
#include "../Core/Input.h"
#include "../Core/Logger/Log.h"
#include "../Core/Random.h"
#include "../AssetManager/BlueprintManager.h"
#include "BlockModel.h"
#include "ExplosiveSystem.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <string>
#include "TurretCore.h"

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
            // Zaino munizioni nella dotazione: riserva aumentata (es. +50%)
            float capacity = 1.0f;
            for (int s = 4; s <= 6; ++s) {
                const auto* bp = BlueprintManager::Get().GetWeapon(inv->GetSlotItemId(s));
                if (bp && bp->is_equipment && bp->ammo_capacity_bonus > 0.0f) capacity += bp->ammo_capacity_bonus;
            }
            for (int i = 0; i < 2; ++i) {
                const auto* w = RangedAt(*inv, i);
                a.magazine[i] = w ? w->magazine_size : 0;
                a.reserve[i] = a.maxReserve[i] = w ? static_cast<int>(std::lround(StartingReserve(*w, unitId) * capacity)) : 0;
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
        void Update(Scene& scene, float dt, const BlockModelLibrary* models = nullptr) {
            auto& reg = scene.GetRegistry();
            UpdateHealZones(scene, dt);
            auto view = reg.view<AmmoComponent, InventoryComponent, TransformComponent>(entt::exclude<CorpseComponent>);
            for (auto e : view) {
                auto& a = view.get<AmmoComponent>(e);
                const auto& inv = view.get<InventoryComponent>(e);
                if (a.feedbackTimer > 0.0f) a.feedbackTimer -= dt;
                if (reg.all_of<TurretMountComponent>(e)) { a.useTimer = 0.0f; a.useSlot = -1; continue; }   // dentro una torretta

                const bool isPlayer = reg.all_of<PlayerTag>(e);
                auto* trigger = reg.try_get<WeaponTriggerComponent>(e);
                bool wantSelf = false, wantGive = false, holdUse = false;
                entt::entity giveTarget = entt::null;
                if (isPlayer) {
                    const bool lmb = Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT);
                    holdUse = lmb;
                    const bool rmb = Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT);
                    wantSelf = lmb && !m_PlayerLmbWasDown;
                    wantGive = rmb && !m_PlayerRmbWasDown;
                    m_PlayerLmbWasDown = lmb;
                    m_PlayerRmbWasDown = rmb;
                }
                else if (trigger) {
                    holdUse = trigger->holdEquipment;   // l'IA tiene premuto per costruire
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

                if (held->equipment_type == "scrap_barricade" || held->equipment_type == "slit_wall" || held->equipment_type == "turret") {
                    UpdateBarricade(scene, e, a, *held, inv.currentSlot, holdUse, wantSelf, dt, models);
                    continue;
                }
                if (held->equipment_type == "food_pack") {
                    if (wantSelf) DropFood(scene, e, a, *held, slotIdx, models);
                    continue;
                }
                if (held->equipment_type == "grenade") {
                    if (wantSelf) ThrowGrenade(scene, e, a, *held, slotIdx, giveTarget, models);
                    continue;
                }
                if (held->equipment_type == "landmine") {
                    if (wantSelf) PlaceMine(scene, e, a, *held, slotIdx, models);
                    continue;
                }
                if (held->equipment_type == "medkit") {
                    UpdateMedkit(scene, e, a, *held, inv.currentSlot, wantSelf, dt);
                    continue;
                }
                if (held->equipment_type == "vox_caster") {
                    UpdateVoxCaster(scene, e, a, *held, inv.currentSlot, wantSelf, dt);
                    continue;
                }

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
                        EmitXp(e, a.useTarget == e ? XpCategory::AmmoSelf : XpCategory::AmmoAlly,
                               a.useTarget == e ? XpValues::AmmoSelf : XpValues::AmmoAlly);
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

        static float FootY(entt::registry& reg, entt::entity e) {
            const auto& t = reg.get<TransformComponent>(e);
            if (const auto* c = reg.try_get<ColliderComponent>(e)) return t.Position.y + (c->localCenterOffset.y - c->halfExtents.y) * t.Scale.y;
            return t.Position.y;
        }

        // Scrap: canalizzazione da fermi, poi barricata davanti (orientata sull'asse piu' vicino alla mira,
        // perche' i collider del motore sono box allineati agli assi)
        void UpdateBarricade(Scene& scene, entt::entity e, AmmoComponent& a, const WeaponBlueprint& bp, int slot,
                             bool holding, bool pressed, float dt, const BlockModelLibrary* models) {
            auto& reg = scene.GetRegistry();
            const int slotIdx = slot - 4;
            const auto& t = reg.get<TransformComponent>(e);
            if (a.equipmentUses[slotIdx] <= 0) {
                if (pressed) Feedback(a, "Scrap finiti");
                a.useTimer = 0.0f;
                return;
            }
            if (!holding) {
                if (a.useTimer > 0.0f) Feedback(a, "Costruzione annullata");
                a.useTimer = 0.0f; a.useSlot = -1;
                return;
            }
            const glm::vec3 here(t.Position.x, 0.0f, t.Position.z);
            if (a.useTimer <= 0.0f || a.useSlot != slot) {
                a.useStartPos = here;
                a.useDuration = std::max(0.1f, bp.equipment_use_time);
                a.useTimer = a.useDuration;
                a.useSlot = slot;
                a.useTarget = e;
                return;
            }
            if (glm::length(here - a.useStartPos) > 0.25f) {   // si e' mosso: si ricomincia da capo
                a.useStartPos = here;
                a.useTimer = a.useDuration;
                Feedback(a, "Resta fermo per costruire");
                return;
            }
            a.useTimer -= dt;
            if (a.useTimer > 0.0f) return;
            a.useTimer = 0.0f; a.useSlot = -1;

            if (bp.equipment_type == "turret") {
                // Arma pesante (es. lascannon): torretta davanti, si manovra con E
                Turret::Spawn(scene, e, bp, slot, FootY(reg, e), models);
                a.equipmentUses[slotIdx] -= 1;
                Feedback(a, bp.displayName + " piazzato: E per usarlo");
                return;
            }
            BuildStructure(scene, e, bp, models);
            EmitXp(e, XpCategory::Build, XpValues::Build);
            a.equipmentUses[slotIdx] -= 1;
            Feedback(a, std::string(bp.equipment_type == "slit_wall" ? "Muro costruito" : "Barricata costruita") +
                        " (" + std::to_string(a.equipmentUses[slotIdx]) + " rimaste)");
        }

        // Un blocco di struttura (collider statico + salute da struttura + riparabile con la chiave inglese)
        static entt::entity SpawnStructurePiece(Scene& scene, entt::entity owner, const glm::vec3& center, const glm::vec3& worldHalf, float rotY,
                                                const BlockModel* model, const glm::vec3& modelOrigin, const glm::vec4& color, float hp) {
            auto& reg = scene.GetRegistry();
            const entt::entity b = scene.CreateEntity();
            auto& bt = reg.emplace<TransformComponent>(b);
            reg.emplace<MeshComponent>(b, 0u);
            auto& c = reg.emplace<ColorComponent>(b);
            c.Color = color; c.BaseColor = color;
            auto& col = reg.emplace<ColliderComponent>(b);
            if (model) {
                bt.Position = modelOrigin;
                bt.Rotation = glm::vec3(0.0f, rotY, 0.0f);
                bt.Scale = glm::vec3(1.0f);
                reg.emplace<BlockModelComponent>(b, BlockModelComponent{ model });
                col.halfExtents = worldHalf;
                col.localCenterOffset = center - modelOrigin;   // il modello ha l'origine a terra
            }
            else {
                bt.Position = center;          // cubo semplice: niente rotazione, dimensioni gia' sugli assi del mondo
                bt.Scale = worldHalf * 2.0f;
                col.halfExtents = glm::vec3(0.5f);
                col.localCenterOffset = glm::vec3(0.0f);
            }
            reg.emplace<BarricadeTag>(b);
            StructureHealthComponent sh;
            sh.hp = sh.maxHp = hp;
            sh.owner = owner;
            reg.emplace<StructureHealthComponent>(b, sh);
            reg.emplace<RepairableComponent>(b, RepairableComponent{ { "Structure", "Construct" } });
            return b;
        }

        // Barricata (scrap dello yoof) o muro con feritoia (loota) davanti a chi costruisce, ruotato di 0 o 90
        // gradi secondo la mira (i collider del motore sono box allineati agli assi)
        static void BuildStructure(Scene& scene, entt::entity e, const WeaponBlueprint& bp, const BlockModelLibrary* models) {
            auto& reg = scene.GetRegistry();
            const auto& t = reg.get<TransformComponent>(e);
            float yaw = std::fmod(t.AimYaw, 360.0f); if (yaw < 0.0f) yaw += 360.0f;
            const float snapped = std::round(yaw / 90.0f) * 90.0f;
            const bool sideways = static_cast<int>(std::lround(snapped / 90.0f)) % 2 != 0;
            const float yr = glm::radians(t.AimYaw);
            const glm::vec3 fwd(-std::sin(yr), 0.0f, -std::cos(yr));
            const glm::vec3 size = bp.barricade_size;
            const float depth = sideways ? size.x : size.z;
            const glm::vec3 base = glm::vec3(t.Position.x, FootY(reg, e), t.Position.z) + fwd * (1.1f + depth * 0.5f);
            // asse "lungo" del muro nel mondo (X se dritto, Z se ruotato di 90)
            const glm::vec3 along = sideways ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            auto worldHalf = [&](float w, float h, float d) { return sideways ? glm::vec3(d, h, w) * 0.5f : glm::vec3(w, h, d) * 0.5f; };
            const glm::vec4 color = bp.primary_color;
            if (bp.equipment_type == "slit_wall") {
                // Muro con feritoia: due pilastri, parapetto sotto e architrave sopra la fessura (da 1.1 a 1.5 m)
                const float w = size.x, h = size.y, d = size.z, slitW = 0.45f, slitLo = 1.1f, slitHi = 1.5f;
                const float sideW = (w - slitW) * 0.5f;
                const BlockModel* pillar = models ? models->Find("slit_wall_pillar") : nullptr;
                for (int s = -1; s <= 1; s += 2) {
                    const glm::vec3 c = base + along * (s * (slitW * 0.5f + sideW * 0.5f)) + glm::vec3(0.0f, h * 0.5f, 0.0f);
                    SpawnStructurePiece(scene, e, c, worldHalf(sideW, h, d), snapped, pillar, glm::vec3(c.x, base.y, c.z), color, bp.structure_health);
                }
                SpawnStructurePiece(scene, e, base + glm::vec3(0.0f, slitLo * 0.5f, 0.0f), worldHalf(slitW, slitLo, d), snapped, nullptr, base, color, bp.structure_health * 0.6f);
                SpawnStructurePiece(scene, e, base + glm::vec3(0.0f, (slitHi + h) * 0.5f, 0.0f), worldHalf(slitW, h - slitHi, d), snapped, nullptr, base, color, bp.structure_health * 0.6f);
                return;
            }
            const BlockModel* model = models ? models->Find("scrap_barricade") : nullptr;
            SpawnStructurePiece(scene, e, base + glm::vec3(0.0f, size.y * 0.5f, 0.0f), worldHalf(size.x, size.y, size.z), snapped, model, base,
                                glm::vec4(0.40f, 0.34f, 0.28f, 1.0f), bp.structure_health);
        }

        // Zaino di cibo: posato ai piedi, cura chi e' nel cerchio
        // Medikit: click e 'equipment_use_time' secondi col medikit in mano (ci si puo' muovere), poi +heal_amount
        // salute. Cambiare oggetto lo annulla (vedi 'holding' in Update).
        void UpdateMedkit(Scene& scene, entt::entity e, AmmoComponent& a, const WeaponBlueprint& bp, int slot, bool pressed, float dt) {
            auto& reg = scene.GetRegistry();
            const int slotIdx = slot - 4;
            auto* hp = reg.try_get<HealthComponent>(e);
            if (!hp) return;
            if (a.useTimer > 0.0f && a.useSlot == slot) {
                a.useTimer -= dt;
                if (a.useTimer > 0.0f) return;
                a.useTimer = 0.0f; a.useSlot = -1;
                const float before = hp->currentHealth;
                hp->currentHealth = std::min(hp->maxHealth, hp->currentHealth + bp.heal_amount);
                a.equipmentUses[slotIdx] = std::max(0, a.equipmentUses[slotIdx] - 1);
                EmitXp(e, XpCategory::Heal, XpValues::HealTick);
                Feedback(a, "Medicato: +" + std::to_string(static_cast<int>(hp->currentHealth - before)) + " salute");
                return;
            }
            if (!pressed) return;
            if (a.equipmentUses[slotIdx] <= 0) { Feedback(a, "Medikit gia' usato"); return; }
            if (hp->currentHealth >= hp->maxHealth) { Feedback(a, "Salute gia' piena"); return; }
            a.useDuration = std::max(0.1f, bp.equipment_use_time);
            a.useTimer = a.useDuration;
            a.useSlot = slot;
            a.useTarget = e;
            Feedback(a, "Medicazione in corso...");
        }

        // Vox caster: chiamata di equipment_use_time secondi, poi il rinforzo e' in arrivo (SummonRequestComponent)
        void UpdateVoxCaster(Scene& scene, entt::entity e, AmmoComponent& a, const WeaponBlueprint& bp, int slot, bool pressed, float dt) {
            auto& reg = scene.GetRegistry();
            const int slotIdx = slot - 4;
            if (a.useTimer > 0.0f && a.useSlot == slot) {
                a.useTimer -= dt;
                if (a.useTimer > 0.0f) return;
                a.useTimer = 0.0f; a.useSlot = -1;
                a.equipmentUses[slotIdx] = std::max(0, a.equipmentUses[slotIdx] - 1);
                if (bp.summon_unit.empty()) { Feedback(a, "Nessuno risponde al vox"); return; }
                auto& req = reg.get_or_emplace<SummonRequestComponent>(e);
                req.pending.push_back({ bp.summon_unit, std::max(0.0f, bp.summon_delay) });
                Feedback(a, "Rinforzi in arrivo tra " + std::to_string(static_cast<int>(std::lround(bp.summon_delay))) + " s");
                return;
            }
            if (!pressed) return;
            if (a.equipmentUses[slotIdx] <= 0) { Feedback(a, "Vox caster gia' usato"); return; }
            a.useDuration = std::max(0.1f, bp.equipment_use_time);
            a.useTimer = a.useDuration;
            a.useSlot = slot;
            a.useTarget = e;
            Feedback(a, "Chiamata al vox...");
        }

        void DropFood(Scene& scene, entt::entity e, AmmoComponent& a, const WeaponBlueprint& bp, int slotIdx, const BlockModelLibrary* models) {
            auto& reg = scene.GetRegistry();
            if (a.equipmentUses[slotIdx] <= 0) { Feedback(a, "Zaino di cibo gia' usato"); return; }
            const auto& t = reg.get<TransformComponent>(e);
            const entt::entity z = scene.CreateEntity();
            auto& zt = reg.emplace<TransformComponent>(z);
            zt.Position = glm::vec3(t.Position.x, FootY(reg, e), t.Position.z);
            zt.Rotation = glm::vec3(0.0f, t.Rotation.y, 0.0f);
            reg.emplace<MeshComponent>(z, 0u);
            auto& c = reg.emplace<ColorComponent>(z);
            c.Color = glm::vec4(0.45f, 0.35f, 0.20f, 1.0f); c.BaseColor = c.Color;
            if (const BlockModel* model = models ? models->Find(bp.id) : nullptr) {   // stesso modello tenuto in mano
                reg.emplace<BlockModelComponent>(z, BlockModelComponent{ model });
                zt.Scale = glm::vec3(1.0f);
            }
            else {
                zt.Scale = glm::vec3(0.35f, 0.30f, 0.25f);
                zt.Position.y += 0.15f;
            }
            HealZoneComponent zone;
            zone.owner = e;
            if (const auto* team = reg.try_get<TeamComponent>(e)) zone.team = team->team;
            zone.radius = bp.equipment_range;
            zone.healPerSecond = bp.heal_per_second;
            zone.duration = zone.timeLeft = std::max(0.5f, bp.effect_duration);
            reg.emplace<HealZoneComponent>(z, zone);
            a.equipmentUses[slotIdx] -= 1;
            Feedback(a, "Zaino di cibo a terra: cura per " + std::to_string(static_cast<int>(zone.duration)) + " s");
        }

        // Granata: giocatore = lungo la mira (raggio del mirino se c'e'), bot = parabola verso il bersaglio (giveTarget)
        void ThrowGrenade(Scene& scene, entt::entity e, AmmoComponent& a, const WeaponBlueprint& bp, int slotIdx,
                          entt::entity target, const BlockModelLibrary* models) {
            auto& reg = scene.GetRegistry();
            if (a.equipmentUses[slotIdx] <= 0) { Feedback(a, "Granate finite"); return; }
            const auto& t = reg.get<TransformComponent>(e);
            const float footY = FootY(reg, e);
            float height = 1.8f;
            if (const auto* c = reg.try_get<ColliderComponent>(e)) height = std::max(0.6f, c->halfExtents.y * 2.0f * t.Scale.y);
            const float yr = glm::radians(t.AimYaw);
            const glm::vec3 fwd(-std::sin(yr), 0.0f, -std::cos(yr));
            const glm::vec3 origin = glm::vec3(t.Position.x, footY + height * 0.85f, t.Position.z) + fwd * 0.4f;
            glm::vec3 vel;
            if (!reg.all_of<PlayerTag>(e) && target != entt::null && reg.valid(target) && reg.all_of<TransformComponent>(target)) {
                // Mira un po' corta: dopo l'impatto la granata rimbalza e rotola in avanti di un paio di metri
                const glm::vec3 tp = reg.get<TransformComponent>(target).Position;
                glm::vec3 toT = tp - origin; toT.y = 0.0f;
                const float dist = glm::length(toT);
                glm::vec3 aimAt = dist > 0.1f ? tp - toT / dist * std::min(2.5f, dist * 0.18f) : tp;
                aimAt += glm::vec3(Random::Range(-1.5f, 1.5f), 0.0f, Random::Range(-1.5f, 1.5f));   // i bot non sono infallibili
                vel = Explosives::BallisticVelocity(origin, aimAt, bp.throw_speed * 1.3f);
            }
            else {
                glm::vec3 dir;
                const auto* aim = reg.try_get<AimRayComponent>(e);
                if (aim && aim->valid) dir = glm::normalize(aim->dir);
                else {
                    const float p = glm::radians(t.AimPitch);
                    dir = glm::normalize(glm::vec3(fwd.x * std::cos(p), std::sin(p), fwd.z * std::cos(p)));
                }
                vel = glm::normalize(dir + glm::vec3(0.0f, 0.25f, 0.0f)) * bp.throw_speed;   // un po' a campanile
            }
            Explosives::ThrowGrenade(scene, e, bp, origin, vel, models);
            a.equipmentUses[slotIdx] -= 1;
            Feedback(a, bp.displayName + " lanciata! (" + std::to_string(a.equipmentUses[slotIdx]) + " rimaste)");
        }

        // Mina: posata un metro davanti, a terra
        void PlaceMine(Scene& scene, entt::entity e, AmmoComponent& a, const WeaponBlueprint& bp, int slotIdx, const BlockModelLibrary* models) {
            auto& reg = scene.GetRegistry();
            if (a.equipmentUses[slotIdx] <= 0) { Feedback(a, "Mine finite"); return; }
            const auto& t = reg.get<TransformComponent>(e);
            const float yr = glm::radians(t.AimYaw);
            glm::vec3 pos = glm::vec3(t.Position.x, FootY(reg, e), t.Position.z) + glm::vec3(-std::sin(yr), 0.0f, -std::cos(yr)) * 1.0f;
            if (const auto g = GetGroundHeightAt(scene, pos, pos.y + 0.3f, 0.05f)) pos.y = *g;
            Explosives::PlaceMine(scene, e, bp, pos, t.AimYaw, models);
            a.equipmentUses[slotIdx] -= 1;
            Feedback(a, "Mina piazzata, si arma in " + std::to_string(static_cast<int>(std::lround(bp.fuse_time))) + " s (" +
                        std::to_string(a.equipmentUses[slotIdx]) + " rimaste)");
        }

        void UpdateHealZones(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            std::vector<entt::entity> expired;
            for (auto z : reg.view<HealZoneComponent, TransformComponent>()) {
                auto& zone = reg.get<HealZoneComponent>(z);
                const glm::vec3 c = reg.get<TransformComponent>(z).Position;
                zone.timeLeft -= dt;
                if (zone.timeLeft <= -0.01f) { expired.push_back(z); continue; }
                // Un impulso di cura al secondo: healPerSecond a ogni compagno nel cerchio che non e' pieno.
                // Il padrone prende 50 punti per ogni compagno curato (anche se gli mancavano pochi HP).
                zone.tickTimer -= dt;
                if (zone.tickTimer > 0.0f) continue;
                zone.tickTimer += 1.0f;
                auto units = reg.view<HealthComponent, TransformComponent>(entt::exclude<CorpseComponent>);
                for (auto u : units) {
                    auto& h = units.get<HealthComponent>(u);
                    if (h.currentHealth <= 0.0f || h.currentHealth >= h.maxHealth) continue;
                    const bool ally = zone.team >= 0 ? (reg.all_of<TeamComponent>(u) && reg.get<TeamComponent>(u).team == zone.team) : (u == zone.owner);
                    if (!ally) continue;
                    const glm::vec3 d = units.get<TransformComponent>(u).Position - c;
                    if (glm::length(glm::vec2(d.x, d.z)) > zone.radius) continue;
                    h.currentHealth = std::min(h.maxHealth, h.currentHealth + zone.healPerSecond);
                    EmitXp(zone.owner, XpCategory::Heal, XpValues::HealTick);
                }
            }
            for (auto z : expired) scene.DestroyEntity(z);
        }

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
