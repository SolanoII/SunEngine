#pragma once

#include "Components.h"
#include "../Scene/Scene.h"
#include "../AssetManager/BlueprintManager.h"

namespace SunEngine {

    class MeleeCombatSystem {
    public:
        void Update(Scene& scene, float deltaTime);

    private:
        void CheckMeleeHits(Scene& scene, entt::entity attacker, UnitStatsComponent& atkStats, MeleeStateComponent& atkMelee, TransformComponent& atkTrans);
    };

}