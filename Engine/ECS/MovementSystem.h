#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include <string>
#include <cstdlib>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <miniaudio.h>

#include "../Scene/Scene.h"
#include "../Core/Input.h"
#include "../Core/MathUtils.h"
#include "../AssetManager/BlueprintManager.h"
#include "../Core/Logger/Log.h"
#include <memory>
#include "Components.h"
#include "SunPhysicsSystem.h"

namespace SunEngine {
    
    struct FootstepSoundPoolItem {
        ma_sound sound{};
        bool loaded = false;
    };
    class MovementSystem {
    public:
        // Rilascia tutti i sound del pool dei passi. Chiamare PRIMA di
        // ma_sound_group_uninit / ma_engine_uninit. Idempotente.
        void ShutdownAudio() {
            for (auto& [surface, pool] : m_FootstepPools) {
                for (auto& item : pool) {
                    if (item && item->loaded) {
                        ma_sound_uninit(&item->sound);
                        item->loaded = false;
                    }
                }
                pool.clear();
            }
            m_FootstepPools.clear();
        }

        ~MovementSystem() {
            // Difensivo: se GameLayer non ha chiamato ShutdownAudio esplicitamente,
            // almeno proviamo a liberare qui. Nota: se m_SFXGroup è già stato
            // distrutto, questo può generare warning di miniaudio — per questo
            // GameLayer DEVE chiamare ShutdownAudio PRIMA di ma_sound_group_uninit.
            ShutdownAudio();
        }

    private:
        float m_WalkAnimTimer = 0.0f;

        // Pool di istanze concorrenti per ciascun tipo di superficie.
        // Ownership: questa classe. Vita: legata a MovementSystem.
        // FIX crash audio: unique_ptr perche' un ma_sound inizializzato NON puo' essere copiato ne'
        // spostato (il grafo audio ne conserva l'indirizzo). Prima: pool.push_back(item) copiava il
        // sound inizializzato da una variabile locale -> il thread audio leggeva memoria gia' riusata.
        std::unordered_map<SurfaceType, std::vector<std::unique_ptr<FootstepSoundPoolItem>>> m_FootstepPools;

        void PlayFootstep(ma_engine* engine, ma_sound_group* sfxGroup, SurfaceType surface,
            const glm::vec3& position, bool isPlayer, float volume) {
            if (!engine || volume <= 0.001f) return;

            // Scegli il path in base alla superficie (come già fai)
            std::string soundPath = "GameData/Audio/Footsteps/";
            switch (surface) {
            case SurfaceType::Concrete: default: soundPath += "step_concrete.wav"; break;
            case SurfaceType::Dirt:     soundPath += "step_dirt.wav";     break;
            case SurfaceType::Metal:    soundPath += "step_metal.wav";    break;
            case SurfaceType::Wood:     soundPath += "step_wood.wav";     break;
            }
            if (!std::filesystem::exists(soundPath)) return;

            // Recupera (o crea) il pool per questa superficie
            auto& pool = m_FootstepPools[surface];
            if (pool.empty()) {
                // Inizializza il pool alla prima chiamata (o fallo in Init)
                for (int i = 0; i < 4; ++i) {   // 4 istanze concorrenti max
                    auto item = std::make_unique<FootstepSoundPoolItem>();   // indirizzo stabile
                    ma_sound_config config = ma_sound_config_init();
                    config.pFilePath = soundPath.c_str();
                    config.flags = MA_SOUND_FLAG_DECODE;   // file breve → decode in RAM
                    config.pInitialAttachment = sfxGroup;  // attacca al gruppo SFX
                    if (ma_sound_init_ex(engine, &config, &item->sound) == MA_SUCCESS) {
                        item->loaded = true;
                        pool.push_back(std::move(item));
                    }
                }
            }

            // Trova un'istanza libera e riproduci
            for (auto& item : pool) {
                if (!item || !item->loaded) continue;
                if (!ma_sound_is_playing(&item->sound)) {
                    ma_sound_set_volume(&item->sound, volume);          // volume individuale
                    ma_sound_set_position(&item->sound, position.x, position.y, position.z);
                    // FIX #7: volume gia' attenuato a mano; qui solo panning rispetto al listener
                    ma_sound_set_attenuation_model(&item->sound, ma_attenuation_model_none);
                    ma_sound_seek_to_pcm_frame(&item->sound, 0);
                    ma_sound_start(&item->sound);
                    return;
                }
            }
        }
        bool RaycastScene(Scene& scene, const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDist, glm::vec3& outHitPoint, entt::entity ignoreEntity) {
            float minTHit = maxDist;
            bool hasHit = false;

            auto view = scene.GetRegistry().view<ColliderComponent, TransformComponent>(
                entt::exclude<PlayerTag, HealthComponent, CorpseComponent>   // <-- AGGIUNTO
            );

            for (auto entity : view) {
                if (entity == ignoreEntity) continue;

                const auto& col = view.get<ColliderComponent>(entity);
                const auto& trans = view.get<TransformComponent>(entity);

                // FIX #1: centro = Position + localCenterOffset
                glm::vec3 half = col.halfExtents * trans.Scale;
                glm::vec3 center = GetColliderWorldCenter(trans, col);
                glm::vec3 minBox = center - half;
                glm::vec3 maxBox = center + half;

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
                        tMin = std::max(tMin, t0);
                        tMax = std::min(tMax, t1);
                        if (tMax < tMin) { hit = false; break; }
                    }
                }

