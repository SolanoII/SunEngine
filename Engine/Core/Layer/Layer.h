#pragma once
#include <string>

namespace SunEngine {

    class Layer {
    public:
        Layer(const std::string& name = "Layer") : m_DebugName(name) {}
        virtual ~Layer() = default;

        virtual void OnAttach() {}
        virtual void OnDetach() {}
        virtual void OnUpdate(float deltaTime) {}
        virtual void OnRender() {}
        virtual void OnImGuiRender() {} // <-- AGGIUNGI QUESTA RIGA SE MANCA
        // FIX #6: notifica di resize del framebuffer (in PIXEL). Chiamato da Application::OnEvent
        // quando la finestra cambia dimensione (drag, fullscreen, cambio risoluzione).
        virtual void OnWindowResize(int width, int height) { (void)width; (void)height; }

        inline const std::string& GetName() const { return m_DebugName; }

    protected:
        std::string m_DebugName;
    };

}