#pragma once
// Hitbox per blocco dello scheletro e feedback di colpo, condivisi da armi a distanza (WeaponSystem)
// e mischia (MeleeCombatSystem): stessa geometria dei colpi, stessi marcatori e flash dei blocchi.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include "Components.h"
#include "XpEvents.h"
#include "Tags.h"
#include "../Scene/Scene.h"

namespace SunEngine {

    // ---------- Hitbox per blocco dello scheletro ----------
    // Margine (metri, per lato) aggiunto a ogni blocco: rende i colpi al bordo meno "fortunati"
    inline constexpr float kHitboxPadding = 0.03f;

    // Intersezione raggio / blocco orientato. blockWorld = matrice del blocco (cubo unitario [-0.5,0.5]^3
    // scalato, ruotato, posizionato): il raggio va nello spazio del cubo, dove il parametro t resta lo stesso.
    // Un raggio che parte DENTRO il blocco conta come colpo a t = 0.
    inline bool IntersectSkeletonBlock(const glm::mat4& blockWorld, const glm::vec3& origin, const glm::vec3& dir,
        float maxDist, float& outT) {
        const glm::vec3 sx = glm::vec3(blockWorld[0]), sy = glm::vec3(blockWorld[1]), sz = glm::vec3(blockWorld[2]);
        const glm::vec3 worldSize(glm::length(sx), glm::length(sy), glm::length(sz));
        if (worldSize.x < 1e-5f || worldSize.y < 1e-5f || worldSize.z < 1e-5f) return false;
        const glm::vec3 half = glm::vec3(0.5f) + glm::vec3(kHitboxPadding) / worldSize;   // margine in spazio cubo

        const glm::mat4 inv = glm::inverse(blockWorld);
        const glm::vec3 lo = glm::vec3(inv * glm::vec4(origin, 1.0f));
        const glm::vec3 ld = glm::vec3(inv * glm::vec4(dir, 0.0f));

        float tMin = 0.0f, tMax = maxDist;
        for (int a = 0; a < 3; ++a) {
            if (std::abs(ld[a]) < 1e-8f) {
                if (lo[a] < -half[a] || lo[a] > half[a]) return false;
            }
            else {
                float t0 = (-half[a] - lo[a]) / ld[a];
                float t1 = (half[a] - lo[a]) / ld[a];
                if (t0 > t1) std::swap(t0, t1);
                tMin = (std::max)(tMin, t0);
                tMax = (std::min)(tMax, t1);
                if (tMax < tMin) return false;
            }
        }
        outT = tMin;
        return true;
    }

    // Zona colpita dal nome del blocco. Blocchi aggiuntivi (es. scheletro degli orchi): la mascella conta
    // come testa (cosi' come orecchie e naso del gretchin), spalle e pugni come il braccio del loro lato, la
    // gobba e la pancia (e ogni altro blocco) come torso.
    inline HitboxLocation LocationFromBlockName(const std::string& name) {
        if (name == "Head" || name == "Jaw" || name.rfind("Ear", 0) == 0 || name == "Nose") return HitboxLocation::Head;
        if (name == "Arm_L" || name == "Shoulder_L" || name == "Fist_L") return HitboxLocation::LeftArm;
        if (name == "Arm_R" || name == "Shoulder_R" || name == "Fist_R") return HitboxLocation::RightArm;
        if (name == "Leg_L") return HitboxLocation::LeftLeg;
        if (name == "Leg_R") return HitboxLocation::RightLeg;
        return HitboxLocation::Torso;
    }

    // ---------- Feedback di colpo ----------
    // Durata di un marcatore e finestra in cui colpi sullo stesso bersaglio si sommano (pallettoni,
    // raffiche, raggio continuo), cosi' il colore riflette il danno della raffica e non del singolo frame.
    inline constexpr float kHitMarkerMergeWindow = 0.12f;

    inline int FindHitBlock(const SkeletonData& data, HitboxLocation loc) {
        const char* name = "Torso";
        switch (loc) {
        case HitboxLocation::Head:     name = "Head"; break;
        case HitboxLocation::LeftArm:  name = "Arm_L"; break;
        case HitboxLocation::RightArm: name = "Arm_R"; break;
        case HitboxLocation::LeftLeg:  name = "Leg_L"; break;
        case HitboxLocation::RightLeg: name = "Leg_R"; break;
        default: break;
        }
        for (size_t i = 0; i < data.blocks.size(); ++i) if (data.blocks[i].name == name) return static_cast<int>(i);
        return -1;
    }

