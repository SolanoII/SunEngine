#include "Renderer.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>
#include <vector>

namespace SunEngine
{
    // --- Variabili statiche ---
    unsigned int Renderer::s_VAO = 0, Renderer::s_VBO = 0, Renderer::s_EBO = 0;
    unsigned int Renderer::s_PlaneVAO = 0, Renderer::s_PlaneVBO = 0, Renderer::s_PlaneEBO = 0;
    unsigned int Renderer::s_LineVAO = 0, Renderer::s_LineVBO = 0; // <-- Nuove
    unsigned int Renderer::s_CubeShaderProgram = 0, Renderer::s_PlaneShaderProgram = 0;
    int Renderer::s_MVPLocation = -1, Renderer::s_ColorLocation = -1;
    int Renderer::s_PlaneMVPLocation = -1;
    glm::mat4 Renderer::s_ViewProjectionMatrix = glm::mat4(1.0f);
    void Renderer::DrawSphere(const glm::vec3& center, float radius, const glm::vec4& color) {
        // Fallback: Disegna un cubo scalato che fa le veci della sfera per i proiettili
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), center);
        transform = glm::scale(transform, glm::vec3(radius * 2.0f)); // Il diametro è raggio * 2

        DrawCube(transform, color);
    }
    void Renderer::Init()
    {
        // ================================
        // 1. GEOMETRIA DEL CUBO (invariata)
        // ================================
        float cubeVertices[] = {
            -0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f, -0.5f,
            -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,
            -0.5f, -0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,
             0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  0.5f,  0.5f, -0.5f,  0.5f,
            -0.5f, -0.5f, -0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,
             0.5f, -0.5f, -0.5f,  0.5f, -0.5f,  0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f, -0.5f
        };
        unsigned int cubeIndices[] = {
            0,1,2, 2,3,0, 4,5,6, 6,7,4, 8,9,10, 10,11,8,
            12,13,14, 14,15,12, 16,17,18, 18,19,16, 20,21,22, 22,23,20
        };

        glGenVertexArrays(1, &s_VAO);
        glGenBuffers(1, &s_VBO);
        glGenBuffers(1, &s_EBO);
        glBindVertexArray(s_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, s_VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVertices), cubeVertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(cubeIndices), cubeIndices, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        // ================================
        // 2. GEOMETRIA DEL PAVIMENTO (invariata)
        // ================================
        const int gridSize = 20;
        const float tileSize = 1.0f;
        float halfGrid = (gridSize * tileSize) / 2.0f;

        std::vector<float> planeVertices;
        std::vector<unsigned int> planeIndices;

        for (int x = 0; x < gridSize; ++x) {
            for (int z = 0; z < gridSize; ++z) {
                float x0 = x * tileSize - halfGrid;
                float z0 = z * tileSize - halfGrid;
                float x1 = x0 + tileSize;
                float z1 = z0 + tileSize;
                glm::vec3 color = ((x + z) % 2 == 0) ? glm::vec3(0.8f, 0.8f, 0.8f) : glm::vec3(0.2f, 0.2f, 0.2f);
                int baseIndex = (int)planeVertices.size() / 6;

                planeVertices.push_back(x0); planeVertices.push_back(-0.5f); planeVertices.push_back(z0);
                planeVertices.push_back(color.r); planeVertices.push_back(color.g); planeVertices.push_back(color.b);
                planeVertices.push_back(x1); planeVertices.push_back(-0.5f); planeVertices.push_back(z0);
                planeVertices.push_back(color.r); planeVertices.push_back(color.g); planeVertices.push_back(color.b);
                planeVertices.push_back(x1); planeVertices.push_back(-0.5f); planeVertices.push_back(z1);
                planeVertices.push_back(color.r); planeVertices.push_back(color.g); planeVertices.push_back(color.b);
                planeVertices.push_back(x0); planeVertices.push_back(-0.5f); planeVertices.push_back(z1);
                planeVertices.push_back(color.r); planeVertices.push_back(color.g); planeVertices.push_back(color.b);

                planeIndices.push_back(baseIndex);   planeIndices.push_back(baseIndex + 1); planeIndices.push_back(baseIndex + 2);
                planeIndices.push_back(baseIndex);   planeIndices.push_back(baseIndex + 2); planeIndices.push_back(baseIndex + 3);
            }
        }

        glGenVertexArrays(1, &s_PlaneVAO);
        glGenBuffers(1, &s_PlaneVBO);
        glGenBuffers(1, &s_PlaneEBO);
        glBindVertexArray(s_PlaneVAO);
        glBindBuffer(GL_ARRAY_BUFFER, s_PlaneVBO);
        glBufferData(GL_ARRAY_BUFFER, planeVertices.size() * sizeof(float), planeVertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_PlaneEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, planeIndices.size() * sizeof(unsigned int), planeIndices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        // ================================
        // 3. GEOMETRIA DELLA LINEA (nuova)
        // ================================
        glGenVertexArrays(1, &s_LineVAO);
        glGenBuffers(1, &s_LineVBO);
        glBindVertexArray(s_LineVAO);
        glBindBuffer(GL_ARRAY_BUFFER, s_LineVBO);
        // I dati verranno aggiornati dinamicamente in DrawLine
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        // ================================
        // 4. SHADER del CUBO (invariato)
        // ================================
        const char* cubeVertSrc = R"(
            #version 330 core
            layout (location = 0) in vec3 aPos;
            uniform mat4 u_MVP;
            void main() { gl_Position = u_MVP * vec4(aPos, 1.0); }
        )";
        const char* cubeFragSrc = R"(
            #version 330 core
            out vec4 FragColor;
            uniform vec4 u_Color;
            void main() { FragColor = u_Color; }
        )";

        unsigned int cubeVertShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(cubeVertShader, 1, &cubeVertSrc, NULL);
        glCompileShader(cubeVertShader);
        unsigned int cubeFragShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(cubeFragShader, 1, &cubeFragSrc, NULL);
        glCompileShader(cubeFragShader);

        s_CubeShaderProgram = glCreateProgram();
        glAttachShader(s_CubeShaderProgram, cubeVertShader);
        glAttachShader(s_CubeShaderProgram, cubeFragShader);
        glLinkProgram(s_CubeShaderProgram);
        glDeleteShader(cubeVertShader);
        glDeleteShader(cubeFragShader);

        s_MVPLocation = glGetUniformLocation(s_CubeShaderProgram, "u_MVP");
        s_ColorLocation = glGetUniformLocation(s_CubeShaderProgram, "u_Color");

        // ================================
        // 5. SHADER del PAVIMENTO (invariato)
        // ================================
        const char* planeVertSrc = R"(
            #version 330 core
            layout (location = 0) in vec3 aPos;
            layout (location = 1) in vec3 aColor;
            out vec3 vColor;
            uniform mat4 u_MVP;
            void main() { gl_Position = u_MVP * vec4(aPos, 1.0); vColor = aColor; }
        )";
        const char* planeFragSrc = R"(
            #version 330 core
            in vec3 vColor;
            out vec4 FragColor;
            void main() { FragColor = vec4(vColor, 1.0); }
        )";

        unsigned int planeVertShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(planeVertShader, 1, &planeVertSrc, NULL);
        glCompileShader(planeVertShader);
        unsigned int planeFragShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(planeFragShader, 1, &planeFragSrc, NULL);
        glCompileShader(planeFragShader);

        s_PlaneShaderProgram = glCreateProgram();
        glAttachShader(s_PlaneShaderProgram, planeVertShader);
        glAttachShader(s_PlaneShaderProgram, planeFragShader);
        glLinkProgram(s_PlaneShaderProgram);
        glDeleteShader(planeVertShader);
        glDeleteShader(planeFragShader);

        s_PlaneMVPLocation = glGetUniformLocation(s_PlaneShaderProgram, "u_MVP");
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void Renderer::Shutdown()
    {
        glDeleteVertexArrays(1, &s_VAO);
        glDeleteBuffers(1, &s_VBO);
        glDeleteBuffers(1, &s_EBO);
        glDeleteVertexArrays(1, &s_PlaneVAO);
        glDeleteBuffers(1, &s_PlaneVBO);
        glDeleteBuffers(1, &s_PlaneEBO);
        glDeleteVertexArrays(1, &s_LineVAO); // <-- Nuovo
        glDeleteBuffers(1, &s_LineVBO);      // <-- Nuovo
        glDeleteProgram(s_CubeShaderProgram);
        glDeleteProgram(s_PlaneShaderProgram);
    }

    void Renderer::SetViewProjection(const glm::mat4& viewProj) {
        s_ViewProjectionMatrix = viewProj;
    }

    void Renderer::DrawCube(const glm::mat4& transform, const glm::vec4& color) {
        glUseProgram(s_CubeShaderProgram);
        if (s_ColorLocation != -1) glUniform4fv(s_ColorLocation, 1, glm::value_ptr(color));

        glBindVertexArray(s_VAO);
        glm::mat4 mvp = s_ViewProjectionMatrix * transform;
        if (s_MVPLocation != -1) glUniformMatrix4fv(s_MVPLocation, 1, GL_FALSE, glm::value_ptr(mvp));
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
    }

    void Renderer::DrawPlane(const glm::mat4& transform) {
        glUseProgram(s_PlaneShaderProgram);
        glBindVertexArray(s_PlaneVAO);
        glm::mat4 mvp = s_ViewProjectionMatrix * transform;
        if (s_PlaneMVPLocation != -1) glUniformMatrix4fv(s_PlaneMVPLocation, 1, GL_FALSE, glm::value_ptr(mvp));
        glDrawElements(GL_TRIANGLES, (GLsizei)(20 * 20 * 6), GL_UNSIGNED_INT, 0);
    }

    // ============================================================
    // DrawLine (nuova)
    // ============================================================
    void Renderer::DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec4& color)
    {
        float vertices[] = { start.x, start.y, start.z, end.x, end.y, end.z };
        glBindVertexArray(s_LineVAO);
        glBindBuffer(GL_ARRAY_BUFFER, s_LineVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

        glUseProgram(s_CubeShaderProgram);
        if (s_ColorLocation != -1) glUniform4fv(s_ColorLocation, 1, glm::value_ptr(color));

        glm::mat4 mvp = s_ViewProjectionMatrix;
        if (s_MVPLocation != -1) glUniformMatrix4fv(s_MVPLocation, 1, GL_FALSE, glm::value_ptr(mvp));

        // Depth test ATTIVO: i beam vengono occlusi dalla geometria.
        // LEQUAL invece del default LESS evita flicker quando la linea coincide
        // con una faccia del box di partenza.
        glDepthFunc(GL_LEQUAL);
        glLineWidth(1.0f);
        glDrawArrays(GL_LINES, 0, 2);
        glLineWidth(1.0f);
        glDepthFunc(GL_LESS);   // ripristina il default
    }
    void Renderer::DrawRingUI(const glm::vec2& center, float radius, float progress, const glm::vec4& color, const glm::vec2& screenSize)
    {
        std::vector<float> vertices;
        int segments = 32; // Più alto è il numero, più il cerchio sarà liscio
        int activeSegments = (int)(segments * progress);

        // Genera i punti del cerchio (partendo dall'alto verso l'asse Y)
        for (int i = 0; i <= activeSegments; i++) {
            float angle = -glm::half_pi<float>() + (glm::two_pi<float>() * i / segments);
            float x = center.x + cos(angle) * radius;
            float y = center.y + sin(angle) * radius;
            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(0.0f); // La Z in UI non serve
        }

        // Carica i dati nel buffer (riutilizziamo quello delle linee)
        glBindVertexArray(s_LineVAO);
        glBindBuffer(GL_ARRAY_BUFFER, s_LineVBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_DYNAMIC_DRAW);

        glUseProgram(s_CubeShaderProgram);
        if (s_ColorLocation != -1) glUniform4fv(s_ColorLocation, 1, glm::value_ptr(color));

        // Magia della UI: Proiezione Ortografica (2D)
        glm::mat4 ortho = glm::ortho(0.0f, screenSize.x, screenSize.y, 0.0f, -1.0f, 1.0f);
        if (s_MVPLocation != -1) glUniformMatrix4fv(s_MVPLocation, 1, GL_FALSE, glm::value_ptr(ortho));

        // Disabilitiamo temporaneamente la profondità per sovrascrivere tutto sullo schermo
        glDisable(GL_DEPTH_TEST);

        glLineWidth(3.0f); // Spessore del cerchio
        glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)(vertices.size() / 3));
        glLineWidth(1.0f);

        glEnable(GL_DEPTH_TEST);
    }
    void Renderer::DrawQuadUI(const glm::vec2& pos, const glm::vec2& size, const glm::vec4& color, const glm::vec2& screenSize)
    {
        float vertices[] = {
            pos.x,          pos.y,          0.0f,
            pos.x,          pos.y + size.y, 0.0f,
            pos.x + size.x, pos.y,          0.0f,
            pos.x + size.x, pos.y + size.y, 0.0f
        };

        glBindVertexArray(s_LineVAO);
        glBindBuffer(GL_ARRAY_BUFFER, s_LineVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

        glUseProgram(s_CubeShaderProgram);
        if (s_ColorLocation != -1) glUniform4fv(s_ColorLocation, 1, glm::value_ptr(color));

        glm::mat4 ortho = glm::ortho(0.0f, screenSize.x, screenSize.y, 0.0f, -1.0f, 1.0f);
        if (s_MVPLocation != -1) glUniformMatrix4fv(s_MVPLocation, 1, GL_FALSE, glm::value_ptr(ortho));

        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }
}