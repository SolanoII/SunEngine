#pragma once
#ifndef SUN_HITBOX_BLOCK_H
#define SUN_HITBOX_BLOCK_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <string>

namespace SunEngine {

    struct HitboxBlock {
        std::string name;
        int parentIndex = -1;           // -1 = Torso (Radice), 0+ = Indice blocco Padre
        glm::vec3 localOffset{ 0.0f };  // Posizione dello snodo (es. spalla) rispetto al padre
        glm::vec3 pivotOffset{ 0.0f };  // Shift del blocco rispetto allo snodo (es. braccio scende dalla spalla)
        glm::vec3 size{ 1.0f };         // Dimensioni del blocco (X, Y, Z)
        glm::vec3 jointRotation{ 0.0f };// Rotazione autonoma dell'articolazione
        // Rotazione di RIPOSO (gradi, campo "rotation" del JSON): postura fissa del blocco, es. il torso
        // ingobbito degli orchi. Si applica PRIMA di jointRotation, che resta libera per le animazioni.
        glm::vec3 restRotation{ 0.0f };
    };

    // Rotazione euleriana in gradi, ordine Y, X, Z (lo stesso usato per jointRotation)
    inline glm::mat4 RotateEulerYXZ(const glm::mat4& m, const glm::vec3& deg) {
        glm::mat4 r = glm::rotate(m, glm::radians(deg.y), glm::vec3(0.0f, 1.0f, 0.0f));
        r = glm::rotate(r, glm::radians(deg.x), glm::vec3(1.0f, 0.0f, 0.0f));
        return glm::rotate(r, glm::radians(deg.z), glm::vec3(0.0f, 0.0f, 1.0f));
    }

    struct BlockPhysicsTransform {
        glm::vec3 blockCenterPos{ 0.0f };
        glm::vec3 jointWorldPos{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    };

    struct SkeletonData {
        std::string id;
        std::string race;
        std::vector<HitboxBlock> blocks;

        // Hitbox guidate dalle ossa del corpo animato (BoneHitboxes.h): matrici nel mondo gia' pronte (con la
        // scala), centri delle articolazioni e misure. Valgono per l'entita' in drivenAt; se l'entita' si e'
        // spostata dopo (stesso fotogramma) si traslano. driven = false: i cubi del JSON come prima.
        bool driven = false;
        glm::vec3 drivenAt{ 0.0f };
        std::vector<glm::mat4> drivenWorld;
        std::vector<glm::vec3> drivenJoint;
        std::vector<glm::vec3> drivenSize;
        bool IsDriven() const { return driven && drivenWorld.size() == blocks.size() && drivenJoint.size() == blocks.size() && drivenSize.size() == blocks.size(); }
        // Misure del blocco nel mondo (ragdoll)
        glm::vec3 BlockWorldSize(size_t i, float entityScale) const {
            if (IsDriven()) return drivenSize[i];
            return blocks[i].size * entityScale;
        }

        // Calcola le matrici di trasformazione per il Rendering (scala applicata alla fine)
        std::vector<glm::mat4> ComputeWorldTransforms(const glm::vec3& entityPos, const glm::vec3& entityRot, float entityScale = 1.0f) const {
            if (IsDriven()) {
                const glm::mat4 shift = glm::translate(glm::mat4(1.0f), entityPos - drivenAt);
                std::vector<glm::mat4> out(blocks.size());
                for (size_t i = 0; i < blocks.size(); ++i) out[i] = shift * drivenWorld[i];
                return out;
            }
            std::vector<glm::mat4> worldTransforms(blocks.size());
            std::vector<glm::mat4> jointWorldMatrices(blocks.size());

            glm::mat4 rootEntityMatrix = glm::translate(glm::mat4(1.0f), entityPos);
            rootEntityMatrix = glm::rotate(rootEntityMatrix, glm::radians(entityRot.y), glm::vec3(0.0f, 1.0f, 0.0f));
            rootEntityMatrix = glm::rotate(rootEntityMatrix, glm::radians(entityRot.x), glm::vec3(1.0f, 0.0f, 0.0f));
            rootEntityMatrix = glm::rotate(rootEntityMatrix, glm::radians(entityRot.z), glm::vec3(0.0f, 0.0f, 1.0f));

            for (size_t i = 0; i < blocks.size(); ++i) {
                const auto& block = blocks[i];

                glm::mat4 parentJointMat = (block.parentIndex >= 0 && block.parentIndex < static_cast<int>(i))
                    ? jointWorldMatrices[block.parentIndex]
                    : rootEntityMatrix;

                glm::mat4 currentJointMat = glm::translate(parentJointMat, block.localOffset * entityScale);
                currentJointMat = RotateEulerYXZ(currentJointMat, block.restRotation);    // postura di riposo
                currentJointMat = RotateEulerYXZ(currentJointMat, block.jointRotation);   // animazione

                jointWorldMatrices[i] = currentJointMat;

                glm::mat4 blockRenderMat = glm::translate(currentJointMat, block.pivotOffset * entityScale);
                worldTransforms[i] = glm::scale(blockRenderMat, block.size * entityScale);
            }

            return worldTransforms;
        }

