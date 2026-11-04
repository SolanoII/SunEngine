#pragma once
// Armi pesanti piazzabili (es. lascannon dei whiteshield): torretta NON automatica che va manovrata da un'unita'.
//  - si piazza come una struttura (equipment_type "turret": fermi, click tenuto per equipment_use_time secondi);
//  - E vicino alla torretta per entrarci (si resta fermi dietro l'arma), E tenuto 1,5 s per uscire;
//  - il proprietario, fuori dalla torretta, tenendo E per 5 s la smonta e la riprende (torna l'uso);
//  - salute da struttura con un po' di armatura; distrutta non si recupera.
// Qui ci sono i componenti e la creazione (usati anche da EquipmentSystem); la logica e' in TurretSystem.h.
#include "Components.h"
#include "BlockModel.h"
#include "../Scene/Scene.h"
#include "../AssetManager/BlueprintManager.h"
#include <cmath>
#include <string>

namespace SunEngine {

    struct TurretComponent {
        entt::entity owner = entt::null;   // chi l'ha piazzata (solo lui puo' smontarla)
        int ownerSlot = -1;                // slot della dotazione da cui viene (4..6): smontandola l'uso torna li'
        int team = 0;
        std::string itemId;                // blueprint dell'arma (danni, munizioni, tempi)
        float baseYaw = 0.0f;              // direzione in cui e' stata piazzata (gradi)
        float yaw = 0.0f, pitch = 0.0f;    // mira attuale
        int loaded = 1;                    // colpi pronti
        int reserve = 10;                  // colpi di riserva
        float reloadTimer = 0.0f;
        float charge = 0.0f;               // secondi di carica del colpo (click tenuto)
        entt::entity gunner = entt::null;  // chi la manovra
        entt::entity gun = entt::null;     // parte che ruota (solo grafica)
    };

    struct TurretGunComponent { entt::entity base = entt::null; };

    namespace Turret {

        inline constexpr float kYawArc = 60.0f;          // brandeggio: +-60 gradi dalla direzione di piazzamento
        inline constexpr float kPitchMin = -20.0f, kPitchMax = 30.0f;
        inline constexpr float kPivotHeight = 0.95f;     // altezza del perno dell'arma da terra
        inline constexpr float kSeatBack = 0.95f;        // il tiratore sta dietro l'arma

        inline glm::vec3 Forward(float yawDeg) { const float r = glm::radians(yawDeg); return glm::vec3(-std::sin(r), 0.0f, -std::cos(r)); }
        inline glm::vec3 SeatPos(const TransformComponent& base, const TurretComponent& t) { return base.Position - Forward(t.baseYaw) * kSeatBack; }
        inline glm::vec3 PivotPos(const TransformComponent& base) { return base.Position + glm::vec3(0.0f, kPivotHeight, 0.0f); }
        // Bocca dell'arma (la canna del modello e' lunga ~1,2 m lungo -Z)
        inline glm::vec3 MuzzlePos(const TransformComponent& base, const TurretComponent& t) {
            const glm::mat4 r = glm::rotate(glm::rotate(glm::mat4(1.0f), glm::radians(t.yaw), glm::vec3(0, 1, 0)), glm::radians(t.pitch), glm::vec3(1, 0, 0));
            return PivotPos(base) + glm::vec3(r * glm::vec4(0.0f, 0.05f, -1.25f, 0.0f));
        }
        inline float WrapDeg(float a) { a = std::fmod(a + 180.0f, 360.0f); if (a < 0.0f) a += 360.0f; return a - 180.0f; }

