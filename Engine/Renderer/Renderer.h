#pragma once

#include <glm/glm.hpp>

namespace SunEngine {

    class Renderer {
    public:
        static void Init();
        static void Shutdown();

        static void SetViewProjection(const glm::mat4& viewProj);

        static void DrawCube(const glm::mat4& transform, const glm::vec4& color);
        static void DrawPlane(const glm::mat4& transform);
        static void DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec4& color);
        static void DrawSphere(const glm::vec3& center, float radius, const glm::vec4& color);

        static void DrawRingUI(const glm::vec2& center, float radius, float progress, const glm::vec4& color, const glm::vec2& screenSize);
        static void DrawQuadUI(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, const glm::vec2& screenSize);

    private:
        static unsigned int s_VAO, s_VBO, s_EBO;
        static unsigned int s_PlaneVAO, s_PlaneVBO, s_PlaneEBO;
        static unsigned int s_LineVAO, s_LineVBO;

        static unsigned int s_CubeShaderProgram, s_PlaneShaderProgram;

        static int s_MVPLocation, s_ColorLocation;
        static int s_PlaneMVPLocation;

        static glm::mat4 s_ViewProjectionMatrix;
    };

}