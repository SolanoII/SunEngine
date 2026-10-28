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
#include "../Core/Random.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>

namespace SunEngine {
    
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

        // Falloff lineare da 1.0 a 0.0 in 40 metri (più secco delle voci Ork)
        float dist = glm::distance(sourcePos, listenerPos);
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
        auto view = scene.GetRegistry().view<TransformComponent, UnitStatsComponent, WeaponStateComponent, InventoryComponent, CameraRecoilComponent>();

        for (auto entity : view) {
            auto& trans = view.get<TransformComponent>(entity);
            auto& stats = view.get<UnitStatsComponent>(entity);
            auto& wState = view.get<WeaponStateComponent>(entity);
            auto& inventory = view.get<InventoryComponent>(entity);
            auto& recoil = view.get<CameraRecoilComponent>(entity);

            std::string activeWeaponId = inventory.GetActiveWeaponId();
            const WeaponBlueprint* wpBP = BlueprintManager::Get().GetWeapon(activeWeaponId);

            if (!wpBP || wpBP->is_melee) {
                // Reset stato trigger: evita di trascinare un fronte di salita
                // stantio quando si passa arma -> melee -> arma.
                wState.wasFiringLastFrame = false;
                wState.burstRemaining = 0;
                continue;
            }

            bool wantsToFire = Input::IsMouseButtonPressed(0);

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
                wState.reloadTimer -= deltaTime;
                if (wState.reloadTimer <= 0.0f) {
                    wState.isReloading = false;
                    wState.reloadTimer = 0.0f;
                    wState.currentAmmo = wpBP->magazine_size;
                    SUN_CORE_INFO("Arma ricaricata! Munizioni: {0}", wState.currentAmmo);
                }
                wState.wasFiringLastFrame = wantsToFire;
                continue;
            }

            if (Input::IsKeyPressed(GLFW_KEY_R) && wState.currentAmmo < wpBP->magazine_size && !wState.isReloading) {
                wState.isReloading = true;
                wState.reloadTimer = wpBP->reload_time_sec;
                SUN_CORE_INFO("Ricarica avviata...");
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
                if (wState.currentAmmo > 0) {
                    FireWeapon(scene, entity, wState, recoil, *wpBP, trans, stats);
                    if (effectiveMode == "burst" && wState.burstRemaining > 0) {
                        wState.burstRemaining--;
                    }
                }
                else if (!wState.isReloading) {
                    wState.isReloading = true;
                    wState.reloadTimer = wpBP->reload_time_sec;
                    SUN_CORE_WARN("Caricatore vuoto! Ricarica automatica...");
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
        float maxRange = (wpBP.range_effective > 1.0f) ? wpBP.range_effective : 12.0f;
        RaycastResult hit = RaycastAll(scene, barrelOrigin, baseDir, maxRange, shooter);

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

            DamageResolver::ResolveHit(frameDamage, effectiveAP, hit.location, targetStats, targetHealth);
        }

        entt::entity meltaBeamEntity = scene.CreateEntity();
        auto& beamTrail = scene.GetRegistry().emplace<TrailRequestComponent>(meltaBeamEntity);
        beamTrail.start = barrelOrigin;
        beamTrail.end = hitPoint;
        beamTrail.color = (wpBP.primary_color.a > 0.01f) ? wpBP.primary_color : glm::vec4(1.0f, 0.40f, 0.05f, 0.95f);
        beamTrail.isBeam = true;
        beamTrail.projectileWidth = (wpBP.projectile_width > 0.05f) ? wpBP.projectile_width : 0.38f;
        beamTrail.travelTime = 0.01f;
        beamTrail.totalLife = deltaTime * 1.8f;
    }

