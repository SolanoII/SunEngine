// --- FIX: va fatto PRIMA di qualsiasi include ---
// 1. NOMINMAX impedisce a <windows.h> (incluso da miniaudio) di definire
//    i macro min/max, che romperebbero std::max / std::min.
// 2. GLFW_INCLUDE_NONE evita che GLFW includa OpenGL (glad è già il loader).
#define NOMINMAX
#define GLFW_INCLUDE_NONE
#include <algorithm>
#include <cmath>
#include "WeaponSystem.h"
#include "DamageResolver.h"
#include "RaycastUtils.h"          // <-- AGGIUNGI
#include "HitUtils.h"
#include "AmmoSystem.h"
#include "Marksmanship.h"
#include "VfxEvents.h"
#include "../Core/Random.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>

namespace SunEngine {

    // Hitbox per blocco e feedback di colpo: vedi HitUtils.h (condivisi con la mischia)

    // Se GameLayer ha pubblicato la bocca reale dell'arma (modello a blocchi), il colpo parte da li'.
    // Ritorna di quanti metri il raggio di COLPO va arretrato lungo la direzione di tiro (vedi sotto).
    //
    // La bocca sta ~1 m davanti al corpo: un nemico in mischia la "ingloba" nel suo box e IntersectAABB
    // ignora un raggio che parte dentro un box (e attraverserebbe anche un muro in cui la canna e' infilata).
    // Per questo il raggio che decide il colpo resta sulla linea della canna ma parte all'altezza del corpo
    // del tiratore; il tracciante/raggio visibile continua a partire dalla bocca.
    static float ApplyMuzzleOverride(Scene& scene, entt::entity shooter, glm::vec3& barrelOrigin,
        const glm::vec3& bodyPos, const glm::vec3& shootDir) {
        auto& reg = scene.GetRegistry();
        const auto* muzzle = reg.try_get<MuzzleComponent>(shooter);
        if (!muzzle) return 0.0f;
        bool leftHand = false;
        if (const auto* inv = reg.try_get<InventoryComponent>(shooter)) {
            leftHand = (inv->GetEquipStance() == EquipStance::HybridRangedMelee);   // pistola nella mano sinistra
        }
        if (leftHand) { if (muzzle->hasLeft) barrelOrigin = muzzle->left; else return 0.0f; }
        else if (muzzle->hasRight) { barrelOrigin = muzzle->right; }
        else return 0.0f;

        // Distanza (lungo la direzione di tiro) tra il corpo e la bocca, misurata alla stessa altezza
        const glm::vec3 bodyAtMuzzleHeight(bodyPos.x, barrelOrigin.y, bodyPos.z);
        return (std::max)(0.0f, glm::dot(barrelOrigin - bodyAtMuzzleHeight, shootDir));
    }

    
    WeaponSystem::~WeaponSystem() {
        ShutdownAudio();
    }

    void WeaponSystem::InitAudio(ma_engine* engine, ma_sound_group* group, const std::string& audioDir) {
        // Se già inizializzato, shakera il vecchio pool per evitare leak
        if (m_AudioInitialized) {
            ShutdownAudio();
        }

        m_Engine = engine;
        m_SFXGroup = group;
        m_AudioDir = audioDir;

        if (m_AudioDir.empty()) {
            m_AudioDir = "GameData/Audio/";
        }
        else if (m_AudioDir.back() != '/' && m_AudioDir.back() != '\\') {
            m_AudioDir += "/";
        }

        m_AudioInitialized = (m_Engine != nullptr);
        SUN_CORE_INFO("WeaponSystem: pool audio inizializzato (dir: {0})", m_AudioDir);
    }

    void WeaponSystem::ShutdownAudio() {
        for (auto& [path, pool] : m_SoundPools) {
            for (auto& item : pool) {
                if (item && item->loaded) {
                    ma_sound_uninit(&item->sound);
                    item->loaded = false;
                }
            }
        }
        m_SoundPools.clear();

        m_Engine = nullptr;
        m_SFXGroup = nullptr;
        m_AudioInitialized = false;
    }

    WeaponSystem::WeaponSoundInstance* WeaponSystem::GetFreeInstance(const std::string& absolutePath) {
        if (!m_AudioInitialized || !m_Engine) return nullptr;

        auto& pool = m_SoundPools[absolutePath];

        // Pool vuoto: crea 8 istanze (abbastanza per full-auto a 700 RPM)
        if (pool.empty()) {
            constexpr int kPoolSize = 8;
            for (int i = 0; i < kPoolSize; ++i) {
                auto inst = std::make_unique<WeaponSoundInstance>();

                ma_sound_config cfg = ma_sound_config_init();
                cfg.pFilePath = absolutePath.c_str();
                cfg.flags = MA_SOUND_FLAG_DECODE;             // suoni brevi → decode in RAM
                cfg.pInitialAttachment = m_SFXGroup;          // aggancia al gruppo SFX

                if (ma_sound_init_ex(m_Engine, &cfg, &inst->sound) == MA_SUCCESS) {
                    inst->loaded = true;
                    pool.push_back(std::move(inst));
                }
                else {
                    SUN_CORE_WARN("WeaponSystem: impossibile caricare {0}", absolutePath);
                    break;   // se il primo fallisce, non insistere
                }
            }
        }

        // Ritorna la prima istanza non in riproduzione
        for (auto& item : pool) {
            if (item && item->loaded && !ma_sound_is_playing(&item->sound)) {
                return item.get();
            }
        }

        // Tutte occupate: ritorna la prima (sovrascrive il più vecchio)
        return pool.empty() ? nullptr : pool.front().get();
    }

    void WeaponSystem::PlayWeaponSound(const glm::vec3& sourcePos, const glm::vec3& listenerPos,
        float baseVolume) {
        if (!m_AudioInitialized || baseVolume <= 0.001f) return;
        if (FeelAudioActive()) return;   // il gioco suona gli spari per tipo di arma (GameFeel): qui resta la riserva

        // Falloff lineare da 1.0 a 0.0 in 40 metri (più secco delle voci Ork)
        // L'ascoltatore vero e' il giocatore locale (il GameLayer lo passa a miniaudio a ogni frame): prima si usava la
        // posizione di chi sparava, cosi' ogni arma, anche lontanissima, suonava a volume pieno come se fosse
        // la propria. listenerPos resta solo come riserva se il motore audio non c'e'.
        glm::vec3 ear = listenerPos;
        if (m_Engine) {
            const ma_vec3f lp = ma_engine_listener_get_position(m_Engine, 0);
            ear = glm::vec3(lp.x, lp.y, lp.z);
        }
        float dist = glm::distance(sourcePos, ear);
        float attenuation = 1.0f;
        if (dist > 1.0f) {
            attenuation = std::clamp(1.0f - ((dist - 1.0f) / 40.0f), 0.0f, 1.0f);
        }
        float finalVol = baseVolume * attenuation;
        if (finalVol <= 0.005f) return;

        // Per ora tutte le armi usano lo stesso file (path risolto una volta)
        std::string path = m_AudioDir + "laser.wav";

        WeaponSoundInstance* inst = GetFreeInstance(path);
        if (!inst) return;

        ma_sound_set_volume(&inst->sound, finalVol);
        ma_sound_set_position(&inst->sound, sourcePos.x, sourcePos.y, sourcePos.z);
        // FIX #7: attenuazione gia' fatta a mano sopra; miniaudio fa solo il panning col listener
        ma_sound_set_attenuation_model(&inst->sound, ma_attenuation_model_none);
        ma_sound_seek_to_pcm_frame(&inst->sound, 0);
        ma_sound_start(&inst->sound);
    }
    // Aspetto del colpo in base all'arma (lampo, proiettile in volo, impatto)
    WeaponFx WeaponSystem::FxFor(const WeaponBlueprint& w, bool underbarrelShotgun) {
        if (underbarrelShotgun || w.pellet_count > 1) return WeaponFx::Shotgun;
        switch (w.projectile_type) {
        case ProjectileType::Laser:
            if (w.melta_shot) return WeaponFx::Melta;
            if (w.base_damage >= 100.0f || w.id.find("long_las") != std::string::npos) return WeaponFx::LongLas;
            if (w.ammo_per_shot >= 2 || w.id.find("hotshot") != std::string::npos) return WeaponFx::HotShot;
            return WeaponFx::Laser;
        case ProjectileType::Ballistic: return w.base_damage >= 100.0f ? WeaponFx::Sniper : WeaponFx::Ballistic;
        case ProjectileType::Bolt: return WeaponFx::Bolt;
        case ProjectileType::Plasma: return WeaponFx::Plasma;
        case ProjectileType::Explosive: return WeaponFx::Explosive;
        case ProjectileType::Flamer: return WeaponFx::Flamer;
        case ProjectileType::ContinuousBeam: return WeaponFx::Melta;
        case ProjectileType::Grav: case ProjectileType::Psychic: return WeaponFx::Grav;
        }
        return WeaponFx::Ballistic;
    }

