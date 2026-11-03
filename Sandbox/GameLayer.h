#pragma once

#include "Engine/Core/Layer/Layer.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Renderer/Camera.h"
#include "Engine/ECS/SunPhysicsSystem.h"
#include "Engine/ECS/MovementSystem.h"
#include "Engine/ECS/WeightSystem.h"
#include "Engine/ECS/HordeAISystem.h"
#include "Engine/ECS/MeleeCombatSystem.h"
#include "Engine/ECS/BotAISystem.h"
#include "VsBotMatchmaker.h"
#include "Engine/ECS/ParticleSystem.h"
#include "Engine/ECS/CorpseSystem.h"
#include "PlayerProfile.h"
#include "ResearchTree.h"
#include "Engine/ECS/BlockModel.h"
#include "Engine/ECS/RenderSystem.h"
#include "Engine/ECS/WeaponSystem.h"
#include "Engine/ECS/AmmoSystem.h"
#include "Engine/ECS/SquigSystem.h"
#include "Engine/ECS/RepairSystem.h"
#include "Engine/Scene/MapLoader.h"
#include "Engine/Navigation/NavMeshQuery.h"
#include "Engine/AssetManager/BlueprintManager.h"

#include "Engine/Renderer/Pipeline/DeferredRenderer.h"
#include "Engine/Renderer/Model.h"
#include "Engine/Renderer/StaticModelShader.h"
#include "Engine/Renderer/SkinnedModel.h"
#include "Engine/Renderer/SkinnedModelShader.h"
#include "Engine/Renderer/RifleAnimationController.h"

