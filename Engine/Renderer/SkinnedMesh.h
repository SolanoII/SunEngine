#pragma once
#include "SkinnedVertex.h"
#include <glad/glad.h>
#include <vector>

namespace SunEngine {

    class SkinnedMesh {
    public:
        std::vector<SkinnedVertex> vertices;
        std::vector<unsigned int> indices;
        unsigned int VAO = 0, VBO = 0, EBO = 0;
        int materialIndex = -1;   // modelli d'autore: indice in SkinnedModel::materials (-1 = nessun materiale)

        SkinnedMesh(std::vector<SkinnedVertex> verts, std::vector<unsigned int> inds)
            : vertices(std::move(verts)), indices(std::move(inds)) {
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
            glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(SkinnedVertex), vertices.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

            glEnableVertexAttribArray(0); // Position
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, Position));
            glEnableVertexAttribArray(1); // Normal
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, Normal));
            glEnableVertexAttribArray(2); // TexCoords
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, TexCoords));
            glEnableVertexAttribArray(3); // BoneIDs (interi, quindi IPointer non Pointer)
            glVertexAttribIPointer(3, 4, GL_INT, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, BoneIDs));
            glEnableVertexAttribArray(4); // Weights
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, Weights));
            glEnableVertexAttribArray(5); // Tangent (normal map dei modelli d'autore)
            glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, Tangent));

            glBindVertexArray(0);
        }
    };
}