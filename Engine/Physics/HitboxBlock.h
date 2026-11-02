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

        // Calcola le matrici di trasformazione per il Rendering (scala applicata alla fine)
        std::vector<glm::mat4> ComputeWorldTransforms(const glm::vec3& entityPos, const glm::vec3& entityRot, float entityScale = 1.0f) const {
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