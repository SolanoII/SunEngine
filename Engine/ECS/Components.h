#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>

#include <entt/entt.hpp>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include "Tags.h"
#include "../Physics/HitboxBlock.h"
#include "../AssetManager/BlueprintManager.h"

namespace SunEngine {

    enum class HordeState {
        Waiting,      // In attesa nell'anello esterno (5.5m)
        Approaching,  // In avanzamento verso uno dei 4 slot attivi
        Attacking,    // In ingaggio melee (Attacco Normale)
        Cooldown      // Arretra temporaneamente dopo un attacco
    };

    struct HordeAgentComponent {
        HordeState state = HordeState::Waiting;
        int assignedSlotIndex = -1;
        float attackTimer = 0.0f;
        float attackDelay = 1.5f;
        float cooldownTimer = 0.0f;
        float circleAngle = 0.0f;

        entt::entity weaponEntity = entt::null;
        int attacksToPerform = 1;
        int currentAttackCount = 0;
    };

    enum class SurfaceType {
        Concrete,
        Dirt,
        Metal,
        Wood
    };

    enum class EquipStance {
        RangedTwoHanded,
        DualRangedOneHanded,
        HybridRangedMelee,
        MeleePure
    };

    enum class MeleeAttackType {
        None,
        Light,
        Normal,
        Heavy,
        Parry,
        Guard
    };

    enum class MeleePhase {
        None,
        Windup,
        Active,
        Recovery,
        Stunned
    };

    struct MeleeStateComponent {
        std::string currentAttackNode = "";
        MeleeAttackType currentAttack = MeleeAttackType::None;
        MeleePhase currentPhase = MeleePhase::None;

        float stateTimer = 0.0f;
        float parryWindowTimer = 0.0f;
        bool hasHyperArmor = false;
        bool isShieldEquipped = false;
        bool hasHitInThisAttack = false;

        float stunTimer = 0.0f;

        float fKeyHoldTimer = 0.0f;
        bool fKeyWasPressed = false;

        std::string bufferedInput = "";
        float inputBufferTimer = 0.0f;
    };

    struct ArmorPiece {
        std::string id = "none";
        std::string name = "Nessuna Protezione";
        int armorRating = 0;
        float coverage = 0.85f;
        float currentDurability = 0.0f;
        float maxDurability = 0.0f;
        float weight = 0.0f;
        int pointCost = 0;
    };

    struct ArmorSet {
        ArmorPiece head;
        ArmorPiece torso;
        ArmorPiece leftArm;
        ArmorPiece rightArm;
        ArmorPiece leftLeg;
        ArmorPiece rightLeg;

        ArmorPiece* GetPiece(HitboxLocation location) {
            switch (location) {
            case HitboxLocation::Head:     return &head;
            case HitboxLocation::Torso:    return &torso;
            case HitboxLocation::LeftArm:  return &leftArm;
            case HitboxLocation::RightArm: return &rightArm;
            case HitboxLocation::LeftLeg:  return &leftLeg;
            case HitboxLocation::RightLeg: return &rightLeg;
            default:                       return &torso;
            }
        }

        const ArmorPiece* GetPiece(HitboxLocation location) const {
            switch (location) {
            case HitboxLocation::Head:     return &head;
            case HitboxLocation::Torso:    return &torso;
            case HitboxLocation::LeftArm:  return &leftArm;
            case HitboxLocation::RightArm: return &rightArm;
            case HitboxLocation::LeftLeg:  return &leftLeg;
            case HitboxLocation::RightLeg: return &rightLeg;
            default:                       return &torso;
            }
        }

        int GetAverageRating() const {
            return (head.armorRating + torso.armorRating + leftArm.armorRating +
                rightArm.armorRating + leftLeg.armorRating + rightLeg.armorRating) / 6;
        }

        float GetTotalWeight() const {
            return head.weight + torso.weight + leftArm.weight +
                rightArm.weight + leftLeg.weight + rightLeg.weight;
        }
    };

    struct TransformComponent {
        glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f };
        float AimYaw = 0.0f;
        float AimPitch = 0.0f;
        glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

