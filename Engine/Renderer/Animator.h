#pragma once
#include "Animation.h"
#include "ProceduralPose.h"
#include <unordered_map>
#include <string>
#include <cmath>

namespace SunEngine {

    static const int ANIMATOR_MAX_BONES = 100;

    class Animator {
    public:
        // blendDuration > 0: crossfade dalla clip corrente alla nuova (si mescolano posizione/rotazione/
        // scala di ogni osso, non le matrici). 0 = cambio secco come prima.
        // loop = false: la clip viene riprodotta una volta e poi tiene l'ultimo fotogramma (IsFinished()).
        // Si puo' rilanciare la stessa clip (es. un nuovo colpo di rinculo): il blend parte dalla posa attuale.
        void PlayAnimation(Animation* animation, float blendDuration = 0.0f, bool loop = true) {
            m_Loop = loop;
            m_Finished = false;
            if (m_CurrentAnimation && blendDuration > 0.0f) {
                m_PrevAnimation = m_CurrentAnimation;
                m_PrevTime = m_CurrentTime;
                m_BlendDuration = blendDuration;
                m_BlendElapsed = 0.0f;
            }
            else {
                m_PrevAnimation = nullptr;
                m_BlendDuration = 0.0f;
                m_BlendElapsed = 0.0f;
            }
            m_CurrentAnimation = animation;
            m_CurrentTime = 0.0f;
            if (m_FinalBoneMatrices.size() != static_cast<size_t>(ANIMATOR_MAX_BONES)) {
                m_FinalBoneMatrices.assign(ANIMATOR_MAX_BONES, glm::mat4(1.0f));
            }
        }

        void UpdateAnimation(float deltaTime) {
            if (!m_CurrentAnimation) return;

            m_CurrentTime += m_CurrentAnimation->GetTicksPerSecond() * deltaTime * m_PlaybackRate;
            if (m_Loop) {
                m_CurrentTime = std::fmod(m_CurrentTime, m_CurrentAnimation->GetDuration());
            }
            else if (m_CurrentTime >= m_CurrentAnimation->GetDuration()) {
                m_CurrentTime = m_CurrentAnimation->GetDuration() - 0.001f;   // tiene l'ultimo fotogramma
                m_Finished = true;
            }

            if (m_PrevAnimation) {
                m_PrevTime += m_PrevAnimation->GetTicksPerSecond() * deltaTime;
                m_PrevTime = std::fmod(m_PrevTime, m_PrevAnimation->GetDuration());
                m_BlendElapsed += deltaTime;
                if (m_BlendElapsed >= m_BlendDuration) m_PrevAnimation = nullptr;
            }

            CalculateBoneTransform(m_CurrentAnimation->GetRootNode(), glm::mat4(1.0f));
        }

        const std::vector<glm::mat4>& GetFinalBoneMatrices() const { return m_FinalBoneMatrices; }

        bool IsFinished() const { return m_Finished; }
        // Posa procedurale sopra la clip (vedi ProceduralPose.h). nullptr = solo la clip. Il puntatore deve restare
        // valido fino a UpdateAnimation.
        void SetProceduralPose(const ProceduralPose* pose) { m_Pose = pose; }
        void SetPlaybackRate(float rate) { m_PlaybackRate = rate; }

        // Trasformazione di un osso nello spazio del MODELLO (stesso spazio dei vertici, prima della
        // matrice del mondo dell'entity). Serve ad agganciare oggetti (armi) alle ossa.
        bool GetBoneModelTransform(const std::string& boneName, glm::mat4& out) const {
            auto it = m_ModelTransforms.find(boneName);
            if (it == m_ModelTransforms.end()) return false;
            out = it->second;
            return true;
        }

    private:
        static void EvalNode(Animation* anim, const AssimpNodeData& node, float time,
            glm::vec3& p, glm::quat& r, glm::vec3& s) {
            Bone* bone = anim ? anim->FindBone(node.name) : nullptr;
            if (bone) {
                bone->Evaluate(time, node.transformation, p, r, s);
            }
            else {
                DecomposeLocalBind(node.transformation, p, r, s);
            }
        }

        void CalculateBoneTransform(const AssimpNodeData& node, glm::mat4 parentTransform) {
            glm::mat4 nodeTransform = node.transformation;

            Bone* bone = m_CurrentAnimation->FindBone(node.name);
            if (m_PrevAnimation) {
                // crossfade: mescola la posa della clip precedente con quella corrente
                if (bone || m_PrevAnimation->FindBone(node.name)) {
                    glm::vec3 p1, s1, p0, s0;
                    glm::quat r1, r0;
                    EvalNode(m_CurrentAnimation, node, m_CurrentTime, p1, r1, s1);
                    EvalNode(m_PrevAnimation, node, m_PrevTime, p0, r0, s0);
                    float a = m_BlendDuration > 0.0001f ? std::clamp(m_BlendElapsed / m_BlendDuration, 0.0f, 1.0f) : 1.0f;
                    glm::vec3 p = glm::mix(p0, p1, a);
                    glm::vec3 s = glm::mix(s0, s1, a);
                    glm::quat r = glm::normalize(glm::slerp(r0, r1, a));
                    nodeTransform = glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
                }
            }
            else if (bone) {
                bone->Update(m_CurrentTime, node.transformation);
                nodeTransform = bone->GetLocalTransform();
            }

            glm::mat4 globalTransform = parentTransform * nodeTransform;
            if (m_Pose && !m_Pose->Empty()) {
                // Posa procedurale: rotazione attorno al giunto (e spostamento delle anche) nello spazio della mesh,
                // poi di nuovo nello spazio dei nodi. I figli partono dalla trasformazione gia' modificata.
                const auto colon = node.name.rfind(':');
                const std::string suffix = colon == std::string::npos ? node.name : node.name.substr(colon + 1);
                auto rit = m_Pose->rotations.find(suffix);
                const bool isHips = suffix == "Hips" && glm::dot(m_Pose->hipsOffset, m_Pose->hipsOffset) > 1e-8f;
                if (rit != m_Pose->rotations.end() || isHips) {
                    const glm::mat4& gi = m_CurrentAnimation->GetGlobalInverseTransform();
                    glm::mat4 mesh = gi * globalTransform;
                    if (isHips) mesh = glm::translate(glm::mat4(1.0f), m_Pose->hipsOffset) * mesh;
                    if (rit != m_Pose->rotations.end()) {
                        const glm::vec3 pivot(mesh[3]);
                        mesh = glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(rit->second) * glm::translate(glm::mat4(1.0f), -pivot) * mesh;
                    }
                    globalTransform = glm::inverse(gi) * mesh;
                }
            }
            m_ModelTransforms[node.name] = m_CurrentAnimation->GetGlobalInverseTransform() * globalTransform;

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
        std::unordered_map<std::string, glm::mat4> m_ModelTransforms;
        Animation* m_CurrentAnimation = nullptr;
        float m_CurrentTime = 0.0f;

        Animation* m_PrevAnimation = nullptr;
        float m_PrevTime = 0.0f;
        float m_BlendDuration = 0.0f;
        float m_BlendElapsed = 0.0f;

        const ProceduralPose* m_Pose = nullptr;
        bool m_Loop = true;
        bool m_Finished = false;
        float m_PlaybackRate = 1.0f;
    };
}