    float WeaponSystem::CalculateDamageWithFalloff(float baseDamage, float distance, float falloffStart, float falloffEnd) {
        if (falloffStart <= 0.0f || falloffEnd <= falloffStart) {
            return baseDamage;
        }

        if (distance <= falloffStart) {
            return baseDamage;
        }

        if (distance >= falloffEnd) {
            return baseDamage * 0.10f;
        }

        float t = (distance - falloffStart) / (falloffEnd - falloffStart);
        float multiplier = glm::mix(1.0f, 0.10f, t);
        return baseDamage * multiplier;
    }

    void WeaponSystem::Update(Scene& scene, PhysicsSystem& physicsSystem, float deltaTime) {
        UpdatePendingShots(scene, deltaTime);   // i colpi sparati prima arrivano adesso
        auto view = scene.GetRegistry().view<TransformComponent, UnitStatsComponent, WeaponStateComponent, InventoryComponent, CameraRecoilComponent>();

        for (auto entity : view) {
            auto& trans = view.get<TransformComponent>(entity);
            auto& stats = view.get<UnitStatsComponent>(entity);
            auto& wState = view.get<WeaponStateComponent>(entity);
            auto& inventory = view.get<InventoryComponent>(entity);
            auto& recoil = view.get<CameraRecoilComponent>(entity);
            if (scene.GetRegistry().all_of<CorpseComponent>(entity)) continue;   // a terra non si spara
            if (scene.GetRegistry().all_of<TurretMountComponent>(entity)) { wState.wasFiringLastFrame = false; continue; }   // spara la torretta (5v5: il mondo gira anche da morto)

            // Munizioni limitate: ogni arma tiene il suo caricatore, cambiare arma non lo riempie
            auto* ammo = scene.GetRegistry().try_get<AmmoComponent>(entity);
            if (ammo) Ammo::SyncHeld(*ammo, wState, inventory);

            std::string activeWeaponId = inventory.GetActiveWeaponId();
            const WeaponBlueprint* wpBP = BlueprintManager::Get().GetWeapon(activeWeaponId);

            if (!wpBP || wpBP->is_melee || wpBP->is_equipment) {
                // Reset stato trigger: evita di trascinare un fronte di salita
                // stantio quando si passa arma -> melee -> arma.
                wState.wasFiringLastFrame = false;
                wState.burstRemaining = 0;
                continue;
            }

            // Giocatore: mouse e tasto R. Bot: il grilletto lo imposta l'IA (WeaponTriggerComponent).
            const bool isPlayer = scene.GetRegistry().all_of<PlayerTag>(entity);
            const auto* trigger = scene.GetRegistry().try_get<WeaponTriggerComponent>(entity);
            if (!isPlayer && !trigger) {
                wState.wasFiringLastFrame = false;   // unita' senza controllo (es. nemici dell'orda): non spara
                continue;
            }
            bool wantsToFire = isPlayer ? Input::IsMouseButtonPressed(0) : trigger->fire;
            const bool wantsReload = isPlayer ? Input::IsKeyPressed(GLFW_KEY_R) : trigger->reload;

            // Preparazione del tiro: fermi la mira si assesta (piu' in fretta con buona capacita' di tiro e
            // accovacciati); dal fianco si arriva al massimo a meta' strada, muovendosi poco, correndo per niente.
            {
                const float skill = Marksmanship::Skill01(stats);
                float speed = 0.0f;
                if (wState.hasLastPos && deltaTime > 0.0f) {
                    const glm::vec2 d(trans.Position.x - wState.lastPos.x, trans.Position.z - wState.lastPos.z);
                    speed = glm::length(d) / deltaTime;
                }
                wState.lastPos = trans.Position; wState.hasLastPos = true;
                float cap = 1.0f;
                if (stats.sprinting) cap = 0.0f;
                else if (speed > 0.6f && speed < 30.0f) cap = 0.3f;
                if (isPlayer && !Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) cap = std::min(cap, 0.6f);
                float rate = 1.0f / Marksmanship::SettleTime(skill);
                if (const auto* cr = scene.GetRegistry().try_get<CrouchComponent>(entity); cr && cr->amount > 0.5f) rate *= 1.5f;
                if (wState.aimSteady < cap) wState.aimSteady = std::min(cap, wState.aimSteady + rate * deltaTime);
                else wState.aimSteady = std::max(cap, wState.aimSteady - 3.0f * deltaTime);
                wState.sinceShot += deltaTime;
                // Rinculo: durante la raffica recupera piano (la mira sale); chi tira bene lo tiene giu' meglio.
                // I bot "tirano giu'" da soli un po' di piu', non avendo il mouse.
                if (wState.sinceShot < 0.3f) recoil.recoverySpeed = isPlayer ? (2.0f + 3.0f * skill) : (4.0f + 8.0f * skill);
                else recoil.recoverySpeed = 10.0f;
            }

            // -------- ARMI A RAGGIO CONTINUO (MELTA GUN) --------
            // Restano "auto finché tieni premuto": non ha senso semi/burst su un beam.
            if (wpBP->projectile_type == ProjectileType::ContinuousBeam) {
                if (wantsToFire && !wState.isOverheating && !wState.isReloading) {
                    FireContinuousBeam(scene, entity, wState, recoil, *wpBP, trans, stats, deltaTime);
                }
                else {
                    if (wState.currentHeat > 0.0f) {
                        float heatDissipationRate = 35.0f;
                        wState.currentHeat -= heatDissipationRate * deltaTime;
                        if (wState.currentHeat <= 0.0f) {
                            wState.currentHeat = 0.0f;
                            wState.isOverheating = false;
                        }
                    }
                }
                wState.wasFiringLastFrame = wantsToFire;
                continue;
            }

            if (wState.fireCooldown > 0.0f) {
                wState.fireCooldown -= deltaTime;
                if (wState.fireCooldown < 0.0f) wState.fireCooldown = 0.0f;
            }

            if (wState.currentHeat > 0.0f) {
                float heatDissipationRate = 25.0f;
                wState.currentHeat -= heatDissipationRate * deltaTime;
                if (wState.currentHeat <= 0.0f) {
                    wState.currentHeat = 0.0f;
                    wState.isOverheating = false;
                }
            }

            if (wState.isReloading) {
                // Buff di fazione (WAAAGH!): ricarica piu' veloce
                const auto* buff = scene.GetRegistry().try_get<FactionBuffComponent>(entity);
                wState.reloadTimer -= deltaTime * (buff ? buff->reloadSpeedMult : 1.0f);
                if (wState.reloadTimer <= 0.0f) {
                    wState.isReloading = false;
                    wState.reloadTimer = 0.0f;
                    if (ammo) Ammo::FinishReload(*ammo, wState, *wpBP);   // i colpi arrivano dalla riserva
                    else wState.currentAmmo = wpBP->magazine_size;
                    if (isPlayer) SUN_CORE_INFO("Arma ricaricata! Munizioni: {0}", wState.currentAmmo);
                }
                wState.wasFiringLastFrame = wantsToFire;
                continue;
            }

            if (wantsReload && wState.currentAmmo < wpBP->magazine_size && !wState.isReloading && (!ammo || Ammo::CanReload(*ammo))) {
                wState.isReloading = true;
                wState.reloadTimer = wpBP->reload_time_sec;
                if (isPlayer) SUN_CORE_INFO("Ricarica avviata...");
                wState.wasFiringLastFrame = wantsToFire;
                continue;
            }

            if (wState.currentSpread > wpBP->spread_base) {
                wState.currentSpread -= 2.0f * deltaTime;
                if (wState.currentSpread < wpBP->spread_base) wState.currentSpread = wpBP->spread_base;
            }

            if (recoil.recoilPitch > 0.0f) {
                recoil.recoilPitch -= recoil.recoverySpeed * deltaTime;
                if (recoil.recoilPitch < 0.0f) recoil.recoilPitch = 0.0f;
            }
            if (std::abs(recoil.recoilYaw) > 0.0f) {
                recoil.recoilYaw -= (recoil.recoilYaw > 0.0f ? 1.0f : -1.0f) * recoil.recoverySpeed * deltaTime;
                if (std::abs(recoil.recoilYaw) < 0.05f) recoil.recoilYaw = 0.0f;
            }

            // ============================================================
            // LOGICA FIRE MODES
            //   "auto"  -> spara finche' il grilletto e' premuto
            //   "semi"  -> spara sul fronte di salita (1 click = 1 colpo)
            //   "burst" -> N colpi per click, gestiti con burstRemaining
            // Fallback: se fire_modes e' vuoto -> "auto" (compatibilita').
            // ============================================================
            std::string effectiveMode = "auto";

            if (wState.currentFireMode != "standard" && !wState.currentFireMode.empty()) {
                // Sotto-canna attivo (es. shotgun): sempre un colpo per click.
                effectiveMode = "semi";
            }
            else if (!wpBP->fire_modes.empty()) {
                effectiveMode = wpBP->fire_modes[0];
            }

            const bool triggerJustPressed = wantsToFire && !wState.wasFiringLastFrame;

            bool wantsToShootThisFrame = false;
            if (effectiveMode == "auto") {
                wantsToShootThisFrame = wantsToFire;
            }
            else if (effectiveMode == "semi" || effectiveMode == "single") {
                wantsToShootThisFrame = triggerJustPressed;
            }
            else if (effectiveMode == "burst") {
                if (triggerJustPressed) {
                    wState.burstRemaining = wpBP->burst_count;
                }
                if (wState.burstRemaining > 0) {
                    wantsToShootThisFrame = true;   // la raffica si completa anche se rilasci il grilletto
                }
            }
            else {
                wantsToShootThisFrame = wantsToFire;   // fallback
            }

            if (wantsToShootThisFrame && wState.fireCooldown <= 0.0f && !wState.isOverheating) {
                if (wState.currentAmmo >= std::max(1, wpBP->ammo_per_shot)) {   // hot-shot: 2 colpi per sparo
                    FireWeapon(scene, entity, wState, recoil, *wpBP, trans, stats);
                    if (effectiveMode == "burst" && wState.burstRemaining > 0) {
                        wState.burstRemaining--;
                    }
                }
                else if (!wState.isReloading && (!ammo || Ammo::CanReload(*ammo))) {
                    wState.isReloading = true;
                    wState.reloadTimer = wpBP->reload_time_sec;
                    if (isPlayer) SUN_CORE_WARN("Caricatore vuoto! Ricarica automatica...");
                }
                else if (triggerJustPressed && ammo) {
                    // Niente caricatore e niente riserva: "click" a vuoto
                    ammo->feedback = "Munizioni esaurite";
                    ammo->feedbackTimer = 1.5f;
                }
            }

            wState.wasFiringLastFrame = wantsToFire;
        }
    }

