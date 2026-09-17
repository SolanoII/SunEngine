#pragma once

#include <glm/glm.hpp>

namespace SunEngine {

    // Layout allineato a std140 per std140 UBO (16-byte boundaries).
    // Ogni campo e' un vec4, la struct e' 64 byte esatti.
    enum LightType : int {
        LIGHT_DIRECTIONAL = 0,
        LIGHT_POINT = 1,
        LIGHT_SPOT = 2
    };

    struct alignas(16) Light {
        glm::vec4 positionRange{ 0.0f, 0.0f, 0.0f, 10.0f };  // xyz=pos, w=range
        glm::vec4 directionCone{ 0.0f, -1.0f, 0.0f, 0.8f };  // xyz=dir, w=cosOuterCone
        glm::vec4 colorIntensity{ 1.0f, 1.0f, 1.0f, 1.0f };  // rgb=color, w=intensity
        glm::ivec4 flags{ 0, 0, 0, 0 };                       // x=LightType

        static Light Directional(const glm::vec3& dir,
            const glm::vec3& color,
            float intensity) {
            Light l;
            l.directionCone = glm::vec4(glm::normalize(dir), 0.0f);
            l.colorIntensity = glm::vec4(color, intensity);
            l.flags.x = LIGHT_DIRECTIONAL;
            return l;
        }

        static Light Point(const glm::vec3& pos, float range,
            const glm::vec3& color, float intensity) {
            Light l;
            l.positionRange = glm::vec4(pos, range);
            l.colorIntensity = glm::vec4(color, intensity);
            l.flags.x = LIGHT_POINT;
            return l;
        }

        static Light Spot(const glm::vec3& pos, float range,
            const glm::vec3& dir, float outerConeCos,
            const glm::vec3& color, float intensity) {
            Light l;
            l.positionRange = glm::vec4(pos, range);
            l.directionCone = glm::vec4(glm::normalize(dir), outerConeCos);
            l.colorIntensity = glm::vec4(color, intensity);
            l.flags.x = LIGHT_SPOT;
            return l;
        }
    };

}