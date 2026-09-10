#pragma once

#include "Components.h"
#include "../Scene/Scene.h"
#include "../Navigation/NavMeshQuery.h"

namespace SunEngine {

    class HordeAISystem {
    public:
        void Update(Scene& scene, NavMeshQuery& navQuery, float deltaTime);
    };

}