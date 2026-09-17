#pragma once

#include "Components.h"
#include "../Core/MathUtils.h"
#include "../Scene/Scene.h"
#include <algorithm>
#include <cmath>

namespace SunEngine {

   

    class CombatVFX {
    public:
        static void SpawnSlashTrail(Scene& scene, entt::entity attacker, const TransformComponent& trans, const MeleeStateComponent& melee, const MeleeNode* node);
    };

}