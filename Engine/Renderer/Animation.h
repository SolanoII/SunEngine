#pragma once
#include "SkinnedModel.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <string>
#include <algorithm>

namespace SunEngine {

    struct KeyPosition { glm::vec3 position{ 0.0f }; float timeStamp = 0.0f; };
    struct KeyRotation { glm::quat orientation{ 1.0f, 0.0f, 0.0f, 0.0f }; float timeStamp = 0.0f; };
    struct KeyScale { glm::vec3 scale{ 1.0f }; float timeStamp = 0.0f; };

    class Bone {
    public:
        Bone(const std::string& name, int id, const aiNodeAnim* channel) : m_Name(name), m_ID(id) {
            if (channel) {
                for (unsigned int i = 0; i < channel->mNumPositionKeys; i++) {
                    auto k = channel->mPositionKeys[i];
                    m_Positions.push_back({ glm::vec3(k.mValue.x, k.mValue.y, k.mValue.z), (float)k.mTime });
                }
                for (unsigned int i = 0; i < channel->mNumRotationKeys; i++) {
                    auto k = channel->mRotationKeys[i];
                    m_Rotations.push_back({ glm::quat(k.mValue.w, k.mValue.x, k.mValue.y, k.mValue.z), (float)k.mTime });
                }
                for (unsigned int i = 0; i < channel->mNumScalingKeys; i++) {
                    auto k = channel->mScalingKeys[i];
                    m_Scales.push_back({ glm::vec3(k.mValue.x, k.mValue.y, k.mValue.z), (float)k.mTime });
                }
            }
        }

        void Update(float animationTime, const glm::mat4& nodeBindTransform) {
            glm::vec3 bindPos = glm::vec3(nodeBindTransform[3]);

            glm::vec3 scaleX = glm::vec3(nodeBindTransform[0]);
            glm::vec3 scaleY = glm::vec3(nodeBindTransform[1]);
            glm::vec3 scaleZ = glm::vec3(nodeBindTransform[2]);

            float sx = glm::length(scaleX);
            float sy = glm::length(scaleY);
            float sz = glm::length(scaleZ);
            glm::vec3 bindScale(sx > 0.0001f ? sx : 1.0f, sy > 0.0001f ? sy : 1.0f, sz > 0.0001f ? sz : 1.0f);

            glm::mat3 rotMat(
                scaleX / bindScale.x,
                scaleY / bindScale.y,
                scaleZ / bindScale.z
            );
            glm::quat bindRot = glm::normalize(glm::quat_cast(rotMat));

            glm::mat4 translation = InterpolatePosition(animationTime, bindPos);
            glm::mat4 rotation = InterpolateRotation(animationTime, bindRot);
            glm::mat4 scale = InterpolateScale(animationTime, bindScale);

            m_LocalTransform = translation * rotation * scale;
        }

        glm::mat4 GetLocalTransform() const { return m_LocalTransform; }
        const std::string& GetBoneName() const { return m_Name; }

    private:
        int GetPositionIndex(float animationTime) const {
            for (int i = 0; i < (int)m_Positions.size() - 1; i++)
                if (animationTime < m_Positions[i + 1].timeStamp) return i;
            return (int)m_Positions.size() - 2 >= 0 ? (int)m_Positions.size() - 2 : 0;
        }
        int GetRotationIndex(float animationTime) const {
            for (int i = 0; i < (int)m_Rotations.size() - 1; i++)
                if (animationTime < m_Rotations[i + 1].timeStamp) return i;
            return (int)m_Rotations.size() - 2 >= 0 ? (int)m_Rotations.size() - 2 : 0;
        }
        int GetScaleIndex(float animationTime) const {
            for (int i = 0; i < (int)m_Scales.size() - 1; i++)
                if (animationTime < m_Scales[i + 1].timeStamp) return i;
            return (int)m_Scales.size() - 2 >= 0 ? (int)m_Scales.size() - 2 : 0;
        }

        float GetScaleFactor(float lastTime, float nextTime, float animationTime) const {
            float midWayLength = animationTime - lastTime;
            float framesDiff = nextTime - lastTime;
            if (framesDiff <= 0.0001f) return 0.0f;
            return std::clamp(midWayLength / framesDiff, 0.0f, 1.0f);
        }

