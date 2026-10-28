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
        // Diagnostica: versione/driver effettivi del contesto (utile per compute, ImageTexture, ecc.)
        SUN_CORE_INFO("OpenGL {0} | {1} | {2}",
            reinterpret_cast<const char*>(glGetString(GL_VERSION)),
            reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
            reinterpret_cast<const char*>(glGetString(GL_VENDOR)));

        SUN_CORE_INFO("Window created successfully");
        m_LastFrameTime = std::chrono::steady_clock::now();
    }

    Application::~Application()
    {
        SUN_CORE_INFO("Shutting down Sun Engine");
        // FIX #15: i layer (GameLayer: ImGui, audio, renderer deferred) vanno distrutti PRIMA della
        // finestra. Prima il membro m_LayerStack veniva distrutto DOPO `delete m_Window`, cioe'
        // con il contesto GL gia' morto (cleanup GL/ImGui su contesto inesistente).
        m_LayerStack.Clear();
        delete m_Window;
    }

    void Application::Run()
    {
        Renderer::Init();

        // FIX #7/#15: la X genera WindowCloseEvent (m_Running=false). glfwWindowShouldClose resta come
        // rete di sicurezza per il pulsante "Esci" (glfwSetWindowShouldClose non invoca il callback),
        // letto dal handle della NOSTRA finestra e non da glfwGetCurrentContext() che puo' essere null.
        GLFWwindow* nativeWindow = m_Window ? static_cast<GLFWwindow*>(m_Window->GetNativeWindow()) : nullptr;
        while (m_Running && !(nativeWindow && glfwWindowShouldClose(nativeWindow)))
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
        // FIX #6: propaga il resize a tutti i Layer (dal piu' alto al piu' basso, come in Hazel)
        else if (event.GetEventType() == EventType::WindowResize)
        {
            auto& resize = static_cast<WindowResizeEvent&>(event);
            const int w = static_cast<int>(resize.GetWidth());
            const int h = static_cast<int>(resize.GetHeight());
            if (w > 0 && h > 0) {   // finestra minimizzata: 0x0, ignoro
                for (Layer* layer : m_LayerStack) layer->OnWindowResize(w, h);
            }
        }
    }

    void Application::PushLayer(Layer* layer)
    {
        m_LayerStack.PushLayer(layer);
    }
}