        glm::mat4 GetTransform() const {
            glm::mat4 transform = glm::translate(glm::mat4(1.0f), Position);
            transform = glm::rotate(transform, glm::radians(Rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
            transform = glm::rotate(transform, glm::radians(Rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
            transform = glm::rotate(transform, glm::radians(Rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
            transform = glm::scale(transform, Scale);
            return transform;
        }
    };

    struct JoltBodyComponent { JPH::BodyID bodyID; };

    struct RagdollComponent {
        std::vector<JPH::BodyID> bodyIDs;
        std::vector<glm::mat4> worldTransforms;
    };

    struct MeshComponent { unsigned int MeshID = 0; };

    struct ColorComponent {
        glm::vec4 Color = glm::vec4(1.0f);
        glm::vec4 BaseColor = glm::vec4(1.0f);
        float FlashTimer = 0.0f;
    };

    struct HealthComponent {
        float currentHealth = 100.0f;
        float maxHealth = 100.0f;
    };

    struct ColliderComponent { glm::vec3 halfExtents = glm::vec3(0.5f); };
    struct UnitBlueprintId { std::string id; };
    struct WeaponBlueprintId { std::string id; };

    inline std::pair<std::string, std::string> GetArmorClassification(int rating) {
        if (rating >= 100) return { "Massima", "Corazza al limite delle possibilita' tecnologiche del setting" };
        if (rating >= 95)  return { "Estremamente impenetrabile", "Protezione fuori scala per equipaggiamento normale" };
        if (rating >= 90)  return { "Quasi impenetrabile", "Solo armi estremamente potenti possono superarla" };
        if (rating >= 85)  return { "Eccezionale", "Necessarie armi specializzate per penetrarla" };
        if (rating >= 80)  return { "Fortificata", "Paragonabile a protezioni corazzate di livello eccezionale" };
        if (rating >= 75)  return { "Colossale", "Corazza estremamente spessa e resistente" };
        if (rating >= 70)  return { "Titanica", "Protezione da veicolo corazzato" };
        if (rating >= 65)  return { "Estrema", "Armi convenzionali difficilmente sufficienti" };
        if (rating >= 60)  return { "Super-corazzata", "Protezione paragonabile a corazze pesanti" };
        if (rating >= 55)  return { "Pesantemente corazzata", "Richiede armi potenti per essere efficacemente penetrata" };
        if (rating >= 50)  return { "Corazzata", "Protezione estremamente elevata" };
        if (rating >= 45)  return { "Molto pesante", "Protezione contro armi che normalmente penetrerebbero" };
        if (rating >= 40)  return { "Pesante", "Armatura militare pesante" };
        if (rating >= 35)  return { "Militare rinforzata", "Protezione significativa contro armi convenzionali" };
        if (rating >= 30)  return { "Militare", "Armatura militare leggera" };
        if (rating >= 25)  return { "Rinforzata", "Buona protezione contro armi leggere" };
        if (rating >= 20)  return { "Leggera (Standard)", "Equipaggiamento protettivo efficace contro minacce comuni" };
        if (rating >= 15)  return { "Base", "Protezione contro piccoli traumi e minacce minori" };
        if (rating >= 10)  return { "Leggera", "Protezioni rudimentali, equipaggiamento civile" };
        if (rating >= 5)   return { "Minima", "Vestiti pesanti, protezioni improvvisate" };
        return { "Nessuna", "Corpo completamente esposto" };
    }

    struct UnitStatsComponent {
        float baseSpeed = 5.0f;
        float speedModifier = 1.0f;
        float stamina = 100.0f;
        float maxStamina = 100.0f;
        float staminaRegen = 10.0f;
        float maxHealth = 100.0f;
        float currentHealth = 100.0f;
        float toughness = 50.0f;
        float strength = 50.0f;
        float vaultModifier = 1.0f;
        float accuracySkill = 1.0f;
        float meleeSkill = 1.0f;
        float morale = 100.0f;
        float suppressionThreshold = 50.0f;
        float visionRange = 30.0f;
        float fovDegrees = 90.0f;
        float weightCapacity = 60.0f;
        float currentWeight = 0.0f;

        ArmorSet armorSet;
    };

    struct WeaponStateComponent {
        std::string currentFireMode = "standard";
        int currentAmmo = 19;
        bool isReloading = false;
        float reloadTimer = 0.0f;
        float fireCooldown = 0.0f;
        float currentSpread = 0.0f;
        float currentHeat = 0.0f;
        bool isOverheating = false;
        bool isScopeActive = false;
    };

    struct TrailRequestComponent {
        glm::vec3 start = glm::vec3(0.0f);
        glm::vec3 end = glm::vec3(0.0f);
        glm::vec4 color = glm::vec4(1.0f, 0.2f, 0.0f, 1.0f);
        float currentAge = 0.0f;
        float travelTime = 0.5f;
        float totalLife = 1.0f;

        bool isBeam = true;
        float projectileLength = 0.5f;
        float projectileWidth = 0.05f;
    };

    struct CameraRecoilComponent {
        float recoilPitch = 0.0f;
        float recoilYaw = 0.0f;
        float recoverySpeed = 10.0f;
        float shakeIntensity = 0.0f;
        float fovOffset = 0.0f;
    };

    struct InventoryComponent {
        std::string primaryWeaponId = "";
        std::string secondaryWeaponId = "";
        std::string meleeWeaponId = "";
        std::string grenadeId = "";
        std::string tool1Id = "";
        std::string tool2Id = "";
        std::string tool3Id = "";

        int currentSlot = 0;

        std::string GetActiveWeaponId() const {
            if (currentSlot == 0) return primaryWeaponId;
            if (currentSlot == 1) return secondaryWeaponId;
            if (currentSlot == 2) return meleeWeaponId;
            if (currentSlot == 3) return grenadeId;
            return "";
        }

        EquipStance GetEquipStance() const {
            const auto* secBP = BlueprintManager::Get().GetWeapon(secondaryWeaponId);
            const auto* melBP = BlueprintManager::Get().GetWeapon(meleeWeaponId);

            bool secIs1H = secBP && (secBP->hand_type == HandType::OneHanded);
            bool melIs1H = melBP && (melBP->hand_type == HandType::OneHanded || melBP->id.find("1h") != std::string::npos);

            if ((currentSlot == 1 || currentSlot == 2) && secIs1H && melIs1H && !secondaryWeaponId.empty() && !meleeWeaponId.empty()) {
                return EquipStance::HybridRangedMelee;
            }

            std::string activeId = GetActiveWeaponId();
            const auto* activeBP = BlueprintManager::Get().GetWeapon(activeId);

            if (!activeBP) return EquipStance::MeleePure;

            if (activeBP->slot_type == WeaponSlotType::Melee || activeBP->is_melee) {
                return EquipStance::MeleePure;
            }

            if (activeBP->hand_type == HandType::OneHanded) {
                return EquipStance::DualRangedOneHanded;
            }

            return EquipStance::RangedTwoHanded;
        }
    };

    struct SkeletonComponent {
        SunEngine::SkeletonData data;
        float scale = 1.0f;
        std::vector<float> blockFlashTimers;
    };

    struct WeaponComponent {
        std::string name = "Blaster";
        float fireRate = 0.15f;
        float cooldown = 0.0f;
        std::string shootSound = "GameData/Audio/laser.wav";
    };

    struct ParticleEmitterComponent {
        glm::vec3 positionOffset{ 0.0f, 0.0f, -0.5f };
        glm::vec4 color{ 1.0f, 0.8f, 0.2f, 1.0f };
        float particleSize = 0.1f;
        float duration = 0.05f;
        bool isEmitting = false;
    };

    struct VaultComponent {
        bool isVaulting = false;
        float vaultTimer = 0.0f;
        float vaultDuration = 0.35f;
        glm::vec3 startPos{ 0.0f };
        glm::vec3 peakPos{ 0.0f };
        glm::vec3 targetPos{ 0.0f };
        bool isClimbing = false;
    };

    struct FootstepComponent {
        SurfaceType currentSurface = SurfaceType::Concrete;
        float stepTimer = 0.0f;
        float stepInterval = 0.42f;
        bool isMoving = false;
    };

    struct ProjectileComponent {
        glm::vec3 velocity{ 0.0f };
        float damage = 25.0f;
        float lifetime = 4.0f;
        entt::entity owner = entt::null;
        glm::vec4 color{ 1.0f, 0.8f, 0.2f, 1.0f };
        bool isLaser = false;
    };

    struct RigidBodyComponent {
        glm::vec3 velocity = glm::vec3(0.0f);
        float mass = 1.0f;
        bool useGravity = true;
        bool isGrounded = false;
    };

    struct CorpseComponent {
        float timeSinceDeath = 0.0f;
        float maxLifetime = 10.0f;
        glm::vec3 angularVelocity = glm::vec3(0.0f);
        bool isSleeping = false;

        float muscleStiffness = 1.0f;
        std::string hitBlockName = "Torso";
        glm::vec3 hitImpactDir{ 0.0f };

        std::vector<glm::vec3> jointVelocities;
        std::vector<glm::vec3> targetJointRotations;

        glm::vec3 headDeadRot{ 0.0f };
        glm::vec3 armLDeadRot{ 0.0f };
        glm::vec3 armRDeadRot{ 0.0f };
        glm::vec3 legLDeadRot{ 0.0f };
        glm::vec3 legRDeadRot{ 0.0f };
    };

    struct TreeObstacleTag {};

}