    void WeaponSystem::FireContinuousBeam(Scene& scene, entt::entity shooter,
        WeaponStateComponent& wState, CameraRecoilComponent& recoil,
        const WeaponBlueprint& wpBP, TransformComponent& trans,
        UnitStatsComponent& stats, float deltaTime) {
        float heatRate = 30.0f;
        wState.currentHeat += heatRate * deltaTime;
        if (wState.currentHeat >= wpBP.heat_capacity) {
            wState.currentHeat = wpBP.heat_capacity;
            wState.isOverheating = true;
            SUN_CORE_WARN("MELTA GUN SURRISCALDATA!");
            return;
        }

        if (Random::Chance(0.25f) && wState.currentAmmo > 0) {
            wState.currentAmmo--;
        }

        recoil.recoilPitch += wpBP.recoil_force * 0.15f * deltaTime;
        recoil.shakeIntensity = 0.12f;

        glm::mat4 rotMat = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
        rotMat = glm::rotate(rotMat, glm::radians(trans.AimPitch + recoil.recoilPitch), glm::vec3(1.0f, 0.0f, 0.0f));
        glm::vec3 baseDir = glm::normalize(glm::vec3(rotMat * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

        glm::vec3 barrelOrigin = trans.Position + glm::vec3(0.0f, 1.2f, 0.0f) + (baseDir * 0.5f);

        if (scene.GetRegistry().all_of<SkeletonComponent>(shooter)) {
            auto& skel = scene.GetRegistry().get<SkeletonComponent>(shooter);
            auto worldTransforms = skel.data.ComputeWorldTransforms(trans.Position, trans.Rotation, skel.scale);

            bool isLeftHand = false;
            if (scene.GetRegistry().all_of<InventoryComponent>(shooter)) {
                auto& inv = scene.GetRegistry().get<InventoryComponent>(shooter);
                if (inv.GetEquipStance() == EquipStance::HybridRangedMelee) {
                    isLeftHand = true;
                }
            }

            std::string targetArm = isLeftHand ? "Arm_L" : "Arm_R";
            int armIndex = -1;
            for (size_t i = 0; i < skel.data.blocks.size(); ++i) {
                if (skel.data.blocks[i].name == targetArm) {
                    armIndex = static_cast<int>(i);
                    break;
                }
            }

            if (armIndex >= 0 && armIndex < static_cast<int>(worldTransforms.size())) {
                glm::mat4 armWorldMat = worldTransforms[armIndex];
                glm::vec3 holdOffset = wpBP.hold_offset;
                if (isLeftHand) holdOffset.x = -holdOffset.x;

                glm::mat4 weaponWorldMat = glm::translate(armWorldMat, holdOffset);
                glm::vec3 weaponPos = glm::vec3(weaponWorldMat[3]);

                glm::mat4 weapRot = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
                weapRot = glm::rotate(weapRot, glm::radians(trans.AimPitch + recoil.recoilPitch), glm::vec3(1.0f, 0.0f, 0.0f));

                glm::vec3 barrelWorldOffset = glm::vec3(weapRot * glm::vec4(wpBP.barrel_offset, 0.0f));
                barrelOrigin = weaponPos + barrelWorldOffset;
            }
        }
        float rayPullBack = ApplyMuzzleOverride(scene, shooter, barrelOrigin, trans.Position, baseDir);
        float maxRange = (wpBP.range_effective > 1.0f) ? wpBP.range_effective : 12.0f;
        // Il raggio converge sul punto del mirino (telecamera sulla spalla): ricalcolo l'arretramento sulla nuova linea
        baseDir = AimAtCrosshair(scene, shooter, barrelOrigin, baseDir, maxRange, recoil.recoilPitch);
        if (rayPullBack > 0.0f) {
            const glm::vec3 bodyAtMuzzleHeight(trans.Position.x, barrelOrigin.y, trans.Position.z);
            rayPullBack = (std::max)(0.0f, glm::dot(barrelOrigin - bodyAtMuzzleHeight, baseDir));
        }
        // Raggio di colpo: stessa linea della canna, ma parte dal corpo (rayPullBack metri piu' indietro)
        RaycastResult hit = RaycastAll(scene, barrelOrigin - baseDir * rayPullBack, baseDir, maxRange + rayPullBack, shooter);
        if (hit.hit) hit.distance = (std::max)(0.0f, hit.distance - rayPullBack);   // distanza dalla bocca

        glm::vec3 hitPoint = hit.hit ? hit.point : (barrelOrigin + baseDir * maxRange);

        if (hit.hitUnit && hit.entity != entt::null &&
            scene.GetRegistry().all_of<UnitStatsComponent, HealthComponent>(hit.entity)) {

            auto& targetStats = scene.GetRegistry().get<UnitStatsComponent>(hit.entity);
            auto& targetHealth = scene.GetRegistry().get<HealthComponent>(hit.entity);

            float dps = (wpBP.base_damage > 1.0f) ? wpBP.base_damage : 120.0f;
            float frameDamage = dps * deltaTime;
            int effectiveAP = wpBP.armor_penetration;

            float meltaThreshold = (wpBP.melta_range > 0.5f) ? wpBP.melta_range : 4.5f;
            if (hit.distance <= meltaThreshold) {
                float meltaMult = (wpBP.melta_multiplier > 1.0f) ? wpBP.melta_multiplier : 2.5f;
                frameDamage *= meltaMult;
                effectiveAP += 20;
            }

            const float healthBefore = targetHealth.currentHealth;
            // Parata (Q) con scudo o "perfect deflect": il raggio viene deviato, niente danno
            if (!DamageResolver::TryParryRanged(scene, hit.entity, baseDir)) {
                DamageResolver::ResolveHit(frameDamage, effectiveAP, hit.location, targetStats, targetHealth);
                EmitHitFeedback(scene, shooter, hit.entity, hit.point, hit.location, healthBefore);
            }
        }

        entt::entity meltaBeamEntity = scene.CreateEntity();
        auto& beamTrail = scene.GetRegistry().emplace<TrailRequestComponent>(meltaBeamEntity);
        beamTrail.start = barrelOrigin;
        beamTrail.end = hitPoint;
        beamTrail.color = (wpBP.primary_color.a > 0.01f) ? wpBP.primary_color : glm::vec4(1.0f, 0.40f, 0.05f, 0.95f);
        beamTrail.isBeam = true;
        beamTrail.projectileWidth = (wpBP.projectile_width > 0.05f) ? wpBP.projectile_width : 0.38f;
        beamTrail.travelTime = 0.001f;
        beamTrail.totalLife = std::max(0.05f, deltaTime * 2.5f);
        beamTrail.fx = static_cast<int>(WeaponFx::Melta);
        beamTrail.hitSomething = hit.hit && Random::Chance(0.3f);   // metallo fuso ogni tanto, non a ogni frame
        beamTrail.hitUnit = hit.hitUnit;
        if (Random::Chance(0.35f)) { VfxEvent mz; mz.kind = VfxKind::Muzzle; mz.fx = WeaponFx::Melta; mz.pos = barrelOrigin; mz.dir = baseDir; EmitVfx(mz); }
        { FeelEvent fe; fe.kind = FeelKind::Shot; fe.fx = WeaponFx::Melta; fe.pos = barrelOrigin; fe.dir = baseDir; fe.source = static_cast<std::uint32_t>(shooter); fe.flag = true; EmitFeel(fe); }   // flag = raggio continuo
    }

    void WeaponSystem::FireWeapon(Scene& scene, entt::entity shooter,
        WeaponStateComponent& wState, CameraRecoilComponent& recoil,
        const WeaponBlueprint& wpBP, TransformComponent& trans,
        UnitStatsComponent& stats) {
        wState.currentAmmo = std::max(0, wState.currentAmmo - std::max(1, wpBP.ammo_per_shot));
        wState.fireCooldown = (wpBP.rate_of_fire > 0.0f) ? (60.0f / wpBP.rate_of_fire) : 0.15f;
        const auto* orderBuff = scene.GetRegistry().try_get<FactionBuffComponent>(shooter);   // es. "Take AIM!"
        if (orderBuff && orderBuff->fireRateMult > 0.0f) wState.fireCooldown /= orderBuff->fireRateMult;

        wState.currentHeat += 12.0f;
        if (wState.currentHeat >= wpBP.heat_capacity) {
            wState.currentHeat = wpBP.heat_capacity;
            wState.isOverheating = true;
            SUN_CORE_WARN("ARMA SURRISCALDATA!");
        }

        // Stabilita' del tiratore: accovacciato (fino a -35%), ordine "Take AIM!", insegna imperiale vicina,
        // elmetto con visiera. Riduce rinculo, ventaglio, deviazione e colpi di striscio.
        const float skill = Marksmanship::Skill01(stats);
        float steadyMult = 1.0f;
        if (const auto* cr = scene.GetRegistry().try_get<CrouchComponent>(shooter))
            steadyMult *= 1.0f - 0.35f * std::clamp(cr->amount, 0.0f, 1.0f);
        if (orderBuff) steadyMult *= orderBuff->spreadMult;
        if (const auto* aura = scene.GetRegistry().try_get<MoraleAuraComponent>(shooter); aura && aura->timeLeft > 0.0f)
            steadyMult *= std::max(0.0f, 1.0f - aura->bonus);
        if (const auto* helm = BlueprintManager::Get().GetArmorPiece(stats.armorSet.head.id); helm && helm->aim_bonus > 0.0f)
            steadyMult *= std::max(0.0f, 1.0f - helm->aim_bonus);
        const float shotSteady = wState.aimSteady;   // assestamento al momento del colpo
        wState.aimSteady = std::max(0.0f, wState.aimSteady - Marksmanship::kSteadyLossPerShot);
        wState.sinceShot = 0.0f;

        // Rinculo di QUESTO colpo: si applica dopo averlo sparato (il primo colpo va dove punti). Spinge la mira
        // verso l'alto lungo un raggio a caso di un arco largo quanto e' scarsa la capacita' di tiro.
        struct KickOnExit {
            CameraRecoilComponent& r; float pitch, yaw;
            ~KickOnExit() { r.recoilPitch = std::min(r.recoilPitch + pitch, Marksmanship::kMaxRecoilPitch); r.recoilYaw += yaw; }
        };
        const float kick = wpBP.recoil_force * Marksmanship::KickMult(skill) * steadyMult;
        const float arcHalf = glm::radians(Marksmanship::SprayArcDeg(skill) * 0.5f) * steadyMult;
        const float theta = Random::Range(-arcHalf, arcHalf);
        KickOnExit kickGuard{ recoil, kick * std::cos(theta), kick * std::sin(theta) };

        glm::mat4 rotMat = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw + recoil.recoilYaw), glm::vec3(0.0f, 1.0f, 0.0f));
        rotMat = glm::rotate(rotMat, glm::radians(trans.AimPitch + recoil.recoilPitch), glm::vec3(1.0f, 0.0f, 0.0f));
        glm::vec3 baseDir = glm::normalize(glm::vec3(rotMat * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

        glm::vec3 barrelOrigin = trans.Position + glm::vec3(0.0f, 1.2f, 0.0f) + (baseDir * 0.5f);

        if (scene.GetRegistry().all_of<SkeletonComponent>(shooter)) {
            auto& skel = scene.GetRegistry().get<SkeletonComponent>(shooter);
            auto worldTransforms = skel.data.ComputeWorldTransforms(trans.Position, trans.Rotation, skel.scale);

            bool isLeftHand = false;
            if (scene.GetRegistry().all_of<InventoryComponent>(shooter)) {
                auto& inv = scene.GetRegistry().get<InventoryComponent>(shooter);
                if (inv.GetEquipStance() == EquipStance::HybridRangedMelee) {
                    isLeftHand = true;
                }
            }

            std::string targetArm = isLeftHand ? "Arm_L" : "Arm_R";
            int armIndex = -1;
            for (size_t i = 0; i < skel.data.blocks.size(); ++i) {
                if (skel.data.blocks[i].name == targetArm) {
                    armIndex = static_cast<int>(i);
                    break;
                }
            }

            if (armIndex >= 0 && armIndex < static_cast<int>(worldTransforms.size())) {
                glm::mat4 armWorldMat = worldTransforms[armIndex];
                glm::vec3 armPos = glm::vec3(armWorldMat[3]);

                glm::vec3 holdOffset = wpBP.hold_offset;
                if (isLeftHand) holdOffset.x = -holdOffset.x;

                glm::vec3 offsetInWorld = glm::vec3(armWorldMat * glm::vec4(holdOffset, 0.0f));
                glm::vec3 weaponPos = armPos + offsetInWorld;

                glm::mat4 weapRot = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
                weapRot = glm::rotate(weapRot, glm::radians(trans.AimPitch + recoil.recoilPitch), glm::vec3(1.0f, 0.0f, 0.0f));

                glm::vec3 barrelWorldOffset = glm::vec3(weapRot * glm::vec4(wpBP.barrel_offset, 0.0f));
                barrelOrigin = weaponPos + barrelWorldOffset;
            }
        }

        float rayPullBack = ApplyMuzzleOverride(scene, shooter, barrelOrigin, trans.Position, baseDir);
        // I colpi convergono sul punto del mirino (telecamera sulla spalla): ricalcolo l'arretramento sulla nuova linea
        baseDir = AimAtCrosshair(scene, shooter, barrelOrigin, baseDir,
            (wpBP.range_effective > 1.0f) ? wpBP.range_effective : 12.0f, recoil.recoilPitch);
        if (rayPullBack > 0.0f) {
            const glm::vec3 bodyAtMuzzleHeight(trans.Position.x, barrelOrigin.y, trans.Position.z);
            rayPullBack = (std::max)(0.0f, glm::dot(barrelOrigin - bodyAtMuzzleHeight, baseDir));
        }
        if (wpBP.projectile_type == ProjectileType::Flamer) {
            // Il cono del lanciafiamme parte dal corpo (stessa ragione del raggio di colpo sotto)
            FireFlamer(scene, shooter, wpBP, barrelOrigin - baseDir * rayPullBack, baseDir);
            // Getto di fiamme vero (particelle che volano, crescono e diventano fumo) al posto della striscia arancione
            VfxEvent jet; jet.kind = VfxKind::FlamerJet; jet.pos = barrelOrigin; jet.dir = baseDir; jet.scale = wpBP.range_effective;
            EmitVfx(jet);
            { FeelEvent fe; fe.kind = FeelKind::Shot; fe.fx = WeaponFx::Flamer; fe.pos = barrelOrigin; fe.dir = baseDir; fe.source = static_cast<std::uint32_t>(shooter); fe.flag = true; EmitFeel(fe); }
            return;
        }

        // Lanciagranate: il colpo e' una granata vera (gravita', rimbalzi o impatto) sparata dalla canna.
        // Il giocatore tira dritto lungo il mirino (alzando la mira va piu' lontano); i bot calcolano la parabola
        // per arrivare sul punto che stanno mirando.
        if (wpBP.launches_grenade) {
            const float speed = (std::max)(5.0f, wpBP.projectile_speed);
            glm::vec3 vel = baseDir * speed;
            if (!scene.GetRegistry().all_of<PlayerTag>(shooter)) {
                const float range = (wpBP.range_effective > 1.0f) ? wpBP.range_effective : 40.0f;
                const RaycastResult aim = RaycastAll(scene, barrelOrigin - baseDir * rayPullBack, baseDir, range + rayPullBack, shooter);
                const glm::vec3 target = aim.hit ? aim.point : barrelOrigin + baseDir * range;
                vel = Explosives::BallisticVelocity(barrelOrigin, target, speed);
            }
            Explosives::ThrowGrenade(scene, shooter, wpBP, barrelOrigin, vel, nullptr);   // nullptr: niente modello dell'arma come granata
            { VfxEvent mz; mz.kind = VfxKind::Muzzle; mz.fx = WeaponFx::Ballistic; mz.pos = barrelOrigin; mz.dir = baseDir; EmitVfx(mz); }
            { FeelEvent fe; fe.kind = FeelKind::Shot; fe.fx = WeaponFx::Explosive; fe.pos = barrelOrigin; fe.dir = baseDir; fe.source = static_cast<std::uint32_t>(shooter); fe.scale = wpBP.base_damage; EmitFeel(fe); }
            PlayWeaponSound(barrelOrigin, trans.Position + glm::vec3(0.0f, 1.2f, 0.0f), 0.8f);
            return;
        }

        bool isUnderbarrelShotgun = (wState.currentFireMode == "shotgun" || wState.currentFireMode == "underbarrel_shotgun" || wState.currentFireMode == wpBP.underbarrel_type);
        bool isShotgun = (wpBP.pellet_count > 1) || isUnderbarrelShotgun;

        int numProjectiles = wpBP.pellet_count;
        if (numProjectiles <= 1 && isUnderbarrelShotgun) {
            numProjectiles = 8;
        }

        float spreadBase = wpBP.spread_base;
        if (spreadBase <= 0.001f && isShotgun) {
            spreadBase = 0.09f;
        }
        // Rosa dei pallettoni (solo fucili a pompa): e' il disegno dell'arma, non dipende dal tiratore
        const float totalSpread = isShotgun ? spreadBase : 0.0f;

        glm::vec3 rightDir = glm::normalize(glm::cross(baseDir, glm::vec3(0.0f, 1.0f, 0.0f)));
        if (glm::length(rightDir) < 0.001f) {
            rightDir = glm::normalize(glm::cross(baseDir, glm::vec3(1.0f, 0.0f, 0.0f)));
        }
        glm::vec3 upDir = glm::normalize(glm::cross(rightDir, baseDir));

        // Mira non assestata (dal fianco, muovendosi, appena dopo un colpo): un po' di deviazione casuale,
        // uguale per tutti i pallettoni. A mira assestata e' zero: il colpo va esattamente sul mirino.
        {
            const float cone = Marksmanship::UnsteadyCone(skill, isShotgun ? 0.0f : spreadBase, shotSteady) * steadyMult;
            if (cone > 1e-5f) {
                const float r = std::sqrt(Random::Float01()) * cone;
                const float a = Random::Range(0.0f, 6.28318530718f);
                baseDir = glm::normalize(baseDir + (rightDir * std::cos(a) + upDir * std::sin(a)) * r);
            }
        }
        const float grazeChance = Marksmanship::GrazeChance(skill, shotSteady) * steadyMult;
        const WeaponFx fx = FxFor(wpBP, isUnderbarrelShotgun);
        { VfxEvent mz; mz.kind = VfxKind::Muzzle; mz.fx = fx; mz.pos = barrelOrigin; mz.dir = baseDir; mz.color = wpBP.primary_color; EmitVfx(mz); }
        { FeelEvent fe; fe.kind = FeelKind::Shot; fe.fx = fx; fe.pos = barrelOrigin; fe.dir = baseDir; fe.color = wpBP.primary_color;
          fe.source = static_cast<std::uint32_t>(shooter); fe.scale = wpBP.base_damage * static_cast<float>(std::max(1, numProjectiles)); EmitFeel(fe); }

        for (int p = 0; p < numProjectiles; ++p) {
            glm::vec3 shootDir = baseDir;

            if (isShotgun || totalSpread > 0.001f) {
                float r = Random::Float01() * totalSpread;
                float theta = Random::Range(0.0f, 6.28318530718f); 
                glm::vec3 pelletOffset = (rightDir * std::cos(theta) + upDir * std::sin(theta)) * r;
                shootDir = glm::normalize(baseDir + pelletOffset);
            }
            // Raggio di colpo: stessa linea della canna, ma parte dal corpo (rayPullBack metri piu' indietro)
            RaycastResult hit = RaycastAll(scene, barrelOrigin - shootDir * rayPullBack, shootDir,
                wpBP.range_effective + rayPullBack, shooter);
            if (hit.hit) hit.distance = (std::max)(0.0f, hit.distance - rayPullBack);   // distanza dalla bocca

            glm::vec3 hitPoint = hit.hit
                ? hit.point
                : (barrelOrigin + shootDir * wpBP.range_effective);
            // Parata (Q) con scudo o "perfect deflect": il colpo viene deviato, niente danno
            const bool parried = hit.hitUnit && hit.entity != entt::null
                && DamageResolver::TryParryRanged(scene, hit.entity, shootDir);

            // Il colpo e' deciso adesso (chi e dove), ma danno, sussulto e suono dell'impatto arrivano quando il
            // proiettile che si vede ci arriva davvero. Prima il bersaglio perdeva vita (e moriva) all'istante,
            // con il bolt o il globo di plasma ancora a meta' strada.
            PendingShot ps;
            ps.shooter = shooter; ps.target = hit.entity; ps.team = Explosives::TeamOf(scene.GetRegistry(), shooter);
            ps.hit = hit.hit; ps.hitUnit = hit.hitUnit; ps.parried = parried;
            ps.location = hit.location; ps.distance = hit.distance; ps.point = hit.point; ps.hitPoint = hitPoint; ps.dir = shootDir;
            ps.underbarrelShotgun = isUnderbarrelShotgun;
            ps.graze = Random::Chance(grazeChance);
            ps.fx = fx; ps.pellets = numProjectiles;
            ps.color = isUnderbarrelShotgun ? glm::vec4(1.0f, 0.7f, 0.2f, 1.0f) : wpBP.primary_color;
            ps.baseDamage = wpBP.base_damage; ps.armorPenetration = wpBP.armor_penetration;
            ps.falloffStart = wpBP.damage_falloff_start; ps.falloffEnd = wpBP.damage_falloff_end;
            ps.meltaShot = wpBP.melta_shot; ps.meltaRange = wpBP.melta_range; ps.meltaMultiplier = wpBP.melta_multiplier;
            ps.type = wpBP.projectile_type;
            ps.explosionRadius = wpBP.explosion_radius; ps.explosionDamage = wpBP.explosion_damage; ps.structureMultiplier = wpBP.structure_damage_multiplier;

            // Colpo psichico su un'unita': nessun proiettile, effetto immediato
            if (wpBP.projectile_type == ProjectileType::Psychic && !parried && hit.hitUnit && hit.entity != entt::null) {
                ResolveShot(scene, ps);
                continue;
            }
            entt::entity bulletTrailEntity = scene.CreateEntity();
            auto& bulletTrail = scene.GetRegistry().emplace<TrailRequestComponent>(bulletTrailEntity);
            bulletTrail.start = barrelOrigin;
            bulletTrail.end = hitPoint;
            bulletTrail.color = isUnderbarrelShotgun ? glm::vec4(1.0f, 0.7f, 0.2f, 1.0f) : wpBP.primary_color;
            bulletTrail.isBeam = (wpBP.projectile_type == ProjectileType::Laser);
            bulletTrail.projectileWidth = isShotgun ? 0.035f : wpBP.projectile_width;
            bulletTrail.travelTime = (wpBP.projectile_type == ProjectileType::Laser) ? 0.05f : 0.12f;
            bulletTrail.totalLife = 0.20f;
            // Aspetto per tipo di arma (ParticleSystem): velocita' del colpo in volo e quanto resta la scia
            bulletTrail.fx = static_cast<int>(fx);
            bulletTrail.hitSomething = hit.hit && !parried;
            bulletTrail.hitUnit = hit.hitUnit && !parried;
            const float dist = glm::length(hitPoint - barrelOrigin);
            auto flight = [&](float speed, float minT, float maxT, float linger) {
                bulletTrail.travelTime = std::clamp(dist / speed, minT, maxT);
                bulletTrail.totalLife = bulletTrail.travelTime + linger;
            };
            switch (fx) {
            case WeaponFx::Laser: case WeaponFx::HotShot: flight(170.0f, 0.03f, 0.45f, 0.01f); break;
            case WeaponFx::LongLas: case WeaponFx::Melta: bulletTrail.travelTime = 0.001f; bulletTrail.totalLife = fx == WeaponFx::Melta ? 0.25f : 0.35f; break;
            case WeaponFx::Ballistic: case WeaponFx::Shotgun: bulletTrail.style = 1; flight(300.0f, 0.02f, 0.14f, 0.01f); break;
            case WeaponFx::Sniper: flight(450.0f, 0.02f, 0.5f, 0.6f); break;
            case WeaponFx::Bolt: bulletTrail.style = 2; flight(140.0f, 0.05f, 0.4f, 0.6f); break;
            case WeaponFx::Plasma: flight(65.0f, 0.05f, 1.0f, 0.01f); break;
            case WeaponFx::Explosive: flight(90.0f, 0.05f, 0.8f, 0.9f); bulletTrail.impactScale = std::max(1.0f, wpBP.explosion_radius); bulletTrail.hitSomething = !parried; break;
            case WeaponFx::Grav: flight(80.0f, 0.05f, 0.6f, 0.01f); break;
            default: break;
            }
            // partita in rete: la scia del mio colpo la vedono anche gli altri
            if (scene.GetRegistry().all_of<PlayerTag>(shooter) && NetShotQueue().size() < 256) NetShotQueue().push_back({ bulletTrail, p == 0 });
            if (bulletTrail.travelTime > 0.03f) { ps.timer = bulletTrail.travelTime; m_PendingShots.push_back(ps); }
            else ResolveShot(scene, ps);
        }
        // Suono posizionale: sorgente = bocca di fuoco; l'attenuazione usa l'ascoltatore (vedi PlayWeaponSound)
        glm::vec3 listenerPos = trans.Position + glm::vec3(0.0f, 1.2f, 0.0f);

        // Distingui per proiettile: melta/flamer/laser hanno timbri diversi, ma per
        // ora usiamo lo stesso file — la struttura è pronta per essere estesa.
        float baseVol = 1.0f;
        if (isShotgun) baseVol = 1.15f;   // shotgun un po' più "cattivo"
        if (isUnderbarrelShotgun) baseVol = 0.9f;

        PlayWeaponSound(barrelOrigin, listenerPos, baseVol);
    }

    // Il colpo arriva: danno, colpo sulle strutture, esplosione, e l'evento di impatto (suono, sussulto, segno)
    void WeaponSystem::ResolveShot(Scene& scene, const PendingShot& ps) {
        auto& reg = scene.GetRegistry();
        const entt::entity shooter = reg.valid(ps.shooter) ? ps.shooter : entt::entity(entt::null);
        const bool targetValid = ps.target != entt::null && reg.valid(ps.target);
        bool killed = false, armored = false;

        if (!ps.parried && ps.hitUnit && targetValid && !reg.all_of<CorpseComponent>(ps.target) &&
            reg.all_of<UnitStatsComponent, HealthComponent>(ps.target)) {
            auto& targetStats = reg.get<UnitStatsComponent>(ps.target);
            auto& targetHealth = reg.get<HealthComponent>(ps.target);

            float finalDamage = ps.underbarrelShotgun ? 15.0f : ps.baseDamage;
            int effectiveAP = ps.underbarrelShotgun ? 5 : ps.armorPenetration;
            const float healthBefore = targetHealth.currentHealth;
            // Di striscio: con scarsa capacita' di tiro il colpo prende solo di lato (meno danno)
            if (ps.graze) finalDamage *= Marksmanship::kGrazeDamage;

            finalDamage = CalculateDamageWithFalloff(finalDamage, ps.distance, ps.falloffStart, ps.falloffEnd);
            // Melta a colpo singolo: a bruciapelo scioglie tutto
            if (ps.meltaShot && ps.distance <= ps.meltaRange) {
                finalDamage *= (std::max)(1.0f, ps.meltaMultiplier);
                effectiveAP += 20;
            }

            if (ps.type == ProjectileType::Grav) {
                ArmorPiece* armor = targetStats.armorSet.GetPiece(ps.location);
                int targetArmorRating = armor ? armor->armorRating : targetStats.armorSet.GetAverageRating();
                finalDamage = ps.baseDamage + (targetArmorRating * 1.5f);
                DamageResolver::ResolveHit(finalDamage, effectiveAP, ps.location, targetStats, targetHealth);
            }
            else if (ps.type == ProjectileType::Psychic) {
                targetHealth.currentHealth -= finalDamage;
            }
            else {
                DamageResolver::ResolveHit(finalDamage, effectiveAP, ps.location, targetStats, targetHealth);
            }
            EmitHitFeedback(scene, shooter, ps.target, ps.point, ps.location, healthBefore, ps.graze);
            killed = healthBefore > 0.0f && targetHealth.currentHealth <= 0.0f;
            { const ArmorPiece* ap = targetStats.armorSet.GetPiece(ps.location); armored = (ap ? ap->armorRating : targetStats.armorSet.GetAverageRating()) >= 40; }
        }
        // Colpo su una struttura costruita (barricata, muro): meta' danno, puo' romperla
        else if (!ps.parried && ps.hit && !ps.hitUnit && targetValid && reg.all_of<StructureHealthComponent>(ps.target)) {
            const float dmg = CalculateDamageWithFalloff(ps.baseDamage, ps.distance, ps.falloffStart, ps.falloffEnd);
            if (DamageStructure(reg, ps.target, dmg * XpValues::StructureBulletDamage)) scene.DestroyEntity(ps.target);
        }
        // Proiettile esplosivo (es. Rokkit Launcha): esplode dove arriva, colpendo i nemici attorno
        if (ps.type == ProjectileType::Explosive && ps.explosionRadius > 0.0f && !ps.parried) {
            Explosives::Explode(scene, ps.hitPoint, ps.explosionRadius, ps.explosionDamage, ps.armorPenetration,
                                shooter, ps.team, ps.structureMultiplier, false);
        }
        if (ps.type == ProjectileType::Psychic) return;
        if ((ps.hit || ps.fx == WeaponFx::Explosive) && !ps.parried) {
            FeelEvent fe; fe.kind = FeelKind::Impact; fe.fx = ps.fx; fe.pos = ps.hitPoint; fe.dir = ps.dir; fe.color = ps.color;
            fe.source = static_cast<std::uint32_t>(ps.shooter); fe.target = static_cast<std::uint32_t>(ps.target);
            fe.hitUnit = ps.hitUnit; fe.killed = killed; fe.armored = armored;
            fe.scale = ps.fx == WeaponFx::Explosive ? std::max(1.0f, ps.explosionRadius) : ps.baseDamage;
            fe.variant = ps.pellets;
            EmitFeel(fe);
        }
    }

    // Colpi in volo: quando il proiettile arriva si applica il danno
    void WeaponSystem::UpdatePendingShots(Scene& scene, float deltaTime) {
        for (size_t i = 0; i < m_PendingShots.size();) {
            m_PendingShots[i].timer -= deltaTime;
            if (m_PendingShots[i].timer <= 0.0f) {
                const PendingShot ps = m_PendingShots[i];
                m_PendingShots[i] = m_PendingShots.back(); m_PendingShots.pop_back();
                ResolveShot(scene, ps);
            }
            else ++i;
        }
    }

    void WeaponSystem::FireFlamer(Scene& scene, entt::entity shooter, const WeaponBlueprint& wpBP,
        const glm::vec3& origin, const glm::vec3& dir) {
        auto view = scene.GetRegistry().view<TransformComponent, UnitStatsComponent, HealthComponent>(
            entt::exclude<CorpseComponent>
        );

        for (auto entity : view) {
            if (entity == shooter) continue;
            if (SameTeam(scene.GetRegistry(), shooter, entity)) continue;   // niente fuoco amico

            const auto& targetTrans = view.get<TransformComponent>(entity);

            // Mira al petto, non ai piedi
            glm::vec3 targetCenter = targetTrans.Position + glm::vec3(0.0f, 0.9f, 0.0f);
            glm::vec3 toTarget = targetCenter - origin;
            float dist = glm::length(toTarget);
            if (dist < 0.001f) continue;
            if (dist > wpBP.range_effective) continue;

            glm::vec3 normToTarget = toTarget / dist;
            float dot = glm::dot(dir, normToTarget);
            float angleDegrees = glm::degrees(std::acos(glm::clamp(dot, -1.0f, 1.0f)));
            if (angleDegrees > (wpBP.flamer_cone_angle * 0.5f)) continue;

            // LOS check: se colpisce qualcosa PRIMA del bersaglio, niente danno
            RaycastResult los = RaycastAll(scene, origin, normToTarget, dist - 0.1f, shooter);
            if (los.hit && los.distance < dist - 0.1f) continue;

            auto& targetStats = view.get<UnitStatsComponent>(entity);
            auto& targetHealth = view.get<HealthComponent>(entity);

            float flamerDamage = wpBP.base_damage * 0.2f;
            const float healthBefore = targetHealth.currentHealth;
            DamageResolver::ResolveHit(flamerDamage, wpBP.armor_penetration,
                HitboxLocation::Torso, targetStats, targetHealth);
            EmitHitFeedback(scene, shooter, entity, targetCenter, HitboxLocation::Torso, healthBefore);
        }
    }
    glm::vec3 WeaponSystem::AimAtCrosshair(Scene& scene, entt::entity shooter, const glm::vec3& muzzle,
        const glm::vec3& baseDir, float range, float currentRecoilPitch) {
        const auto* ray = scene.GetRegistry().try_get<AimRayComponent>(shooter);
        if (!ray || !ray->valid || glm::length(ray->dir) < 1e-4f) return baseDir;
        const glm::vec3 D = glm::normalize(ray->dir);

        // Punto inquadrato: primo ostacolo o unita' lungo il raggio del mirino, cercato a partire dall'altezza
        // della bocca (cosi' non contano oggetti fra la telecamera e il personaggio).
        const float along = (std::max)(0.0f, glm::dot(muzzle - ray->origin, D));
        const glm::vec3 start = ray->origin + D * along;
        const RaycastResult hit = RaycastAll(scene, start, D, range, shooter);
        const glm::vec3 aimPoint = hit.hit ? hit.point : (start + D * range);

        glm::vec3 dir = aimPoint - muzzle;
        if (glm::length(dir) < 0.3f) return baseDir;   // bersaglio addosso alla canna: tiro dritto
        dir = glm::normalize(dir);
        // Bersaglio molto vicino e di lato: niente correzioni esagerate, si spara parallelo al mirino
        if (glm::dot(dir, D) < std::cos(glm::radians(20.0f))) dir = D;

        // Il rinculo di questo colpo non e' ancora nella vista della telecamera: lo aggiungo alzando la mira
        const float kick = glm::radians(currentRecoilPitch - ray->recoilPitch);
        if (std::abs(kick) > 1e-5f) {
            glm::vec3 right = glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f));
            if (glm::length(right) > 1e-4f) {
                right = glm::normalize(right);
                dir = glm::normalize(dir * std::cos(kick) + glm::cross(right, dir) * std::sin(kick)
                    + right * glm::dot(right, dir) * (1.0f - std::cos(kick)));
            }
        }
        return dir;
    }

