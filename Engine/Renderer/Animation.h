#pragma once
#include "SkinnedModel.h"
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdio>

namespace SunEngine {

    struct KeyPosition { glm::vec3 position{ 0.0f }; float timeStamp = 0.0f; };
    struct KeyRotation { glm::quat orientation{ 1.0f, 0.0f, 0.0f, 0.0f }; float timeStamp = 0.0f; };
    struct KeyScale { glm::vec3 scale{ 1.0f }; float timeStamp = 0.0f; };

    // Scompone una matrice locale di nodo in posizione / rotazione / scala.
    inline void DecomposeLocalBind(const glm::mat4& m, glm::vec3& outPos, glm::quat& outRot, glm::vec3& outScale) {
        outPos = glm::vec3(m[3]);
        glm::vec3 cx = glm::vec3(m[0]), cy = glm::vec3(m[1]), cz = glm::vec3(m[2]);
        float sx = glm::length(cx), sy = glm::length(cy), sz = glm::length(cz);
        outScale = glm::vec3(sx > 0.0001f ? sx : 1.0f, sy > 0.0001f ? sy : 1.0f, sz > 0.0001f ? sz : 1.0f);
        glm::mat3 rotMat(cx / outScale.x, cy / outScale.y, cz / outScale.z);
        outRot = glm::normalize(glm::quat_cast(rotMat));
    }

    class Bone {
    public:
        // animBindLocal = trasformazione locale del nodo nel file dell'ANIMAZIONE (la sua posa di riposo).
        Bone(const std::string& name, int id, const aiNodeAnim* channel, const glm::mat4& animBindLocal)
            : m_Name(name), m_ID(id) {
            DecomposeLocalBind(animBindLocal, m_AnimBindPos, m_AnimBindRot, m_AnimBindScale);
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

        // RETARGET (fix upside-down): le chiavi dell'animazione sono applicate come VARIAZIONE rispetto
        // alla posa di riposo dell'animazione, sopra la posa di riposo del MODELLO:
        //   rot   = modelBindRot * inverse(animBindRot) * keyRot
        //   pos   = modelBindPos + (keyPos - animBindPos) * rapportoUnita'
        //   scale = modelBindScale * keyScale / animBindScale
        // Con modello e clip sullo stesso rig (bind uguali) il risultato coincide con la chiave assoluta,
        // quindi l'idle (letta dal file del modello) non cambia. Con rig diversi (armature ruotata,
        // unita' cm/m, Hips con altro bind) evita che una rotazione costante capovolga il corpo.
        void Update(float animationTime, const glm::mat4& modelBindLocal) {
            glm::vec3 mPos, mScale;
            glm::quat mRot;
            DecomposeLocalBind(modelBindLocal, mPos, mRot, mScale);

            glm::vec3 keyPos = SamplePosition(animationTime);
            float ratio = 1.0f;
            float lm = glm::length(mPos), la = glm::length(m_AnimBindPos);
            if (lm > 1e-4f && la > 1e-4f) ratio = std::clamp(lm / la, 1e-3f, 1e3f);
            glm::vec3 pos = mPos + (keyPos - m_AnimBindPos) * ratio;

            glm::quat keyRot = SampleRotation(animationTime);
            glm::quat rot = glm::normalize(mRot * (glm::inverse(m_AnimBindRot) * keyRot));

            glm::vec3 keyScale = SampleScale(animationTime);
            glm::vec3 scale = mScale * (keyScale / m_AnimBindScale);

            m_LocalTransform = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot) * glm::scale(glm::mat4(1.0f), scale);
        }

        glm::mat4 GetLocalTransform() const { return m_LocalTransform; }
        const std::string& GetBoneName() const { return m_Name; }
        bool HasPositionKeys() const { return !m_Positions.empty(); }
        const glm::quat& GetAnimBindRot() const { return m_AnimBindRot; }
        const glm::vec3& GetAnimBindPos() const { return m_AnimBindPos; }

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

