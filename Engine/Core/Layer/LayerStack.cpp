#include "LayerStack.h"

namespace SunEngine
{
    LayerStack::LayerStack()
    {
    }

    LayerStack::~LayerStack()
    {
        for (Layer* layer : Layers)
        {
            delete layer;
        }
    }

    void LayerStack::PushLayer(Layer* layer)
    {
        Layers.emplace_back(layer);
        layer->OnAttach();
    }
}