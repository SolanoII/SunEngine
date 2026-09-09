#pragma once

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "Mesh.h"
#include "../Core/Logger/Log.h"

namespace SunEngine {

    class Model {
    public:
        std::vector<Mesh> meshes;
        std::string directory;

        bool LoadModel(const std::string& path) {
            Assimp::Importer importer;
            const aiScene* scene = importer.ReadFile(path,
                aiProcess_Triangulate |
                aiProcess_GenSmoothNormals |
                aiProcess_FlipUVs |
                aiProcess_CalcTangentSpace);

            if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
                SUN_CORE_ERROR("ERRORE ASSIMP: {0}", importer.GetErrorString());
                return false;
            }

            directory = path.substr(0, path.find_last_of('/'));
            ProcessNode(scene->mRootNode, scene);
            return true;
        }

        void Draw() const {
            for (const auto& mesh : meshes) {
                mesh.Draw();
            }
        }

    private:
        void ProcessNode(aiNode* node, const aiScene* scene) {
            for (unsigned int i = 0; i < node->mNumMeshes; i++) {
                aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
                meshes.push_back(ProcessMesh(mesh, scene));
            }
            for (unsigned int i = 0; i < node->mNumChildren; i++) {
                ProcessNode(node->mChildren[i], scene);
            }
        }

        Mesh ProcessMesh(aiMesh* mesh, const aiScene* scene) {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            std::vector<Texture> textures;

            for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
                Vertex vertex;
                vertex.Position = glm::vec3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);

                if (mesh->HasNormals()) {
                    vertex.Normal = glm::vec3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z);
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