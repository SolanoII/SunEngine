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

        std::vector<Layer*>::iterator begin() { return Layers.begin(); }
        std::vector<Layer*>::iterator end() { return Layers.end(); }
    };
}