#include "CombatVFX.h"
#include "DamageResolver.h"
#include "VfxEvents.h"

namespace SunEngine {

    void CombatVFX::SpawnSlashTrail(Scene& scene, entt::entity attacker, const TransformComponent& trans, const MeleeStateComponent& melee, const MeleeNode* node) {

        std::string nodeKey = melee.currentAttackNode;
        std::transform(nodeKey.begin(), nodeKey.end(), nodeKey.begin(), ::tolower);

        glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(trans.AimYaw), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::vec3 fwd = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        glm::vec3 right = glm::normalize(glm::vec3(rot * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)));
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);

        glm::vec3 startPos(0.0f);
        glm::vec3 endPos(0.0f);

        if (nodeKey.find("heavy") != std::string::npos || melee.currentAttack == MeleeAttackType::Heavy || (node && node->damageMultiplier >= 2.0f)) {
            startPos = trans.Position + up * 2.2f + fwd * 0.5f + (nodeKey == "heavy_2" ? -right : right) * 0.4f;
            endPos = trans.Position + up * 0.2f + fwd * 1.6f + (nodeKey == "heavy_2" ? right : -right) * 0.3f;
        }
        else if (nodeKey.find("low") != std::string::npos || nodeKey.find("basso") != std::string::npos || melee.currentAttack == MeleeAttackType::Light) {
            startPos = trans.Position + up * 0.45f + fwd * 0.7f + right * 0.9f;   // spazzata bassa da destra a sinistra
            endPos = trans.Position + up * 0.35f + fwd * 0.9f - right * 0.9f;
        }
        else {
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

        // Arco del fendente: dall'inizio alla fine passando davanti al corpo (punto di controllo spinto in avanti)
        VfxEvent e;
        e.kind = VfxKind::MeleeSlash;
        e.pos = startPos; e.pos2 = endPos;
        e.dir = (startPos + endPos) * 0.5f + fwd * 0.75f + up * 0.1f;
        const bool heavy = nodeKey.find("heavy") != std::string::npos || melee.currentAttack == MeleeAttackType::Heavy || (node && node->damageMultiplier >= 2.0f);
        const bool low = !heavy && (nodeKey.find("low") != std::string::npos || nodeKey.find("basso") != std::string::npos || melee.currentAttack == MeleeAttackType::Light);
        e.variant = heavy ? 2 : (low ? 0 : 1);
        e.scale = 0.15f;
        if (node) {
            const MeleeTiming tm = DamageResolver::EffectiveTiming(*node, melee.currentAttack, melee.currentAttackNode);
            e.scale = tm.active;
        }
        const auto* wbp = DamageResolver::ResolveMeleeWeapon(scene.GetRegistry(), attacker);
        e.flag = wbp && (wbp->id.find("chain") != std::string::npos || wbp->id.find("eviscerator") != std::string::npos);
        EmitVfx(e);
        { FeelEvent fe; fe.kind = FeelKind::MeleeSwing; fe.pos = (startPos + endPos) * 0.5f; fe.variant = e.variant; fe.flag = e.flag;
          fe.source = static_cast<std::uint32_t>(attacker); EmitFeel(fe); }
    }

}