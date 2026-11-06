#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <utility>          // per std::move

#include "SkinnedMesh.h"
#include "SkinnedMaterial.h"
#include "BoneNames.h"
#include "../Core/Logger/Log.h"
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
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

    class SkinnedModel;
    // Materiali e texture di un modello d'autore (AuthoredModel.cpp): letti dalla scena appena importata
    void ReadSkinnedMaterials(const aiScene* scene, const std::string& modelPath, SkinnedModel& model);

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
        bool bakedSkin = false;   // coordinate texture = curvatura e occlusione della pelle (vedi MuscleTone.h)
        // Modello d'autore (AuthoredModel.h): texture PBR proprie per ogni sotto-mesh, niente dettagli procedurali,
        // niente vestiti generati. unitScale porta le sue unita' a quelle dei modelli del gioco (centimetri).
        bool authored = false;
        std::vector<SkinnedMaterial> materials;
        float unitScale = 1.0f;

        // withMaterials = true: tangenti per le normal map e materiali/texture del file (modelli d'autore)
        bool LoadModel(const std::string& path, bool withMaterials = false) {
            Assimp::Importer importer;
            // FIX animazioni capovolte: di default Assimp spezza ogni osso FBX in nodi ausiliari
            // (X_$AssimpFbx$_Translation / _PreRotation / _Rotation) lasciando il nodo dell'osso con
            // trasformazione identica, mentre le chiavi di animazione contengono la trasformazione
            // locale COMPLETA. Applicate al nodo dell'osso, traslazione e pre-rotazione venivano contate
            // due volte (piedi sopra la testa). Con PRESERVE_PIVOTS=false ogni osso e' un solo nodo.
            // DEVE essere identico in Animation::LoadFromFile: stessa gerarchia in modello e clip.
            importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
            unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs;
            if (withMaterials) flags |= aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights;   // max 4 ossa per vertice
            m_WithMaterials = withMaterials;
            const aiScene* scene = importer.ReadFile(path, flags);

            if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
                SUN_CORE_ERROR("Assimp Error (SkinnedModel) {0}: {1}", path, importer.GetErrorString());
                return false;
            }

            ProcessNode(scene->mRootNode, scene);
            ReadHierarchyData(rootNode, scene->mRootNode);

            m_GlobalInverseTransform = glm::inverse(AiMatToGlm(scene->mRootNode->mTransformation));
            if (withMaterials) ReadSkinnedMaterials(scene, path, *this);

            SUN_CORE_INFO("Modello skinnato caricato: {0} ({1} sub-mesh, {2} ossa)", path, meshes.size(), boneInfoMap.size());
            return true;
        }

        void Draw() const {
            for (const auto& mesh : meshes) mesh.Draw();
        }

    private:
        bool m_WithMaterials = false;

        void ReadHierarchyData(AssimpNodeData& dest, const aiNode* src) {
            // nomi delle ossa portati a quelli del gioco (BoneNames.h): per i modelli Mixamo del gioco non cambia nulla
            dest.name = CanonicalBoneName(src->mName.C_Str());
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
                if (mesh->HasTangentsAndBitangents() && mesh->HasNormals()) {
                    const glm::vec3 T(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z);
                    const glm::vec3 B(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z);
                    const float handed = glm::dot(glm::cross(v.Normal, T), B) < 0.0f ? -1.0f : 1.0f;
                    v.Tangent = glm::vec4(T, handed);
                }
                vertices[i] = v;
            }

            for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
                aiFace face = mesh->mFaces[i];
                for (unsigned int j = 0; j < face.mNumIndices; j++)
                    indices.push_back(face.mIndices[j]);
            }

            for (unsigned int boneIdx = 0; boneIdx < mesh->mNumBones; boneIdx++) {
                aiBone* bone = mesh->mBones[boneIdx];
                std::string boneName = CanonicalBoneName(bone->mName.C_Str());

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

            SkinnedMesh out(std::move(vertices), std::move(indices));
            if (m_WithMaterials) out.materialIndex = static_cast<int>(mesh->mMaterialIndex);
            return out;
        }
    };
}