#include "CombatVFX.h"

namespace SunEngine {

    void CombatVFX::SpawnSlashTrail(Scene& scene, entt::entity attacker, const TransformComponent& trans, const MeleeStateComponent& melee, const MeleeNode* node) {
        glm::vec4 slashColor = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);

        std::string nodeKey = melee.currentAttackNode;
        std::transform(nodeKey.begin(), nodeKey.end(), nodeKey.begin(), ::tolower);

        glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::vec3 fwd = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        glm::vec3 right = glm::normalize(glm::vec3(rot * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)));
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);

        glm::vec3 startPos(0.0f);
        glm::vec3 endPos(0.0f);
        float width = 0.20f;

        if (nodeKey.find("heavy") != std::string::npos || melee.currentAttack == MeleeAttackType::Heavy || (node && node->damageMultiplier >= 2.0f)) {
            slashColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            startPos = trans.Position + up * 2.2f + fwd * 0.5f + (nodeKey == "heavy_2" ? -right : right) * 0.4f;
            endPos = trans.Position + up * 0.2f + fwd * 1.6f + (nodeKey == "heavy_2" ? right : -right) * 0.3f;
            width = 0.30f;
        }
        else if (nodeKey.find("low") != std::string::npos || nodeKey.find("basso") != std::string::npos || melee.currentAttack == MeleeAttackType::Light) {
            slashColor = glm::vec4(0.0f, 0.4f, 1.0f, 1.0f);
            startPos = trans.Position + up * 0.5f + fwd * 0.4f;
            endPos = trans.Position + up * 0.45f + fwd * 1.7f + right * 0.6f;
            width = 0.42f;
        }
        else {
            slashColor = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
            if (nodeKey == "normal_1") {
                startPos = trans.Position + up * 1.25f + fwd * 0.8f + right * 1.1f;
                endPos = trans.Position + up * 1.15f + fwd * 1.5f - right * 1.1f;
            }
            else if (nodeKey == "normal_2") {
                startPos = trans.Position + up * 1.15f + fwd * 0.8f - right * 1.1f;
                endPos = trans.Position + up * 1.25f + fwd * 1.5f + right * 1.1f;
            }
            else {
                startPos = trans.Position + up * 1.3f + fwd * 0.7f + right * 0.8f;
                endPos = trans.Position + up * 0.9f + fwd * 1.4f - right * 0.8f;
            }
        }

        entt::entity slashEntity = scene.CreateEntity();
        auto& trail = scene.GetRegistry().emplace<TrailRequestComponent>(slashEntity);
        trail.start = startPos;
        trail.end = endPos;
        trail.color = slashColor;
        trail.isBeam = true;
        trail.projectileWidth = width;
        trail.travelTime = 0.05f;
        trail.totalLife = 0.38f;
    }

}