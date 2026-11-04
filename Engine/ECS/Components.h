#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>

#include <entt/entt.hpp>
#include "XpEvents.h"
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

        // Scatto dell'attacco pesante (armi con "heavy_lunge_distance", es. lancia dello yoof)
        float lungeTimer = 0.0f;
        glm::vec3 lungeVelocity{ 0.0f };

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
        bool blocksHeadshotBonus = false;   // elmetto: finche' regge, niente moltiplicatore del colpo in testa
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
        float climbHeight = 0.0f;   // arrampicata lenta oltre lo scavalcamento (m, 0 = no)
        float climbSpeed = 0.6f;    // m/s in salita
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

    // Munizioni limitate. Il caricatore dell'arma a distanza in mano vive in WeaponStateComponent::currentAmmo;
    // quelli delle altre armi restano qui finche' non le riprendi in mano (indice 0 = primaria, 1 = secondaria).
    // La riserva ("colpi rimasti") si consuma ricaricando e si recupera con la scatola di munizioni.
    struct AmmoComponent {
        int magazine[2] = { 0, 0 };
        int reserve[2] = { 0, 0 };
        int maxReserve[2] = { 0, 0 };
        int activeIndex = -1;          // arma il cui caricatore e' in currentAmmo (-1 = nessuna)
        bool infinite = false;         // campo da tiro: la riserva non cala

        // Dotazione tattica (slot 4-6)
        int equipmentUses[3] = { 0, 0, 0 };
        float useTimer = 0.0f;         // >0: rifornimento in corso
        float useDuration = 0.0f;
        int useSlot = -1;              // slot equipaggiamento in uso (4-6)
        entt::entity useTarget = entt::null;   // chi riceve le munizioni (se' stessi o un compagno)
        float feedbackTimer = 0.0f;    // messaggio a schermo dopo l'uso
        std::string feedback;
        glm::vec3 useStartPos{ 0.0f }; // dove e' iniziata la canalizzazione (scrap: bisogna restare fermi)
    };

    // Cassa di munizioni posata a terra (schola progenium): E vicino = un caricatore dell'arma in mano, 'charges' volte
    struct AmmoCrateComponent {
        entt::entity owner = entt::null;
        int team = -1;                 // squadra di chi l'ha posata (-1 = nessuna: solo il padrone)
        int charges = 3;
        int maxCharges = 3;
        float cooldown = 0.0f;         // fra un prelievo e l'altro dei bot
    };

    // Rinforzi chiamati col vox caster: arrivano dopo 'timer' secondi accanto a chi li ha chiamati (li crea GameLayer)
    struct SummonRequestComponent {
        struct Pending { std::string unitId; float timer = 5.0f; };
        std::vector<Pending> pending;
    };
    // Unita' evocata (rinforzo del vox caster): segue il padrone e combatte con lui. Cerchio verde sotto i piedi.
    struct SummonedComponent {
        entt::entity owner = entt::null;
        float side = 1.0f;             // da che lato del padrone si mette (-1 sinistra, +1 destra)
    };

    // Zaino di cibo posato a terra: cura i compagni nel raggio finche' dura
    struct HealZoneComponent {
        entt::entity owner = entt::null;
        int team = -1;                 // squadra del padrone (-1 = nessuna: cura solo il padrone)
        float radius = 5.0f;
        float healPerSecond = 5.0f;
        float timeLeft = 10.0f;
        float duration = 10.0f;
        float tickTimer = 1.0f;        // la cura arriva a impulsi, uno al secondo
    };

    // Barricata/muro costruito (copertura: collider statico). Non ha HealthComponent (cosi' resta un ostacolo per
    // colpi, linea di vista e movimento) ma una salute da struttura: la rompono le esplosioni, la ripara la chiave.
    struct BarricadeTag {};
    struct StructureHealthComponent {
        float hp = 250.0f;
        float maxHp = 250.0f;
        entt::entity owner = entt::null;   // chi l'ha costruita (prende l'esperienza del danno assorbito)
        float xpGiven = 0.0f;              // esperienza gia' data per il danno assorbito (max = maxHp, anche se riparata)
        float xpPending = 0.0f;            // frazioni di punto non ancora date
        float damageReduction = 0.0f;      // armatura della struttura (0.3 = -30% di danno, es. torretta)
    };

    // Danno a una struttura costruita: il padrone guadagna 1 punto per ogni punto di danno assorbito, ma in tutta
    // la vita della struttura non piu' della sua salute massima. Ritorna true se e' distrutta (la distrugge il chiamante).
    inline bool DamageStructure(entt::registry& reg, entt::entity e, float amount) {
        auto* sh = reg.try_get<StructureHealthComponent>(e);
        if (!sh || amount <= 0.0f) return false;
        amount *= 1.0f - std::clamp(sh->damageReduction, 0.0f, 0.95f);
        const float taken = std::min(amount, std::max(0.0f, sh->hp));
        sh->hp -= amount;
        const float xp = std::min(taken, std::max(0.0f, sh->maxHp - sh->xpGiven));
        if (xp > 0.0f) {
            sh->xpGiven += xp;
            sh->xpPending += xp;
            if (sh->xpPending >= 1.0f) {
                const float whole = std::floor(sh->xpPending);
                sh->xpPending -= whole;
                EmitXp(sh->owner, XpCategory::Absorb, static_cast<int64_t>(whole));
            }
        }
        return sh->hp <= 0.0f;
    }

    // Entita' riparabile con un attrezzo (chiave inglese): veicoli, strutture, torrette, semoventi...
    // Tag tipici: "Structure", "Construct", "Mechanical". La salute e' quella dell'HealthComponent.
    struct RepairableComponent {
        std::vector<std::string> tags;
        bool HasAnyTag(const std::vector<std::string>& wanted) const {
            for (const auto& w : wanted) for (const auto& t : tags) if (w == t) return true;
            return false;
        }
    };

    // Probabilita' di schivare un colpo (a distanza o in mischia): il colpo non fa danno e, se a distanza,
    // prosegue oltre. Es. squig 0.30 = 30%.
    struct EvasionComponent {
        float chance = 0.0f;
    };

    // Compagno squig che segue il padrone e attacca quello a cui il padrone spara.
    enum class SquigKind { Biter, Bomb };
    enum class SquigState { Follow, Attack, Charge };
    struct SquigComponent {
        entt::entity owner = entt::null;
        int ownerSlot = 4;                  // slot equipaggiamento del padrone (4-6) da cui viene
        std::string itemId;                 // blueprint dello squig
        SquigKind kind = SquigKind::Biter;
        SquigState state = SquigState::Follow;
        entt::entity target = entt::null;
        float targetTimer = 0.0f;           // per quanto insegue ancora l'ordine ricevuto
        float attackCooldown = 0.0f;
        float prevOwnerFireCooldown = 0.0f; // per accorgersi che il padrone ha sparato
        std::vector<glm::vec3> path;
        float pathTimer = 0.0f;
        float hopPhase = 0.0f;              // saltello mentre corre
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

    // Raggio del mirino (centro dello schermo) pubblicato ogni frame dalla telecamera di gioco. La telecamera
    // sta sopra la spalla destra: i colpi del giocatore convergono sul punto inquadrato dal mirino invece di
    // partire paralleli alla vista dalla canna (che li faceva finire a sinistra del mirino).
    struct AimRayComponent {
        glm::vec3 origin{ 0.0f };
        glm::vec3 dir{ 0.0f, 0.0f, -1.0f };
        float recoilPitch = 0.0f;   // rinculo verticale gia' compreso nella vista quando il raggio e' stato preso
        bool valid = false;
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
            return GetSlotItemId(currentSlot);
        }

        // Oggetto in uno slot: 0 primaria, 1 secondaria, 2 mischia, 3 granata, 4-6 equipaggiamento (tasti 4, 5, 6)
        std::string GetSlotItemId(int slot) const {
            switch (slot) {
            case 0: return primaryWeaponId;
            case 1: return secondaryWeaponId;
            case 2: return meleeWeaponId;
            case 3: return grenadeId;
            case 4: return tool1Id;
            case 5: return tool2Id;
            case 6: return tool3Id;
            default: return "";
            }
        }

        bool HasSlotItem(int slot) const {
            const std::string id = GetSlotItemId(slot);
            return !id.empty() && id != "none" && BlueprintManager::Get().GetWeapon(id) != nullptr;
        }

        // Si puo' prendere in mano? (squig e zaino munizioni sono passivi: occupano lo slot ma non si impugnano)
        bool IsSlotSelectable(int slot) const {
            if (!HasSlotItem(slot)) return false;
            const auto* bp = BlueprintManager::Get().GetWeapon(GetSlotItemId(slot));
            return !(bp->is_equipment && (bp->equipment_type.rfind("squig", 0) == 0 || bp->equipment_type == "ammo_backpack"));
        }

        // Primo slot con qualcosa dentro (es. il gretchin non ha primaria: parte con la Grot Blasta)
        int FirstUsableSlot() const {
            for (int s : { 0, 1, 2, 4, 5, 6 }) if (IsSlotSelectable(s)) return s;
            return 0;
        }

        // In mano c'e' un oggetto della dotazione tattica (es. scatola di munizioni)?
        bool IsHoldingEquipment() const {
            const auto* bp = BlueprintManager::Get().GetWeapon(GetActiveWeaponId());
            return bp && bp->is_equipment;
        }

        // Passiva di velocita' dell'arma da mischia tenuta in mano (es. coltello del gretchin +5%)
        float HeldSpeedMultiplier() const {
            const WeaponBlueprint* held = nullptr;
            if (currentSlot == 2 || GetEquipStance() == EquipStance::HybridRangedMelee)
                held = BlueprintManager::Get().GetWeapon(meleeWeaponId);
            return held ? 1.0f + held->wielder_speed_bonus : 1.0f;
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

            if (activeBP->slot_type == WeaponSlotType::Melee || activeBP->is_melee || activeBP->is_equipment) {
                return EquipStance::MeleePure;   // la dotazione tattica si tiene in una mano, come un'arma bianca
            }

            if (activeBP->hand_type == HandType::OneHanded) {
                return EquipStance::DualRangedOneHanded;
            }

            return EquipStance::RangedTwoHanded;
        }
    };

    // Skin dell'armatura/vesti sul campo (vedi ArmorBlueprint: "skin_blocks" e "skin_parts").
    // blockColors: colore per blocco dello scheletro (alpha 0 = colore dell'unita'); parts: cubi aggiunti,
    // agganciati a un blocco (modalita' cuboni) e a un osso (modello animato del giocatore).
    struct SkinPart {
        int block = -1;
        glm::vec3 offset{ 0.0f };
        std::string bone;
        glm::vec3 boneOffset{ 0.0f };
        glm::vec3 size{ 0.1f };
        glm::vec4 color{ 0.5f, 0.5f, 0.5f, 1.0f };
        float roughness = 0.7f;
        float metallic = 0.0f;
    };
    struct SkinComponent {
        std::vector<glm::vec4> blockColors;
        std::vector<SkinPart> parts;
        std::vector<GarmentDef> garments;   // strati (vestiti/armature) sul modello animato
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

    // Sull'unita' che manovra una torretta: niente movimento, armi, dotazione e mischia finche' resta dentro
    struct TurretMountComponent {
        entt::entity turret = entt::null;
        float exitHold = 0.0f;   // giocatore: secondi di E tenuto per uscire
        float idle = 0.0f;       // bot: secondi senza bersagli
    };

    // Buff di fazione attivo sull'unita' (es. WAAAGH! degli orki): moltiplicatori letti da movimento, armi e mischia
    struct FactionBuffComponent {
        float moveSpeedMult = 1.0f;     // velocita' di movimento
        float reloadSpeedMult = 1.0f;   // velocita' di ricarica (1.4 = ricarica il 40% piu' in fretta)
        float meleeSpeedMult = 1.0f;    // velocita' dei colpi in mischia
        float meleeDamageMult = 1.0f;   // danni in mischia inflitti
    };

    // Accovacciato (CTRL tenuto): piu' basso dietro le coperture, hitbox comprese, ma piu' lento.
    struct CrouchComponent {
        float amount = 0.0f;          // 0 in piedi, 1 accovacciato (transizione morbida)
        float baseRootY = 0.0f;       // altezza di riposo del blocco radice (Torso) dello scheletro
        std::string skeletonId;       // scheletro su cui e' stata presa baseRootY (cambio unita' = si riprende)
        float legAngle = 0.0f;        // apertura delle gambe a piena accovacciata (calcolata per lo scheletro)
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
        std::vector<GarmentDraw> garments;   // vestiti e armature disegnati sopra il corpo, con le stesse ossa
        // Modello vuoto di default: non renderizzato finché non setti i puntatori
    };                               // ← aggiungi ; 

    // ===================== MODALITA' VS BOT (5 contro 5) =====================

    // Squadra di appartenenza. 0 = giocatore e alleati, 1 = avversari. Le unita' della stessa squadra non si
    // colpiscono (ne' a distanza ne' in mischia). Le unita' senza squadra (orda, manichini) restano colpibili da tutti.
    struct TeamComponent {
        int team = 0;
    };

    inline bool SameTeam(const entt::registry& reg, entt::entity a, entt::entity b) {
        const auto* ta = reg.try_get<TeamComponent>(a);
        const auto* tb = reg.try_get<TeamComponent>(b);
        return ta && tb && ta->team == tb->team;
    }

    // Grilletto per le unita' non controllate dal giocatore: lo imposta l'IA, lo legge WeaponSystem al posto
    // del mouse e del tasto R.
    struct WeaponTriggerComponent {
        bool fire = false;
        bool reload = false;
        bool useEquipment = false;              // dotazione in mano: usa su se' stesso (click sinistro)
        bool giveEquipment = false;             // dotazione in mano: dalla a un compagno (click destro)
        entt::entity giveTarget = entt::null;   // compagno scelto dall'IA (null = il piu' vicino)
        bool holdEquipment = false;             // dotazione tenuta premuta (es. costruire la barricata: 3 s fermi)
    };

    // Ruolo di combattimento dell'IA ("ai_role" nel blueprint dell'unita')
    enum class BotRole {
        Ranged,      // tiratore: tiene le distanze, spara ai bersagli facili, si riposiziona se minacciato
        Versatile,   // valuta se puo' vincere il corpo a corpo, altrimenti combatte a distanza
        Melee        // carica sempre per arrivare alle mani, anche prendendo colpi
    };

    enum class BotIntent {
        Advance,     // nessun nemico noto: avanza verso la zona avversaria
        Engage,      // spara da fermo (o quasi) al bersaglio
        Reposition,  // si sposta in una posizione migliore (copertura o distanza giusta)
        Charge,      // corre verso il bersaglio per la mischia
        Melee        // a contatto: colpisce
    };

    struct BotBrainComponent {
        BotRole role = BotRole::Versatile;
        BotIntent intent = BotIntent::Advance;
        std::string displayName;               // nome mostrato sopra l'unita'
        int rosterIndex = -1;                  // alleati presi dal garage del giocatore (-1 = generato)

        // Percezione
        entt::entity target = entt::null;
        glm::vec3 lastKnownTargetPos{ 0.0f };
        bool targetVisible = false;
        float memoryTimer = 0.0f;              // secondi da cui non vede il bersaglio
        float thinkTimer = 0.0f;               // prossima decisione
        float healthLastThink = 0.0f;
        float recentDamage = 0.0f;             // danno ricevuto negli ultimi secondi (decade nel tempo)
        float threat = 0.0f;                   // ultima valutazione della minaccia

        // Movimento
        glm::vec3 moveGoal{ 0.0f };
        bool hasMoveGoal = false;
        std::vector<glm::vec3> path;
        float pathTimer = 0.0f;
        float moveSpeedFactor = 1.0f;
        bool isMoving = false;
        float stuckTimer = 0.0f;
        glm::vec3 lastPos{ 0.0f };
        float advanceSide = 0.0f;              // spostamento laterale preferito quando avanza (-1..1)
        bool searching = false;                // arrivato alla base nemica senza trovare nessuno: perlustra
        float stalemateTimer = 0.0f;           // da quanto ingaggia senza fare danni al bersaglio
        float stalemateTargetHp = -1.0f;
        entt::entity stalemateTarget = entt::null;

        // Fuoco e mischia
        float burstTimer = 0.0f;               // raffica in corso (armi automatiche)
        float pauseTimer = 0.0f;               // pausa fra raffiche / colpi
        bool triggerToggle = false;            // semi-automatiche: alterna premuto/rilasciato
        glm::vec2 aimError{ 0.0f };            // errore di mira corrente (gradi), dipende da accuracy_skill
        float aimErrorTimer = 0.0f;
        float meleeCooldown = 0.0f;
        float parryCooldown = 0.0f;
        bool isAiming = false;

        entt::entity weaponEntity = entt::null; // arma visibile in mano

        // Dotazione tattica (scatola di munizioni): slot in mano mentre rifornisce se' o un compagno (-1 = no)
        int resupplySlot = -1;
        float resupplyCheck = 0.0f;
        float healCheck = 0.0f;                 // prossima valutazione "curare o attaccare"

        // Granate: tempo prima della prossima e lancio in preparazione (granata gia' in mano)
        float grenadeCooldown = 4.0f;
        bool throwingGrenade = false;

        // Mine, barricate di scrap e muri con feritoia: posa in preparazione / costruzione in corso
        float deployCooldown = 6.0f;
        bool deploying = false;
        bool holdStill = false;                 // costruendo: resta fermo (altrimenti la canalizzazione riparte)
        float deployTimer = 0.0f;
        int deployUsesBefore = 0;
        // Dopo il comando "usa" (granata, mina) la dotazione resta in mano ancora un attimo: l'EquipmentSystem la
        // usa al frame successivo e, se l'IA tornasse subito all'arma, non troverebbe piu' niente in mano
        float equipReleaseTimer = 0.0f;
        int equipReleaseSlot = -1;
    };

}                                    // ← aggiungi chiusura namespace