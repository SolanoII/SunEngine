#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <glad/glad.h>

namespace SunEngine {

    struct Vertex {
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec2 TexCoords;
    };

    struct Texture {
        uint32_t id;
        std::string type; // "texture_diffuse", "texture_normal"
        std::string path;
    };

    class Mesh {
    public:
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<Texture> textures;

        uint32_t VAO, VBO, EBO;

        Mesh(std::vector<Vertex> verts, std::vector<uint32_t> inds, std::vector<Texture> texs)
            : vertices(verts), indices(inds), textures(texs)
        {
            SetupMesh();
        }

        void Draw() const {
            glBindVertexArray(VAO);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);
        }

    private:
        void SetupMesh() {
            glGenVertexArrays(1, &VAO);
            glGenBuffers(1, &VBO);
            glGenBuffers(1, &EBO);

            glBindVertexArray(VAO);

            glBindBuffer(GL_ARRAY_BUFFER, VBO);
            glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);

            // Posizioni
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
            // Normali
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));
            // Texture UV
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, TexCoords));

            glBindVertexArray(0);
        }
    };
}