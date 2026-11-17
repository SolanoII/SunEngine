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
        CrouchWalk,
        // Azioni (opzionali, dall'animset.json della cartella): se la clip c'e' sostituisce la posa procedurale
        MeleeLight,     // colpo rapido
        MeleeNormal,    // colpo normale
        MeleeHeavy,     // colpo pesante (o carica)
        Parry,          // parata
        Stunned,        // stordito (in loop)
        Roar,           // urlo di guerra
        Charge          // corsa a quattro zampe del Feral Pig (in loop)
    };
}