        glm::mat4 InterpolatePosition(float animationTime, const glm::vec3& defaultPos) const {
            if (m_Positions.empty()) return glm::translate(glm::mat4(1.0f), defaultPos);
            if (m_Positions.size() == 1) return glm::translate(glm::mat4(1.0f), m_Positions[0].position);
            int p0 = GetPositionIndex(animationTime), p1 = p0 + 1;
            float t = GetScaleFactor(m_Positions[p0].timeStamp, m_Positions[p1].timeStamp, animationTime);
            glm::vec3 pos = glm::mix(m_Positions[p0].position, m_Positions[p1].position, t);
            return glm::translate(glm::mat4(1.0f), pos);
        }
        glm::mat4 InterpolateRotation(float animationTime, const glm::quat& defaultRot) const {
            if (m_Rotations.empty()) return glm::mat4_cast(defaultRot);
            if (m_Rotations.size() == 1) return glm::mat4_cast(glm::normalize(m_Rotations[0].orientation));
            int p0 = GetRotationIndex(animationTime), p1 = p0 + 1;
            float t = GetScaleFactor(m_Rotations[p0].timeStamp, m_Rotations[p1].timeStamp, animationTime);
            glm::quat rot = glm::slerp(m_Rotations[p0].orientation, m_Rotations[p1].orientation, t);
            return glm::mat4_cast(glm::normalize(rot));
        }
        glm::mat4 InterpolateScale(float animationTime, const glm::vec3& defaultScale) const {
            if (m_Scales.empty()) return glm::scale(glm::mat4(1.0f), defaultScale);
            if (m_Scales.size() == 1) return glm::scale(glm::mat4(1.0f), m_Scales[0].scale);
            int p0 = GetScaleIndex(animationTime), p1 = p0 + 1;
            float t = GetScaleFactor(m_Scales[p0].timeStamp, m_Scales[p1].timeStamp, animationTime);
            glm::vec3 sc = glm::mix(m_Scales[p0].scale, m_Scales[p1].scale, t);
            return glm::scale(glm::mat4(1.0f), sc);
        }

        std::vector<KeyPosition> m_Positions;
        std::vector<KeyRotation> m_Rotations;
        std::vector<KeyScale> m_Scales;
        glm::mat4 m_LocalTransform{ 1.0f };
        std::string m_Name;
        int m_ID = -1;
    };

    class Animation {
    public:
        bool LoadFromFile(const std::string& path, SkinnedModel* model) {
            Assimp::Importer importer;
            const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate);

            if (!scene || !scene->mRootNode || scene->mNumAnimations == 0) {
                SUN_CORE_ERROR("Animation Error {0}: nessuna animazione trovata / file non valido", path);
                return false;
            }

            m_GlobalInverseTransform = model->m_GlobalInverseTransform;

            aiAnimation* anim = scene->mAnimations[0];
            m_Duration = (float)anim->mDuration;
            m_TicksPerSecond = anim->mTicksPerSecond != 0 ? (float)anim->mTicksPerSecond : 25.0f;

            ReadHierarchyData(m_RootNode, scene->mRootNode);
            ReadMissingBones(anim, *model);

            SUN_CORE_INFO("Animazione caricata: {0} (durata: {1} tick, {2} ossa animate)", path, m_Duration, m_Bones.size());
            return true;
        }

        Bone* FindBone(const std::string& name) {
            for (auto& b : m_Bones) if (b.GetBoneName() == name) return &b;
            return nullptr;
        }

        float GetTicksPerSecond() const { return m_TicksPerSecond; }
        float GetDuration() const { return m_Duration; }
        const AssimpNodeData& GetRootNode() const { return m_RootNode; }
        const std::unordered_map<std::string, BoneInfo>& GetBoneIDMap() const { return m_BoneInfoMap; }
        const glm::mat4& GetGlobalInverseTransform() const { return m_GlobalInverseTransform; }

    private:
        void ReadMissingBones(const aiAnimation* animation, SkinnedModel& model) {
            m_BoneInfoMap = model.boneInfoMap;

            for (unsigned int i = 0; i < animation->mNumChannels; i++) {
                auto channel = animation->mChannels[i];
                std::string boneName = channel->mNodeName.C_Str();

                if (m_BoneInfoMap.find(boneName) == m_BoneInfoMap.end()) {
                    BoneInfo info{};
                    info.id = model.boneCounter++;
                    info.offsetMatrix = glm::mat4(1.0f);
                    m_BoneInfoMap[boneName] = info;
                }
                m_Bones.push_back(Bone(boneName, m_BoneInfoMap[boneName].id, channel));
            }
        }

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

        float m_Duration = 0.0f;
        float m_TicksPerSecond = 25.0f;
        std::vector<Bone> m_Bones;
        AssimpNodeData m_RootNode;
        std::unordered_map<std::string, BoneInfo> m_BoneInfoMap;
        glm::mat4 m_GlobalInverseTransform{ 1.0f };
    };
}