    void WeaponSystem::FireWeapon(Scene& scene, entt::entity shooter,
        WeaponStateComponent& wState, CameraRecoilComponent& recoil,
        const WeaponBlueprint& wpBP, TransformComponent& trans,
        UnitStatsComponent& stats) {
        wState.currentAmmo--;
        wState.fireCooldown = (wpBP.rate_of_fire > 0.0f) ? (60.0f / wpBP.rate_of_fire) : 0.15f;

        wState.currentHeat += 12.0f;
        if (wState.currentHeat >= wpBP.heat_capacity) {
            wState.currentHeat = wpBP.heat_capacity;
            wState.isOverheating = true;
            SUN_CORE_WARN("ARMA SURRISCALDATA!");
        }

        recoil.recoilPitch += wpBP.recoil_force;
        recoil.recoilYaw += (Random::Float01() - 0.5f) * wpBP.recoil_force * 0.5f;
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

        if (wpBP.projectile_type == ProjectileType::Flamer) {
            FireFlamer(scene, shooter, wpBP, barrelOrigin, baseDir);

            entt::entity flamerTrailEntity = scene.CreateEntity();
            auto& flamerTrail = scene.GetRegistry().emplace<TrailRequestComponent>(flamerTrailEntity);
            flamerTrail.start = barrelOrigin;
            flamerTrail.end = barrelOrigin + (baseDir * wpBP.range_effective);
            flamerTrail.color = glm::vec4(1.0f, 0.4f, 0.0f, 0.8f);
            flamerTrail.isBeam = true;
            flamerTrail.projectileWidth = 0.8f;
            flamerTrail.travelTime = 0.1f;
            flamerTrail.totalLife = 0.3f;
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
        float totalSpread = spreadBase + wState.currentSpread;

        glm::vec3 rightDir = glm::normalize(glm::cross(baseDir, glm::vec3(0.0f, 1.0f, 0.0f)));
        if (glm::length(rightDir) < 0.001f) {
            rightDir = glm::normalize(glm::cross(baseDir, glm::vec3(1.0f, 0.0f, 0.0f)));
        }
        glm::vec3 upDir = glm::normalize(glm::cross(rightDir, baseDir));

        for (int p = 0; p < numProjectiles; ++p) {
            glm::vec3 shootDir = baseDir;

            if (isShotgun || totalSpread > 0.001f) {
                float r = Random::Float01() * totalSpread;
                float theta = Random::Range(0.0f, 6.28318530718f); 
                glm::vec3 pelletOffset = (rightDir * std::cos(theta) + upDir * std::sin(theta)) * r;
                shootDir = glm::normalize(baseDir + pelletOffset);
            }
            RaycastResult hit = RaycastAll(scene, barrelOrigin, shootDir, wpBP.range_effective, shooter);

            glm::vec3 hitPoint = hit.hit
                ? hit.point
                : (barrelOrigin + shootDir * wpBP.range_effective);
            bool skipTrail = false;
            if (hit.hitUnit && hit.entity != entt::null &&
                scene.GetRegistry().all_of<UnitStatsComponent, HealthComponent>(hit.entity)) {

                auto& targetStats = scene.GetRegistry().get<UnitStatsComponent>(hit.entity);
                auto& targetHealth = scene.GetRegistry().get<HealthComponent>(hit.entity);

                float finalDamage = isUnderbarrelShotgun ? 15.0f : wpBP.base_damage;
                int effectiveAP = isUnderbarrelShotgun ? 5 : wpBP.armor_penetration;

                finalDamage = CalculateDamageWithFalloff(finalDamage, hit.distance,
                    wpBP.damage_falloff_start,
                    wpBP.damage_falloff_end);

                if (wpBP.projectile_type == ProjectileType::Grav) {
                    ArmorPiece* armor = targetStats.armorSet.GetPiece(hit.location);
                    int targetArmorRating = armor ? armor->armorRating
                        : targetStats.armorSet.GetAverageRating();
                    finalDamage = wpBP.base_damage + (targetArmorRating * 1.5f);
                }
                else if (wpBP.projectile_type == ProjectileType::Psychic) {
                    targetHealth.currentHealth -= finalDamage;
                    skipTrail = true;   // era un "continue" prima
                }
                else {
                    DamageResolver::ResolveHit(finalDamage, effectiveAP, hit.location,
                        targetStats, targetHealth);
                }
            }
            if (skipTrail) continue;
            entt::entity bulletTrailEntity = scene.CreateEntity();
            auto& bulletTrail = scene.GetRegistry().emplace<TrailRequestComponent>(bulletTrailEntity);
            bulletTrail.start = barrelOrigin;
            bulletTrail.end = hitPoint;
            bulletTrail.color = isUnderbarrelShotgun ? glm::vec4(1.0f, 0.7f, 0.2f, 1.0f) : wpBP.primary_color;
            bulletTrail.isBeam = (wpBP.projectile_type == ProjectileType::Laser);
            bulletTrail.projectileWidth = isShotgun ? 0.035f : wpBP.projectile_width;
            bulletTrail.travelTime = (wpBP.projectile_type == ProjectileType::Laser) ? 0.05f : 0.12f;
            bulletTrail.totalLife = 0.20f;
        }
        // Suono positional: sorgente = bocca di fuoco, listener = posizione del player
// (il player è sempre l'ascoltatore; per ora solo il player spara, quindi dist=0)
        glm::vec3 listenerPos = trans.Position + glm::vec3(0.0f, 1.2f, 0.0f);

        // Distingui per proiettile: melta/flamer/laser hanno timbri diversi, ma per
        // ora usiamo lo stesso file — la struttura è pronta per essere estesa.
        float baseVol = 1.0f;
        if (isShotgun) baseVol = 1.15f;   // shotgun un po' più "cattivo"
        if (isUnderbarrelShotgun) baseVol = 0.9f;

        PlayWeaponSound(barrelOrigin, listenerPos, baseVol);
    }

    void WeaponSystem::FireFlamer(Scene& scene, entt::entity shooter, const WeaponBlueprint& wpBP,
        const glm::vec3& origin, const glm::vec3& dir) {
        auto view = scene.GetRegistry().view<TransformComponent, UnitStatsComponent, HealthComponent>(
            entt::exclude<CorpseComponent>
        );

        for (auto entity : view) {
            if (entity == shooter) continue;

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
            DamageResolver::ResolveHit(flamerDamage, wpBP.armor_penetration,
                HitboxLocation::Torso, targetStats, targetHealth);
        }
    }
    WeaponSystem::RaycastResult WeaponSystem::RaycastAll(Scene& scene, const glm::vec3& rayOrigin,
        const glm::vec3& rayDir, float maxDist,
        entt::entity shooter) {
        RaycastResult result;
        float closest = maxDist;

        auto& reg = scene.GetRegistry();

        // ---- 1. UNITA' (player + nemici), hitbox ai piedi ----
        auto unitView = reg.view<TransformComponent, UnitStatsComponent, HealthComponent>(
            entt::exclude<CorpseComponent>
        );
        for (auto e : unitView) {
            if (e == shooter) continue;
            const auto& trans = unitView.get<TransformComponent>(e);

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
            if (e == shooter) continue;
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