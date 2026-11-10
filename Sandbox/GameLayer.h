#pragma once

#include "Engine/Core/Layer/Layer.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Renderer/Camera.h"
#include "Engine/ECS/SunPhysicsSystem.h"
#include "Engine/ECS/MovementSystem.h"
#include "Engine/ECS/WeightSystem.h"
#include "Engine/ECS/HealthRegenSystem.h"
#include "Engine/Renderer/BodyFit.h"
#include "Engine/Renderer/ExtraBones.h"
#include "Engine/Renderer/RaceMotion.h"
#include "Engine/Renderer/MuscleTone.h"
#include "Engine/Renderer/AuthoredModel.h"
#include "Engine/Renderer/SurfaceMaterials.h"
#include "Engine/Renderer/SquigModel.h"
#include "Engine/ECS/HordeAISystem.h"
#include "Engine/ECS/MeleeCombatSystem.h"
#include "Engine/ECS/BotAISystem.h"
#include "VsBotMatchmaker.h"
#include "Engine/ECS/ParticleSystem.h"
#include "Engine/ECS/CorpseSystem.h"
#include "PlayerProfile.h"
#include "GameFeel.h"
#include "ResearchTree.h"
#include "Engine/ECS/BlockModel.h"
#include "Engine/ECS/RenderSystem.h"
#include "Engine/ECS/WeaponSystem.h"
#include "Engine/ECS/AmmoSystem.h"
#include "Engine/ECS/SquigSystem.h"
#include "Engine/ECS/RepairSystem.h"
#include "Engine/ECS/ActionPose.h"
#include "Engine/ECS/TurretSystem.h"
#include "Engine/Scene/MapLoader.h"
#include "Engine/Navigation/NavMeshQuery.h"
#include "Engine/AssetManager/BlueprintManager.h"

#include "Engine/Renderer/Pipeline/DeferredRenderer.h"
#include "Engine/Renderer/Model.h"
#include "Engine/Renderer/StaticModelShader.h"
#include "Engine/Renderer/SkinnedModel.h"
#include "Engine/Renderer/GarmentLibrary.h"
#include "Engine/Renderer/SkinnedModelShader.h"
#include "Engine/Renderer/RifleAnimationController.h"