        // Calcola trasformazioni ortonormali (SENZA SCALA) per i corpi rigidi di Jolt Physics
        std::vector<BlockPhysicsTransform> ComputePhysicsTransforms(const glm::vec3& entityPos, const glm::vec3& entityRot, float entityScale = 1.0f) const {
            std::vector<BlockPhysicsTransform> physicsTransforms(blocks.size());
            if (IsDriven()) {
                // Il ragdoll parte dalla posa vera del corpo animato. entityPos/entityRot: dove nasce il ragdoll
                // (di solito la stessa posizione dell'unita'), si trasla rispetto a drivenAt.
                (void)entityRot; (void)entityScale;
                const glm::vec3 d = entityPos - drivenAt;
                for (size_t i = 0; i < blocks.size(); ++i) {
                    const glm::mat4& m = drivenWorld[i];
                    glm::mat3 r = glm::mat3(glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2]));
                    for (int k = 0; k < 3; ++k) { const float l = glm::length(r[k]); r[k] = l > 1e-6f ? r[k] / l : glm::vec3(k == 0, k == 1, k == 2); }
                    physicsTransforms[i].blockCenterPos = glm::vec3(m[3]) + d;
                    physicsTransforms[i].jointWorldPos = drivenJoint[i] + d;
                    physicsTransforms[i].rotation = glm::normalize(glm::quat_cast(r));
                }
                return physicsTransforms;
            }
            std::vector<glm::mat4> jointWorldMatrices(blocks.size());

            glm::mat4 rootEntityMatrix = glm::translate(glm::mat4(1.0f), entityPos);
            rootEntityMatrix = glm::rotate(rootEntityMatrix, glm::radians(entityRot.y), glm::vec3(0.0f, 1.0f, 0.0f));
            rootEntityMatrix = glm::rotate(rootEntityMatrix, glm::radians(entityRot.x), glm::vec3(1.0f, 0.0f, 0.0f));
            rootEntityMatrix = glm::rotate(rootEntityMatrix, glm::radians(entityRot.z), glm::vec3(0.0f, 0.0f, 1.0f));

            for (size_t i = 0; i < blocks.size(); ++i) {
                const auto& block = blocks[i];

                glm::mat4 parentJointMat = (block.parentIndex >= 0 && block.parentIndex < static_cast<int>(i))
                    ? jointWorldMatrices[block.parentIndex]
                    : rootEntityMatrix;

                glm::mat4 currentJointMat = glm::translate(parentJointMat, block.localOffset * entityScale);
                currentJointMat = RotateEulerYXZ(currentJointMat, block.restRotation);    // postura di riposo
                currentJointMat = RotateEulerYXZ(currentJointMat, block.jointRotation);   // animazione

                jointWorldMatrices[i] = currentJointMat;

                physicsTransforms[i].jointWorldPos = glm::vec3(currentJointMat[3]);
                physicsTransforms[i].blockCenterPos = glm::vec3(glm::translate(currentJointMat, block.pivotOffset * entityScale)[3]);
                physicsTransforms[i].rotation = glm::normalize(glm::quat_cast(currentJointMat));
            }

            return physicsTransforms;
        }
    };
}

#endif // SUN_HITBOX_BLOCK_H