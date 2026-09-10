// Engine/ECS/RifleLocomotionState.h (nuovo)
#pragma once

namespace SunEngine {

    enum class RifleAnimState {
        Idle,
        AimIdle,
        WalkStart,
        Walk,
        WalkStop,
        WalkBackStart,
        WalkBack,
        WalkBackStop,
        StrafeLeft,
        StrafeRight,
        Sprint,
        JumpForward,
        JumpBackward,
        Firing
    };
}