    // Registra un colpo andato a segno: marcatore nell'HUD + flash del blocco colpito.
    // Solo per i colpi del giocatore (il feedback e' per chi spara o colpisce in mischia).
    // Danni inflitti dal giocatore (anche col suo squig e i suoi esplosivi) dall'ultima lettura: GameLayer li
    // usa per caricare il buff di fazione (WAAAGH!) e poi azzera
    inline float& PlayerDamageDealtAccum() { static float v = 0.0f; return v; }
    // Bersagli colpiti dal giocatore locale (partita in rete: l'host distingue i suoi colpi da quelli dei bot per dare
    // il merito delle uccisioni). Lo svuota il gioco a ogni fotogramma.
    inline std::vector<entt::entity>& PlayerHitLog() { static std::vector<entt::entity> v; return v; }

    inline void EmitHitFeedback(Scene& scene, entt::entity shooter, entt::entity target,
        const glm::vec3& hitPoint, HitboxLocation location, float healthBefore, bool graze = false) {
        auto& reg = scene.GetRegistry();
        if (!reg.valid(shooter) || !reg.all_of<PlayerTag>(shooter)) return;
        if (!reg.valid(target)) return;
        const auto* health = reg.try_get<HealthComponent>(target);
        const auto* tTrans = reg.try_get<TransformComponent>(target);
        if (!health || !tTrans) return;

        const float dealt = (std::max)(0.0f, healthBefore - health->currentHealth);
        const bool killed = health->currentHealth <= 0.0f && healthBefore > 0.0f;
        if (dealt > 0.0f && !SameTeam(reg, shooter, target)) PlayerDamageDealtAccum() += dealt;
        if (dealt > 0.0f && PlayerHitLog().size() < 512) PlayerHitLog().push_back(target);

        // Esperienza: danni a un nemico (una volta per nemico, lo controlla GameLayer) e colpo di grazia.
        // Non contano manichini e squig (si potrebbero "coltivare").
        if (dealt > 0.0f && !SameTeam(reg, shooter, target) &&
            !reg.all_of<TrainingDummyComponent>(target) && !reg.all_of<SquigComponent>(target)) {
            // Quanto vale il nemico = quanti punti costa la sua unita' (25 XP a punto per il danno, 50 per l'uccisione):
            // un gretchin da 3 punti rende poco, un ogryn o un capitano moltissimo. Orda = boy.
            int cost = 10;
            std::string victimId;
            if (const auto* uid = reg.try_get<UnitBlueprintId>(target)) victimId = uid->id;
            else if (reg.all_of<HordeAgentComponent>(target)) victimId = "orks_boy";
            if (!victimId.empty())
                if (const auto* ubp = BlueprintManager::Get().GetUnit(victimId); ubp && ubp->point_cost > 0) cost = ubp->point_cost;
            EmitXp(shooter, XpCategory::Damage, XpValues::DamagePerPoint * cost, target);
            if (killed) EmitXp(shooter, XpCategory::Kill, XpValues::KillPerPoint * cost, target);
        }

        // Somma ai colpi recenti sullo stesso bersaglio
        HitMarkerComponent* marker = nullptr;
        auto view = reg.view<HitMarkerComponent>();
        for (auto e : view) {
            auto& m = view.get<HitMarkerComponent>(e);
            if (m.target == target && m.age < kHitMarkerMergeWindow) { marker = &m; break; }
        }
        if (!marker) {
            entt::entity e = scene.CreateEntity();
            marker = &reg.emplace<HitMarkerComponent>(e);
            marker->target = target;
            marker->targetMaxHealth = (health->maxHealth > 1.0f) ? health->maxHealth : 100.0f;
        }
        marker->damage += dealt;
        marker->killed = marker->killed || killed;
        marker->graze = graze;
        marker->localOffset = hitPoint - tTrans->Position;
        marker->worldPoint = hitPoint;

        if (auto* dummy = reg.try_get<TrainingDummyComponent>(target)) {
            dummy->lastLocation = static_cast<int>(location);
        }

        // Flash del blocco colpito (scheletro a cubi del bersaglio)
        if (auto* skel = reg.try_get<SkeletonComponent>(target)) {
            const int idx = FindHitBlock(skel->data, location);
            if (idx >= 0) {
                if (skel->blockFlashTimers.size() < skel->data.blocks.size()) skel->blockFlashTimers.resize(skel->data.blocks.size(), 0.0f);
                if (skel->blockFlashColors.size() < skel->data.blocks.size()) skel->blockFlashColors.resize(skel->data.blocks.size(), glm::vec3(1.0f));
                skel->blockFlashTimers[idx] = 0.18f;
                skel->blockFlashColors[idx] = HitEffectivenessColor(marker->damage, marker->targetMaxHealth, marker->killed);
            }
        }
    }

}