#include <glm/glm.hpp>
#include <miniaudio.h>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

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
        Shop,       // negozio (Pietra Nera): per ora WIP
        Showcase    // vetrina: razze affiancate (o un'unita' dall'albero di ricerca) con le loro animazioni
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
            ApplyDefaultArmor();
        }

        // Toglie l'equipaggiamento che l'unita' non puo' piu' usare (es. orki: armi generiche o choppa vecchia),
        // rimettendo al suo posto quello della dotazione base. Ritorna true se ha cambiato qualcosa.
        bool SanitizeLoadout() {
            const auto& bpm = BlueprintManager::Get();
            const auto* bp = bpm.GetUnit(blueprintId);
            if (!bp) return false;
            bool changed = false;
            auto fixWeapon = [&](std::string& slot, const std::string& def) {
                if (slot.empty() || slot == "none") return;
                const auto* w = bpm.GetWeapon(slot);
                if (w && bpm.CanEquip(*w, blueprintId)) return;
                slot = def; changed = true;
            };
            fixWeapon(primaryWeaponId, bp->default_primary);
            fixWeapon(secondaryWeaponId, bp->default_secondary);
            fixWeapon(meleeWeaponId, bp->no_melee ? std::string() : bp->default_melee);
            for (int i = 0; i < 3; ++i) fixWeapon(equipmentSlots[i], bp->default_equipment[i]);
            std::string* armor[6] = { &armorHeadId, &armorTorsoId, &armorLeftArmId, &armorRightArmId, &armorLeftLegId, &armorRightLegId };
            for (int i = 0; i < 6; ++i) {
                std::string& slot = *armor[i];
                if (slot.empty() || slot == "none") continue;
                const auto* a = bpm.GetArmorPiece(slot);
                if (a && bpm.CanEquip(*a, blueprintId)) continue;
                slot = bp->default_armor[i].empty() ? std::string("none") : bp->default_armor[i];
                changed = true;
            }
            return changed;
        }

        // Armatura / vesti base del blueprint (es. gretchin: vesti di stracci, testa scoperta)
        void ApplyDefaultArmor() {
            const auto* bp = BlueprintManager::Get().GetUnit(blueprintId);
            if (!bp || !bp->has_default_armor) return;
            std::string* slots[6] = { &armorHeadId, &armorTorsoId, &armorLeftArmId, &armorRightArmId, &armorLeftLegId, &armorRightLegId };
            for (int i = 0; i < 6; ++i) *slots[i] = bp->default_armor[i].empty() ? std::string("none") : bp->default_armor[i];
        }

        // Punti battaglia (code Vs Bot e limite della squadra): costo dell'unita' + costo di ogni equipaggiamento
        // gia' rivisto, cioe' con "allowed_units" (armi, dotazione, armature: es. tutto il kit di gretchin e yoof).
        // Le armi e armature vecchie (senza "allowed_units") non contano finche' non vengono rifatte, cosi' le
        // unita' non ancora rivedute restano nelle loro code.
        int CalculateBattlePoints() const {
            int points = 0;
            if (const auto* unitBp = BlueprintManager::Get().GetUnit(blueprintId)) points += unitBp->point_cost;
            for (const std::string* wId : { &primaryWeaponId, &secondaryWeaponId, &meleeWeaponId,
                                            &equipmentSlots[0], &equipmentSlots[1], &equipmentSlots[2] }) {
                if (const auto* wBp = BlueprintManager::Get().GetWeapon(*wId); wBp && !wBp->allowed_units.empty()) points += wBp->point_cost;
            }
            for (const std::string* aId : { &armorHeadId, &armorTorsoId, &armorLeftArmId, &armorRightArmId, &armorLeftLegId, &armorRightLegId }) {
                if (const auto* aBp = BlueprintManager::Get().GetArmorPiece(*aId); aBp && !aBp->allowed_units.empty()) points += aBp->pointCost;
            }
            return points;
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

    // 5v5: ogni giocatore (tu o un'IA) porta la sua squadra; quando l'unita' in campo cade rientra con un'altra.
    enum class VsBotSlotState { Reserve, InField, Fallen };
    struct VsBotSlot {
        TrainedUnit unit;
        int rosterIdx = -1;                      // solo per il giocatore: indice nel roster del garage
        int points = 0;                          // punti battaglia (unita' + equipaggiamento)
        VsBotSlotState state = VsBotSlotState::Reserve;
        entt::entity entity = entt::null;        // bot in campo (il giocatore usa m_PlayerEntity)
    };
    struct VsBotPlayer {
        std::string name;
        int team = 0;
        bool human = false;
        std::vector<VsBotSlot> squad;
        int active = -1;                         // slot in campo, -1 = in attesa di rientrare
        float respawnTimer = 0.0f;
        int CountState(VsBotSlotState st) const { int n = 0; for (const auto& s : squad) n += s.state == st ? 1 : 0; return n; }
        int PointsState(VsBotSlotState st) const { int n = 0; for (const auto& s : squad) n += s.state == st ? s.points : 0; return n; }
        int TotalPoints() const { int n = 0; for (const auto& s : squad) n += s.points; return n; }
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
        void RenderGraphicsOptions();   // scheda GRAFICA del menu Opzioni
        void RenderPerfOverlay();       // F3: FPS e tempi di CPU e GPU
        bool m_ShowPerf = false;
        std::string m_GfxAutoNotice;    // primo avvio: messaggio sulla qualita' scelta in automatico
        std::string m_GfxDetectInfo;    // ultimo "Rileva automaticamente" dalle opzioni
        bool m_F3WasDown = false;
        int  m_AppliedVSync = -1;       // ultimo glfwSwapInterval applicato (-1 = da applicare)
        double m_PerfFrameStart = 0.0, m_PerfUpdateStart = 0.0, m_PerfRenderStart = 0.0;
        float m_PerfFrameMs = 0.0f, m_PerfUpdateMs = 0.0f, m_PerfRenderMs = 0.0f;

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
        std::string SquadTypeCapProblem() const;          // "" se la squadra rispetta il massimo per tipo
        TrainedUnit MakeBotUnit(const std::string& unitId, int number) const;
        void AddBotExtras(TrainedUnit& u, int& budget) const;   // oggetti ricercabili a caso, entro il budget della coda
        entt::entity SpawnBot(const TrainedUnit& unit, int team, const glm::vec3& spawnPos, float yawDeg, int rosterIndex);
        void UpdateVsBot(float deltaTime);                 // fine partita (eliminazione o tempo scaduto)
        // Rinforzi del vox caster: arrivano dopo il ritardo, seguono chi li ha chiamati, cerchio verde sotto i piedi
        void UpdateSummons(float deltaTime);
        entt::entity SpawnSummon(entt::entity owner, const std::string& unitId);
        void ClearSummons();
        void RenderSummonRings(bool deferred);
        bool HasSummons();
        void UpdateBotVisuals();                           // arma in mano e posa delle braccia dei bot
        void RenderVsBotHUD();
        void RenderVsBotDeathUI();                         // giocatore caduto: sceglie con quale unita' della squadra rientrare
        void RenderVsBotScoreboard();                      // TAB: giocatori, squadre alleate e caduti avversari
        void VsBotSpawnAi(VsBotPlayer& p, int slot, const glm::vec3& pos);
        void VsBotRespawnHuman(int slot);
        glm::vec3 VsBotSpawnPoint(int team, int i, bool jitter) const;
        void UpdateVsBotWorld(float deltaTime);           // il mondo continua mentre sei a terra / osservi
        void UpdateVsBotSpectator();                       // camera sugli alleati mentre sei a terra
        void AnimateUnits(float deltaTime, const glm::vec3& playerPos, bool pushFromPlayer);
        void RenderVsBotResults();
        void EndVsBotMatch(int result, const char* reason);
        void LeaveVsBotMatch();                            // torna al garage e pulisce la scena
        void RenderAmmoHUD();
        void RenderPlayerStatusHUD();
        // Danni subiti: bordi dello schermo che sanguinano (vignetta rossa, gocce che colano, macchie), battito a vita bassa
        void RenderDamageVignette();
        void RenderVfxTexts();   // scritte della mischia che salgono
        struct BloodSplat { float x = 0.0f, y = 0.0f, radius = 0.0f, life = 0.0f, maxLife = 1.0f; uint32_t seed = 0; bool drip = false; };
        std::vector<BloodSplat> m_BloodSplats;
        float m_DamageFlash = 0.0f;
        float m_LastPlayerHp = -1.0f;
        float m_DamagePulseTime = 0.0f;
        void RenderTurretHUD();
        void BuildPlayerPose(float deltaTime, RifleAnimationController& controller);   // animazioni procedurali                      // in alto a sinistra: salute, stamina, buff di fazione
        void UpdateFactionBuff(float deltaTime);           // WAAAGH! degli orki, Follow Orders della Guardia
        void ResetFactionBuff();
        void ApplyOrderBuff();                             // effetti dell'ordine attivo sul giocatore
        void ScanFactionEnemies();                         // nemici in vista davanti / alle spalle (per i buff)
        void RenderOrdersRadial();
        void SyncCloakVisuals();                           // mimetismo copiato su arma in mano e modello animato
        void RenderAuspex();                               // sonar dell'auspex (scout) quando e' in mano                         // selettore radiale degli ordini (tasto V tenuto)

        // --- Esperienza di partita (punti studio) e riepilogo di fine partita ---
        struct MatchXpLine { int count = 0; int64_t xp = 0; };
        std::map<std::string, std::array<MatchXpLine, static_cast<size_t>(XpCategory::Count)>> m_MatchXp;   // per tipo di unita'
        std::set<uint32_t> m_MatchXpDamaged;     // nemici gia' danneggiati (i danni contano una volta per nemico)
        float m_MatchSurvivalAccum = 0.0f;
        bool m_MatchXpApplied = false;
        std::map<std::string, int64_t> m_MatchPsBefore;   // punti studio prima della partita (per le barre)
        int64_t m_MatchXpTotal = 0;
        int64_t m_MatchEnergy = 0;
        float m_MatchRevealTimer = 0.0f;
        bool m_MatchPlayerDied = false;          // il giocatore e' morto almeno una volta (5v5: ricompensa ridotta)
        int64_t m_MatchRawXp = 0;                // esperienza prima dei moltiplicatori
        float m_MatchOutcomeMult = 1.0f;         // esito della 5v5 (100% / 75% / 50%)
        std::string m_MatchOutcomeLabel;
        bool m_MatchVip = false;
        std::map<std::string, int64_t> m_MatchUnitGain;   // punti studio dati a ogni unita' (dopo i moltiplicatori)
        void ResetMatchXp();
        void CollectMatchXp(float deltaTime, bool countSurvival);
        void ApplyMatchXp(float outcomeMult = 1.0f, const std::string& outcomeLabel = "");
        void RenderMatchRecap();
        void RenderHealZones(bool deferred);       // cerchio di cura dello zaino di cibo
        void ApplyArmorSkin(entt::entity e, const TrainedUnit& unit);   // vesti / elmetto visibili sul campo
        void UpdateAnimatedSkin();                                    // pezzi della skin sul modello animato
        std::vector<entt::entity> m_SkinPartEntities;
        GarmentLibrary m_Garments;   // vestiti/armature sul modello animato (gusci skinnati + texture), in cache
        SquigModel m_SquigModel;     // modello skinnato dello squig (SquigModel.h)
        std::unordered_map<entt::entity, std::unique_ptr<std::vector<glm::mat4>>> m_SquigBones;
        void UpdateSquigVisuals();
        // Pelle della razza sul modello animato
        static const char* BodyLabelOf(int kind);
        void ApplySkinStyle(SkinnedModelInstanceComponent& sk, int kind);
        // Schermata di personalizzazione (anteprima a tutto schermo)
        TrainedUnit m_CustomizeBackup;          // com'era l'unita' quando si e' aperta (ANNULLA)
        bool m_CustomizeHasBackup = false;
        int m_CustomizePreviewAnim = 0;         // 0 fermo, 1 mira, 2 cammina, 3 corre
        bool m_CustomizeAutoRotate = false;
        float m_CustomizeYaw = 200.0f;
        float m_CustomizeZoom = 1.0f;
        bool m_CustomizeCamInit = false;
        glm::vec3 m_CustomizeCamTarget{ 0.0f };
        float m_CustomizeCamDist = 3.0f;
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
        AnimatedBodyKind BodyKindForUnit(const std::string& unitId) const;   // corpo animato di un tipo di unita'

        // Corpi animati di tutte le unita' (bot, orda, manichini): ognuna ha il suo controller (clip condivise col
        // corpo), la sua entita' visiva e i pezzi della skin sulle ossa; i cubi restano come hitbox, nascosti.
        struct UnitAnimInstance {
            std::unique_ptr<RifleAnimationController> ctrl;
            ProceduralPose pose;
            float poseTime = 0.0f, climb = 0.0f;
            entt::entity visual = entt::null;
            AnimatedBodyKind kind = AnimatedBodyKind::Human;
            glm::vec3 lastPos{ 0.0f };
            bool hasLast = false;
            int lastAmmo = -1;
            bool bindPose = false;                 // manichini del campo da tiro: posa in T come le hitbox
            bool poseFresh = false;                // posa ricostruita in questo frame: l'IK delle mani va ancora aggiunta
            std::vector<entt::entity> skinParts;   // pezzi della skin agganciati alle ossa
        };
        std::unordered_map<entt::entity, UnitAnimInstance> m_UnitAnims;
        bool m_FitBodiesFromHuman = true;   // corpi delle altre razze ricavati dal corpo umano base (BodyFit.h)
        // Modo di muoversi della razza (RaceMotion.h): stato per unita' (fase del passo, sguardo, molle)
        struct RaceMotionSlot { RaceMotionState state; glm::vec3 lastPos{ 0.0f }; bool has = false; };
        std::unordered_map<entt::entity, RaceMotionSlot> m_RaceMotion;
        bool m_PlayerAimingNow = false;

        // --- Vetrina (GameLayerShowcase.cpp) ---
        enum class ShowcaseAnim { Idle = 0, Aim, Walk, Run, Fire, MeleeNormal, MeleeLow, MeleeHeavy, Parry, Stunned, Roar, Count };
        struct ShowcaseActor {
            std::string unitId, weaponId, meleeId;
            AnimatedBodyKind kind = AnimatedBodyKind::Human;
            std::unique_ptr<RifleAnimationController> ctrl;
            ProceduralPose pose;
            RaceMotionState motion;
            entt::entity visual = entt::null, weapon = entt::null;
            glm::vec3 pos{ 0.0f };
            float facing = 0.0f, scale = 1.0f, legLen = 0.0f, time = 0.0f, shotTimer = 0.0f;
            int lastMeleePhase = -1;
        };
        std::vector<ShowcaseActor> m_ShowcaseActors;
        std::string m_ShowcaseFocusUnit;            // vuoto = tutte le razze in fila; altrimenti anteprima di un'unita'
        std::string m_ShowcaseBuiltFor = "#";       // per cosa sono stati creati gli attori
        bool m_ShowcaseFromResearch = false;
        bool m_ShowcaseStyle = true;                // stile di movimento della razza acceso/spento (per confrontare)
        bool m_ShowcaseAutoRotate = true;
        bool m_ShowcaseCycle = false;               // tutte le animazioni in sequenza
        int m_ShowcaseAnim = 0;
        float m_ShowcaseCycleTimer = 0.0f, m_ShowcaseYaw = 0.0f, m_ShowcasePitch = -8.0f, m_ShowcaseZoom = 1.0f;
        glm::vec3 m_ShowcaseCenter{ 0.0f };
        float m_ShowcaseExtent = 4.0f;
        void UpdateShowcase(float deltaTime);
        void BuildShowcase();
        void ClearShowcase();
        void RenderShowcaseUI();
        void OpenShowcasePreview(const std::string& unitId);
        void UpdateShowcaseActor(ShowcaseActor& a, float deltaTime);
        static RaceMotionKind RaceMotionOf(AnimatedBodyKind k);
        void ApplyRaceStyle(entt::entity e, ProceduralPose& pose, RifleAnimationController& controller, float legLen, float deltaTime);
        void UpdateUnitAnimations(float deltaTime);
        void ClearUnitAnimations();
        void BuildUnitPose(entt::entity e, ProceduralPose& pose, RifleAnimationController& controller, float poseTime, float& climb, float deltaTime);
        void RenderHitboxOverlay();                        // campo da tiro: hitbox trasparenti sopra il corpo 3D
        bool PlaceBotWeaponOnHand(entt::entity bot, TransformComponent& weaponTransform, const BlockModel* model);
        // IK delle braccia: porta i palmi sulle impugnature dell'arma (punti nel mondo, nullptr = mano libera)
        void ApplyHandIK(RifleAnimationController& ctrl, ProceduralPose& pose, const TransformComponent& visualT,
            const glm::vec3* gripRightWorld, const glm::vec3* gripLeftWorld);
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
        HealthRegenSystem m_HealthRegen;   // vita che torna dopo 7 s fuori dal combattimento
        HordeAISystem m_HordeAI;
        BotAISystem m_BotAI;
        MeleeCombatSystem m_MeleeCombat;
        WeaponSystem m_WeaponSystem;
        EquipmentSystem m_EquipmentSystem;   // scatola di munizioni e altra dotazione tattica
        SquigSystem m_SquigSystem;           // compagni squig dei gretchin
        ExplosiveSystem m_ExplosiveSystem;
        TurretSystem m_TurretSystem;        // armi pesanti piazzabili e manovrate (lascannon)   // granate, mine, lampi delle esplosioni
        std::optional<SkeletonData> m_SquigSkeleton;   // scheletro a cubi dello squig (Skeletons/squig.json)
        bool m_SquigSkeletonLoaded = false;
        RepairSystem m_RepairSystem;         // chiave inglese: ripara veicoli, strutture, torrette
        ParticleSystem m_ParticleSystem;
        GameFeel m_Feel;   // suoni a strati, scossoni, lampi, bossoli, segni dei colpi (vedi GameFeel.h)
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
        ma_sound_group m_VoiceGroup;    // voci, schiamazzi e versi
        bool m_VoiceGroupInitialized = false;
        ma_sound_group m_MusicGroup;    // musica di sottofondo
        bool m_MusicGroupInitialized = false;
        ma_sound m_MusicSound;          // brano in riproduzione (in streaming, a ciclo)
        bool m_MusicLoaded = false;
        std::vector<std::string> m_MusicTracks;   // file in GameData/Audio/Music
        int m_MusicTrackIdx = 0;
        void InitMusic();
        void PlayMusicTrack(int idx);
        void UpdateMusic(float deltaTime);
        void ApplyAudioVolumes();       // porta i volumi delle barre su motore e gruppi

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
        float m_VolumeMaster = 1.0f;          // volume generale (tutto il motore audio)
        float m_MusicDuck = 1.0f;             // musica piu' bassa in partita (sfuma da sola)

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
        std::unordered_map<entt::entity, TrainedUnit> m_VsBotUnits;   // unita' dei bot in campo
        std::vector<VsBotPlayer> m_VsBotPlayers;
        // Animazioni procedurali del modello animato del giocatore
        ProceduralPose m_PlayerPose;
        bool m_PlayerPoseFresh = false;   // posa del giocatore ricostruita in questo frame (IK delle mani da aggiungere)
        float m_PoseTime = 0.0f;
        float m_PoseClimb = 0.0f;                    // peso dell'arrampicata (le altre azioni: ActionPoseComponent)
        // Buff di fazione (orki: WAAAGH!)
        std::string m_FactionBuffFaction;          // fazione dell'unita' con cui si sta caricando
        float m_WaaaghPoints = 0.0f;               // 0..350 (7 caratteri di "Waaagh!" da 50 punti)
        float m_WaaaghTimer = 0.0f;                // >0: WAAAGH! attiva, secondi rimasti
        float m_WaaaghPrevHp = -1.0f;              // per misurare i danni subiti
        static constexpr float kWaaaghMax = 350.0f;
        static constexpr float kWaaaghDuration = 20.0f;
        // Tasto del buff di fazione (V): orki = scatena la WAAAGH! carica; Guardia = selettore radiale degli ordini
        bool m_FactionKeyWasDown = true;
        float m_FactionFightIdle = 0.0f;           // orki: secondi senza combattere (oltre 5 la barra si svuota)
        int m_FactionPrevAmmo = -1, m_FactionPrevSlot = -1;
        bool m_FactionPrevWindup = false;
        bool m_FactionWasAlive = false;
        float m_FactionScanTimer = 0.0f;
        bool m_FactionEnemyFront = false;          // nemico in vista davanti (entro la portata dell'arma)
        bool m_FactionEnemyBehind = false;         // nemico in vista alle spalle (la Guardia perde punti)
        float m_FactionNearestEnemy = 1e9f;
        // Guardia imperiale: "Follow Orders"
        float m_OrdersAssistAccum = 0.0f;          // aiuti ai compagni dall'ultimo frame (munizioni, cure, strutture)
        float m_CohesionDelay = 10.0f;             // dopo ogni schieramento la coesione non carica per 10 s
        int m_OrderActive = -1;                    // 0 Take AIM!, 1 Take Cover, 2 MOVE MOVE MOVE!, 3 Fix Bayonets!
        float m_OrderTimer = 0.0f;
        bool m_RadialOpen = false;
        glm::vec2 m_RadialVec{ 0.0f };
        static constexpr float kOrdersMax = 300.0f;
        static constexpr float kOrderDuration = 10.0f;   // 0 = tu, poi alleati IA, poi avversari IA
        std::vector<glm::vec3> m_VsBotSpawns[2];
        float m_VsBotYaw[2] = { 0.0f, 180.0f };
        float m_VsBotDeadTime = 0.0f;              // secondi da quando la tua unita' e' caduta
        int m_VsBotSpectate = 0;                   // alleato osservato
        static constexpr float kVsBotAiRespawn = 5.0f;      // l'IA rientra dopo 5 s
        static constexpr float kVsBotHumanRespawn = 3.0f;   // tu puoi scegliere dopo 3 s
    };

}