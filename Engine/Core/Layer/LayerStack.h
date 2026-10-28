#pragma once

#include "Layer.h"
#include <vector>

namespace SunEngine
{
    class LayerStack
    {
    private:
        std::vector<Layer*> Layers;

    public:
        LayerStack();
        ~LayerStack();

        void PushLayer(Layer* layer);
        // FIX #15: distrugge i layer subito (con il contesto OpenGL ancora valido). Idempotente.
        void Clear();

        std::vector<Layer*>::iterator begin() { return Layers.begin(); }
        std::vector<Layer*>::iterator end() { return Layers.end(); }
    };
}