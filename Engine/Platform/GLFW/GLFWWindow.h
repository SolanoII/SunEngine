#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "../../Window/Window.h"

namespace SunEngine {

    class GLFWWindow : public Window {
    public:
        GLFWWindow(int width, int height, const std::string& title);
        virtual ~GLFWWindow();

        virtual void Update() override;
        virtual void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
        virtual void* GetNativeWindow() const override { return m_Window; }   // FIX #7

    private:
        virtual void Init(int width, int height, const std::string& title);
        virtual void Shutdown();

    private:
        GLFWwindow* m_Window = nullptr;

        struct WindowData {
            std::string Title;
            int Width = 0;
            int Height = 0;
            EventCallbackFn EventCallback;
        };

        WindowData m_Data;
    };

}