#include "Window.h"
#include "../Platform/GLFW/GLFWWindow.h"

namespace SunEngine {

    Window* Window::Create(int width, int height, const std::string& title) {
        return new GLFWWindow(width, height, title);
    }

}