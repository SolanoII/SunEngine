#include "GLFWWindow.h"
#include "../../Core/Logger/Log.h"
#include "../../Core/Input.h"

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