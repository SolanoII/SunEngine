#include "Input.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "Logger/Log.h"

namespace SunEngine
{
    std::array<bool, 512> Input::s_KeyStates = { false };
    std::array<bool, 8> Input::s_MouseStates = { false };
    double Input::s_LastMouseX = 0.0;
    double Input::s_LastMouseY = 0.0;
    glm::vec2 Input::s_MouseDelta = { 0.0f, 0.0f };

    glm::vec2 Input::GetMouseDelta()
    {
        glm::vec2 delta = s_MouseDelta;
        s_MouseDelta = { 0.0f, 0.0f };
        return delta;
    }

    bool Input::IsKeyPressed(int keycode) {
        if (keycode >= 0 && keycode < 512) return s_KeyStates[keycode];
        return false;
    }

    bool Input::IsMouseButtonPressed(int button) {
        if (button >= 0 && button < 8) return s_MouseStates[button];
        return false;
    }

    void Input::OnKeyPressed(int keycode) {
        if (keycode >= 0 && keycode < 512) s_KeyStates[keycode] = true;
    }

    void Input::OnKeyReleased(int keycode) {
        if (keycode >= 0 && keycode < 512) s_KeyStates[keycode] = false;
    }

    void Input::OnMouseButtonPressed(int button)
    {
        if (button >= 0 && button < 8) {
            s_MouseStates[button] = true;
        }
    }

    void Input::OnMouseButtonReleased(int button)
    {
        if (button >= 0 && button < 8) {
            s_MouseStates[button] = false;
        }
    }

    void Input::OnMouseMoved(double x, double y)
    {
        s_MouseDelta.x = static_cast<float>(x - s_LastMouseX);
        s_MouseDelta.y = static_cast<float>(y - s_LastMouseY);
        s_LastMouseX = x;
        s_LastMouseY = y;
    }
}