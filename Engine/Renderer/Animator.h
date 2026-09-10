#pragma once
#include "Animation.h"

namespace SunEngine {

    static const int ANIMATOR_MAX_BONES = 100;

    class Animator {
    public:
        void PlayAnimation(Animation* animation) {
            m_CurrentAnimation = animation;
            m_CurrentTime = 0.0f;
            m_FinalBoneMatrices.assign(ANIMATOR_MAX_BONES, glm::mat4(1.0f));
        }

        void UpdateAnimation(float deltaTime) {
            if (!m_CurrentAnimation) return;

            m_CurrentTime += m_CurrentAnimation->GetTicksPerSecond() * deltaTime;
            m_CurrentTime = std::fmod(m_CurrentTime, m_CurrentAnimation->GetDuration());

            CalculateBoneTransform(m_CurrentAnimation->GetRootNode(), glm::mat4(1.0f));
        }

        const std::vector<glm::mat4>& GetFinalBoneMatrices() const { return m_FinalBoneMatrices; }

    private:
        void CalculateBoneTransform(const AssimpNodeData& node, glm::mat4 parentTransform) {
            glm::mat4 nodeTransform = node.transformation;

            Bone* bone = m_CurrentAnimation->FindBone(node.name);
            if (bone) {
                bone->Update(m_CurrentTime, node.transformation);
                nodeTransform = bone->GetLocalTransform();
            }

            glm::mat4 globalTransform = parentTransform * nodeTransform;

            const auto& boneInfoMap = m_CurrentAnimation->GetBoneIDMap();
            auto it = boneInfoMap.find(node.name);
            if (it != boneInfoMap.end() && it->second.id < ANIMATOR_MAX_BONES) {
                m_FinalBoneMatrices[it->second.id] = m_CurrentAnimation->GetGlobalInverseTransform() * globalTransform * it->second.offsetMatrix;
            }

            for (const auto& child : node.children) {
                CalculateBoneTransform(child, globalTransform);
            }
        }

        std::vector<glm::mat4> m_FinalBoneMatrices;
        Animation* m_CurrentAnimation = nullptr;
        float m_CurrentTime = 0.0f;
    };
}