    void WeaponSystem::FireHeavyBeam(Scene& scene, entt::entity shooter, entt::entity ignore, const glm::vec3& origin,
        const glm::vec3& dir, const WeaponBlueprint& bp) {
        auto& reg = scene.GetRegistry();
        const RaycastResult hit = RaycastAll(scene, origin, dir, bp.range_effective, shooter, ignore);
        const glm::vec3 end = hit.hit ? hit.point : origin + dir * bp.range_effective;
        if (hit.hitUnit && hit.entity != entt::null && reg.all_of<UnitStatsComponent, HealthComponent>(hit.entity) &&
            !DamageResolver::TryParryRanged(scene, hit.entity, dir)) {
            auto& st = reg.get<UnitStatsComponent>(hit.entity);
            auto& hp = reg.get<HealthComponent>(hit.entity);
            const float before = hp.currentHealth;
            DamageResolver::ResolveHit(bp.base_damage, bp.armor_penetration, hit.location, st, hp);
            EmitHitFeedback(scene, shooter, hit.entity, hit.point, hit.location, before);
        }
        else if (hit.hit && !hit.hitUnit && hit.entity != entt::null && reg.valid(hit.entity) && reg.all_of<StructureHealthComponent>(hit.entity)) {
            if (DamageStructure(reg, hit.entity, bp.base_damage * std::max(0.0f, bp.structure_damage_multiplier))) scene.DestroyEntity(hit.entity);
        }
        const entt::entity trail = scene.CreateEntity();
        auto& tr = reg.emplace<TrailRequestComponent>(trail);
        tr.start = origin; tr.end = end;
        tr.color = bp.primary_color;
        tr.isBeam = true;
        tr.projectileWidth = std::max(0.08f, bp.projectile_width);
        tr.travelTime = 0.001f;
        tr.totalLife = 0.45f;
        tr.fx = static_cast<int>(WeaponFx::LongLas);
        tr.hitSomething = hit.hit;
        tr.hitUnit = hit.hitUnit;
        { VfxEvent mz; mz.kind = VfxKind::Muzzle; mz.fx = WeaponFx::LongLas; mz.pos = origin; mz.dir = dir; mz.color = bp.primary_color; EmitVfx(mz); }
        { FeelEvent fe; fe.kind = FeelKind::Shot; fe.fx = WeaponFx::LongLas; fe.pos = origin; fe.dir = dir; fe.color = bp.primary_color; fe.scale = bp.base_damage; EmitFeel(fe); }
        if (hit.hit) { FeelEvent fe; fe.kind = FeelKind::Impact; fe.fx = WeaponFx::LongLas; fe.pos = end; fe.dir = dir; fe.target = static_cast<std::uint32_t>(hit.entity); fe.hitUnit = hit.hitUnit; fe.scale = bp.base_damage; EmitFeel(fe); }
    }

