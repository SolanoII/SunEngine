#pragma once

#include "Components.h"
#include "../Scene/Scene.h"
#include <algorithm>
#include <cmath>

namespace SunEngine {

    inline float LerpAngle(float a, float b, float t) {
        float diff = std::fmod(b - a + 180.0f, 360.0f) - 180.0f;
        if (diff < -180.0f) diff += 360.0f;
        return a + diff * std::clamp(t, 0.0f, 1.0f);
    }

    class CombatVFX {
    public:
        static void SpawnSlashTrail(Scene& scene, entt::entity attacker, const TransformComponent& trans, const MeleeStateComponent& melee, const MeleeNode* node);
    };

}