#include <glm/glm.hpp>
#include <miniaudio.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace SunEngine {

    enum class GameState {
        MainMenu,
        Gameplay,
        MapEditor,
        GameOver
    };

    enum class MenuTab {
        StasisVault,
        Deployment,
        Research,   // albero di ricerca delle unita'
        Shop        // negozio (Pietra Nera): per ora WIP
    };

    enum class EditorGizmoMode {
        Translate,
        Rotate,
        Scale
    };

    struct TrainedUnit {
        std::string customName;
        std::string blueprintId;
        glm::vec4 armorColor{ 1.0f };
        int facePreset = 0;

        std::string primaryWeaponId = "lasgun_standard";
        std::string secondaryWeaponId = "laspistol_standard";
        std::string meleeWeaponId = "";
        std::string equipmentSlots[3] = { "", "", "" };
        std::vector<std::string> ownedItems;   // oggetti ricercabili comprati per questa unita' (chiave inglese, trombone...)
        bool OwnsItem(const std::string& id) const {
            for (const auto& o : ownedItems) if (o == id) return true;
            return false;
        }

        std::string armorHeadId = "head_guardsman_recruit";
        std::string armorTorsoId = "torso_guardsman_recruit";
        std::string armorLeftArmId = "arm_l_guardsman_recruit";
        std::string armorRightArmId = "arm_r_guardsman_recruit";
        std::string armorLeftLegId = "leg_l_guardsman_recruit";
        std::string armorRightLegId = "leg_r_guardsman_recruit";

        int exp = 0;   // esperienza accumulata da questa unita' (1 per ogni orco ucciso)

        // Dotazione base del blueprint ("default_loadout"): armi ed equipaggiamento con cui arriva l'unita'.
        // Le unita' senza dotazione base (da definire) tengono le armi di sempre.
        void ApplyDefaultLoadout() {
            const auto* bp = BlueprintManager::Get().GetUnit(blueprintId);
            if (!bp || !bp->has_default_loadout) return;
            primaryWeaponId = bp->default_primary;
            secondaryWeaponId = bp->default_secondary;
            meleeWeaponId = bp->default_melee;
            for (int i = 0; i < 3; ++i) equipmentSlots[i] = bp->default_equipment[i];
        }

        // Costo in punti squadra: unita' + armi (primaria, secondaria, mischia) + dotazione + pezzi di armatura
        int CalculateTotalPoints() const {
            int points = 0;
            const auto* unitBp = BlueprintManager::Get().GetUnit(blueprintId);
            if (unitBp) points += unitBp->point_cost;
            for (const std::string* wId : { &primaryWeaponId, &secondaryWeaponId, &meleeWeaponId,
                                            &equipmentSlots[0], &equipmentSlots[1], &equipmentSlots[2] }) {
                if (const auto* wBp = BlueprintManager::Get().GetWeapon(*wId)) points += wBp->point_cost;
            }

            std::string ids[6] = { armorHeadId, armorTorsoId, armorLeftArmId, armorRightArmId, armorLeftLegId, armorRightLegId };
            for (const auto& aId : ids) {
                const auto* aBp = BlueprintManager::Get().GetArmorPiece(aId);
                if (aBp) points += aBp->pointCost;
            }
            return points;
        }

        float CalculateTotalWeight() const {
            float weight = 0.0f;
            const auto* w1 = BlueprintManager::Get().GetWeapon(primaryWeaponId);
            if (w1) weight += w1->weight_kg;

            const auto* w2 = BlueprintManager::Get().GetWeapon(secondaryWeaponId);
            if (w2) weight += w2->weight_kg;

            const auto* w3 = BlueprintManager::Get().GetWeapon(meleeWeaponId);
            if (w3) weight += w3->weight_kg;

            for (const auto& eId : equipmentSlots)
                if (const auto* eq = BlueprintManager::Get().GetWeapon(eId)) weight += eq->weight_kg;

            std::string ids[6] = { armorHeadId, armorTorsoId, armorLeftArmId, armorRightArmId, armorLeftLegId, armorRightLegId };
            for (const auto& aId : ids) {
                const auto* aBp = BlueprintManager::Get().GetArmorPiece(aId);
                if (aBp) weight += aBp->weight;
            }
            return weight;
        }
    };

    struct ActiveSquadMember {
        int rosterIndex = -1;
        bool isAlive = true;
        float currentHealth = 100.0f;
    };

    struct EditorStructure {
        std::string type = "block";
        std::string shape = "cube";

        glm::vec3 position{ 0.0f };
        glm::vec3 rotation{ 0.0f };
        glm::vec3 scale{ 2.0f, 2.0f, 2.0f };

        float width = 12.0f;
        float depth = 12.0f;
        float floorHeight = 4.0f;
        float wallThickness = 0.4f;
        float doorWidth = 2.5f;
        float doorHeight = 3.0f;

        glm::vec4 color{ 0.5f, 0.5f, 0.5f, 1.0f };
    };

    struct EditorMapData {
        std::string id = "custom_map_01";
        std::string name = "Nuova Mappa Custom";
        glm::vec3 terrainSize{ 150.0f, 1.0f, 150.0f };
        glm::vec4 terrainColor{ 0.22f, 0.22f, 0.25f, 1.0f };
        std::vector<glm::vec3> playerSpawns;
        std::vector<glm::vec3> enemySpawns;
        std::vector<EditorStructure> structures;
    };

    class GameLayer : public Layer {
    public:
        GameLayer();
        virtual ~GameLayer();

        virtual void OnAttach() override;
        virtual void OnDetach() override;
        virtual void OnUpdate(float deltaTime) override;
        virtual void OnRender() override;
        virtual void OnImGuiRender() override;

    private:
        void RenderTopNavBar();
        void RenderStasisVaultUI();
        void RenderDeploymentUI();
        void RenderRecruitModal();
        void RenderCustomizeModal();
        void RenderRespawnUI();
        void RenderOptionsMenu();

        void RenderMapEditorUI();
        void CreateNewMap();
        void SaveMapToJSON();
        void ExportMapToExternal();
        void LoadMapIntoPreview();
        void SyncEditorEntitiesWithScene();
        void HandleEditor3DInteraction(float deltaTime);
        void RenderEditorGizmos3D();

        void ApplyDisplaySettings(int width, int height, bool fullscreen);
        void UpdateWindowSize();               // ← AGGIUNGI QUESTA DICHIARAZIONE
        void OnWindowResize(int width, int height) override;   // FIX #6
        void ApplyUnitVisuals(const TrainedUnit& unit);
        int CalculateSquadPoints() const;
        void CheckUnitDeaths();
        void UpdateHitFeedback(float deltaTime);

        // --- Modalita' Vs Bot: 5 contro 5 contro l'IA, tre code a punteggio ---
        void RenderVsBotQueues();                          // i 3 bottoni delle code con l'idoneita' della squadra
        bool StartVsBotMatch(int tierIdx);
        TrainedUnit MakeBotUnit(const std::string& unitId, int number) const;
        entt::entity SpawnBot(const TrainedUnit& unit, int team, const glm::vec3& spawnPos, float yawDeg, int rosterIndex);
        void UpdateVsBot(float deltaTime);                 // fine partita (eliminazione o tempo scaduto)
        void UpdateBotVisuals();                           // arma in mano e posa delle braccia dei bot
        void RenderVsBotHUD();
        void RenderVsBotDeathUI();                         // giocatore caduto: prende il controllo di un alleato
        void RenderVsBotResults();
        void TakeOverBot(entt::entity bot);
        void EndVsBotMatch(int result, const char* reason);
        void LeaveVsBotMatch();                            // torna al garage e pulisce la scena
        void RenderAmmoHUD();
        bool DrawItemOption(TrainedUnit& unit, std::string& slotRef, const std::string& wId, const WeaponBlueprint& wBP);                      // caricatore / riserva in basso a destra, dotazione in mano
        void RenderParryRings(bool deferred);      // anello giallo sotto i piedi durante la parata (Q)   // invecchia marcatori di colpo e flash dei blocchi
        void RenderHitMarkers();                   // cerchi sul punto colpito (HUD)

        // --- Campo da tiro (Testing Lobby): manichini in T-pose con tutti gli scheletri ---
        void SpawnTrainingDummies();               // dai "training_dummies" della mappa caricata
        void UpdateTrainingDummies(float deltaTime);
        void RenderTrainingDummyLabels();          // nome, salute e danno sopra ogni manichino

        // --- Profilo locale (impostazioni, valute, exp, roster) ---
        void LoadProfile();                    // legge il file e applica le impostazioni
        void LoadRosterFromProfile();          // sostituisce il roster di default se il profilo ne ha uno
        void SaveProfileIfChanged();           // scrive su disco solo se qualcosa e' cambiato
        void AwardKill();                      // +1 kill, +1 energia, +1 exp unita' e arma
        std::string WeaponLabel(const std::string& name, const std::string& weaponId) const;
        void SyncWeaponAttachment();    // FIX #8/#10: arma del player agganciata allo scheletro

        // --- Ricerca unita' (punti studio -> sblocco, energia -> acquisto) e negozio ---
        void LoadResearchTree();
        void BuildStarterRoster();   // profilo nuovo: un'unita' iniziale per ramo dell'albero di ricerca
        void RenderResearchUI();
        void DrawResearchFactionTree(const ResearchFaction& faction);
        void RenderShopUI();
        bool PurchaseResearchedUnit(const ResearchNode& node);

        // --- Personaggio animato (selettore cuboni / FBX) ---
        RifleAnimInput BuildPlayerAnimInput(bool isAiming, float deltaTime);
        void UpdateAnimatedPlayer(float deltaTime, const RifleAnimInput& input);
        bool IsAnimatedPlayerActive();
        // Corpo animato da usare per l'unita' del giocatore (umano, ork, ogryn) e relativi accessori
        enum class AnimatedBodyKind { Human = 0, Ork, Ogryn, Neophyte, Astartes, Gretchin, Count };
        AnimatedBodyKind CurrentAnimatedBodyKind();
        bool AnimatedBodyReady(AnimatedBodyKind kind) const;
        SkinnedModel& AnimatedBodyModel(AnimatedBodyKind kind);
        RifleAnimationController& AnimatedBodyController(AnimatedBodyKind kind);
        RifleAnimationController& ActiveAnimController();   // controller del modello animato in uso
        bool PlaceWeaponOnAnimatedHand(bool leftHand, TransformComponent& weaponTransform, const BlockModel* model = nullptr);

        // --- Modelli a blocchi delle armi (GameData/BlockModels/<id arma>.json) ---
        void LoadBlockModels();
        void SetWeaponBlockModel(entt::entity weaponEntity, const std::string& weaponId);
        void ClearPlayerDeathState();   // FIX #4: toglie ragdoll + CorpseComponent dal player

    private:
        Scene m_Scene;
        Camera m_Camera;
        MapData m_CurrentMapData;
        NavMeshQuery m_NavQuery;
        bool ReloadMap(const std::string& mapId);
        PhysicsSystem m_PhysicsSystem;
        MovementSystem m_MovementSystem;
        WeightSystem m_WeightSystem;
        HordeAISystem m_HordeAI;
        BotAISystem m_BotAI;
        MeleeCombatSystem m_MeleeCombat;
        WeaponSystem m_WeaponSystem;
        EquipmentSystem m_EquipmentSystem;   // scatola di munizioni e altra dotazione tattica
        SquigSystem m_SquigSystem;           // compagni squig dei gretchin
        RepairSystem m_RepairSystem;         // chiave inglese: ripara veicoli, strutture, torrette
        ParticleSystem m_ParticleSystem;
        RenderSystem m_RenderSystem;

        Model m_TestModel;
        StaticModelShader m_StaticShader;
        SkinnedModel m_SkinnedTestModel;
        entt::entity m_TestSkinnedEntity = entt::null;
        SkinnedModelShader m_SkinnedShader;
        bool m_SkinnedModelLoaded = false;
        bool m_TestModelLoaded = false;
        RifleAnimationController m_RifleAnimController;
        bool m_RifleAnimLoaded = false;

        // Corpi animati alternativi al guardsman, con lo stesso rig Mixamo (52 ossa, stessi nomi):
        //   orchi    -> Characters/ork_master.fbx      + Animations/Orks/
        //   ogryn    -> Characters/ogryn_master.fbx    + Animations/Ogryn/
        //   neofiti  -> Characters/neophyte_master.fbx + Animations/Neophyte/  (neofita, scout)
        //   astartes -> Characters/astartes_master.fbx + Animations/Astartes/  (battle brother, phobos, gravis)
        //   gretchin -> Characters/gretchin_master.fbx + Animations/Gretchin/
        // Se manca la cartella delle animazioni si usano quelle Human, adattate dal retarget.
        struct AnimatedBody {
            SkinnedModel model;
            RifleAnimationController controller;
            bool modelLoaded = false;
            bool animLoaded = false;
        };
        AnimatedBody m_AnimatedBodies[static_cast<int>(AnimatedBodyKind::Count)];   // [Human] inutilizzato
        AnimatedBody& Body(AnimatedBodyKind kind) { return m_AnimatedBodies[static_cast<int>(kind)]; }
        const AnimatedBody& Body(AnimatedBodyKind kind) const { return m_AnimatedBodies[static_cast<int>(kind)]; }
        AnimatedBodyKind m_AnimatedBodyInUse = AnimatedBodyKind::Human;   // modello assegnato ora all'entity animata
        bool m_AnimatedBodyAssigned = false;
        void LoadAnimatedBody(AnimatedBody& body, const std::string& modelsDir, const char* fbxName,
                              const char* animFolder, const char* label);

        ma_engine m_AudioEngine;
        ma_sound_group m_SFXGroup;      // o "m_FootstepGroup"
        bool m_SFXGroupInitialized = false;
        bool m_AudioEngineInitialized = false;   // ← AGGIUNGI QUESTA RIGA

        entt::entity m_PlayerEntity = entt::null;
        entt::entity m_WeaponEntity = entt::null;
        entt::entity m_LeftWeaponEntity = entt::null;   // FIX #6: era static in SyncWeaponAttachment

        bool m_IsPaused = false;
        GameState m_GameState = GameState::MainMenu;
        MenuTab m_CurrentTab = MenuTab::StasisVault;

        float m_GarageCameraAngle = 0.0f;
        float m_GarageCameraDistance = 3.2f;

        std::vector<TrainedUnit> m_PlayerRoster;
        const int MAX_ROSTER_SIZE = 10;
        int m_SelectedVaultIndex = 0;

        bool m_ShowRecruitModal = false;
        bool m_ShowCustomizeModal = false;
        int m_CustomizeMainTab = 0;
        int m_EquipSubTab = 0;
        int m_WeaponCategoryTab = 0;
        int m_GearCategoryTab = 0;
        int m_RecruitStep = 0;
        std::string m_SelectedFaction = "";

        std::vector<int> m_SelectedSquadIndices;
        // Punti battaglia della squadra = somma dei costi delle UNITA' (l'equipaggiamento non conta, come il tier
        // dei carri in WoT). Massimo 5 unita' e 300 punti, cioe' il cap della coda piu' alta della modalita' Vs Bot.
        const int MAX_SQUAD_POINTS = 300;
        const int MAX_SQUAD_SIZE = 5;
        int m_SelectedEventIndex = 0;
        std::string m_SelectedMapId = "warzone_sector4";

        std::vector<ActiveSquadMember> m_ActiveSquad;
        int m_CurrentSquadMemberIdx = 0;
        bool m_PlayerDeathHandled = false;
        int m_KillCount = 0;

        // Personaggio animato: m_UseAnimatedPlayer e' l'impostazione scelta dal giocatore (salvata nel profilo)
        bool m_UseAnimatedPlayer = false;
        entt::entity m_AnimatedPlayerEntity = entt::null;   // entity solo-visiva col modello skinnato
        float m_AnimatedYawOffset = 180.0f;                 // il modello Mixamo guarda +Z, il motore -Z
        float m_AnimatedModelScale = 0.01f;                 // il modello e' in centimetri
        bool m_F2WasDown = false;
        float m_PrevFireCooldown = 0.0f;                 // per riconoscere il momento in cui parte un colpo
        glm::vec3 m_PrevAnimPlayerPos = glm::vec3(0.0f);
        bool m_HasPrevAnimPlayerPos = false;
        float m_SmoothedGroundSpeed = 0.0f;              // m/s, per adattare la velocita' delle animazioni
        // Aggancio armi (cubi) alle mani del modello animato: tarabili in gioco dalle Opzioni
        glm::vec3 m_HandWeaponOffset = glm::vec3(0.0f, 0.0f, 0.20f);   // (destra, su, avanti) in metri
        glm::vec3 m_HandWeaponRotation = glm::vec3(0.0f);              // correzione in gradi
        float m_WeaponPitchFactor = 1.0f;
        BlockModelLibrary m_BlockModels;
        float m_HitMarkerLife = 0.6f;          // durata del cerchio di colpo (s)
        float m_HitMarkerWorldRadius = 0.12f;  // raggio del cerchio sul corpo (m)
        float m_WeaponHandAlign = 0.0f;   // 0 = arma dritta lungo la mira, 1 = segue la linea tra le mani
        glm::vec3 m_WeaponModelGripOffset = glm::vec3(0.0f);   // spostamento dell'arma (spazio arma: dx, su, indietro)
        glm::vec3 m_WeaponModelRotation = glm::vec3(0.0f);     // correzione di rotazione (gradi)
        // Arma degli orchi (modello a blocchi): punto di presa sul blocco del braccio (in unita' del blocco,
        // come il vecchio cubo) e rotazione che porta l'asse della lama (-Z del modello) lungo il braccio.
        glm::vec3 m_OrkWeaponHoldOffset = glm::vec3(0.0f, 0.05f, 0.0f);       // metri, dal fondo del braccio (= la mano)
        glm::vec3 m_OrkWeaponModelRotation = glm::vec3(-30.0f, 0.0f, 0.0f);   // gradi: 0 = lama in avanti dal pugno
        bool m_WeaponFollowsHand = false;

        PlayerProfile m_Profile;
        ResearchTree m_ResearchTree;
        std::string m_ResearchSelectedNode;      // id del nodo selezionato nell'albero
        std::string m_ResearchMessage;           // esito dell'ultima azione (ricerca/acquisto)
        float m_ResearchMessageTimer = 0.0f;
        std::string m_LastSavedProfile;
        float m_ProfileSaveTimer = 0.0f;

        bool m_ShowOptionsMenu = false;
        bool m_IsFullscreen = false;
        int m_SelectedResolutionIdx = 0;
        // Dimensioni correnti della finestra (in pixel framebuffer).
// Aggiornate ad ogni frame da UpdateWindowSize().
        float m_WindowWidth = 1280.0f;
        float m_WindowHeight = 720.0f;
        float m_VolumeSFX = 0.8f;
        float m_VolumeVoice = 0.8f;
        float m_VolumeMusic = 0.8f;

        EditorMapData m_EditorMap;
        int m_SelectedStructureIdx = -1;
        glm::vec3 m_EditorCameraTarget{ 0.0f, 2.0f, 0.0f };
        float m_EditorCameraPitch = -30.0f;
        float m_EditorCameraYaw = 45.0f;
        float m_EditorCameraDistance = 30.0f;
        std::string m_LastExportPathStatus = "";

        EditorGizmoMode m_GizmoMode = EditorGizmoMode::Translate;
        bool m_IsDraggingGizmo = false;

        DeferredRenderer m_DeferredRenderer;

        // Stato della partita Vs Bot
        bool m_VsBotActive = false;
        int m_VsBotTier = 0;
        float m_VsBotTime = 0.0f;
        const float m_VsBotTimeLimit = 600.0f;     // 10 minuti
        int m_VsBotResult = 0;                     // 0 in corso, 1 vittoria, 2 sconfitta, 3 pareggio
        std::string m_VsBotEndReason;
        int m_VsBotReward = 0;
        int m_VsBotEnemyKills = 0;
        std::string m_VsBotMessage;                // esito dell'ultimo tentativo di avvio (es. squadra nemica impossibile)
        std::unordered_map<entt::entity, TrainedUnit> m_VsBotUnits;   // unita' dei bot (per prendere il controllo degli alleati)
    };

}