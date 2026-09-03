#pragma once

#include <chrono>
#include "../Events/Event.h"
#include "../Layer/LayerStack.h"
#include "../Layer/Layer.h"

namespace SunEngine
{
    class Window; // Forward declaration per evitare dipendenze circolari

    class Application
    {
    private:
        Window* m_Window = nullptr;
        bool m_Running = true;
        LayerStack m_LayerStack;

        std::chrono::steady_clock::time_point m_LastFrameTime;

    public:
        Application();
        virtual ~Application();
        void Run();
        void OnEvent(Event& event);
        void PushLayer(Layer* layer);
    };
}