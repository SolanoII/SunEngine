#pragma once

#include "Components.h"
#include "VfxEvents.h"
#include "../Scene/Scene.h"
#include "../Core/Input.h"
#include "../Core/Logger/Log.h"
#include "../AssetManager/BlueprintManager.h"
#include <miniaudio.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>

namespace SunEngine {

    class PhysicsSystem;

    class WeaponSystem {
    public:
        // Aspetto del colpo di un'arma (lampo, proiettile in volo, impatto): vedi VfxEvents.h
        static WeaponFx FxFor(const WeaponBlueprint& w, bool underbarrelShotgun = false);
        WeaponSystem() = default;
        ~WeaponSystem();

        // Inizializza il pool audio. Chiamare UNA VOLTA dal GameLayer dopo ma_engine_init.
        // audioDir è il path assoluto (da FindValidDir) alla cartella Audio.
        // group può essere nullptr: in quel caso il volume SFX globale non ha effetto.
        void InitAudio(ma_engine* engine, ma_sound_group* group, const std::string& audioDir);

        // Libera i sound del pool. Chiamare PRIMA di ma_sound_group_uninit / ma_engine_uninit.
        void ShutdownAudio();

        void Update(Scene& scene, PhysicsSystem& physicsSystem, float deltaTime);
        // Fine o inizio partita: via i colpi ancora in volo (puntano a entita' che non esistono piu')
        void ClearPendingShots() { m_PendingShots.clear(); }

        // --- Raycast ---
        struct RaycastResult {
            bool hit = false;
            bool hitUnit = false;
            entt::entity entity = entt::null;
            HitboxLocation location = HitboxLocation::Torso;
            float distance = 0.0f;
            glm::vec3 point{ 0.0f };
        };

        // ignore: un'altra entity da attraversare (es. la torretta da cui si spara)
        RaycastResult RaycastAll(Scene& scene, const glm::vec3& rayOrigin, const glm::vec3& rayDir,
            float maxDist, entt::entity shooter, entt::entity ignore = entt::null);

        // Colpo singolo di un'arma pesante (es. lascannon dalla torretta): raggio dalla bocca, danno pieno senza
        // calo con la distanza, danno x structure_damage_multiplier alle strutture, scia del raggio.
        void FireHeavyBeam(Scene& scene, entt::entity shooter, entt::entity ignore, const glm::vec3& origin,
            const glm::vec3& dir, const WeaponBlueprint& bp);

    private:
        // --- Pool di suoni per path (fire-and-forget con volume/posizione individuali) ---
        struct WeaponSoundInstance {
            ma_sound sound{};
            bool loaded = false;
        };

        ma_engine* m_Engine = nullptr;
        ma_sound_group* m_SFXGroup = nullptr;
        bool m_AudioInitialized = false;
        std::string m_AudioDir;

        std::unordered_map<std::string, std::vector<std::unique_ptr<WeaponSoundInstance>>> m_SoundPools;

        // Ritorna un'istanza libera dal pool per quel path, creando il pool se necessario.
        WeaponSoundInstance* GetFreeInstance(const std::string& absolutePath);

        // Riproduce lo sparo con attenuazione spaziale simulata sul volume.
        void PlayWeaponSound(const glm::vec3& sourcePos, const glm::vec3& listenerPos, float baseVolume = 1.0f);

        // Colpo deciso allo sparo ma che deve ancora arrivare (il proiettile visibile e' in volo)
        struct PendingShot {
            entt::entity shooter = entt::null, target = entt::null;
            int team = 0;
            bool hit = false, hitUnit = false, parried = false, underbarrelShotgun = false, graze = false, meltaShot = false;
            HitboxLocation location = HitboxLocation::Torso;
            float distance = 0.0f, timer = 0.0f;
            glm::vec3 point{ 0.0f }, hitPoint{ 0.0f }, dir{ 0.0f, 0.0f, -1.0f };
            glm::vec4 color{ 1.0f };
            WeaponFx fx = WeaponFx::Ballistic;
            ProjectileType type = ProjectileType::Ballistic;
            int pellets = 1, armorPenetration = 0;
            float baseDamage = 0.0f, falloffStart = 0.0f, falloffEnd = 0.0f, meltaRange = 0.0f, meltaMultiplier = 1.0f;
            float explosionRadius = 0.0f, explosionDamage = 0.0f, structureMultiplier = 1.0f;
        };
        std::vector<PendingShot> m_PendingShots;
        void ResolveShot(Scene& scene, const PendingShot& shot);
        void UpdatePendingShots(Scene& scene, float deltaTime);

        // --- Sistemi interni ---
        void FireWeapon(Scene& scene, entt::entity shooter,
            WeaponStateComponent& wState, CameraRecoilComponent& recoil,
            const WeaponBlueprint& wpBP, TransformComponent& trans,
            UnitStatsComponent& stats);

        void FireContinuousBeam(Scene& scene, entt::entity shooter,
            WeaponStateComponent& wState, CameraRecoilComponent& recoil,
            const WeaponBlueprint& wpBP, TransformComponent& trans,
            UnitStatsComponent& stats, float deltaTime);

        void FireFlamer(Scene& scene, entt::entity shooter, const WeaponBlueprint& wpBP,
            const glm::vec3& origin, const glm::vec3& dir);


        // Direzione di tiro dalla bocca verso il punto inquadrato dal mirino (AimRayComponent). Senza raggio
        // del mirino (nemici) ritorna baseDir.
        glm::vec3 AimAtCrosshair(Scene& scene, entt::entity shooter, const glm::vec3& muzzle,
            const glm::vec3& baseDir, float range, float currentRecoilPitch);

        bool RaycastHitbox(Scene& scene, const glm::vec3& rayOrigin, const glm::vec3& rayDir,
            float maxDist, entt::entity shooter, entt::entity& outHitEntity,
            HitboxLocation& outLocation, float& outHitDist);

        float CalculateDamageWithFalloff(float baseDamage, float distance,
            float falloffStart, float falloffEnd);
    };

}