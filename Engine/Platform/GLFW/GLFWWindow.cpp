#include "GLFWWindow.h"
#include "../../Core/Logger/Log.h"
#include "../../Core/Input.h"
#include "../../Core/Events/WindowEvent.h"

namespace SunEngine {

    GLFWWindow::GLFWWindow(int width, int height, const std::string& title) {
        Init(width, height, title);
    }

    GLFWWindow::~GLFWWindow() {
        Shutdown();
    }

    void GLFWWindow::Init(int width, int height, const std::string& title) {
        m_Data.Title = title;
        m_Data.Width = width;
        m_Data.Height = height;

        if (!glfwInit()) {
            SUN_CORE_ERROR("Impossibile inizializzare GLFW!");
            return;
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        m_Window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
        if (!m_Window) {
            SUN_CORE_ERROR("Impossibile creare la finestra GLFW!");
            glfwTerminate();
            return;
        }

        glfwMakeContextCurrent(m_Window);
        glfwSwapInterval(1);
        glfwSetWindowUserPointer(m_Window, &m_Data);

        glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
            (void)window; (void)scancode; (void)mods;
            if (action == GLFW_PRESS)
                Input::OnKeyPressed(key);
            else if (action == GLFW_RELEASE)
                Input::OnKeyReleased(key);
        });

        glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* window, int button, int action, int mods) {
            (void)window; (void)mods;
            if (action == GLFW_PRESS)
                Input::OnMouseButtonPressed(button);
            else if (action == GLFW_RELEASE)
                Input::OnMouseButtonReleased(button);
        });

        glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double xpos, double ypos) {
            (void)window;
            Input::OnMouseMoved(xpos, ypos);
        });

        // FIX #7: chiusura finestra (X) -> WindowCloseEvent -> Application::OnEvent mette m_Running=false
        glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* window) {
            auto* data = static_cast<WindowData*>(glfwGetWindowUserPointer(window));
            if (data && data->EventCallback) {
                WindowCloseEvent event;
                data->EventCallback(event);
            }
        });

        // FIX #6: resize del FRAMEBUFFER (pixel reali, corretto anche su HiDPI e in fullscreen).
        // Genera un WindowResizeEvent verso Application::OnEvent, che lo gira ai Layer.
        glfwSetFramebufferSizeCallback(m_Window, [](GLFWwindow* window, int width, int height) {
            auto* data = static_cast<WindowData*>(glfwGetWindowUserPointer(window));
            if (!data) return;
            data->Width = width;
            data->Height = height;
            if (data->EventCallback) {
                WindowResizeEvent event(static_cast<unsigned int>(width), static_cast<unsigned int>(height));
                data->EventCallback(event);
            }
        });
    }

    void GLFWWindow::Shutdown() {
        if (m_Window) {
            glfwDestroyWindow(m_Window);
            m_Window = nullptr;
        }
        glfwTerminate();
    }

    void GLFWWindow::Update() {
        glfwPollEvents();
        glfwSwapBuffers(m_Window);
    }

}