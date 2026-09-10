#pragma once
#include <glm/glm.hpp>

namespace SunEngine {

    static const int MAX_BONE_INFLUENCE = 4;

    struct SkinnedVertex {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Normal{ 0.0f };
        glm::vec2 TexCoords{ 0.0f };
        int   BoneIDs[MAX_BONE_INFLUENCE] = { -1, -1, -1, -1 };
        float Weights[MAX_BONE_INFLUENCE] = { 0.0f, 0.0f, 0.0f, 0.0f };

        void AddBoneData(int boneID, float weight) {
            if (weight <= 0.0001f) return; // Ignora pesi trascurabili
            for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) {
                if (Weights[i] == 0.0f) {
                    BoneIDs[i] = boneID;
                    Weights[i] = weight;
                    return;
                }
            }
        }
    };

    struct BoneInfo {
        int id = -1;
        glm::mat4 offsetMatrix{ 1.0f };
    };
}