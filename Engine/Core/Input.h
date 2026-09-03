#pragma once
#include <array>
#include <glm/glm.hpp>

namespace SunEngine
{
    class Input
    {
    public:
        static bool IsKeyPressed(int keycode);
        static bool IsMouseButtonPressed(int button);
        static glm::vec2 GetMouseDelta(); // Nuovo: ottiene il movimento del mouse nell'ultimo frame

        // Chiamate interne dal motore
        static void OnKeyPressed(int keycode);
        static void OnKeyReleased(int keycode);
        static void OnMouseMoved(double x, double y);   // Nuovo
        static void OnMouseButtonPressed(int button);  // Nuovo
        static void OnMouseButtonReleased(int button); // Nuovo

    private:
        static std::array<bool, 512> s_KeyStates;
        static std::array<bool, 8> s_MouseStates;
        static double s_LastMouseX, s_LastMouseY;
        static glm::vec2 s_MouseDelta;
    };
}