#include "CombatSystem.h"

namespace SunEngine {

    static float LerpAngle(float a, float b, float t) {
        float diff = std::fmod(b - a + 180.0f, 360.0f) - 180.0f;
        if (diff < -180.0f) diff += 360.0f;
        return a + diff * std::clamp(t, 0.0f, 1.0f);
    }

    static void SpawnDebugSlashTrail(Scene& scene, entt::entity attacker, const TransformComponent& trans, const MeleeStateComponent& melee, const MeleeNode* node) {
        glm::vec4 slashColor = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);

        std::string nodeKey = melee.currentAttackNode;
        std::transform(nodeKey.begin(), nodeKey.end(), nodeKey.begin(), ::tolower);

        glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::vec3 fwd = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        glm::vec3 right = glm::normalize(glm::vec3(rot * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)));
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);

        glm::vec3 startPos(0.0f);
        glm::vec3 endPos(0.0f);
        float width = 0.20f;

        if (nodeKey.find("heavy") != std::string::npos || melee.currentAttack == MeleeAttackType::Heavy || (node && node->damageMultiplier >= 2.0f)) {
            slashColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            startPos = trans.Position + up * 2.2f + fwd * 0.5f + (nodeKey == "heavy_2" ? -right : right) * 0.4f;
            endPos = trans.Position + up * 0.2f + fwd * 1.6f + (nodeKey == "heavy_2" ? right : -right) * 0.3f;
            width = 0.30f;
        }
        else if (nodeKey.find("low") != std::string::npos || nodeKey.find("basso") != std::string::npos || melee.currentAttack == MeleeAttackType::Light) {
            slashColor = glm::vec4(0.0f, 0.4f, 1.0f, 1.0f);
            startPos = trans.Position + up * 0.5f + fwd * 0.4f;
            endPos = trans.Position + up * 0.45f + fwd * 1.7f + right * 0.6f;
            width = 0.42f;
        }
        else {
            slashColor = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
            if (nodeKey == "normal_1") {
                startPos = trans.Position + up * 1.25f + fwd * 0.8f + right * 1.1f;
                endPos = trans.Position + up * 1.15f + fwd * 1.5f - right * 1.1f;
            }
            else if (nodeKey == "normal_2") {
                startPos = trans.Position + up * 1.15f + fwd * 0.8f - right * 1.1f;
                endPos = trans.Position + up * 1.25f + fwd * 1.5f + right * 1.1f;
            }
            else {
                startPos = trans.Position + up * 1.3f + fwd * 0.7f + right * 0.8f;
                endPos = trans.Position + up * 0.9f + fwd * 1.4f - right * 0.8f;
            }
        }

        entt::entity slashEntity = scene.CreateEntity();
        auto& trail = scene.GetRegistry().emplace<TrailRequestComponent>(slashEntity);
        trail.start = startPos;
        trail.end = endPos;
        trail.color = slashColor;
        trail.isBeam = true;
        trail.projectileWidth = width;
        trail.travelTime = 0.05f;
        trail.totalLife = 0.38f;
    }
    void CombatSystem::Update(Scene& scene, PhysicsSystem& physicsSystem, NavMeshQuery& navQuery, float deltaTime) {
        entt::entity playerEnt = entt::null;
        glm::vec3 playerPos(0.0f);
        glm::vec3 playerVelocity(0.0f);

        auto playerView = scene.GetRegistry().view<PlayerTag, TransformComponent>();
        for (auto p : playerView) {
            playerEnt = p;
            playerPos = playerView.get<TransformComponent>(p).Position;

            // Recupero velocità del giocatore per il calcolo predittivo
            if (scene.GetRegistry().all_of<RigidBodyComponent>(p)) {
                playerVelocity = scene.GetRegistry().get<RigidBodyComponent>(p).velocity;
            }
            break;
        }

        if (playerEnt != entt::null) {
            bool activeSlots[4] = { false, false, false, false };
            int activeAttackersCount = 0;

            auto hordeView = scene.GetRegistry().view<HordeAgentComponent, TransformComponent, HealthComponent>(
                entt::exclude<PlayerTag, CorpseComponent>
            );

            for (auto ork : hordeView) {
                auto& horde = hordeView.get<HordeAgentComponent>(ork);
                if (horde.state == HordeState::Approaching || horde.state == HordeState::Attacking) {
                    if (horde.assignedSlotIndex >= 0 && horde.assignedSlotIndex < 4) {
                        activeSlots[horde.assignedSlotIndex] = true;
                    }
                    activeAttackersCount++;
                }
            }

            int totalOrksInHorde = 0;
            for (auto ork : hordeView) totalOrksInHorde++;

            int orkIndex = 0;
            for (auto ork : hordeView) {
                auto& horde = hordeView.get<HordeAgentComponent>(ork);
                auto& trans = hordeView.get<TransformComponent>(ork);
                auto& melee = scene.GetRegistry().get_or_emplace<MeleeStateComponent>(ork);

                float distToPlayer = glm::distance(trans.Position, playerPos);

                // 1. MIRA PREDITTIVA (Studio del movimento del giocatore)
                if (distToPlayer <= 8.0f) {
                    float leadTime = 0.28f; // Tempo di anticipo sul movimento
                    glm::vec3 predictedPlayerPos = playerPos + (playerVelocity * leadTime);
                    glm::vec3 dirToP = predictedPlayerPos - trans.Position;
                    dirToP.y = 0.0f;

                    if (glm::length(dirToP) > 0.001f) {
                        float targetYaw = glm::degrees(std::atan2(-dirToP.x, -dirToP.z));
                        trans.AimYaw = targetYaw;
                        trans.Rotation.y = LerpAngle(trans.Rotation.y, targetYaw, 14.0f * deltaTime);
                    }
                }

                // --- CALCOLO TARGET E VELOCITA' IN BASE ALLO STATO ---
                glm::vec3 targetPos = trans.Position;
                float moveSpeed = 0.0f;
                bool needsToMove = false;

                if (horde.state == HordeState::Cooldown) {
                    horde.cooldownTimer -= deltaTime;

                    if (distToPlayer < 4.5f) {
                        glm::vec3 backDir = glm::normalize(trans.Position - playerPos);
                        backDir.y = 0.0f;
                        // Usa GetNearestValidPoint per non indietreggiare fuori dalla mappa calpestabile
                        targetPos = navQuery.GetNearestValidPoint(trans.Position + backDir * 3.5f);
                        moveSpeed = 2.8f;
                        needsToMove = true;
                    }

                    if (horde.cooldownTimer <= 0.0f) {
                        horde.state = HordeState::Waiting;
                    }
                }
                else if (horde.state == HordeState::Waiting) {
                    float angle = (360.0f / std::max(1, totalOrksInHorde)) * orkIndex;
                    horde.circleAngle = angle;

                    targetPos = playerPos + glm::vec3(
                        std::cos(glm::radians(angle)) * 5.5f,
                        0.0f,
                        std::sin(glm::radians(angle)) * 5.5f
                    );

                    if (glm::distance(trans.Position, targetPos) > 0.4f) {
                        moveSpeed = 3.5f;
                        needsToMove = true;
                    }

                    // Logica assegnazione slot (invariata)
                    if (activeAttackersCount < 4) {
                        for (int s = 0; s < 4; ++s) {
                            if (!activeSlots[s]) {
                                activeSlots[s] = true;
                                horde.assignedSlotIndex = s;
                                horde.state = HordeState::Approaching;
                                horde.attackDelay = 0.4f + (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * 0.8f;
                                horde.attacksToPerform = (rand() % 2 == 0) ? 1 : 2;
                                horde.currentAttackCount = 0;
                                activeAttackersCount++;
                                break;
                            }
                        }
                    }
                }
                else if (horde.state == HordeState::Approaching) {
                    float slotAngles[4] = { 0.0f, 90.0f, 180.0f, 270.0f };
                    float slotAngle = slotAngles[horde.assignedSlotIndex];

                    targetPos = playerPos + glm::vec3(
                        std::cos(glm::radians(slotAngle)) * 1.45f,
                        0.0f,
                        std::sin(glm::radians(slotAngle)) * 1.45f
                    );

                    moveSpeed = 4.2f;
                    needsToMove = true;

                    if (distToPlayer <= 1.65f) {
                        horde.state = HordeState::Attacking;
                        horde.attackTimer = 0.0f;
                    }
                }

                // --- ESECUZIONE NAVMESH PATHFINDING E MOVIMENTO ---
                if (needsToMove) {
                    horde.pathUpdateTimer -= deltaTime;

                    // Ricalcola percorso ogni 0.5s o se il percorso è finito (stagger randomico per spalmare carico CPU)
                    if (horde.pathUpdateTimer <= 0.0f || horde.currentPath.empty()) {
                        horde.currentPath = navQuery.FindPath(trans.Position, targetPos);
                        horde.pathUpdateTimer = 0.4f + (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * 0.2f;
                    }

                    // Segue i waypoint
                    if (!horde.currentPath.empty()) {
                        glm::vec3 nextWaypoint = horde.currentPath.front();
                        glm::vec3 dirToWaypoint = nextWaypoint - trans.Position;
                        dirToWaypoint.y = 0.0f; // Teniamo Y piatta per il MovementSystem

                        float distToWay = glm::length(dirToWaypoint);

                        // Se siamo vicinissimi al waypoint attuale, passiamo al successivo
                        if (distToWay < 0.25f) {
                            horde.currentPath.erase(horde.currentPath.begin());
                        }
                        else {
                            trans.Position += glm::normalize(dirToWaypoint) * moveSpeed * deltaTime;
                        }
                    }
                }
                else {
                    horde.currentPath.clear(); // Arresta e pulisce memoria se non serve muoversi
                }

                if (horde.state == HordeState::Attacking) {
                    horde.attackTimer += deltaTime;

                    if (horde.attackTimer >= horde.attackDelay && melee.currentPhase == MeleePhase::None && horde.currentAttackCount == 0) {
                        melee.currentPhase = MeleePhase::Windup;
                        melee.currentAttack = MeleeAttackType::Normal;
                        melee.currentAttackNode = "normal_1";
                        melee.stateTimer = 0.0f;
                        melee.hasHitInThisAttack = false;
                        horde.currentAttackCount = 1;
                    }

                    // 2. SLANCIO IN AVANTI (Lunge durante il wind-up del colpo)
                    if (melee.currentPhase == MeleePhase::Windup) {
                        glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
                        glm::vec3 fwd = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
                        trans.Position += fwd * 3.6f * deltaTime; // Impulso di chiusura range
                    }

                    if (horde.attacksToPerform == 2 && horde.currentAttackCount == 1) {
                        if (melee.currentAttackNode == "normal_1" && melee.currentPhase == MeleePhase::Active) {
                            melee.bufferedInput = "F_CLICK";
                            horde.currentAttackCount = 2;
                        }
                    }

                    // 3. COOLDOWN DINAMICO BASATO SUL NUMERO DI ORCHI NELL'ORDA
                    if (horde.currentAttackCount >= horde.attacksToPerform && melee.currentPhase == MeleePhase::None) {
                        horde.state = HordeState::Cooldown;

                        float baseCooldown = 2.5f + (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * 1.5f;
                        // Ogni orco oltre il primo riduce il cooldown dell'8% (fino a un max del 60% di riduzione)
                        float hordeReductionFactor = std::clamp(1.0f - ((totalOrksInHorde - 1) * 0.08f), 0.40f, 1.0f);
                        horde.cooldownTimer = std::max(0.7f, baseCooldown * hordeReductionFactor);

                        if (horde.assignedSlotIndex >= 0 && horde.assignedSlotIndex < 4) {
                            activeSlots[horde.assignedSlotIndex] = false;
                        }
                        horde.assignedSlotIndex = -1;
                    }
                }

                orkIndex++;
            }
        }

        auto view = scene.GetRegistry().view<UnitStatsComponent, MeleeStateComponent, TransformComponent>();

        for (auto entity : view) {
            auto& stats = view.get<UnitStatsComponent>(entity);
            auto& melee = view.get<MeleeStateComponent>(entity);
            auto& trans = view.get<TransformComponent>(entity);

            std::string activeMeleeId = "";
            if (scene.GetRegistry().all_of<WeaponBlueprintId>(entity)) {
                activeMeleeId = scene.GetRegistry().get<WeaponBlueprintId>(entity).id;
            }

            const auto* weaponBP = BlueprintManager::Get().GetWeapon(activeMeleeId);
            if (!weaponBP || !weaponBP->hasMoveset) {
                if (scene.GetRegistry().all_of<InventoryComponent>(entity)) {
                    const auto& inv = scene.GetRegistry().get<InventoryComponent>(entity);
                    if (!inv.meleeWeaponId.empty()) {
                        weaponBP = BlueprintManager::Get().GetWeapon(inv.meleeWeaponId);
                    }
                }
            }
            if (!weaponBP || !weaponBP->hasMoveset) {
                weaponBP = BlueprintManager::Get().GetWeapon("ork_choppa_1h");
            }

            if (melee.inputBufferTimer > 0.0f) {
                melee.inputBufferTimer -= deltaTime;
                if (melee.inputBufferTimer <= 0.0f) {
                    melee.bufferedInput = "";
                }
            }

            if (melee.currentPhase == MeleePhase::None) {
                stats.stamina = std::min(stats.maxStamina, stats.stamina + stats.staminaRegen * deltaTime);
            }

            if (melee.currentPhase == MeleePhase::Stunned) {
                melee.stunTimer -= deltaTime;
                if (melee.stunTimer <= 0.0f) {
                    melee.stunTimer = 0.0f;
                    melee.currentPhase = MeleePhase::None;
                    melee.currentAttackNode = "";
                    melee.currentAttack = MeleeAttackType::None;
                }
                continue;
            }

            if (melee.currentAttack == MeleeAttackType::Parry) {
                melee.stateTimer += deltaTime;
                if (melee.currentPhase == MeleePhase::Windup && melee.stateTimer >= 0.08f) {
                    melee.currentPhase = MeleePhase::Active;
                }
                else if (melee.currentPhase == MeleePhase::Active && melee.stateTimer >= 0.28f) {
                    melee.currentPhase = MeleePhase::Recovery;
                }
                else if (melee.currentPhase == MeleePhase::Recovery && melee.stateTimer >= 0.48f) {
                    melee.currentPhase = MeleePhase::None;
                    melee.currentAttack = MeleeAttackType::None;
                    melee.stateTimer = 0.0f;
                }
                continue;
            }

            if (weaponBP && weaponBP->hasMoveset && !melee.currentAttackNode.empty()) {
                auto it = weaponBP->moveset.nodes.find(melee.currentAttackNode);
                if (it != weaponBP->moveset.nodes.end()) {
                    const auto& node = it->second;
                    melee.stateTimer += deltaTime;

                    if (melee.stateTimer >= node.comboWindow.start && melee.stateTimer <= node.comboWindow.end) {
                        if (!melee.bufferedInput.empty() && node.transitions.find(melee.bufferedInput) != node.transitions.end()) {
                            std::string nextNodeKey = node.transitions.at(melee.bufferedInput);

                            auto nextIt = weaponBP->moveset.nodes.find(nextNodeKey);
                            if (nextIt != weaponBP->moveset.nodes.end()) {
                                const auto& nextNode = nextIt->second;
                                if (stats.stamina >= nextNode.staminaCost) {
                                    stats.stamina -= nextNode.staminaCost;
                                    melee.currentAttackNode = nextNodeKey;
                                    melee.currentPhase = MeleePhase::Windup;
                                    melee.stateTimer = 0.0f;
                                    melee.bufferedInput = "";
                                    melee.hasHitInThisAttack = false;
                                    continue;
                                }
                            }
                        }
                    }

                    if (melee.currentPhase == MeleePhase::Windup && melee.stateTimer >= node.timings.windup) {
                        melee.currentPhase = MeleePhase::Active;
                        melee.stateTimer = 0.0f;
                        SpawnDebugSlashTrail(scene, entity, trans, melee, &node);
                    }
                    else if (melee.currentPhase == MeleePhase::Active && melee.stateTimer >= node.timings.active) {
                        melee.currentPhase = MeleePhase::Recovery;
                        melee.stateTimer = 0.0f;
                    }
                    else if (melee.currentPhase == MeleePhase::Recovery && melee.stateTimer >= node.timings.recovery) {
                        melee.currentPhase = MeleePhase::None;
                        melee.currentAttackNode = "";
                        melee.stateTimer = 0.0f;
                        melee.hasHitInThisAttack = false;
                    }

                    if (melee.currentPhase == MeleePhase::Active && !melee.hasHitInThisAttack) {
                        CheckMeleeHits(scene, entity, stats, melee, trans);
                    }
                }
            }
        }
    }

}