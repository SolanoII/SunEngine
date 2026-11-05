#pragma once
// Parti in piu' di un modello skinnato, disegnate con le stesse ossa ma con un materiale loro
// (es. denti e occhi dello squig, vedi SquigModel.h).
#include "GarmentShell.h"
#include <vector>

namespace SunEngine {
    struct SkinnedPartSet {
        std::vector<GarmentDraw> parts;
    };
}