                if (hit && tMin < minTHit && tMin > 0.0f) {
                    minTHit = tMin;
                    hasHit = true;
                }
            }

            if (hasHit) {
                outHitPoint = rayOrigin + rayDir * minTHit;
                return true;
            }
            return false;
        }

        // VERIFICA OSTACOLI: ALTEZZA GAMBE + CAPACITA' DI SALTO (FORZA FISICA)
        // VERIFICA OSTACOLI CON CALCOLO PROFONDITA' PER EVITARE INCASITRI
        bool CheckVaultObstacle(Scene& scene, entt::entity entity, glm::vec3& outPeakPos, glm::vec3& outTargetPos, bool& outIsClimbing, float& outDuration) {
            const auto& trans = scene.GetRegistry().get<TransformComponent>(entity);
            const auto& stats = scene.GetRegistry().get<UnitStatsComponent>(entity);

            float legHeight = 0.9f;
            if (scene.GetRegistry().all_of<ColliderComponent>(entity)) {
                const auto& col = scene.GetRegistry().get<ColliderComponent>(entity);
                legHeight = col.halfExtents.y * trans.Scale.y;
            }

            float vaultMod = (stats.vaultModifier > 0.01f) ? stats.vaultModifier : 1.0f;
            float jumpReach = (stats.strength * 0.032f) * vaultMod;
            const float vaultHeight = legHeight + jumpReach;            // scavalcamento normale
            // Arrampicata lenta (es. gretchin): puo' salire su muri piu' alti di quanto scavalcherebbe
            float maxClimbHeight = std::max(vaultHeight, stats.climbHeight);

            glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
            glm::vec3 forwardDir = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

            glm::vec3 forwardRayStart = trans.Position + glm::vec3(0.0f, 0.4f, 0.0f);
            glm::vec3 forwardHitPoint;
            float checkDistance = 1.4f;

            if (RaycastScene(scene, forwardRayStart, forwardDir, checkDistance, forwardHitPoint, entity)) {
                float wallDist = glm::distance(glm::vec2(trans.Position.x, trans.Position.z), glm::vec2(forwardHitPoint.x, forwardHitPoint.z));

                // Raycast verso il basso per trovare il bordo superiore dell'ostacolo.
                // FIX vault: prima si provava solo a wallDist+0.35m. Un ostacolo piu' sottile di 0.35m
                // (barricata 0.3m, muretto 0.2m) veniva "saltato" dal raggio, che cadeva sul terreno
                // dietro di lui (altezza 0 -> nessun vault). Per questo funzionava solo guardando di
                // lato: il raggio obliquo penetra meno in profondita'. Ora si provano piu' offset,
                // dal piu' profondo al piu' vicino alla faccia anteriore, e vince il primo che trova
                // una cima valida.
                glm::vec3 downHitPoint(0.0f);
                float obstacleHeight = 0.0f;
                bool foundTop = false;
                const float topProbeOffsets[] = { 0.35f, 0.20f, 0.08f };
                for (float probeOffset : topProbeOffsets) {
                    glm::vec3 downRayStart = trans.Position + forwardDir * (wallDist + probeOffset) + glm::vec3(0.0f, maxClimbHeight + 0.5f, 0.0f);
                    glm::vec3 probeHit;
                    if (RaycastScene(scene, downRayStart, glm::vec3(0.0f, -1.0f, 0.0f), maxClimbHeight + 1.0f, probeHit, entity)) {
                        float h = probeHit.y - trans.Position.y;
                        if (h > 0.25f && h <= maxClimbHeight) {
                            downHitPoint = probeHit;
                            obstacleHeight = h;
                            foundTop = true;
                            break;
                        }
                    }
                }

                {
                    if (foundTop && obstacleHeight > vaultHeight && wallDist > 0.75f) {
                        return false;   // arrampicata: bisogna essere attaccati al muro, non a un metro e mezzo
                    }
                    if (foundTop) {
                        // Muro piu' alto dello scavalcamento: si arrampica lentamente (sale a climbSpeed m/s)
                        const bool slowClimb = obstacleHeight > vaultHeight;
                        const float climbTime = slowClimb ? 0.45f + obstacleHeight / std::max(0.1f, stats.climbSpeed) : 0.0f;

                        // Secondo raycast più avanti (1.3m) per verificare se la cassa è larga o stretta
                        glm::vec3 farDownRayStart = trans.Position + forwardDir * (wallDist + 1.30f) + glm::vec3(0.0f, maxClimbHeight + 0.5f, 0.0f);
                        glm::vec3 farDownHitPoint;
                        bool hasFarHit = RaycastScene(scene, farDownRayStart, glm::vec3(0.0f, -1.0f, 0.0f), maxClimbHeight + 1.0f, farDownHitPoint, entity);

                        if (hasFarHit && (farDownHitPoint.y > trans.Position.y + 0.25f)) {
                            // 1. OSTACOLO LARGO (Tetto / Cassa Grande): Arrampicata e atterraggio SOPRA la cassa
                            outIsClimbing = (obstacleHeight > legHeight);
                            outDuration = slowClimb ? climbTime : (outIsClimbing ? 0.75f : 0.40f);
                            outPeakPos = downHitPoint + glm::vec3(0.0f, 0.35f, 0.0f);
                            outTargetPos = downHitPoint + forwardDir * 0.70f;
                            outTargetPos.y = downHitPoint.y; // Resta sulla superficie superiore della cassa
                        }
                        else {
                            // 2. OSTACOLO STRETTO (Muretto / Cassa Stretta): Scavalcamento OLTRE l'ostacolo
                            outIsClimbing = (obstacleHeight > legHeight);
                            outDuration = slowClimb ? climbTime + 0.3f : (outIsClimbing ? 0.80f : 0.38f);

                            // Peak posizionato in alto e leggermente avanzato
                            outPeakPos = downHitPoint + glm::vec3(0.0f, 0.45f, 0.0f) + forwardDir * 0.25f;

                            // Atterraggio a terra ben oltre la cassa (1.3m avanti)
                            // FIX #21: se non c'e' terreno oltre l'ostacolo atterra alla quota di partenza
                            float targetY = hasFarHit ? farDownHitPoint.y
                                : GetGroundHeightAt(scene, trans.Position + forwardDir * (wallDist + 1.30f), trans.Position.y).value_or(trans.Position.y);
                            outTargetPos = trans.Position + forwardDir * (wallDist + 1.30f);
                            outTargetPos.y = targetY;
                        }

                        return true;
                    }
                }
            }
            return false;
        }
        // Posa accovacciata: il blocco radice (Torso) scende del 40% della sua altezza e le gambe (blocco unico,
        // senza ginocchio) si aprono una avanti e una indietro quanto basta per restare a terra. Le hitbox sono gli
        // stessi blocchi, quindi dietro una copertura bassa si e' davvero piu' difficili da colpire.
        static void ApplyCrouchPose(SkeletonComponent& skel, CrouchComponent& crouch) {
            int root = -1;
            for (size_t i = 0; i < skel.data.blocks.size(); ++i) if (skel.data.blocks[i].parentIndex < 0) { root = static_cast<int>(i); break; }
            if (root < 0) return;
            auto& rootBlock = skel.data.blocks[root];
            if (crouch.skeletonId != skel.data.id) {
                // Nuovo scheletro (cambio unita'): altezza di riposo e angolo delle gambe a piena accovacciata.
                // L'angolo si cerca in modo che i piedi non vadano sotto il livello che hanno in piedi (le gambe
                // sono blocchi spessi: col solo calcolo trigonometrico gli spigoli entrerebbero nel terreno).
                crouch.skeletonId = skel.data.id;
                crouch.baseRootY = rootBlock.localOffset.y;
                SkeletonData probe = skel.data;
                for (auto& b : probe.blocks) b.jointRotation = glm::vec3(0.0f);
                auto lowest = [&]() {
                    float mn = 1e9f;
                    const auto mats = probe.ComputeWorldTransforms(glm::vec3(0.0f), glm::vec3(0.0f), 1.0f);
                    for (size_t i = 0; i < mats.size(); ++i) {
                        if (probe.blocks[i].name != "Leg_L" && probe.blocks[i].name != "Leg_R") continue;   // solo i piedi
                        const glm::mat4& m = mats[i];
                        for (int c = 0; c < 8; ++c) {
                            const glm::vec4 p = m * glm::vec4((c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 0.5f : -0.5f, 1.0f);
                            mn = std::min(mn, p.y);
                        }
                    }
                    return mn;
                };
                const float standLow = lowest();
                probe.blocks[root].localOffset.y = crouch.baseRootY * 0.60f;
                crouch.legAngle = 89.0f;
                for (float a = 0.0f; a <= 89.0f; a += 1.0f) {
                    for (auto& b : probe.blocks) {
                        if (b.name == "Leg_L") b.jointRotation.x = a;
                        else if (b.name == "Leg_R") b.jointRotation.x = -a;
                    }
                    if (lowest() >= standLow - 0.01f) { crouch.legAngle = a; break; }
                }
            }
            const float drop = crouch.baseRootY * 0.40f * crouch.amount;
            rootBlock.localOffset.y = crouch.baseRootY - drop;
            if (crouch.amount <= 0.0f) return;
            for (auto& block : skel.data.blocks) {
                if (block.name != "Leg_L" && block.name != "Leg_R") continue;
                const float target = block.name == "Leg_L" ? crouch.legAngle : -crouch.legAngle;   // sinistra avanti, destra indietro
                block.jointRotation.x = glm::mix(block.jointRotation.x, target, crouch.amount);
            }
        }

        void ProcessVaulting(Scene& scene, PhysicsSystem& physicsSystem, entt::entity entity, float deltaTime) {
            auto& vault = scene.GetRegistry().get<VaultComponent>(entity);
            auto& trans = scene.GetRegistry().get<TransformComponent>(entity);

            vault.vaultTimer += deltaTime;
            float progress = std::clamp(vault.vaultTimer / vault.vaultDuration, 0.0f, 1.0f);

            glm::vec3 currentPos;
            if (progress < 0.5f) {
                // Prima metà: Salita fluida dal punto di partenza al picco
                float t = progress * 2.0f;
                currentPos = glm::mix(vault.startPos, vault.peakPos, t);
            }
            else {
                // Seconda metà: Discesa parabolica (t^2) per scavalcare lo spigolo posteriore senza compenetrare
                float t = (progress - 0.5f) * 2.0f;
                float yFactor = t * t; // Mantiene l'altezza XZ prima di scendere in picchiata a terra

                currentPos.x = glm::mix(vault.peakPos.x, vault.targetPos.x, t);
                currentPos.z = glm::mix(vault.peakPos.z, vault.targetPos.z, t);
                currentPos.y = glm::mix(vault.peakPos.y, vault.targetPos.y, yFactor);
            }

            trans.Position = currentPos;
            physicsSystem.SetBodyPosition(scene, entity, currentPos);

            // ANIMAZIONE PROCEDURALE
            if (scene.GetRegistry().all_of<SkeletonComponent>(entity)) {
                auto& skel = scene.GetRegistry().get<SkeletonComponent>(entity);

                if (vault.isClimbing) {
                    float grabPhase = std::sin(progress * 3.14159f);
                    for (auto& block : skel.data.blocks) {
                        if (block.name == "Arm_R" || block.name == "Arm_L") {
                            block.jointRotation.x = -85.0f + (progress * 60.0f);
                        }
                        if (block.name == "Leg_L" || block.name == "Leg_R") {
                            block.jointRotation.x = grabPhase * 45.0f;
                        }
                        if (block.name == "Torso") {
                            block.jointRotation.x = grabPhase * 25.0f;
                        }
                    }
                }
                else {
                    float legPitch = std::sin(progress * 3.14159f) * 55.0f;
                    for (auto& block : skel.data.blocks) {
                        if (block.name == "Leg_L") block.jointRotation.x = legPitch;
                        if (block.name == "Leg_R") block.jointRotation.x = legPitch * 0.4f;
                        if (block.name == "Arm_R" || block.name == "Arm_L") block.jointRotation.x = -25.0f;
                    }
                }
            }

            if (progress >= 1.0f) {
                // Snap finale preciso per garantire che il corpo Jolt sia pulito fuori dai collider
                trans.Position = vault.targetPos;
                physicsSystem.SetBodyPosition(scene, entity, vault.targetPos);

                vault.isVaulting = false;
                vault.vaultTimer = 0.0f;
                scene.GetRegistry().remove<VaultComponent>(entity);
            }
        }
        
        bool CheckWallCollision(Scene& scene, const glm::vec3& targetPos, float currentY, float playerRadius = 0.35f) {
            auto view = scene.GetRegistry().view<ColliderComponent, TransformComponent>(
                entt::exclude<PlayerTag, HealthComponent, CorpseComponent>   // <-- AGGIUNTO
            );

            for (auto entity : view) {
                const auto& col = view.get<ColliderComponent>(entity);
                const auto& trans = view.get<TransformComponent>(entity);

                // FIX #1: centro = Position + localCenterOffset
                glm::vec3 halfExtents = col.halfExtents * trans.Scale;
                glm::vec3 center = GetColliderWorldCenter(trans, col);
                glm::vec3 minBox = center - halfExtents;
                glm::vec3 maxBox = center + halfExtents;

                if (maxBox.y > currentY + 0.6f) {
                    if (targetPos.x + playerRadius > minBox.x && targetPos.x - playerRadius < maxBox.x &&
                        targetPos.z + playerRadius > minBox.z && targetPos.z - playerRadius < maxBox.z &&
                        targetPos.y < maxBox.y && (targetPos.y + 1.8f) > minBox.y) {
                        return true;
                    }
                }
            }
            return false;
        }
public:
    void Update(Scene& scene, PhysicsSystem& physicsSystem, ma_engine* audioEngine,
        ma_sound_group* sfxGroup, float deltaTime, float volumeSFX = 1.0f) {
        // -------------------------------------------------------------------------
        // 1. MOVIMENTO GIOCATORE
        // -------------------------------------------------------------------------
        auto view = scene.GetRegistry().view<PlayerTag, TransformComponent, UnitStatsComponent, WeaponBlueprintId>();
        for (auto entity : view) {
            auto& transform = view.get<TransformComponent>(entity);
            auto& stats = view.get<UnitStatsComponent>(entity);
            stats.sprinting = false;   // lo rimette TickSprint piu' sotto se si corre davvero

            if (scene.GetRegistry().all_of<VaultComponent>(entity)) {
                ProcessVaulting(scene, physicsSystem, entity, deltaTime);
                continue;
            }
            if (scene.GetRegistry().all_of<TurretMountComponent>(entity)) continue;   // dentro una torretta: la posizione la tiene TurretSystem

            // Accovacciamento: CTRL tenuto (transizione di ~0,15 s)
            auto& crouch = scene.GetRegistry().get_or_emplace<CrouchComponent>(entity);
            const bool wantCrouch = Input::IsKeyPressed(GLFW_KEY_LEFT_CONTROL) || Input::IsKeyPressed(GLFW_KEY_RIGHT_CONTROL);
            crouch.amount = std::clamp(crouch.amount + (wantCrouch ? 1.0f : -1.0f) * deltaTime * 7.0f, 0.0f, 1.0f);

            if (Input::IsKeyPressed(GLFW_KEY_SPACE) && Input::IsKeyPressed(GLFW_KEY_W) && crouch.amount < 0.5f) {
                glm::vec3 peakPos, targetPos;
                bool isClimbing = false;
                float duration = 0.35f;

                if (CheckVaultObstacle(scene, entity, peakPos, targetPos, isClimbing, duration)) {
                    auto& vault = scene.GetRegistry().emplace<VaultComponent>(entity);
                    vault.isVaulting = true;
                    vault.vaultTimer = 0.0f;
                    vault.vaultDuration = duration;
                    vault.isClimbing = isClimbing;
                    vault.startPos = transform.Position;
                    vault.peakPos = peakPos;
                    vault.targetPos = targetPos;

                    SUN_CORE_INFO(isClimbing ? "ARRAMPICATA AVVIATA!" : "SCAVALCO RAPIDO AVVIATO!");
                    continue;
                }
            }

            const auto* moveBuff = scene.GetRegistry().try_get<FactionBuffComponent>(entity);
            const bool ignoreSlows = moveBuff && moveBuff->ignoreSlows;   // "MOVE MOVE MOVE!": niente rallentamenti
            const float slow = ignoreSlows ? std::max(1.0f, stats.speedModifier) : stats.speedModifier;   // mira
            float moveSpeed = (stats.baseSpeed > 0.1f) ? (stats.baseSpeed * slow) : 5.0f;
            if (!ignoreSlows) moveSpeed *= CarrySpeedFactor(stats);   // peso addosso rispetto alla capacita' di trasporto
            // Passiva dell'arma bianca in mano (es. coltello del gretchin: +5%)
            if (const auto* inv = scene.GetRegistry().try_get<InventoryComponent>(entity)) moveSpeed *= inv->HeldSpeedMultiplier();
            moveSpeed *= 1.0f - 0.5f * crouch.amount;   // accovacciato: meta' velocita'
            if (const auto* buff = scene.GetRegistry().try_get<FactionBuffComponent>(entity)) moveSpeed *= buff->moveSpeedMult;   // WAAAGH!
            if (const auto* boost = scene.GetRegistry().try_get<SpeedBoostComponent>(entity)) moveSpeed *= boost->mult;          // zaino energetico

            glm::vec3 moveDir{ 0.0f };
            if (Input::IsKeyPressed(GLFW_KEY_W)) moveDir.z -= 1.0f;
            if (Input::IsKeyPressed(GLFW_KEY_S)) moveDir.z += 1.0f;
            if (Input::IsKeyPressed(GLFW_KEY_A)) moveDir.x -= 1.0f;
            if (Input::IsKeyPressed(GLFW_KEY_D)) moveDir.x += 1.0f;

            bool isMoving = glm::length(moveDir) > 0.001f;
            // Corsa: SHIFT + avanti, non mirando e non accovacciati. Consuma stamina in base al carico.
            const bool wantSprint = isMoving && Input::IsKeyPressed(GLFW_KEY_LEFT_SHIFT) && Input::IsKeyPressed(GLFW_KEY_W)
                && !Input::IsKeyPressed(GLFW_KEY_S) && crouch.amount < 0.3f && !Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT);
            if (TickSprint(stats, wantSprint, deltaTime)) moveSpeed *= kSprintSpeedMult;
            const glm::vec3 posBeforeMove = transform.Position;   // FIX #21: ultima posizione valida del frame

            transform.Rotation.x = 0.0f;
            transform.Rotation.z = 0.0f;

            if (isMoving) {
                moveDir = glm::normalize(moveDir);

                glm::mat4 rotMat = glm::rotate(glm::mat4(1.0f), glm::radians(transform.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
                glm::vec3 worldMove = glm::vec3(rotMat * glm::vec4(moveDir, 0.0f));

                glm::vec3 nextPosX = transform.Position;
                nextPosX.x += worldMove.x * moveSpeed * deltaTime;
                if (!CheckWallCollision(scene, nextPosX, transform.Position.y)) {
                    transform.Position.x = nextPosX.x;
                }

                glm::vec3 nextPosZ = transform.Position;
                nextPosZ.z += worldMove.z * moveSpeed * deltaTime;
                if (!CheckWallCollision(scene, nextPosZ, transform.Position.y)) {
                    transform.Position.z = nextPosZ.z;
                }

                transform.Rotation.y = LerpAngle(transform.Rotation.y, transform.AimYaw, 15.0f * deltaTime);
                m_WalkAnimTimer += deltaTime * moveSpeed * 2.5f;
            }
            else {
                m_WalkAnimTimer = glm::mix(m_WalkAnimTimer, 0.0f, 10.0f * deltaTime);
                transform.Rotation.y = LerpAngle(transform.Rotation.y, transform.AimYaw, 15.0f * deltaTime);
            }

            // Scatto dell'attacco pesante (lancia): spinta in avanti che si ferma contro i muri
            if (auto* melee = scene.GetRegistry().try_get<MeleeStateComponent>(entity); melee && melee->lungeTimer > 0.0f) {
                const float dt = std::min(deltaTime, melee->lungeTimer);
                melee->lungeTimer -= deltaTime;
                glm::vec3 nx = transform.Position; nx.x += melee->lungeVelocity.x * dt;
                if (!CheckWallCollision(scene, nx, transform.Position.y)) transform.Position.x = nx.x;
                glm::vec3 nz = transform.Position; nz.z += melee->lungeVelocity.z * dt;
                if (!CheckWallCollision(scene, nz, transform.Position.y)) transform.Position.z = nz.z;
            }

            // FIX #21: nessun terreno sotto (bordo mappa) -> annulla lo spostamento orizzontale di
            // questo frame e mantieni Y, invece di teletrasportare a y=0.
            if (auto groundY = GetGroundHeightAt(scene, transform.Position, transform.Position.y)) {
                transform.Position.y = glm::mix(transform.Position.y, *groundY, 20.0f * deltaTime);
            }
            else {
                transform.Position.x = posBeforeMove.x;
                transform.Position.z = posBeforeMove.z;
            }

            physicsSystem.SetBodyPosition(scene, entity, transform.Position);

            // Ostacoli alberi
            auto treeView = scene.GetRegistry().view<TreeObstacleTag, TransformComponent>();
            for (auto treeEntity : treeView) {
                const auto& treeTrans = treeView.get<TransformComponent>(treeEntity);
                glm::vec2 playerXZ(transform.Position.x, transform.Position.z);
                glm::vec2 treeXZ(treeTrans.Position.x, treeTrans.Position.z);

                float minDist = 0.75f;
                float dist = glm::distance(playerXZ, treeXZ);

                if (dist < minDist && dist > 0.0001f) {
                    glm::vec2 pushDir = glm::normalize(playerXZ - treeXZ);
                    glm::vec2 correctedPos = treeXZ + pushDir * minDist;
                    transform.Position.x = correctedPos.x;
                    transform.Position.z = correctedPos.y;
                    physicsSystem.SetBodyPosition(scene, entity, transform.Position);
                }
            }

            // Animazione scheletro
            if (scene.GetRegistry().all_of<SkeletonComponent>(entity)) {
                auto& skel = scene.GetRegistry().get<SkeletonComponent>(entity);
                float legAngle = isMoving ? std::sin(m_WalkAnimTimer) * 30.0f : 0.0f;

                for (auto& block : skel.data.blocks) {
                    if (block.name == "Leg_L") {
                        block.jointRotation.x = legAngle;
                    }
                    else if (block.name == "Leg_R") {
                        block.jointRotation.x = -legAngle;
                    }
                    else if (block.name == "Arm_R" || block.name == "Arm_L") {
                        float armSwing = isMoving ? (block.name == "Arm_R" ? legAngle * 0.3f : -legAngle * 0.3f) : 0.0f;
                        block.jointRotation.x = -transform.AimPitch + armSwing;
                    }
                    else if (block.name == "Head") {
                        block.jointRotation.x = -transform.AimPitch * 0.5f;
                    }
                }
                ApplyCrouchPose(skel, crouch);
            }
        }

        // -------------------------------------------------------------------------
        // 2. GESTORE PASSI GLOBALE CON LIMITATORE DI PROSSIMITA' (MAX 10)
        // -------------------------------------------------------------------------
        struct FootstepCandidate {
            entt::entity entity;
            float distanceToPlayer;
            SurfaceType surface;
            glm::vec3 position;
            bool isPlayer;
        };

        std::vector<FootstepCandidate> candidates;

        glm::vec3 playerPos(0.0f);
        auto playerSearch = scene.GetRegistry().view<PlayerTag, TransformComponent>();
        for (auto pEnt : playerSearch) {
            playerPos = playerSearch.get<TransformComponent>(pEnt).Position;
            break;
        }

        auto footstepView = scene.GetRegistry().view<TransformComponent, FootstepComponent>();
        for (auto entity : footstepView) {
            auto& trans = footstepView.get<TransformComponent>(entity);
            auto& footstep = footstepView.get<FootstepComponent>(entity);
            bool isPlayer = scene.GetRegistry().all_of<PlayerTag>(entity);

            float distMoved = glm::distance(trans.Position, footstep.lastPosition);
            bool isMoving = (distMoved > 0.001f);
            footstep.lastPosition = trans.Position;

            if (isMoving) {
                footstep.stepTimer += deltaTime;
                if (footstep.stepTimer >= footstep.stepInterval) {
                    footstep.stepTimer = 0.0f;
                    float distToPlayer = isPlayer ? 0.0f : glm::distance(trans.Position, playerPos);

                    if (isPlayer || distToPlayer <= 25.0f) {
                        candidates.push_back({ entity, distToPlayer, footstep.currentSurface, trans.Position, isPlayer });
                    }
                }
            }
            else {
                footstep.stepTimer = footstep.stepInterval * 0.8f;
            }
        }

        // Ordina i candidati per vicinanza al giocatore
        std::sort(candidates.begin(), candidates.end(), [](const FootstepCandidate& a, const FootstepCandidate& b) {
            return a.distanceToPlayer < b.distanceToPlayer;
        });

        // Riproduce il suono solo per i primi 10 nemici/giocatore più vicini
        constexpr int MAX_AUDIBLE_FOOTSTEPS = 10;
        int count = std::min(static_cast<int>(candidates.size()), MAX_AUDIBLE_FOOTSTEPS);

        for (int i = 0; i < count; ++i) {
            const auto& cand = candidates[i];
            float volume = 0.0f;

            if (cand.isPlayer) {
                volume = 0.85f * volumeSFX;
            }
            else {
                float attenuation = std::clamp(1.0f - (cand.distanceToPlayer / 25.0f), 0.0f, 1.0f);
                volume = attenuation * 0.65f * volumeSFX;
            }

            PlayFootstep(audioEngine, sfxGroup, cand.surface, cand.position, cand.isPlayer, volume);
        }
    }
    };
}