        inline void Mount(entt::registry& reg, entt::entity turret, entt::entity unit) {
            auto& t = reg.get<TurretComponent>(turret);
            t.gunner = unit;
            t.charge = 0.0f;
            reg.emplace_or_replace<TurretMountComponent>(unit, TurretMountComponent{ turret });
            if (auto* ut = reg.try_get<TransformComponent>(unit)) { ut->AimYaw = t.yaw; ut->AimPitch = t.pitch; }
        }
        inline void Unmount(entt::registry& reg, entt::entity unit) {
            if (!reg.valid(unit)) return;
            if (auto* m = reg.try_get<TurretMountComponent>(unit)) {
                if (reg.valid(m->turret)) if (auto* t = reg.try_get<TurretComponent>(m->turret)) { t->gunner = entt::null; t->charge = 0.0f; }
                reg.remove<TurretMountComponent>(unit);
            }
        }

        // Piazza la torretta davanti a 'owner' (a terra, rivolta dove mira). I bot ci entrano subito.
        inline entt::entity Spawn(Scene& scene, entt::entity owner, const WeaponBlueprint& bp, int slot, float footY, const BlockModelLibrary* models) {
            auto& reg = scene.GetRegistry();
            const auto& ot = reg.get<TransformComponent>(owner);
            const float yaw = ot.AimYaw;
            const glm::vec3 pos = glm::vec3(ot.Position.x, footY, ot.Position.z) + Forward(yaw) * (kSeatBack + 0.15f);

            const entt::entity base = scene.CreateEntity();
            auto& bt = reg.emplace<TransformComponent>(base);
            bt.Position = pos;
            bt.Rotation = glm::vec3(0.0f, yaw, 0.0f);
            bt.Scale = glm::vec3(1.0f);
            reg.emplace<MeshComponent>(base, 0u);
            auto& bc = reg.emplace<ColorComponent>(base);
            bc.Color = glm::vec4(0.25f, 0.27f, 0.2f, 1.0f); bc.BaseColor = bc.Color;
            if (const BlockModel* m = models ? models->Find(bp.id + "_base") : nullptr) reg.emplace<BlockModelComponent>(base, BlockModelComponent{ m });
            auto& col = reg.emplace<ColliderComponent>(base);   // copertura bassa davanti al tiratore
            col.halfExtents = glm::vec3(0.45f, 0.6f, 0.45f);
            col.localCenterOffset = glm::vec3(0.0f, 0.6f, 0.0f);
            StructureHealthComponent sh;
            sh.hp = sh.maxHp = bp.structure_health;
            sh.owner = owner;
            sh.damageReduction = bp.structure_armor;
            reg.emplace<StructureHealthComponent>(base, sh);
            reg.emplace<RepairableComponent>(base, RepairableComponent{ { "Mechanical", "Construct" } });

            TurretComponent t;
            t.owner = owner;
            t.ownerSlot = slot;
            if (const auto* team = reg.try_get<TeamComponent>(owner)) t.team = team->team;
            t.itemId = bp.id;
            t.baseYaw = t.yaw = yaw;
            t.loaded = std::max(1, bp.magazine_size);
            t.reserve = std::max(0, bp.magazine_size * bp.spare_magazines);

            // Arma che ruota (solo grafica)
            t.gun = scene.CreateEntity();
            auto& gt = reg.emplace<TransformComponent>(t.gun);
            gt.Position = PivotPos(bt);
            gt.Rotation = glm::vec3(0.0f, yaw, 0.0f);
            gt.Scale = glm::vec3(1.0f);
            reg.emplace<MeshComponent>(t.gun, 0u);
            auto& gc = reg.emplace<ColorComponent>(t.gun);
            gc.Color = glm::vec4(0.3f, 0.32f, 0.25f, 1.0f); gc.BaseColor = gc.Color;
            if (const BlockModel* m = models ? models->Find(bp.id) : nullptr) reg.emplace<BlockModelComponent>(t.gun, BlockModelComponent{ m });
            else gt.Scale = glm::vec3(0.18f, 0.2f, 1.2f);
            reg.emplace<TurretGunComponent>(t.gun, TurretGunComponent{ base });
            reg.emplace<TurretComponent>(base, t);

            if (!reg.all_of<PlayerTag>(owner)) Mount(reg, base, owner);   // i bot la manovrano subito
            return base;
        }
    }
}
