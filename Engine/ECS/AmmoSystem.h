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
// Cassa di munizioni ("ammo_crate"): click sinistro la posa a terra davanti; poi chiunque della squadra, vicino e
//   premendo E, recupera un caricatore dell'arma che ha in mano (crate_charges volte in tutto, poi la cassa sparisce).
//   I bot vicini la usano da soli quando hanno la riserva bassa.
// Zaino energetico ("power_pack"): click sinistro = +move_speed_bonus di velocita' per effect_duration secondi, poi
//   equipment_cooldown secondi di ricarica (nessun limite di usi).
// Insegna imperiale ("insignia"): passiva, basta averla in uno slot: morale alto a chi della squadra e' entro
//   equipment_range metri, portatore compreso (MoraleAuraComponent).
// Mantello mimetico ("camo_cloak"): click sinistro per effect_duration secondi il corpo diventa un liquido trasparente
//   (CloakComponent); i nemici IA non lo vedono oltre 6 m. Sparare o colpire in mischia lo interrompe.
// Auspex ("auspex"): in mano mostra un sonar coi nemici entro equipment_range metri (lo disegna GameLayer).
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
#include <unordered_map>
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
            UpdateCrates(scene, dt);
            UpdateCloaks(scene, dt);
            UpdateBoostsAndAuras(scene, dt);
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
                if (held->equipment_type == "power_pack") {
                    if (wantSelf) {
                        if (a.equipmentCooldown[slotIdx] > 0.0f) Feedback(a, "Zaino energetico in ricarica: " + std::to_string(static_cast<int>(std::ceil(a.equipmentCooldown[slotIdx]))) + " s");
                        else {
                            auto& b = reg.get_or_emplace<SpeedBoostComponent>(e);
                            b.timeLeft = std::max(0.5f, held->effect_duration);
                            b.mult = 1.0f + held->move_speed_bonus;
                            a.equipmentCooldown[slotIdx] = held->equipment_cooldown;
                            Feedback(a, "Zaino energetico: +" + std::to_string(static_cast<int>(std::lround(held->move_speed_bonus * 100.0f))) + "% velocita' per " +
                                        std::to_string(static_cast<int>(std::lround(held->effect_duration))) + " s");
                        }
                    }
                    continue;
                }
                if (held->equipment_type == "camo_cloak") {
                    if (wantSelf) ActivateCloak(scene, e, a, *held, slotIdx);
                    continue;
                }
                if (held->equipment_type == "ammo_crate") {
                    if (wantSelf) PlaceCrate(scene, e, a, *held, slotIdx, models);
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

        entt::entity nearCrate = entt::null;   // cassa di munizioni a portata del giocatore (per l'HUD)

    private:
        bool m_PlayerLmbWasDown = true;   // true: il click che ha avviato la partita non conta
        bool m_PlayerEWasDown = true;
        std::unordered_map<entt::entity, int> m_CloakAmmo;   // colpi in canna al frame prima (sparare interrompe il mimetismo)
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

        // Ricariche della dotazione, spinte dello zaino energetico e aure delle insegne
        void UpdateBoostsAndAuras(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            for (auto e : reg.view<AmmoComponent>()) {
                auto& a = reg.get<AmmoComponent>(e);
                for (float& c : a.equipmentCooldown) c = std::max(0.0f, c - dt);
            }
            std::vector<entt::entity> ended;
            for (auto e : reg.view<SpeedBoostComponent>()) {
                auto& b = reg.get<SpeedBoostComponent>(e);
                b.timeLeft -= dt;
                if (b.timeLeft <= 0.0f || reg.all_of<CorpseComponent>(e)) ended.push_back(e);
            }
            for (auto e : ended) reg.remove<SpeedBoostComponent>(e);
            ended.clear();
            for (auto e : reg.view<MoraleAuraComponent>()) {
                auto& m = reg.get<MoraleAuraComponent>(e);
                m.timeLeft -= dt;
                if (m.timeLeft <= 0.0f) ended.push_back(e);
            }
            for (auto e : ended) reg.remove<MoraleAuraComponent>(e);
            // Insegne: chi la porta (vivo) rinnova l'aura a se' e ai compagni vicini
            auto holders = reg.view<InventoryComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>);
            for (auto h : holders) {
                if (holders.get<HealthComponent>(h).currentHealth <= 0.0f) continue;
                const auto& inv = holders.get<InventoryComponent>(h);
                const WeaponBlueprint* insignia = nullptr;
                for (int s = 4; s <= 6 && !insignia; ++s) {
                    const auto* bp = BlueprintManager::Get().GetWeapon(inv.GetSlotItemId(s));
                    if (bp && bp->equipment_type == "insignia") insignia = bp;
                }
                if (!insignia) continue;
                const glm::vec3 c = holders.get<TransformComponent>(h).Position;
                const auto* team = reg.try_get<TeamComponent>(h);
                auto give = [&](entt::entity u) {
                    auto& m = reg.get_or_emplace<MoraleAuraComponent>(u);
                    m.timeLeft = 0.25f;
                    m.bonus = std::max(m.bonus, insignia->morale_bonus);
                };
                give(h);
                if (!team) continue;
                auto mates = reg.view<TeamComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>);
                for (auto u : mates) {
                    if (u == h || mates.get<TeamComponent>(u).team != team->team || mates.get<HealthComponent>(u).currentHealth <= 0.0f) continue;
                    const glm::vec3 p = mates.get<TransformComponent>(u).Position;
                    if (glm::length(glm::vec2(p.x - c.x, p.z - c.z)) <= insignia->equipment_range) give(u);
                }
            }
        }

        // Mantello mimetico: attivato per effect_duration secondi
        void ActivateCloak(Scene& scene, entt::entity e, AmmoComponent& a, const WeaponBlueprint& bp, int slotIdx) {
            auto& reg = scene.GetRegistry();
            if (const auto* c = reg.try_get<CloakComponent>(e); c && c->timeLeft > 0.0f) { Feedback(a, "Mantello gia' attivo"); return; }
            if (a.equipmentUses[slotIdx] <= 0) { Feedback(a, "Mantello mimetico gia' usato"); return; }
            auto& c = reg.get_or_emplace<CloakComponent>(e);
            c.owner = true;
            c.timeLeft = std::max(1.0f, bp.effect_duration);
            a.equipmentUses[slotIdx] -= 1;
            m_CloakAmmo[e] = -1;
            Feedback(a, "Mimetizzato per " + std::to_string(static_cast<int>(std::lround(c.timeLeft))) + " s (sparare o colpire lo interrompe)");
        }

        // Mimetismo: tempo, transizione morbida, interruzione quando si spara o si colpisce in mischia
        void UpdateCloaks(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            std::vector<entt::entity> done;
            for (auto e : reg.view<CloakComponent>()) {
                auto& c = reg.get<CloakComponent>(e);
                if (!c.owner) continue;   // copie per la grafica: le aggiorna GameLayer
                const auto* hp = reg.try_get<HealthComponent>(e);
                if (!hp || hp->currentHealth <= 0.0f || reg.all_of<CorpseComponent>(e)) { done.push_back(e); continue; }
                if (c.timeLeft > 0.0f) {
                    c.timeLeft -= dt;
                    bool attacked = false;
                    if (const auto* ws = reg.try_get<WeaponStateComponent>(e)) {
                        int& prev = m_CloakAmmo[e];
                        if (prev >= 0 && ws->currentAmmo < prev) attacked = true;
                        prev = ws->currentAmmo;
                    }
                    if (const auto* m = reg.try_get<MeleeStateComponent>(e); m && (m->currentPhase == MeleePhase::Windup || m->currentPhase == MeleePhase::Active)) attacked = true;
                    if (attacked) {
                        c.timeLeft = 0.0f;
                        if (auto* a = reg.try_get<AmmoComponent>(e)) Feedback(*a, "Mimetismo interrotto");
                    }
                }
                c.amount = std::clamp(c.amount + (c.timeLeft > 0.0f ? 3.0f : -3.0f) * dt, 0.0f, 1.0f);
                if (c.timeLeft <= 0.0f && c.amount <= 0.0f) done.push_back(e);
            }
            for (auto e : done) { if (reg.valid(e)) reg.remove<CloakComponent>(e); m_CloakAmmo.erase(e); }
        }

        // Cassa di munizioni: posata un metro davanti, a terra
        void PlaceCrate(Scene& scene, entt::entity e, AmmoComponent& a, const WeaponBlueprint& bp, int slotIdx, const BlockModelLibrary* models) {
            auto& reg = scene.GetRegistry();
            if (a.equipmentUses[slotIdx] <= 0) { Feedback(a, "Cassa gia' posata"); return; }
            const auto& t = reg.get<TransformComponent>(e);
            const float yr = glm::radians(t.AimYaw);
            glm::vec3 pos = glm::vec3(t.Position.x, FootY(reg, e), t.Position.z) + glm::vec3(-std::sin(yr), 0.0f, -std::cos(yr)) * 1.0f;
            if (const auto g = GetGroundHeightAt(scene, pos, pos.y + 0.3f, 0.05f)) pos.y = *g;
            const entt::entity c = Explosives::MakeVisual(scene, pos, t.AimYaw, bp.id, models, bp.primary_color, glm::vec3(0.6f, 0.35f, 0.4f));
            if (!models || !models->Find(bp.id)) reg.get<TransformComponent>(c).Position.y += 0.175f;   // cubo: appoggiato a terra
            AmmoCrateComponent crate;
            crate.owner = e;
            if (const auto* team = reg.try_get<TeamComponent>(e)) crate.team = team->team;
            crate.charges = crate.maxCharges = bp.crate_charges;
            reg.emplace<AmmoCrateComponent>(c, crate);
            a.equipmentUses[slotIdx] -= 1;
            Feedback(a, "Cassa di munizioni a terra: E vicino per un caricatore (" + std::to_string(crate.charges) + " in tutto)");
        }

        // Un caricatore dell'arma in mano dalla cassa (nella riserva, fino al massimo). false = niente preso.
        bool TakeFromCrate(Scene& scene, entt::entity e, entt::entity crateEntity, bool feedback) {
            auto& reg = scene.GetRegistry();
            auto& crate = reg.get<AmmoCrateComponent>(crateEntity);
            auto* a = reg.try_get<AmmoComponent>(e);
            const auto* inv = reg.try_get<InventoryComponent>(e);
            if (!a || !inv || crate.charges <= 0) return false;
            const int idx = Ammo::HeldRangedIndex(*inv);
            const WeaponBlueprint* w = idx >= 0 ? Ammo::RangedAt(*inv, idx) : nullptr;
            if (!w) { if (feedback) Feedback(*a, "Prendi in mano l'arma da ricaricare"); return false; }
            const int room = a->maxReserve[idx] - a->reserve[idx];
            if (room <= 0) { if (feedback) Feedback(*a, "Riserva di " + w->displayName + " gia' piena"); return false; }
            const int got = std::min(room, w->magazine_size);
            a->reserve[idx] += got;
            crate.charges -= 1;
            if (crate.owner != e && reg.valid(crate.owner)) EmitXp(crate.owner, XpCategory::AmmoAlly, XpValues::AmmoAlly);
            else EmitXp(e, XpCategory::AmmoSelf, XpValues::AmmoSelf);
            if (feedback) Feedback(*a, "+" + std::to_string(got) + " colpi per " + w->displayName + " (cassa: " +
                                       std::to_string(crate.charges) + "/" + std::to_string(crate.maxCharges) + ")");
            if (crate.charges <= 0) scene.DestroyEntity(crateEntity);   // vuota: sparisce
            return true;
        }

        // Casse: il giocatore preme E vicino; i bot della squadra la usano passandoci accanto con la riserva bassa
        void UpdateCrates(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            nearCrate = entt::null;
            const bool eDown = Input::IsKeyPressed(GLFW_KEY_E);
            const bool ePressed = eDown && !m_PlayerEWasDown;
            m_PlayerEWasDown = eDown;
            auto crates = reg.view<AmmoCrateComponent, TransformComponent>();
            if (crates.begin() == crates.end()) return;
            constexpr float kReach = 2.0f;
            auto usable = [&](const AmmoCrateComponent& c, entt::entity u) {
                if (c.owner == u) return true;
                if (c.team < 0) return false;
                const auto* t = reg.try_get<TeamComponent>(u);
                return t && t->team == c.team;
            };
            auto nearest = [&](entt::entity u) {
                entt::entity best = entt::null;
                float bestD = kReach;
                const auto& p = reg.get<TransformComponent>(u).Position;
                for (auto c : crates) {
                    const auto& cc = crates.get<AmmoCrateComponent>(c);
                    if (cc.charges <= 0 || !usable(cc, u)) continue;
                    const auto& cp = crates.get<TransformComponent>(c).Position;
                    const float d = glm::length(glm::vec2(p.x - cp.x, p.z - cp.z));
                    if (d < bestD) { bestD = d; best = c; }
                }
                return best;
            };
            for (auto c : crates) { auto& cc = crates.get<AmmoCrateComponent>(c); cc.cooldown = std::max(0.0f, cc.cooldown - dt); }

            std::vector<std::pair<entt::entity, entt::entity>> takes;   // (unita', cassa)
            auto units = reg.view<AmmoComponent, InventoryComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>);
            for (auto u : units) {
                if (units.get<HealthComponent>(u).currentHealth <= 0.0f || reg.all_of<TurretMountComponent>(u)) continue;
                const bool isPlayer = reg.all_of<PlayerTag>(u);
                if (!isPlayer && !reg.all_of<WeaponTriggerComponent>(u)) continue;
                const entt::entity c = nearest(u);
                if (c == entt::null) continue;
                if (isPlayer) {
                    nearCrate = c;
                    if (ePressed) takes.emplace_back(u, c);
                }
                else if (crates.get<AmmoCrateComponent>(c).cooldown <= 0.0f && Ammo::ReserveFraction(reg, u) <= 0.6f) {
                    takes.emplace_back(u, c);
                }
            }
            for (auto [u, c] : takes) {
                if (!reg.valid(c)) continue;   // svuotata da un prelievo prima
                const bool isPlayer = reg.all_of<PlayerTag>(u);
                if (TakeFromCrate(scene, u, c, isPlayer) && reg.valid(c)) reg.get<AmmoCrateComponent>(c).cooldown = 1.0f;
            }
            if (nearCrate != entt::null && !reg.valid(nearCrate)) nearCrate = entt::null;
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
                if (bp.summon_unit.empty()) { Feedback(a, "Nessuno risponde al Communication Device"); return; }
                auto& req = reg.get_or_emplace<SummonRequestComponent>(e);
                req.pending.push_back({ bp.summon_unit, std::max(0.0f, bp.summon_delay) });
                Feedback(a, "Rinforzi in arrivo tra " + std::to_string(static_cast<int>(std::lround(bp.summon_delay))) + " s");
                return;
            }
            if (!pressed) return;
            if (a.equipmentUses[slotIdx] <= 0) { Feedback(a, "Communication Device gia' usato"); return; }
            a.useDuration = std::max(0.1f, bp.equipment_use_time);
            a.useTimer = a.useDuration;
            a.useSlot = slot;
            a.useTarget = e;
            Feedback(a, "Chiamata col Communication Device...");
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
