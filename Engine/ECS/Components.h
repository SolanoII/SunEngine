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
#include <Jolt/Physics/Constraints/Constraint.h>

#include "Tags.h"
#include "../Physics/HitboxBlock.h"
#include "../AssetManager/BlueprintManager.h"
#include "../Renderer/SkinnedModel.h"
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

        std::vector<glm::vec3> currentPath;
        float pathUpdateTimer = 0.0f;

        bool hasLOS = false;
        glm::vec3 lastKnownPlayerPos{ 0.0f };
        bool hasEverSeenPlayer = false;   // <-- AGGIUNGI QUESTA RIGA
        float losCheckTimer = 0.0f;
        float losCheckInterval = 0.30f;
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
        bool isShieldEquipped = false;      // scudo: la parata (Q) ferma anche i colpi a distanza
        bool parryBlocksRanged = false;     // abilita'/effetti ("perfect deflect"): come sopra, senza scudo
        float parrySuccessTimer = 0.0f;     // >0 subito dopo una parata riuscita (lampo dell'anello di parata)
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
        // FIX crash Jolt: i vincoli (giunti) del ragdoll vanno ricordati per poterli RIMUOVERE dal
        // PhysicsSystem prima di distruggere i body. Prima restavano registrati con puntatori a body
        // gia' distrutti e Jolt crashava in ConstraintManager::sBuildIslands -> Body::IsActive.
        std::vector<JPH::Ref<JPH::Constraint>> constraints;
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

    struct ColliderComponent {
        glm::vec3 halfExtents = glm::vec3(0.5f);
        // Offset dal TransformComponent.Position al CENTRO del box, in spazio
        // locale (pre-scala, pre-rotazione). Viene moltiplicato per transform.Scale
        // e ruotato dalla rotazione dell'entita'.
        //
        // Convenzione "box con la base a Position": (0, halfExtents.y, 0).
        // Convenzione "box centrato su Position":   (0, 0, 0).
        // Convenzione "skeleton-based":             (0, (minY+maxY)/2, 0).
        glm::vec3 localCenterOffset = glm::vec3(0.0f);
    };

    // FIX #1: centro MONDO del collider = Position + R * (localCenterOffset * Scale).
    // Stessa formula usata da PhysicsSystem::RefreshBodies/SetBodyPosition, cosi' raycast,
    // LOS, collisioni muro e corpi Jolt vedono lo STESSO box. Usare questa funzione in
    // OGNI test AABB (mai trans.Position da solo, altrimenti il box e' shiftato di offset).
    // La rotazione (Y, X, Z, come in TransformComponent::GetTransform) e' applicata solo
    // se l'entita' e' ruotata: per i blocchi normali costo nullo.
    inline glm::vec3 GetColliderWorldCenter(const TransformComponent& trans, const ColliderComponent& col) {
        glm::vec3 scaledOffset = col.localCenterOffset * trans.Scale;
        if (trans.Rotation.x != 0.0f || trans.Rotation.y != 0.0f || trans.Rotation.z != 0.0f) {
            glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(trans.Rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
            rot = glm::rotate(rot, glm::radians(trans.Rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
            rot = glm::rotate(rot, glm::radians(trans.Rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
            scaledOffset = glm::vec3(rot * glm::vec4(scaledOffset, 0.0f));
        }
        return trans.Position + scaledOffset;
    }
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

        // --- Trigger tracking per fire modes ---
        // wasFiringLastFrame: stato del grilletto al frame precedente.
        //   Serve a rilevare il fronte di salita (click) per semi-auto.
        // burstRemaining: colpi ancora da sparare nella raffica corrente.
        //   Quando e' 0, serve un nuovo click per ripartire (burst non auto-ripetente).
        bool wasFiringLastFrame = false;
        int  burstRemaining = 0;
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
        std::vector<float> blockFlashTimers;        // >0: il blocco e' stato colpito da poco e si illumina
        std::vector<glm::vec3> blockFlashColors;    // colore del flash (verde -> rosso in base al danno)
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

    // Posizione MONDO della bocca della canna dell'arma in mano (per mano), aggiornata ogni frame da
    // GameLayer dal modello a blocchi dell'arma. WeaponSystem la usa come origine del colpo/tracciante:
    // cosi' il colpo parte dalla punta dell'arma che si vede, non da un offset fisso calcolato sui cubi.
    // Feedback di colpo per il giocatore: un cerchio sul punto colpito, colorato in base all'efficacia
    // del colpo (verde = poco danno, rosso = colpo pesante / uccisione). Creato da WeaponSystem come
    // entity a parte; GameLayer lo disegna nell'HUD, lo fa invecchiare e lo distrugge.
    struct HitMarkerComponent {
        entt::entity target = entt::null;
        glm::vec3 localOffset{ 0.0f };   // punto colpito relativo a target.Position (segue il bersaglio)
        glm::vec3 worldPoint{ 0.0f };    // ultimo punto mondo noto (se il bersaglio sparisce)
        float damage = 0.0f;             // danno REALE inflitto (dopo armatura/robustezza), sommato per raffica
        float targetMaxHealth = 100.0f;
        bool killed = false;
        float age = 0.0f;
    };

    // Colore di efficacia del colpo: verde (danno trascurabile) -> giallo -> rosso (colpo pesante).
    // Pieno rosso quando un colpo (o raffica) toglie kHitRedFraction della salute massima, o uccide.
    inline constexpr float kHitRedFraction = 0.25f;
    inline glm::vec3 HitEffectivenessColor(float damage, float targetMaxHealth, bool killed) {
        float k = killed ? 1.0f : damage / ((targetMaxHealth > 1.0f ? targetMaxHealth : 100.0f) * kHitRedFraction);
        k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
        // verde (0.2,1,0.3) -> giallo (1,0.9,0.2) -> rosso (1,0.15,0.1)
        if (k < 0.5f) {
            const float t = k / 0.5f;
            return glm::vec3(0.2f + 0.8f * t, 1.0f - 0.1f * t, 0.3f - 0.1f * t);
        }
        const float t = (k - 0.5f) / 0.5f;
        return glm::vec3(1.0f, 0.9f - 0.75f * t, 0.2f - 0.1f * t);
    }

    // Manichino del campo da tiro (Testing Lobby): non muore, non attacca, mostra il danno ricevuto.
    // La salute torna piena qualche secondo dopo l'ultimo colpo.
    struct TrainingDummyComponent {
        std::string unitId;
        std::string displayName;
        float prevHealth = 0.0f;
        float lastDamage = 0.0f;       // danno dell'ultimo colpo (o raffica nello stesso frame)
        float totalDamage = 0.0f;      // danno dall'ultimo reset
        int hits = 0;
        float sinceHit = 999.0f;       // secondi dall'ultimo colpo
        bool lethal = false;           // la salute e' arrivata a zero (avrebbe ucciso)
        int lastLocation = -1;         // HitboxLocation dell'ultimo colpo a distanza (-1 = sconosciuta/mischia)
    };

    struct MuzzleComponent {
        glm::vec3 right{ 0.0f };
        glm::vec3 left{ 0.0f };
        bool hasRight = false;
        bool hasLeft = false;
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
        glm::vec3 lastPosition{ 0.0f }; // <--- TRACCIAMENTO POSIZIONE PRECEDENTE
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

    struct SkinnedModelInstanceComponent {
        SunEngine::SkinnedModel* sourceModel = nullptr;
        const std::vector<glm::mat4>* boneMatrices = nullptr;
        glm::vec4 albedo = glm::vec4(0.65f, 0.50f, 0.35f, 1.0f);
        float roughness = 0.7f;
        float metallic = 0.0f;
        float emissive = 0.0f;
        // Modello vuoto di default: non renderizzato finché non setti i puntatori
    };                               // ← aggiungi ; 

}                                    // ← aggiungi chiusura namespace