        // Campionamento chiavi. Senza chiavi: valore di riposo dell'animazione (=> delta nullo).
        glm::vec3 SamplePosition(float t) const {
            if (m_Positions.empty()) return m_AnimBindPos;
            if (m_Positions.size() == 1) return m_Positions[0].position;
            int p0 = GetPositionIndex(t), p1 = p0 + 1;
            float f = GetScaleFactor(m_Positions[p0].timeStamp, m_Positions[p1].timeStamp, t);
            return glm::mix(m_Positions[p0].position, m_Positions[p1].position, f);
        }
        glm::quat SampleRotation(float t) const {
            if (m_Rotations.empty()) return m_AnimBindRot;
            if (m_Rotations.size() == 1) return glm::normalize(m_Rotations[0].orientation);
            int p0 = GetRotationIndex(t), p1 = p0 + 1;
            float f = GetScaleFactor(m_Rotations[p0].timeStamp, m_Rotations[p1].timeStamp, t);
            return glm::normalize(glm::slerp(m_Rotations[p0].orientation, m_Rotations[p1].orientation, f));
        }
        glm::vec3 SampleScale(float t) const {
            if (m_Scales.empty()) return m_AnimBindScale;
            if (m_Scales.size() == 1) return m_Scales[0].scale;
            int p0 = GetScaleIndex(t), p1 = p0 + 1;
            float f = GetScaleFactor(m_Scales[p0].timeStamp, m_Scales[p1].timeStamp, t);
            return glm::mix(m_Scales[p0].scale, m_Scales[p1].scale, f);
        }

        std::vector<KeyPosition> m_Positions;
        std::vector<KeyRotation> m_Rotations;
        std::vector<KeyScale> m_Scales;
        glm::mat4 m_LocalTransform{ 1.0f };
        std::string m_Name;
        int m_ID = -1;
        glm::vec3 m_AnimBindPos{ 0.0f };
        glm::quat m_AnimBindRot{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 m_AnimBindScale{ 1.0f };
    };

    class Animation {
    public:
        bool LoadFromFile(const std::string& path, SkinnedModel* model) {
            Assimp::Importer importer;
            // Stessa impostazione di SkinnedModel::LoadModel (vedi li' la spiegazione): niente nodi
            // ausiliari $AssimpFbx$, altrimenti le chiavi vengono applicate sopra pivot gia' presenti.
            importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
            const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate);

            if (!scene || !scene->mRootNode || scene->mNumAnimations == 0) {
                SUN_CORE_ERROR("Animation Error {0}: nessuna animazione trovata / file non valido", path);
                return false;
            }

            m_GlobalInverseTransform = model->m_GlobalInverseTransform;

            aiAnimation* anim = scene->mAnimations[0];
            m_Duration = (float)anim->mDuration;
            m_TicksPerSecond = anim->mTicksPerSecond != 0 ? (float)anim->mTicksPerSecond : 25.0f;

            // Gerarchia del file ANIMAZIONE: serve solo per leggere le pose di riposo dei suoi nodi.
            AssimpNodeData animRoot;
            ReadHierarchyData(animRoot, scene->mRootNode);
            std::unordered_map<std::string, glm::mat4> animBind;
            CollectLocalBind(animRoot, animBind);

            // Gerarchia usata per il calcolo = quella del MODELLO, coerente con le sue offsetMatrix.
            m_RootNode = model->rootNode;
            std::unordered_set<std::string> modelNodes;
            CollectNodeNames(m_RootNode, modelNodes);
            std::unordered_map<std::string, glm::mat4> modelBind;
            CollectLocalBind(m_RootNode, modelBind);

            ReadMissingBones(anim, *model, animBind, modelNodes);

