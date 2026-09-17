#include "Application.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "../../Window/Window.h"
#include "../Events/WindowEvent.h"
#include "../Logger/Log.h"
#include "../../Renderer/Renderer.h"

namespace SunEngine
{
    Application::Application()
    {
        Log::Init();

        SUN_CORE_INFO("==================================");
        SUN_CORE_INFO("Starting Sun Engine");
        SUN_CORE_INFO("Initializing Window...");

        m_Window = Window::Create(1280, 720, "Sun Engine");

        m_Window->SetEventCallback([this](Event& event) { OnEvent(event); });

        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
        {
            SUN_CORE_ERROR("Failed to initialize GLAD!");
            m_Running = false;
        }
        SUN_CORE_INFO("Window created successfully");
        m_LastFrameTime = std::chrono::steady_clock::now();
    }

    Application::~Application()
    {
        SUN_CORE_INFO("Shutting down Sun Engine");
        delete m_Window;
    }

    void Application::Run()
    {
        Renderer::Init();

        while (m_Running)
        {
            auto currentTime = std::chrono::steady_clock::now();
            float deltaTime = std::chrono::duration<float>(currentTime - m_LastFrameTime).count();
            m_LastFrameTime = currentTime;

            // Clamp: se un frame dura più di 50ms (20 FPS) significa che è successo
            // qualcosa di anomalo (loading, breakpoint, hitch del driver). Passare
            // un deltaTime enorme a Jolt causerebbe tunneling e ragdoll esplosivi.
            // Il clamp "rallenta" il mondo in caso di hitch invece di rompere la sim.
            constexpr float MAX_DELTA = 0.05f;
            if (deltaTime > MAX_DELTA) {
                deltaTime = MAX_DELTA;
            }

            glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            for (Layer* layer : m_LayerStack)
            {
                layer->OnUpdate(deltaTime);
            }

            for (Layer* layer : m_LayerStack)
            {
                layer->OnRender();
            }

            m_Window->Update();
        }

        Renderer::Shutdown();
    }

    void Application::OnEvent(Event& event)
    {
        if (event.GetEventType() == EventType::WindowClose)
        {
            m_Running = false;
            event.Handled = true;
        }
    }

    void Application::PushLayer(Layer* layer)
    {
        m_LayerStack.PushLayer(layer);
    }
}