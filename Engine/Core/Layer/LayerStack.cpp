#include "LayerStack.h"

namespace SunEngine
{
    LayerStack::LayerStack()
    {
    }

    LayerStack::~LayerStack()
    {
        Clear();
    }

    void LayerStack::Clear()
    {
        for (Layer* layer : Layers)
        {
            delete layer;
        }
        Layers.clear();
    }

    void LayerStack::PushLayer(Layer* layer)
    {
        Layers.emplace_back(layer);
        layer->OnAttach();
    }
}