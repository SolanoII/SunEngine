#pragma once
#include "SkinnedMesh.h"
#include "../Core/Logger/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>
#include <filesystem>

namespace SunEngine {

    inline glm::mat4 AiMatToGlm(const aiMatrix4x4& from) {
        glm::mat4 to{ 1.0f };
        to[0][0] = from.a1; to[1][0] = from.a2; to[2][0] = from.a3; to[3][0] = from.a4;
        to[0][1] = from.b1; to[1][1] = from.b2; to[2][1] = from.b3; to[3][1] = from.b4;
        to[0][2] = from.c1; to[1][2] = from.c2; to[2][2] = from.c3; to[3][2] = from.c4;
        to[0][3] = from.d1; to[1][3] = from.d2; to[2][3] = from.d3; to[3][3] = from.d4;
        return to;
    }

    struct AssimpNodeData {
        glm::mat4 transformation{ 1.0f };
        std::string name;
        int childrenCount = 0;
        std::vector<AssimpNodeData> children;
    };

    class SkinnedModel {
    public:
        std::vector<SkinnedMesh> meshes;
        std::unordered_map<std::string, BoneInfo> boneInfoMap;
        int boneCounter = 0;
        AssimpNodeData rootNode;
        glm::mat4 m_GlobalInverseTransform{ 1.0f };

        bool LoadModel(const std::string& path) {
            Assimp::Importer importer;
            const aiScene* scene = importer.ReadFile(path,
                aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs);

            if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
                SUN_CORE_ERROR("Assimp Error (SkinnedModel) {0}: {1}", path, importer.GetErrorString());
                return false;
            }

            ProcessNode(scene->mRootNode, scene);
            ReadHierarchyData(rootNode, scene->mRootNode);

            m_GlobalInverseTransform = glm::inverse(AiMatToGlm(scene->mRootNode->mTransformation));

            SUN_CORE_INFO("Modello skinnato caricato: {0} ({1} sub-mesh, {2} ossa)", path, meshes.size(), boneInfoMap.size());
            return true;
        }

        void Draw() const {
            for (const auto& mesh : meshes) mesh.Draw();
        }

    private:
        void ReadHierarchyData(AssimpNodeData& dest, const aiNode* src) {
            dest.name = src->mName.C_Str();
            dest.transformation = AiMatToGlm(src->mTransformation);
            dest.childrenCount = src->mNumChildren;

            for (unsigned int i = 0; i < src->mNumChildren; i++) {
                AssimpNodeData newData;
                ReadHierarchyData(newData, src->mChildren[i]);
                dest.children.push_back(newData);
            }
        }

        void ProcessNode(aiNode* node, const aiScene* scene) {
            for (unsigned int i = 0; i < node->mNumMeshes; i++) {
                aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
                meshes.push_back(ProcessMesh(mesh, scene));
            }
            for (unsigned int i = 0; i < node->mNumChildren; i++) {
                ProcessNode(node->mChildren[i], scene);
            }
        }

        SkinnedMesh ProcessMesh(aiMesh* mesh, const aiScene* scene) {
            std::vector<SkinnedVertex> vertices(mesh->mNumVertices);
            std::vector<unsigned int> indices;

            for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
                SkinnedVertex v{};
                v.Position = glm::vec3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);
                if (mesh->HasNormals())
                    v.Normal = glm::vec3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z);
                if (mesh->mTextureCoords[0])
                    v.TexCoords = glm::vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
                vertices[i] = v;
            }

            for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
                aiFace face = mesh->mFaces[i];
                for (unsigned int j = 0; j < face.mNumIndices; j++)
                    indices.push_back(face.mIndices[j]);
            }

            for (unsigned int boneIdx = 0; boneIdx < mesh->mNumBones; boneIdx++) {
                aiBone* bone = mesh->mBones[boneIdx];
                std::string boneName = bone->mName.C_Str();

                int boneID;
                auto it = boneInfoMap.find(boneName);
                if (it == boneInfoMap.end()) {
                    boneID = boneCounter++;
                    boneInfoMap[boneName] = { boneID, AiMatToGlm(bone->mOffsetMatrix) };
                }
                else {
                    boneID = it->second.id;
                }

                for (unsigned int w = 0; w < bone->mNumWeights; w++) {
                    unsigned int vertexId = bone->mWeights[w].mVertexId;
                    float weight = bone->mWeights[w].mWeight;
                    vertices[vertexId].AddBoneData(boneID, weight);
                }
            }

            return SkinnedMesh(std::move(vertices), std::move(indices));
        }
    };
}