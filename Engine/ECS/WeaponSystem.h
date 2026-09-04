#pragma once

#include "Components.h"
#include "../Scene/Scene.h"
#include "../Core/Input.h"
#include "../Core/Logger/Log.h"
#include "../AssetManager/BlueprintManager.h"
#include <miniaudio.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

#define GLFW_KEY_R 82

namespace SunEngine {

    class PhysicsSystem;

    class WeaponSystem {
    public:
        WeaponSystem() = default;

        void Update(Scene& scene, PhysicsSystem& physicsSystem, ma_engine* audioEngine, float deltaTime);

    private:
        void FireWeapon(Scene& scene, entt::entity shooter, WeaponStateComponent& wState, CameraRecoilComponent& recoil, const WeaponBlueprint& wpBP, TransformComponent& trans, UnitStatsComponent& stats, ma_engine* audioEngine);

        void FireContinuousBeam(Scene& scene, entt::entity shooter, WeaponStateComponent& wState, CameraRecoilComponent& recoil, const WeaponBlueprint& wpBP, TransformComponent& trans, UnitStatsComponent& stats, ma_engine* audioEngine, float deltaTime);

        void FireFlamer(Scene& scene, entt::entity shooter, const WeaponBlueprint& wpBP, const glm::vec3& origin, const glm::vec3& dir);

        bool RaycastHitbox(Scene& scene, const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDist, entt::entity shooter, entt::entity& outHitEntity, HitboxLocation& outLocation, float& outHitDist);

        float CalculateDamageWithFalloff(float baseDamage, float distance, float falloffStart, float falloffEnd);
    };

}