    WeaponSystem::RaycastResult WeaponSystem::RaycastAll(Scene& scene, const glm::vec3& rayOrigin,
        const glm::vec3& rayDir, float maxDist,
        entt::entity shooter, entt::entity ignore) {
        RaycastResult result;
        float closest = maxDist;

        auto& reg = scene.GetRegistry();

        // ---- 1. UNITA' (player + nemici), hitbox ai piedi ----
        auto unitView = reg.view<TransformComponent, UnitStatsComponent, HealthComponent>(
            entt::exclude<CorpseComponent>
        );
        for (auto e : unitView) {
            if (e == shooter) continue;
            if (SameTeam(reg, shooter, e)) continue;   // niente fuoco amico: i colpi attraversano gli alleati
            const auto& trans = unitView.get<TransformComponent>(e);
            // Evasione (es. squig 30%): il colpo lo manca e prosegue verso quello che c'e' dietro
            if (const auto* ev = reg.try_get<EvasionComponent>(e); ev && ev->chance > 0.0f) {
                const glm::vec3 toUnit = trans.Position - rayOrigin;
                const float along = glm::dot(toUnit, rayDir);
                if (along > -1.0f && along < closest + 1.0f && glm::length(toUnit - rayDir * along) < 1.5f &&
                    Random::Chance(ev->chance)) continue;
            }

            // Unita' con scheletro a cubi: il colpo si testa sui SINGOLI blocchi (testa, torso, braccia,
            // gambe) nella posa attuale. Prima si usava il collider di movimento, che per gli orchi e'
            // 0.7 x 1.3 m contro uno scheletro largo 1.85 m e alto 1.8 m: testa e braccia non erano colpibili,
            // e la zona colpita era solo stimata dall'altezza.
            if (const auto* skel = reg.try_get<SkeletonComponent>(e); skel && !skel->data.blocks.empty()) {
                // Scarto veloce: unita' troppo lontana dalla linea di tiro
                const glm::vec3 toUnit = trans.Position + glm::vec3(0.0f, 1.0f, 0.0f) - rayOrigin;
                const float along = glm::dot(toUnit, rayDir);
                if (along < -3.0f || along > closest + 3.0f) continue;
                if (glm::length(toUnit - rayDir * along) > 4.0f) continue;

                const auto blockMats = skel->data.ComputeWorldTransforms(trans.Position, trans.Rotation, skel->scale);
                for (size_t b = 0; b < blockMats.size() && b < skel->data.blocks.size(); ++b) {
                    float t;
                    if (IntersectSkeletonBlock(blockMats[b], rayOrigin, rayDir, closest, t) && t < closest) {
                        closest = t;
                        result.hit = true;
                        result.hitUnit = true;
                        result.entity = e;
                        result.distance = t;
                        result.point = rayOrigin + rayDir * t;
                        result.location = LocationFromBlockName(skel->data.blocks[b].name);
                    }
                }
                continue;
            }

            // FIX #1: centro = Position + localCenterOffset. Prima il box andava da Position a
            // Position+2*half su TUTTI gli assi: in X/Z era spostato di +half (colpi a sinistra/dietro
            // mancavano, a destra/davanti "colpivano" aria). Fallback (nessun collider): box 1.8m ai piedi.
            glm::vec3 half = glm::vec3(0.5f, 0.9f, 0.5f);
            glm::vec3 center = trans.Position + glm::vec3(0.0f, half.y, 0.0f);
            if (auto* col = reg.try_get<ColliderComponent>(e)) {
                half = col->halfExtents * trans.Scale;
                center = GetColliderWorldCenter(trans, *col);
            }
            glm::vec3 minB = center - half;
            glm::vec3 maxB = center + half;

            float t;
            if (IntersectAABB(rayOrigin, rayDir, minB, maxB, closest, t) && t < closest) {
                closest = t;
                result.hit = true;
                result.hitUnit = true;
                result.entity = e;
                result.distance = t;
                result.point = rayOrigin + rayDir * t;

                // FIX #20: altezza normalizzata sul box dell'unita' (0 = piedi, 1 = cima) invece di
                // soglie fisse per 1.8m: Nob (2m+) e ratling (1.2m) ora hanno testa/torso proporzionati.
                float spanY = (std::max)(maxB.y - minB.y, 0.001f);
                float hitYn = std::clamp((result.point.y - minB.y) / spanY, 0.0f, 1.0f);
                if (hitYn > 0.82f)      result.location = HitboxLocation::Head;
                else if (hitYn > 0.45f) result.location = HitboxLocation::Torso;
                else {
                    // Gambe: sinistra/destra in base al lato rispetto all'asse destro dell'unita'
                    float yawRad = glm::radians(trans.AimYaw);
                    glm::vec3 right(std::cos(yawRad), 0.0f, -std::sin(yawRad));
                    float side = glm::dot(result.point - center, right);
                    result.location = (side < 0.0f) ? HitboxLocation::LeftLeg : HitboxLocation::RightLeg;
                }
            }
        }

        // ---- 2. MURI / STRUTTURE / PROPS (collider centrato su Position) ----
        auto staticView = reg.view<TransformComponent, ColliderComponent>(
            entt::exclude<PlayerTag, HealthComponent, CorpseComponent>   // <-- AGGIUNTO
        );
        for (auto e : staticView) {
            if (e == shooter || e == ignore) continue;
            const auto& trans = staticView.get<TransformComponent>(e);
            const auto& col = staticView.get<ColliderComponent>(e);

            // FIX #1: centro = Position + localCenterOffset
            glm::vec3 half = col.halfExtents * trans.Scale;
            glm::vec3 center = GetColliderWorldCenter(trans, col);
            glm::vec3 minB = center - half;
            glm::vec3 maxB = center + half;

            float t;
            if (IntersectAABB(rayOrigin, rayDir, minB, maxB, closest, t) && t < closest) {
                closest = t;
                result.hit = true;
                result.hitUnit = false;
                result.entity = e;
                result.distance = t;
                result.point = rayOrigin + rayDir * t;
            }
        }

        return result;
    }

    // Wrapper retro-compatibile (se qualcosa lo chiama ancora)
    bool WeaponSystem::RaycastHitbox(Scene& scene, const glm::vec3& rayOrigin, const glm::vec3& rayDir,
        float maxDist, entt::entity shooter, entt::entity& outHitEntity,
        HitboxLocation& outLocation, float& outHitDist) {
        auto r = RaycastAll(scene, rayOrigin, rayDir, maxDist, shooter);
        if (!r.hit || !r.hitUnit) return false;
        outHitEntity = r.entity;
        outLocation = r.location;
        outHitDist = r.distance;
        return true;
    }
}