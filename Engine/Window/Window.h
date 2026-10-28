#pragma once

#include <string>
#include <functional>
#include "../Core/Events/Event.h"

namespace SunEngine
{
    class Window
    {
    public:
        using EventCallbackFn = std::function<void(Event&)>;

        virtual ~Window() = default;
        virtual void Update() = 0;
        // FIX #7: handle nativo (GLFWwindow*) per i controlli di chiusura; nullptr se non disponibile
        virtual void* GetNativeWindow() const { return nullptr; }
        virtual void SetEventCallback(const EventCallbackFn& callback) = 0;

        static Window* Create(int width = 1280, int height = 720, const std::string& title = "Sun Engine");
    };
}