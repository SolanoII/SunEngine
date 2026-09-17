#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdlib>
#include <algorithm>

namespace SunEngine {
    class Camera {
    private:
        glm::mat4 m_Projection = glm::mat4(1.0f);
        glm::mat4 m_View = glm::mat4(1.0f);
        glm::vec3 m_TargetPosition = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_CameraPosition = { 0.0f, 0.0f, 0.0f };
        float m_TargetYaw = 0.0f;
        float m_Width = 1280.0f;
        float m_Height = 720.0f;
        float m_PitchOffset = 0.0f;
        float m_YawOffset = 0.0f;

        // Parametri adattativi calcolati dinamicamente (Gears Style)
        float m_ShoulderOffset = 0.45f;
        float m_HeightOffset = 0.20f;
        float m_Distance = 3.00f;

    public:
        Camera() = default;
        const glm::vec3& GetPosition() const { return m_CameraPosition; }
        void SetPitch(float pitch) { m_PitchOffset = pitch; }
        void SetYawOffset(float yaw) { m_YawOffset = yaw; }

        // Aggiorna la spaziatura della telecamera in base alla stazza dell'unità
        void SetCameraOffsets(float shoulderOffset, float heightOffset, float distance) {
            m_ShoulderOffset = shoulderOffset;
            m_HeightOffset = heightOffset;
            m_Distance = distance;
        }

        void SetViewportSize(float width, float height, float extraFovDegrees = 0.0f) {
            m_Width = std::max(1.0f, width);
            m_Height = std::max(1.0f, height);
            float baseFov = 50.0f + extraFovDegrees;
            m_Projection = glm::perspective(glm::radians(baseFov), m_Width / m_Height, 0.1f, 100.0f);
        }

        void SetTarget(const glm::vec3& pos, float yaw) {
            m_TargetPosition = pos;
            m_TargetYaw = yaw;
        }

        void Update(glm::vec3 shakeOffset = glm::vec3(0.0f), float wallZoom = 1.0f) {
            float totalYaw = m_TargetYaw + m_YawOffset;
            float totalPitch = m_PitchOffset;

            glm::mat4 yawRot = glm::rotate(glm::mat4(1.0f), glm::radians(totalYaw), glm::vec3(0.0f, 1.0f, 0.0f));
            glm::mat4 pitchRot = glm::rotate(glm::mat4(1.0f), glm::radians(totalPitch), glm::vec3(1.0f, 0.0f, 0.0f));
            glm::mat4 camRot = yawRot * pitchRot;

            glm::vec3 localOffset = glm::vec3(m_ShoulderOffset, m_HeightOffset, m_Distance * wallZoom);
            glm::vec3 rotatedOffset = glm::vec3(camRot * glm::vec4(localOffset, 0.0f));

            glm::vec3 cameraPos = m_TargetPosition + rotatedOffset + shakeOffset;
            m_CameraPosition = cameraPos;
            glm::vec3 forwardDir = glm::vec3(camRot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
            glm::vec3 lookTarget = cameraPos + (forwardDir * 50.0f);

            m_View = glm::lookAt(cameraPos, lookTarget, glm::vec3(0.0f, 1.0f, 0.0f));
        }

        glm::mat4 GetViewProjection() const {
            return m_Projection * m_View;
        }
    };
}