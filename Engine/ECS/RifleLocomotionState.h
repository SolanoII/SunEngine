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
        Firing,
        CrouchIdle,     // opzionali: "crouch idle.fbx" / "crouch walking.fbx" nella cartella delle animazioni
        CrouchWalk
    };
}