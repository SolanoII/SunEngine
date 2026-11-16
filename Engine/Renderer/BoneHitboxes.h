#pragma once
// Hitbox dal corpo animato: i blocchi dello scheletro a cubi (Torso, Head, Arm_L/R, Leg_L/R) misurati sulla
// mesh e mossi dalle ossa a ogni fotogramma.
//
// Prima le hitbox erano cubi scritti a mano nel JSON dello scheletro e animati per conto loro: non seguivano le
// clip (accovacciato, carica, colpi) e non avevano le misure del corpo. Ora:
//   1. al caricamento del corpo (Build) per ogni blocco si prende un segmento d'ossa (anche -> collo, braccio ->
//      mano, coscia -> piede, collo -> cima della testa) e si misurano i vertici della mesh che appartengono a
//      quel blocco (osso con il peso maggiore), nel riferimento del segmento: dal 3% al 97% su ogni asse;
//   2. a ogni fotogramma (Drive) il riferimento segue le ossa animate: l'asse lungo va dall'osso d'inizio a
//      quello di fine (il braccio piegato accorcia il blocco), gli altri ruotano con l'osso d'inizio.
// Cosi' quando l'artista consegna il modello vero (AuthoredModel.h) le hitbox si rifanno da sole sulla sua mesh.
// I blocchi in piu' degli scheletri a cubi (gobba, mascella, spalle e pugni degli orchi) restano senza misura:
// li coprono gia' i blocchi principali, misurati sul corpo vero.
#include "SkinnedModel.h"
#include "BodySculpt.h"
#include "../Physics/HitboxBlock.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SunEngine {

    struct BoneHitboxTemplate {
        struct Box {
            std::string block;              // nome del blocco nello scheletro a cubi
            std::string startBone, endBone; // segmento d'ossa (senza prefisso)
            glm::vec3 lo{ 0.0f }, hi{ 0.0f };   // ingombro nel riferimento del segmento (unita' della mesh)
            float bindLen = 1.0f;
            glm::mat3 bindFrame{ 1.0f };    // assi del segmento in posa di riposo (colonne X, Y, Z)
            glm::mat3 bindBoneRot{ 1.0f };  // rotazione dell'osso d'inizio in posa di riposo
            bool limb = false;              // l'asse lungo si allunga/accorcia col segmento
            bool valid = false;
        };
        std::vector<Box> boxes;
        std::string prefix;                 // prefisso delle ossa del modello ("mixamorig1:")
        std::unordered_map<std::string, glm::mat4> bindBones;   // ossa in posa di riposo (manichini in posa a T)
        bool valid = false;
        bool BindBone(const std::string& n, glm::mat4& out) const {
            auto it = bindBones.find(n); if (it == bindBones.end()) return false; out = it->second; return true;
        }
    };

    namespace BoneHitboxDetail {
        inline glm::mat3 Ortho(const glm::mat3& m) {
            glm::mat3 r = m;
            for (int k = 0; k < 3; ++k) { const float l = glm::length(r[k]); r[k] = l > 1e-8f ? r[k] / l : glm::vec3(k == 0, k == 1, k == 2); }
            return r;
        }
        // Riferimento del segmento: Y lungo il segmento, X il piu' vicino possibile a xRef
        inline glm::mat3 Frame(const glm::vec3& a, const glm::vec3& b, const glm::vec3& xRef) {
            glm::vec3 y = b - a;
            y = glm::length(y) > 1e-8f ? glm::normalize(y) : glm::vec3(0.0f, 1.0f, 0.0f);
            glm::vec3 x = xRef - y * glm::dot(xRef, y);
            if (glm::length(x) < 1e-4f) x = glm::cross(y, glm::vec3(0.0f, 0.0f, 1.0f));
            x = glm::normalize(x);
            return glm::mat3(x, y, glm::cross(x, y));
        }
        inline float Percentile(std::vector<float>& v, float q) {
            if (v.empty()) return 0.0f;
            const size_t i = std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1)));
            std::nth_element(v.begin(), v.begin() + i, v.end());
            return v[i];
        }
        // A quale blocco appartiene un osso
        inline int BlockOfBone(const std::string& b) {
            auto starts = [&](const char* p) { return b.rfind(p, 0) == 0; };
            if (b == "Neck" || starts("Head") || b == "Jaw" || b.find("Ear") != std::string::npos) return 1;
            if (starts("LeftArm") || starts("LeftForeArm") || starts("LeftHand") || b == "LeftPauldron") return 2;
            if (starts("RightArm") || starts("RightForeArm") || starts("RightHand") || b == "RightPauldron") return 3;
            if (starts("LeftUpLeg") || b == "LeftLeg" || starts("LeftFoot") || starts("LeftToe")) return 4;
            if (starts("RightUpLeg") || b == "RightLeg" || starts("RightFoot") || starts("RightToe")) return 5;
            return 0;   // anche, colonna, clavicole, pancia
        }
    }

    // Misura dei blocchi sulla mesh del corpo (posa di riposo)
    inline BoneHitboxTemplate BuildBoneHitboxes(SkinnedModel& model) {
        using namespace BoneHitboxDetail;
        BoneHitboxTemplate t;
        BodySculptor s(model);
        const char* names[6] = { "Torso", "Head", "Arm_L", "Arm_R", "Leg_L", "Leg_R" };
        const char* seg[6][2] = { { "Hips", "Neck" }, { "Neck", "HeadTop_End" }, { "LeftArm", "LeftHand" }, { "RightArm", "RightHand" },
                                  { "LeftUpLeg", "LeftFoot" }, { "RightUpLeg", "RightFoot" } };
        if (!s.Has("Hips") || !s.Has("Neck") || !s.Has("LeftArm") || !s.Has("LeftUpLeg")) return t;
        for (const auto& [name, info] : model.boneInfoMap) { const size_t c = name.find_last_of(":|"); if (c != std::string::npos) { t.prefix = name.substr(0, c + 1); break; } }
        // X di riferimento: verso la sinistra del personaggio (dal braccio destro al sinistro)
        glm::vec3 left = s.Pos("LeftArm") - s.Pos("RightArm"); left.y = 0.0f;
        left = glm::length(left) > 1e-6f ? glm::normalize(left) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::mat4 gi = model.m_GlobalInverseTransform;
        BodySculptDetail::Bind bind = BodySculptDetail::ReadBind(model);
        for (const auto& [n, g] : bind.global) t.bindBones[n] = gi * g;
        auto boneRot = [&](const char* suffix) {
            auto it = bind.bySuffix.find(suffix);
            return it == bind.bySuffix.end() ? glm::mat3(1.0f) : Ortho(glm::mat3(gi * bind.global[it->second]));
        };
        std::array<std::vector<float>, 6> xs, ys, zs;
        t.boxes.resize(6);
        for (int i = 0; i < 6; ++i) {
            auto& b = t.boxes[i];
            b.block = names[i]; b.startBone = seg[i][0]; b.endBone = seg[i][1];
            if (!s.Has(seg[i][1])) b.endBone = i == 1 ? "Head" : b.endBone;
            const glm::vec3 a = s.Pos(b.startBone.c_str());
            glm::vec3 e = s.Pos(b.endBone.c_str());
            if (i == 1 && b.endBone == "Head") e = a + glm::vec3(0.0f, glm::length(s.Pos("Head") - a) * 2.5f, 0.0f);   // senza HeadTop_End
            b.bindLen = std::max(1e-4f, glm::length(e - a));
            b.bindFrame = Frame(a, e, left);
            b.bindBoneRot = boneRot(b.startBone.c_str());
            b.limb = i >= 2;
        }
        for (const auto& mesh : model.meshes) {
            for (const auto& v : mesh.vertices) {
                float w[6] = { 0, 0, 0, 0, 0, 0 };
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) if (v.BoneIDs[k] >= 0 && v.Weights[k] > 0.0f) w[BlockOfBone(s.NameOf(v.BoneIDs[k]))] += v.Weights[k];
                const int g = static_cast<int>(std::max_element(w, w + 6) - w);
                if (w[g] <= 0.0f) continue;
                const auto& b = t.boxes[g];
                const glm::vec3 q = glm::transpose(b.bindFrame) * (v.Position - s.Pos(b.startBone.c_str()));
                xs[g].push_back(q.x); ys[g].push_back(q.y); zs[g].push_back(q.z);
            }
        }
        for (int i = 0; i < 6; ++i) {
            auto& b = t.boxes[i];
            if (xs[i].size() < 12) continue;
            b.lo = glm::vec3(Percentile(xs[i], 0.03f), Percentile(ys[i], 0.01f), Percentile(zs[i], 0.03f));
            b.hi = glm::vec3(Percentile(xs[i], 0.97f), Percentile(ys[i], 0.99f), Percentile(zs[i], 0.97f));
            b.valid = true;
        }
        t.valid = t.boxes[0].valid;
        return t;
    }

    // Posa delle hitbox: getBone(nome con prefisso, out) = trasformazione dell'osso nello spazio della mesh;
    // world = trasformazione del corpo visibile (posizione, rotazione, scala).
    inline void DriveBoneHitboxes(const BoneHitboxTemplate& t, const std::function<bool(const std::string&, glm::mat4&)>& getBone,
                                  const glm::mat4& world, const glm::vec3& entityPos, SkeletonData& skel) {
        using namespace BoneHitboxDetail;
        if (!t.valid) { skel.driven = false; return; }
        const size_t n = skel.blocks.size();
        skel.drivenWorld.assign(n, glm::mat4(1.0f));
        skel.drivenJoint.assign(n, glm::vec3(0.0f));
        skel.drivenSize.assign(n, glm::vec3(0.0f));
        glm::mat4 torsoMat(1.0f);
        glm::vec3 torsoJoint(0.0f);
        std::vector<int> placed(n, 0);
        for (const auto& b : t.boxes) {
            if (!b.valid) continue;
            int idx = -1;
            for (size_t i = 0; i < n; ++i) if (skel.blocks[i].name == b.block) { idx = static_cast<int>(i); break; }
            if (idx < 0) continue;
            glm::mat4 ms(1.0f), me(1.0f);
            if (!getBone(t.prefix + b.startBone, ms)) { skel.driven = false; return; }
            glm::vec3 a(ms[3]), e;
            if (b.endBone == "Head" && b.block == "Head") {
                glm::mat4 mh(1.0f); getBone(t.prefix + "Head", mh);
                e = a + (glm::vec3(mh[3]) - a) * 2.5f;
            }
            else { if (!getBone(t.prefix + b.endBone, me)) { skel.driven = false; return; } e = glm::vec3(me[3]); }
            // assi: ruotano con l'osso d'inizio, poi l'asse lungo si riallinea al segmento
            const glm::mat3 delta = Ortho(glm::mat3(ms)) * glm::transpose(b.bindBoneRot);
            const glm::mat3 f = Frame(a, e, delta * b.bindFrame[0]);
            const float len = glm::length(e - a);
            const float k = b.limb ? std::clamp(len / b.bindLen, 0.6f, 1.2f) : 1.0f;
            glm::vec3 lo = b.lo, hi = b.hi;
            if (b.limb) { lo.y *= k; hi.y *= k; }
            const glm::vec3 c = (lo + hi) * 0.5f, size = glm::max(hi - lo, glm::vec3(1e-3f));
            glm::mat4 local(1.0f);
            local[0] = glm::vec4(f[0], 0.0f); local[1] = glm::vec4(f[1], 0.0f); local[2] = glm::vec4(f[2], 0.0f); local[3] = glm::vec4(a, 1.0f);
            const glm::mat4 m = world * local * glm::translate(glm::mat4(1.0f), c) * glm::scale(glm::mat4(1.0f), size);
            skel.drivenWorld[idx] = m;
            skel.drivenJoint[idx] = glm::vec3(world * glm::vec4(a, 1.0f));
            skel.drivenSize[idx] = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
            placed[idx] = 1;
            if (b.block == "Torso") { torsoMat = m; torsoJoint = skel.drivenJoint[idx]; }
        }
        // blocchi senza misura (gobba, mascella, spalle e pugni degli orchi): spenti, al centro del busto
        for (size_t i = 0; i < n; ++i) {
            if (placed[i]) continue;
            skel.drivenWorld[i] = glm::translate(glm::mat4(1.0f), glm::vec3(torsoMat[3])) * glm::scale(glm::mat4(1.0f), glm::vec3(1e-4f));
            skel.drivenJoint[i] = torsoJoint;
            skel.drivenSize[i] = glm::vec3(0.12f);   // solo per il ragdoll (dentro il busto); i colpi non lo trovano
        }
        skel.drivenAt = entityPos;
        skel.driven = true;
    }

}