            SUN_CORE_INFO("Animazione caricata: {0} (durata: {1} tick, {2} ossa animate)", path, m_Duration, m_Bones.size());
            LogRetargetDiagnostics(path, animRoot, *model, animBind, modelBind);
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
        static std::string Describe(const glm::mat4& m) {
            glm::vec3 p, s;
            glm::quat r;
            DecomposeLocalBind(m, p, r, s);
            glm::vec3 e = glm::degrees(glm::eulerAngles(r));
            char buf[200];
            snprintf(buf, sizeof(buf), "pos(%.2f,%.2f,%.2f) rotEuler(%.1f,%.1f,%.1f) scala(%.3f,%.3f,%.3f)",
                p.x, p.y, p.z, e.x, e.y, e.z, s.x, s.y, s.z);
            return buf;
        }

        static void CollectLocalBind(const AssimpNodeData& n, std::unordered_map<std::string, glm::mat4>& out) {
            out[n.name] = n.transformation;
            for (const auto& c : n.children) CollectLocalBind(c, out);
        }
        static void CollectNodeNames(const AssimpNodeData& n, std::unordered_set<std::string>& out) {
            out.insert(n.name);
            for (const auto& c : n.children) CollectNodeNames(c, out);
        }

        void ReadMissingBones(const aiAnimation* animation, SkinnedModel& model,
            const std::unordered_map<std::string, glm::mat4>& animBind,
            const std::unordered_set<std::string>& modelNodes) {
            m_BoneInfoMap = model.boneInfoMap;

            int dropped = 0;
            std::string droppedNames;
            for (unsigned int i = 0; i < animation->mNumChannels; i++) {
                auto channel = animation->mChannels[i];
                std::string boneName = channel->mNodeName.C_Str();

                // Canale su un nodo che il modello non ha: non si puo' applicare. (Prima veniva creata
                // una BoneInfo fittizia modificando model.boneCounter.)
                if (modelNodes.find(boneName) == modelNodes.end()) {
                    dropped++;
                    if (dropped <= 5) droppedNames += boneName + "; ";
                    continue;
                }

                glm::mat4 ab(1.0f);
                auto ait = animBind.find(boneName);
                if (ait != animBind.end()) ab = ait->second;

                int id = -1;
                auto bit = m_BoneInfoMap.find(boneName);
                if (bit != m_BoneInfoMap.end()) id = bit->second.id;

                m_Bones.push_back(Bone(boneName, id, channel, ab));
            }

            if (dropped > 0) {
                SUN_CORE_WARN("Animazione: {0} canali su {1} NON presenti nel modello (ignorati). Primi: {2}",
                    dropped, animation->mNumChannels, droppedNames);
            }
        }

        // Diagnostica retarget: confronta le pose di riposo di modello e clip. Se nodo radice o Hips
        // differiscono molto (rotazione, scala, unita') e' la causa di un corpo capovolto/deformato.
        void LogRetargetDiagnostics(const std::string& path, const AssimpNodeData& animRoot, const SkinnedModel& model,
            const std::unordered_map<std::string, glm::mat4>& animBind,
            const std::unordered_map<std::string, glm::mat4>& modelBind) {
            SUN_CORE_INFO("[RETARGET] {0}", path);
            SUN_CORE_INFO("[RETARGET]   radice modello '{0}': {1}", model.rootNode.name, Describe(model.rootNode.transformation));
            SUN_CORE_INFO("[RETARGET]   radice animazione '{0}': {1}", animRoot.name, Describe(animRoot.transformation));
            int shown = 0;
            for (const auto& b : m_Bones) {
                if (!b.HasPositionKeys() && shown >= 2) continue;   // priorita' alle ossa con traslazione (Hips)
                auto m = modelBind.find(b.GetBoneName());
                auto a = animBind.find(b.GetBoneName());
                if (m == modelBind.end() || a == animBind.end()) continue;
                SUN_CORE_INFO("[RETARGET]   osso '{0}': modello {1} | anim {2}", b.GetBoneName(), Describe(m->second), Describe(a->second));
                if (++shown >= 4) break;
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