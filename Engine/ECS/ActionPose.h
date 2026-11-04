#pragma once
// Animazione delle azioni per tutte le unita' (giocatore e bot, ogni fazione e ogni scheletro):
//  - rileva cosa sta facendo l'unita': lancio di granata, oggetto posato a terra (cibo, mine), scatola di munizioni,
//    chiave inglese, costruzione di strutture;
//  - sugli scheletri a blocchi (cubi: hitbox e bot) mette braccia e busto nella posa dell'azione, e per i bot anche
//    i colpi in mischia e la parata (il giocatore a cubi ha gia' le sue pose di mischia in GameLayer).
// Il modello animato del giocatore usa lo stesso stato per le sue pose procedurali (ProceduralPose.h).
#include "Components.h"
#include "../Scene/Scene.h"
#include "../AssetManager/BlueprintManager.h"
#include "DamageResolver.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace SunEngine {

    struct ActionPoseComponent {
        int action = 0;                  // 0 niente, 1 lancio granata, 2 oggetto posato a terra
        float time = 0.0f;               // secondi dall'inizio dell'azione
        float duration = 1.0f;
        int prevUses[3] = { -1, -1, -1 };
        std::string unitId;              // cambio di unita' = si riparte (gli usi cambiano senza essere usati)
        float give = 0.0f, build = 0.0f, repair = 0.0f;   // azioni continue (pesi 0..1, transizione morbida)
        float medkit = 0.0f;             // medicazione in corso (medikit)
        bool repairing = false;          // lo imposta chi ripara (oggi RepairSystem per il giocatore)
        float loopTime = 0.0f;           // tempo per i movimenti ritmati
    };

    namespace ActionPose {

        inline float Smooth(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
        inline float Bell(float t, float rise, float hold) {
            if (t <= 0.0f || t >= 1.0f) return 0.0f;
            if (t < rise) return Smooth(t / rise);
            if (t < hold) return 1.0f;
            return 1.0f - Smooth((t - hold) / std::max(0.001f, 1.0f - hold));
        }

        // Rileva le azioni di ogni unita' con dotazione (usi che calano, rifornimento/costruzione in corso)
        inline void Update(Scene& scene, float dt) {
            auto& reg = scene.GetRegistry();
            auto view = reg.view<InventoryComponent, AmmoComponent>(entt::exclude<CorpseComponent>);
            for (auto e : view) {
                auto& ap = reg.get_or_emplace<ActionPoseComponent>(e);
                const auto& inv = view.get<InventoryComponent>(e);
                const auto& ammo = view.get<AmmoComponent>(e);
                ap.loopTime += dt;
                std::string unitId;
                if (const auto* uid = reg.try_get<UnitBlueprintId>(e)) unitId = uid->id;
                if (unitId != ap.unitId) { ap.unitId = unitId; for (int& u : ap.prevUses) u = -1; ap.action = 0; }
                for (int s = 0; s < 3; ++s) {
                    const int uses = ammo.equipmentUses[s];
                    if (ap.prevUses[s] >= 0 && uses < ap.prevUses[s]) {
                        const auto* bp = BlueprintManager::Get().GetWeapon(inv.GetSlotItemId(4 + s));
                        // Il lancio parte gia' quasi caricato: la granata esce subito al comando
                        if (bp && bp->equipment_type == "grenade") { ap.action = 1; ap.time = 0.30f; ap.duration = 0.75f; }
                        else if (bp && (bp->equipment_type == "food_pack" || bp->equipment_type == "landmine")) { ap.action = 2; ap.time = 0.15f; ap.duration = 0.9f; }
                    }
                    ap.prevUses[s] = uses;
                }
                if (ap.action != 0) {
                    ap.time += dt;
                    if (ap.time >= ap.duration) ap.action = 0;
                }
                const auto* held = BlueprintManager::Get().GetWeapon(inv.GetActiveWeaponId());
                const bool using_ = ammo.useTimer > 0.0f && held && held->is_equipment;
                auto approach = [&](float& v, bool on, float rate) { v = std::clamp(v + (on ? 1.0f : -1.0f) * rate * dt, 0.0f, 1.0f); };
                approach(ap.give, using_ && held->equipment_type == "ammo_box", 6.0f);
                approach(ap.build, using_ && (held->equipment_type == "scrap_barricade" || held->equipment_type == "slit_wall"), 5.0f);
                approach(ap.repair, ap.repairing, 6.0f);
                approach(ap.medkit, using_ && held->equipment_type == "medkit", 6.0f);
            }
        }

        // Posa sugli scheletri a blocchi. jointRotation.x: braccia e gambe +x = avanti (78 = braccio orizzontale),
        // busto -x = piegato in avanti. weight mescola con la posa gia' calcolata (camminata, mira).
        inline void ApplyCubes(Scene& scene, entt::entity player) {
            auto& reg = scene.GetRegistry();
            auto view = reg.view<ActionPoseComponent, SkeletonComponent>(entt::exclude<CorpseComponent>);
            for (auto e : view) {
                const auto& ap = view.get<ActionPoseComponent>(e);
                auto& skel = view.get<SkeletonComponent>(e);
                auto set = [&](const char* name, const glm::vec3& target, float w) {
                    if (w <= 0.0f) return;
                    for (auto& b : skel.data.blocks) if (b.name == name) b.jointRotation = glm::mix(b.jointRotation, target, std::min(1.0f, w));
                };
                const float lt = ap.loopTime;

                // Colpi in mischia e parata (solo bot: il giocatore a cubi ha le sue pose in GameLayer)
                if (e != player) {
                    if (const auto* melee = reg.try_get<MeleeStateComponent>(e)) {
                        if (melee->currentAttack == MeleeAttackType::Parry && melee->currentPhase != MeleePhase::None) {
                            set("Arm_R", glm::vec3(110.0f, -30.0f, 0.0f), 1.0f);
                            set("Arm_L", glm::vec3(110.0f, 30.0f, 0.0f), 1.0f);
                        }
                        else if (melee->currentPhase == MeleePhase::Windup || melee->currentPhase == MeleePhase::Active || melee->currentPhase == MeleePhase::Recovery) {
                            const auto* wbp = DamageResolver::ResolveMeleeWeapon(reg, e);
                            MeleeTiming tm;
                            if (wbp && wbp->hasMoveset) {
                                auto it = wbp->moveset.nodes.find(melee->currentAttackNode);
                                if (it != wbp->moveset.nodes.end()) tm = it->second.timings;
                            }
                            const int phase = melee->currentPhase == MeleePhase::Windup ? 0 : (melee->currentPhase == MeleePhase::Active ? 1 : 2);
                            const float dur = phase == 0 ? tm.windup : (phase == 1 ? tm.active : tm.recovery);
                            const float t = std::clamp(melee->stateTimer / std::max(0.01f, dur), 0.0f, 1.0f);
                            // carica (braccio alto) -> colpo (braccio giu' davanti) -> ritorno
                            const float k = phase == 0 ? Smooth(t) : (phase == 1 ? 1.0f - 2.0f * Smooth(t) : -1.0f + Smooth(t));
                            const float wind = std::max(0.0f, k), hit = std::max(0.0f, -k);
                            const bool heavy = melee->currentAttack == MeleeAttackType::Heavy;
                            const bool twoHanded = wbp && wbp->hand_type == HandType::TwoHanded;
                            const glm::vec3 arm(25.0f + 135.0f * wind + 30.0f * hit, heavy ? 0.0f : (-35.0f * wind + 45.0f * hit), 0.0f);
                            set("Arm_R", arm, 1.0f);
                            if (heavy && twoHanded) set("Arm_L", arm, 1.0f);
                            set("Torso", glm::vec3(heavy ? (8.0f * wind - 20.0f * hit) : -6.0f * hit, -20.0f * wind + 20.0f * hit, 0.0f), 1.0f);
                        }
                    }
                }

                // Azioni con la dotazione (tutti, giocatore compreso)
                if (ap.action == 1) {   // lancio della granata
                    const float t = ap.time / ap.duration;
                    const float wind = t < 0.45f ? Smooth(t / 0.45f) : 1.0f - Smooth((t - 0.45f) / 0.15f);
                    const float rel = t < 0.45f ? 0.0f : Bell((t - 0.45f) / 0.55f, 0.25f, 0.45f);
                    const float w = std::max(wind, rel);
                    set("Arm_R", glm::vec3(200.0f * wind + 70.0f * rel, -20.0f * wind, 0.0f), w);
                    set("Arm_L", glm::vec3(60.0f, 0.0f, 0.0f), wind);
                    set("Torso", glm::vec3(6.0f * wind - 15.0f * rel, -25.0f * wind + 25.0f * rel, 0.0f), w);
                }
                else if (ap.action == 2) {   // oggetto posato a terra (cibo, mina)
                    const float b = Bell(ap.time / ap.duration, 0.4f, 0.6f);
                    set("Torso", glm::vec3(-40.0f, 0.0f, 0.0f), b);
                    set("Arm_R", glm::vec3(55.0f, 0.0f, 0.0f), b);
                    set("Head", glm::vec3(-20.0f, 0.0f, 0.0f), b);
                }
                if (ap.give > 0.0f) {   // porge la scatola di munizioni
                    const float sway = std::sin(lt * 3.0f) * 6.0f;
                    set("Arm_R", glm::vec3(60.0f + sway, -10.0f, 0.0f), ap.give);
                    set("Arm_L", glm::vec3(60.0f - sway, 10.0f, 0.0f), ap.give);
                    set("Torso", glm::vec3(-10.0f, 0.0f, 0.0f), ap.give);
                }
                if (ap.repair > 0.0f) {   // chiave inglese: colpi ritmati
                    const float hammer = std::abs(std::sin(lt * 7.0f));
                    set("Arm_R", glm::vec3(120.0f - 70.0f * hammer, 0.0f, 0.0f), ap.repair);
                    set("Arm_L", glm::vec3(35.0f, 0.0f, 0.0f), ap.repair);
                    set("Torso", glm::vec3(-20.0f, 0.0f, 0.0f), ap.repair);
                }
                if (ap.medkit > 0.0f) {   // medicazione: braccio sinistro piegato, la mano destra fascia
                    const float wrap = std::sin(lt * 5.0f) * 12.0f;
                    set("Arm_L", glm::vec3(70.0f, 30.0f, 0.0f), ap.medkit);
                    set("Arm_R", glm::vec3(60.0f + wrap, -35.0f, 0.0f), ap.medkit);
                    set("Head", glm::vec3(-25.0f, 0.0f, 0.0f), ap.medkit);
                    set("Torso", glm::vec3(-12.0f, 0.0f, 0.0f), ap.medkit);
                }
                if (ap.build > 0.0f) {   // costruzione: chinato, le due braccia battono a turno
                    const float c = std::sin(lt * 6.0f);
                    const float r = 0.5f + 0.5f * c, l = 0.5f - 0.5f * c;
                    set("Arm_R", glm::vec3(110.0f - 70.0f * r, 0.0f, 0.0f), ap.build);
                    set("Arm_L", glm::vec3(110.0f - 70.0f * l, 0.0f, 0.0f), ap.build);
                    set("Torso", glm::vec3(-35.0f, 0.0f, 0.0f), ap.build);
                    set("Head", glm::vec3(-15.0f, 0.0f, 0.0f), ap.build);
                }
            }
        }
    }
}
