#pragma once
#include "Engine/Core/Layer/Layer.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Renderer/Camera.h"
#include "Engine/ECS/SunPhysicsSystem.h"
#include "Engine/ECS/MovementSystem.h"
#include "Engine/ECS/WeightSystem.h"
#include "Engine/ECS/CombatSystem.h"
#include "Engine/ECS/ParticleSystem.h"
#include "Engine/ECS/RenderSystem.h"
#include "Engine/Scene/MapLoader.h"
#include "Engine/Navigation/NavMeshQuery.h"
#include "Engine/AssetManager/BlueprintManager.h"
#include <glm/glm.hpp>
#include <miniaudio.h>
#include <string>
#include <vector>

enum class GameState {
    MainMenu,
    Gameplay,
    MapEditor,
    GameOver
};

enum class MenuTab {
    StasisVault,
    Deployment
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
    std::string secondaryWeaponId = "laspistol_m-g";
    std::string meleeWeaponId = "";
    std::string equipmentSlots[3] = { "", "", "" };

    std::string armorHeadId = "head_guardsman_recruit";
    std::string armorTorsoId = "torso_guardsman_recruit";
    std::string armorLeftArmId = "arm_l_guardsman_recruit";
    std::string armorRightArmId = "arm_r_guardsman_recruit";
    std::string armorLeftLegId = "leg_l_guardsman_recruit";
    std::string armorRightLegId = "leg_r_guardsman_recruit";

    int CalculateTotalPoints() const {
        int points = 0;
        const auto* unitBp = SunEngine::BlueprintManager::Get().GetUnit(blueprintId);
        if (unitBp) points += unitBp->point_cost;

        std::string ids[6] = { armorHeadId, armorTorsoId, armorLeftArmId, armorRightArmId, armorLeftLegId, armorRightLegId };
        for (const auto& aId : ids) {
            const auto* aBp = SunEngine::BlueprintManager::Get().GetArmorPiece(aId);
            if (aBp) points += aBp->pointCost;
        }
        return points;
    }

    float CalculateTotalWeight() const {
        float weight = 0.0f;
        const auto* w1 = SunEngine::BlueprintManager::Get().GetWeapon(primaryWeaponId);
        if (w1) weight += w1->weight_kg;

        const auto* w2 = SunEngine::BlueprintManager::Get().GetWeapon(secondaryWeaponId);
        if (w2) weight += w2->weight_kg;

        std::string ids[6] = { armorHeadId, armorTorsoId, armorLeftArmId, armorRightArmId, armorLeftLegId, armorRightLegId };
        for (const auto& aId : ids) {
            const auto* aBp = SunEngine::BlueprintManager::Get().GetArmorPiece(aId);
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

class GameLayer : public SunEngine::Layer {
public:
    GameLayer();
    virtual ~GameLayer();

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
    void ApplyUnitVisuals(const TrainedUnit& unit);
    int CalculateSquadPoints() const;

private:
    SunEngine::Scene m_Scene;
    SunEngine::Camera m_Camera;
    SunEngine::MapData m_CurrentMapData;
    SunEngine::NavMeshQuery m_NavQuery;

    SunEngine::PhysicsSystem m_PhysicsSystem;
    SunEngine::MovementSystem m_MovementSystem;
    SunEngine::WeightSystem m_WeightSystem;
    SunEngine::CombatSystem m_CombatSystem;
    SunEngine::ParticleSystem m_ParticleSystem;
    SunEngine::RenderSystem m_RenderSystem;

    ma_engine m_AudioEngine;

    entt::entity m_PlayerEntity;
    entt::entity m_WeaponEntity;

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
    const int MAX_SQUAD_POINTS = 150;
    int m_SelectedEventIndex = 0;
    std::string m_SelectedMapId = "warzone_sector4";

    std::vector<ActiveSquadMember> m_ActiveSquad;
    int m_CurrentSquadMemberIdx = 0;

    int m_KillCount = 0;

    bool m_ShowOptionsMenu = false;
    bool m_IsFullscreen = false;
    int m_SelectedResolutionIdx = 0;

    // --- CANALI VOLUMI AUDIO ---
    float m_VolumeSFX = 0.8f;   // Ambienza e Suoni del gioco
    float m_VolumeVoice = 0.8f; // Voci, schiamazzi e versi
    float m_VolumeMusic = 0.8f; // Tracce musicali di sottofondo

    // VARIABILI EDITOR
    EditorMapData m_EditorMap;
    int m_SelectedStructureIdx = -1;
    glm::vec3 m_EditorCameraTarget{ 0.0f, 2.0f, 0.0f };
    float m_EditorCameraPitch = -30.0f;
    float m_EditorCameraYaw = 45.0f;
    float m_EditorCameraDistance = 30.0f;
    std::string m_LastExportPathStatus = "";

    EditorGizmoMode m_GizmoMode = EditorGizmoMode::Translate;
    bool m_IsDraggingGizmo = false;
};