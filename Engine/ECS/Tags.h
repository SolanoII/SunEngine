#pragma once

namespace SunEngine {
    struct PlayerTag {};
    // Nasconde il disegno dei cubi procedurali (SkeletonComponent) di un'entity, lasciandoli come
// hitbox invisibili. Usato quando il giocatore e' mostrato col modello animato FBX.
    struct HideProceduralVisualTag {};
}