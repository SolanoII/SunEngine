#pragma once

#include "Mesh.h"
#include "../Core/Logger/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <iostream>
#include <unordered_map>
#include <filesystem>

namespace SunEngine {

    inline glm::mat4 AiMatrixToGlm(const aiMatrix4x4& from) {
        glm::mat4 to{ 1.0f };
        to[0][0] = from.a1; to[1][0] = from.a2; to[2][0] = from.a3; to[3][0] = from.a4;
        to[0][1] = from.b1; to[1][1] = from.b2; to[2][1] = from.b3; to[3][1] = from.b4;
        to[0][2] = from.c1; to[1][2] = from.c2; to[2][2] = from.c3; to[3][2] = from.c4;
        to[0][3] = from.d1; to[1][3] = from.d2; to[2][3] = from.d3; to[3][3] = from.d4;
        return to;
    }

    class Model {
    public:
        std::vector<Texture> textures_loaded;
        std::vector<Mesh> meshes;
        std::string directory{};

        Model() = default;
        Model(const std::string& path) {
            LoadModel(path);
        }

        bool LoadModel(const std::string& path) {
            Assimp::Importer importer;
            const aiScene* scene = importer.ReadFile(path,
                aiProcess_Triangulate |
                aiProcess_GenSmoothNormals |
                aiProcess_FlipUVs |
                aiProcess_CalcTangentSpace);

            if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
                SUN_CORE_ERROR("Assimp Error durante il caricamento di {0}: {1}", path, importer.GetErrorString());
                return false;
            }

            std::filesystem::path fsPath(path);
            directory = fsPath.parent_path().string();

            ProcessNode(scene->mRootNode, scene, glm::mat4(1.0f));
            SUN_CORE_INFO("Modello 3D statico caricato: {0} ({1} sub-mesh)", path, meshes.size());
            return true;
        }

        void Draw(unsigned int shaderProgram) const {
            for (const auto& mesh : meshes) {
                mesh.Draw(shaderProgram);
            }
        }

    private:
        void ProcessNode(aiNode* node, const aiScene* scene, glm::mat4 parentTransform) {
            glm::mat4 nodeTransform = parentTransform * AiMatrixToGlm(node->mTransformation);

            for (unsigned int i = 0; i < node->mNumMeshes; i++) {
                aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
                meshes.push_back(ProcessMesh(mesh, scene, nodeTransform));
            }

            for (unsigned int i = 0; i < node->mNumChildren; i++) {
                ProcessNode(node->mChildren[i], scene, nodeTransform);
            }
        }

        Mesh ProcessMesh(aiMesh* mesh, const aiScene* scene, const glm::mat4& transform) {
            std::vector<StaticVertex> vertices;
            std::vector<unsigned int> indices;
            std::vector<Texture> textures;

            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));

            for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
                StaticVertex vertex{}; // Inizializzazione pulita per il linter

                glm::vec4 pos = transform * glm::vec4(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z, 1.0f);
                vertex.Position = glm::vec3(pos);

                if (mesh->HasNormals()) {
                    vertex.Normal = glm::normalize(normalMatrix * glm::vec3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z));
                }
                else {
                    vertex.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
                }

                if (mesh->mTextureCoords[0]) {
                    vertex.TexCoords = glm::vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
                }
                else {
                    vertex.TexCoords = glm::vec2(0.0f, 0.0f);
                }

                vertices.push_back(vertex);
            }

            for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
                aiFace face = mesh->mFaces[i];
                for (unsigned int j = 0; j < face.mNumIndices; j++)
                    indices.push_back(face.mIndices[j]);
            }

            return Mesh(vertices, indices, textures);
        }
    };

}