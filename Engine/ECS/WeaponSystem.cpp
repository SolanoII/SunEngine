#include "WeaponSystem.h"
#include "CombatSystem.h"

namespace SunEngine {

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

    void WeaponSystem::Update(Scene& scene, PhysicsSystem& physicsSystem, ma_engine* audioEngine, float deltaTime) {
        auto view = scene.GetRegistry().view<TransformComponent, UnitStatsComponent, WeaponStateComponent, InventoryComponent, CameraRecoilComponent>();

        for (auto entity : view) {
            auto& trans = view.get<TransformComponent>(entity);
            auto& stats = view.get<UnitStatsComponent>(entity);
            auto& wState = view.get<WeaponStateComponent>(entity);
            auto& inventory = view.get<InventoryComponent>(entity);
            auto& recoil = view.get<CameraRecoilComponent>(entity);

            std::string activeWeaponId = inventory.GetActiveWeaponId();
            const WeaponBlueprint* wpBP = BlueprintManager::Get().GetWeapon(activeWeaponId);

            if (!wpBP || wpBP->is_melee) continue;

            bool wantsToFire = Input::IsMouseButtonPressed(0);

            // GESTIONE ARMI A RAGGIO CONTINUO (MELTA GUN)
            if (wpBP->projectile_type == ProjectileType::ContinuousBeam) {
                if (wantsToFire && !wState.isOverheating && !wState.isReloading) {
                    FireContinuousBeam(scene, entity, wState, recoil, *wpBP, trans, stats, audioEngine, deltaTime);
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
                continue;
            }

            if (Input::IsKeyPressed(GLFW_KEY_R) && wState.currentAmmo < wpBP->magazine_size && !wState.isReloading) {
                wState.isReloading = true;
                wState.reloadTimer = wpBP->reload_time_sec;
                SUN_CORE_INFO("Ricarica avviata...");
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

            if (wantsToFire && wState.fireCooldown <= 0.0f && !wState.isOverheating) {
                if (wState.currentAmmo > 0) {
                    FireWeapon(scene, entity, wState, recoil, *wpBP, trans, stats, audioEngine);
                }
                else if (!wState.isReloading) {
                    wState.isReloading = true;
                    wState.reloadTimer = wpBP->reload_time_sec;
                    SUN_CORE_WARN("Caricatore vuoto! Ricarica automatica...");
                }
            }
        }
    }

    void WeaponSystem::FireContinuousBeam(Scene& scene, entt::entity shooter, WeaponStateComponent& wState, CameraRecoilComponent& recoil, const WeaponBlueprint& wpBP, TransformComponent& trans, UnitStatsComponent& stats, ma_engine* audioEngine, float deltaTime) {
        float heatRate = 30.0f;
        wState.currentHeat += heatRate * deltaTime;
        if (wState.currentHeat >= wpBP.heat_capacity) {
            wState.currentHeat = wpBP.heat_capacity;
            wState.isOverheating = true;
            SUN_CORE_WARN("MELTA GUN SURRISCALDATA!");
            return;
        }

        if ((rand() % 4 == 0) && wState.currentAmmo > 0) {
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
        glm::vec3 hitPoint = barrelOrigin + (baseDir * maxRange);
        entt::entity hitEntity = entt::null;
        HitboxLocation hitLoc = HitboxLocation::Torso;
        float hitDist = maxRange;

        if (RaycastHitbox(scene, barrelOrigin, baseDir, maxRange, shooter, hitEntity, hitLoc, hitDist)) {
            hitPoint = barrelOrigin + (baseDir * hitDist);

            if (hitEntity != entt::null && scene.GetRegistry().all_of<UnitStatsComponent, HealthComponent>(hitEntity)) {
                auto& targetStats = scene.GetRegistry().get<UnitStatsComponent>(hitEntity);
                auto& targetHealth = scene.GetRegistry().get<HealthComponent>(hitEntity);

                float dps = (wpBP.base_damage > 1.0f) ? wpBP.base_damage : 120.0f;
                float frameDamage = dps * deltaTime;
                int effectiveAP = wpBP.armor_penetration;

                float meltaThreshold = (wpBP.melta_range > 0.5f) ? wpBP.melta_range : 4.5f;
                if (hitDist <= meltaThreshold) {
                    float meltaMult = (wpBP.melta_multiplier > 1.0f) ? wpBP.melta_multiplier : 2.5f;
                    frameDamage *= meltaMult;
                    effectiveAP += 20;
                }

                CombatSystem::ResolveHit(frameDamage, effectiveAP, hitLoc, targetStats, targetHealth);
            }
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

    void WeaponSystem::FireWeapon(Scene& scene, entt::entity shooter, WeaponStateComponent& wState, CameraRecoilComponent& recoil, const WeaponBlueprint& wpBP, TransformComponent& trans, UnitStatsComponent& stats, ma_engine* audioEngine) {
        wState.currentAmmo--;
        wState.fireCooldown = (wpBP.rate_of_fire > 0.0f) ? (60.0f / wpBP.rate_of_fire) : 0.15f;

        wState.currentHeat += 12.0f;
        if (wState.currentHeat >= wpBP.heat_capacity) {
            wState.currentHeat = wpBP.heat_capacity;
            wState.isOverheating = true;
            SUN_CORE_WARN("ARMA SURRISCALDATA!");
        }

        recoil.recoilPitch += wpBP.recoil_force;
        recoil.recoilYaw += ((static_cast<float>(rand()) / RAND_MAX) - 0.5f) * wpBP.recoil_force * 0.5f;

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
                float r = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * totalSpread;
                float theta = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * 6.28318530718f;
                glm::vec3 pelletOffset = (rightDir * std::cos(theta) + upDir * std::sin(theta)) * r;
                shootDir = glm::normalize(baseDir + pelletOffset);
            }

            glm::vec3 hitPoint = barrelOrigin + (shootDir * wpBP.range_effective);
            entt::entity hitEntity = entt::null;
            HitboxLocation hitLoc = HitboxLocation::Torso;
            float hitDist = wpBP.range_effective;

            if (RaycastHitbox(scene, barrelOrigin, shootDir, wpBP.range_effective, shooter, hitEntity, hitLoc, hitDist)) {
                hitPoint = barrelOrigin + (shootDir * hitDist);

                if (hitEntity != entt::null && scene.GetRegistry().all_of<UnitStatsComponent, HealthComponent>(hitEntity)) {
                    auto& targetStats = scene.GetRegistry().get<UnitStatsComponent>(hitEntity);
                    auto& targetHealth = scene.GetRegistry().get<HealthComponent>(hitEntity);

                    float finalDamage = isUnderbarrelShotgun ? 15.0f : wpBP.base_damage;
                    int effectiveAP = isUnderbarrelShotgun ? 5 : wpBP.armor_penetration;

                    finalDamage = CalculateDamageWithFalloff(finalDamage, hitDist, wpBP.damage_falloff_start, wpBP.damage_falloff_end);

                    if (wpBP.projectile_type == ProjectileType::Grav) {
                        ArmorPiece* armor = targetStats.armorSet.GetPiece(hitLoc);
                        int targetArmorRating = armor ? armor->armorRating : targetStats.armorSet.GetAverageRating();
                        finalDamage = wpBP.base_damage + (targetArmorRating * 1.5f);
                    }
                    else if (wpBP.projectile_type == ProjectileType::Psychic) {
                        targetHealth.currentHealth -= finalDamage;
                        continue;
                    }

                    CombatSystem::ResolveHit(finalDamage, effectiveAP, hitLoc, targetStats, targetHealth);
                }
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
        }

        if (audioEngine) {
            ma_engine_play_sound(audioEngine, "GameData/Audio/laser.wav", NULL);
        }
    }

    void WeaponSystem::FireFlamer(Scene& scene, entt::entity shooter, const WeaponBlueprint& wpBP, const glm::vec3& origin, const glm::vec3& dir) {
        auto view = scene.GetRegistry().view<TransformComponent, UnitStatsComponent, HealthComponent>();

        for (auto entity : view) {
            if (entity == shooter) continue;

            const auto& targetTrans = view.get<TransformComponent>(entity);
            glm::vec3 toTarget = targetTrans.Position - origin;
            float dist = glm::length(toTarget);

            if (dist <= wpBP.range_effective) {
                glm::vec3 normToTarget = glm::normalize(toTarget);
                float dot = glm::dot(dir, normToTarget);
                float angleDegrees = glm::degrees(std::acos(glm::clamp(dot, -1.0f, 1.0f)));

                if (angleDegrees <= (wpBP.flamer_cone_angle * 0.5f)) {
                    auto& targetStats = view.get<UnitStatsComponent>(entity);
                    auto& targetHealth = view.get<HealthComponent>(entity);

                    float flamerDamage = wpBP.base_damage * 0.2f;
                    CombatSystem::ResolveHit(flamerDamage, wpBP.armor_penetration, HitboxLocation::Torso, targetStats, targetHealth);
                }
            }
        }
    }

    bool WeaponSystem::RaycastHitbox(Scene& scene, const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDist, entt::entity shooter, entt::entity& outHitEntity, HitboxLocation& outLocation, float& outHitDist) {
        float minT = maxDist;
        bool hasHit = false;

        auto view = scene.GetRegistry().view<TransformComponent, UnitStatsComponent, HealthComponent>();

        for (auto entity : view) {
            if (entity == shooter) continue;

            const auto& targetTrans = view.get<TransformComponent>(entity);

            glm::vec3 minBox = targetTrans.Position - glm::vec3(0.5f, 0.0f, 0.5f);
            glm::vec3 maxBox = targetTrans.Position + glm::vec3(0.5f, 1.8f, 0.5f);

            float tMin = 0.0f, tMax = maxDist;
            bool hit = true;

            for (int i = 0; i < 3; ++i) {
                if (std::abs(rayDir[i]) < 0.0001f) {
                    if (rayOrigin[i] < minBox[i] || rayOrigin[i] > maxBox[i]) { hit = false; break; }
                }
                else {
                    float invD = 1.0f / rayDir[i];
                    float t0 = (minBox[i] - rayOrigin[i]) * invD;
                    float t1 = (maxBox[i] - rayOrigin[i]) * invD;
                    if (invD < 0.0f) std::swap(t0, t1);
                    tMin = (std::max)(tMin, t0);
                    tMax = (std::min)(tMax, t1);
                    if (tMax < tMin) { hit = false; break; }
                }
            }

            if (hit && tMin < minT && tMin > 0.0f) {
                minT = tMin;
                outHitEntity = entity;
                hasHit = true;

                float hitY = rayOrigin.y + (rayDir.y * tMin);
                float relativeY = hitY - targetTrans.Position.y;

                if (relativeY >= 1.4f) outLocation = HitboxLocation::Head;
                else if (relativeY >= 0.7f) outLocation = HitboxLocation::Torso;
                else outLocation = HitboxLocation::RightLeg;
            }
        }

        if (hasHit) {
            outHitDist = minT;
            return true;
        }

        return false;
    }

}