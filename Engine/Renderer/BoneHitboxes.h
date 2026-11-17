#pragma once
// Hitbox dal corpo animato: i blocchi dello scheletro a cubi misurati sulla mesh e mossi dalle ossa a ogni fotogramma.
//
// 1. Build: per ogni blocco dello scheletro (Torso, Head, Arm_L... e, se lo scheletro li ha, Jaw, Hump,
//    Shoulder_L/R, Fist_L/R, Belly, Ear_L/R) si prende un segmento d'ossa e si misurano i vertici che gli
//    appartengono (osso con il peso maggiore) nel riferimento del segmento, dal 3% al 97% su ogni asse.
//    Quali vertici vanno a quale blocco dipende dai blocchi che lo scheletro ha davvero: con "Fist_L" la mano ha
//    il suo blocco, senza resta nel braccio; con "Hump" la parte alta della schiena e' la gobba; e cosi' via.
//    Oltre al corpo si misurano le parti rigide che lo vestono: grugno e zanne del vrepigh, elmo, spallacci e
//    piastre dell'armatura (non la stoffa: un colpo nel mantello non e' un colpo nel busto).
// 2. Drive: il riferimento segue le ossa animate. L'asse lungo va dall'osso d'inizio a quello di fine (il braccio
//    piegato accorcia il blocco), gli altri ruotano con l'osso di riferimento (per il busto lo Spine1: il petto
//    gira mirando anche se il bacino resta fermo).
// Quando l'artista consegna il modello vero (AuthoredModel.h) le hitbox si rifanno da sole sulla sua mesh.
#include "SkinnedModel.h"
#include "BodySculpt.h"
#include "../Physics/HitboxBlock.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SunEngine {

    struct BoneHitboxTemplate {
        struct Box {
            std::string block;              // nome del blocco nello scheletro a cubi
            std::string startBone, endBone; // segmento d'ossa (senza prefisso)
            std::string rotBone;            // osso che fa girare il blocco (vuoto = startBone)
            float endScale = 1.0f;          // fine = inizio + (endBone - inizio) * endScale (testa senza HeadTop_End)
            glm::vec3 lo{ 0.0f }, hi{ 0.0f };   // ingombro nel riferimento del segmento (unita' della mesh)
            float bindLen = 1.0f;
            glm::mat3 bindFrame{ 1.0f };    // assi del segmento in posa di riposo (colonne X, Y, Z)
            glm::mat3 bindBoneRot{ 1.0f };  // rotazione dell'osso di riferimento in posa di riposo
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
        inline bool Starts(const std::string& b, const char* p) { return b.rfind(p, 0) == 0; }
    }

    // Blocchi base (gli scheletri a cubi umani e Vanguard)
    inline std::vector<std::string> DefaultHitboxBlocks() { return { "Torso", "Head", "Arm_L", "Arm_R", "Leg_L", "Leg_R" }; }

    // Misura dei blocchi. blockNames = blocchi dello scheletro a cubi; extra = mesh rigide che vestono il corpo
    // (stesse ossa), comprese nella misura.
    inline BoneHitboxTemplate BuildBoneHitboxes(SkinnedModel& model, const std::vector<std::string>& blockNames = DefaultHitboxBlocks(),
                                                const std::vector<const std::vector<SkinnedVertex>*>& extra = {}) {
        using namespace BoneHitboxDetail;
        BoneHitboxTemplate t;
        BodySculptor s(model);
        if (!s.Has("Hips") || !s.Has("Neck") || !s.Has("LeftArm") || !s.Has("LeftUpLeg")) return t;
        for (const auto& [name, info] : model.boneInfoMap) { const size_t c = name.find_last_of(":|"); if (c != std::string::npos) { t.prefix = name.substr(0, c + 1); break; } }
        const std::unordered_set<std::string> has(blockNames.begin(), blockNames.end());
        auto H = [&](const char* b) { return has.count(b) > 0; };

        // Segmenti dei blocchi (solo quelli presenti nello scheletro)
        struct Spec { const char* block; const char* a; const char* b; const char* rot; float endScale; bool limb; };
        std::vector<Spec> specs = {
            { "Torso", "Hips", "Neck", "Spine1", 1.0f, false },
            { "Head", "Neck", "HeadTop_End", "Head", 1.0f, false },
            { "Arm_L", "LeftArm", "LeftHand", "LeftArm", 1.0f, true },
            { "Arm_R", "RightArm", "RightHand", "RightArm", 1.0f, true },
            { "Leg_L", "LeftUpLeg", "LeftFoot", "LeftUpLeg", 1.0f, true },
            { "Leg_R", "RightUpLeg", "RightFoot", "RightUpLeg", 1.0f, true },
            { "Jaw", "Head", "Jaw", "Jaw", 1.6f, false },
            { "Hump", "Spine1", "Neck", "Spine2", 1.0f, false },
            { "Belly", "Spine", "Spine1", "Spine", 1.0f, false },
            { "Shoulder_L", "LeftShoulder", "LeftArm", "LeftArm", 1.35f, false },
            { "Shoulder_R", "RightShoulder", "RightArm", "RightArm", 1.35f, false },
            { "Fist_L", "LeftHand", "LeftHandMiddle1", "LeftHand", 2.2f, false },
            { "Fist_R", "RightHand", "RightHandMiddle1", "RightHand", 2.2f, false },
            { "Ear_L", "Head", "LeftEar", "LeftEar", 1.5f, false },
            { "Ear_R", "Head", "RightEar", "RightEar", 1.5f, false },
        };
        if (!s.Has("HeadTop_End")) specs[1] = { "Head", "Neck", "Head", "Head", 2.5f, false };
        if (H("Fist_L")) { specs[2].b = "LeftForeArm"; specs[2].endScale = 1.0f; }    // il pugno ha il suo blocco: il braccio
        if (H("Fist_R")) { specs[3].b = "RightForeArm"; specs[3].endScale = 1.0f; }   // arriva al polso (vedi sotto)

        glm::vec3 left = s.Pos("LeftArm") - s.Pos("RightArm"); left.y = 0.0f;
        left = glm::length(left) > 1e-6f ? glm::normalize(left) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::mat4 gi = model.m_GlobalInverseTransform;
        BodySculptDetail::Bind bind = BodySculptDetail::ReadBind(model);
        for (const auto& [n, g] : bind.global) t.bindBones[n] = gi * g;
        auto boneRot = [&](const char* suffix) {
            auto it = bind.bySuffix.find(suffix);
            return it == bind.bySuffix.end() ? glm::mat3(1.0f) : Ortho(glm::mat3(gi * bind.global[it->second]));
        };

        std::unordered_map<std::string, int> index;
        for (const auto& sp : specs) {
            if (!H(sp.block) || !s.Has(sp.a) || !s.Has(sp.b)) continue;
            BoneHitboxTemplate::Box b;
            b.block = sp.block; b.startBone = sp.a; b.endBone = sp.b; b.rotBone = s.Has(sp.rot) ? sp.rot : sp.a; b.endScale = sp.endScale; b.limb = sp.limb;
            const glm::vec3 a = s.Pos(sp.a), e = a + (s.Pos(sp.b) - a) * sp.endScale;
            b.bindLen = std::max(1e-4f, glm::length(e - a));
            b.bindFrame = Frame(a, e, left);
            b.bindBoneRot = boneRot(b.rotBone.c_str());
            index[b.block] = static_cast<int>(t.boxes.size());
            t.boxes.push_back(b);
        }
        if (!index.count("Torso")) return t;

        // A quale blocco va un vertice (dall'osso con il peso maggiore e da dove sta)
        const float spineZ = s.Pos("Spine2").z, yMidBack = (s.Pos("Spine1").y + s.Pos("Spine2").y) * 0.5f;
        auto armShoulder = [&](const glm::vec3& p, bool l) {   // primo quarto del braccio: deltoide
            const glm::vec3 a = s.Pos(l ? "LeftArm" : "RightArm"), b = s.Pos(l ? "LeftForeArm" : "RightForeArm");
            const glm::vec3 ab = b - a;
            return glm::dot(p - a, ab) / std::max(1e-8f, glm::dot(ab, ab)) < 0.25f;
        };
        auto blockOf = [&](const std::string& b, const glm::vec3& p) -> const char* {
            const bool L = Starts(b, "Left");
            if (b == "Jaw") return H("Jaw") ? "Jaw" : "Head";
            if (b.find("Ear") != std::string::npos) return H(L ? "Ear_L" : "Ear_R") ? (L ? "Ear_L" : "Ear_R") : "Head";
            if (b == "Neck" || Starts(b, "Head")) return "Head";
            if (b == "Belly") return H("Belly") ? "Belly" : "Torso";
            if (b == "LeftShoulder" || b == "RightShoulder" || b == "LeftPauldron" || b == "RightPauldron")
                return H(L ? "Shoulder_L" : "Shoulder_R") ? (L ? "Shoulder_L" : "Shoulder_R") : (b.find("Pauldron") != std::string::npos ? (L ? "Arm_L" : "Arm_R") : "Torso");
            if (Starts(b, "LeftHand") || Starts(b, "RightHand")) return H(L ? "Fist_L" : "Fist_R") ? (L ? "Fist_L" : "Fist_R") : (L ? "Arm_L" : "Arm_R");
            if (Starts(b, "LeftArm") || Starts(b, "RightArm")) {
                if (H(L ? "Shoulder_L" : "Shoulder_R") && armShoulder(p, L)) return L ? "Shoulder_L" : "Shoulder_R";
                return L ? "Arm_L" : "Arm_R";
            }
            if (Starts(b, "LeftForeArm") || Starts(b, "RightForeArm")) return L ? "Arm_L" : "Arm_R";
            if (Starts(b, "LeftUpLeg") || b == "LeftLeg" || Starts(b, "LeftFoot") || Starts(b, "LeftToe")) return "Leg_L";
            if (Starts(b, "RightUpLeg") || b == "RightLeg" || Starts(b, "RightFoot") || Starts(b, "RightToe")) return "Leg_R";
            if (H("Hump") && (b == "Spine2" || b == "Spine1") && p.z < spineZ && p.y > yMidBack) return "Hump";
            return "Torso";
        };

        std::vector<std::vector<float>> xs(t.boxes.size()), ys(t.boxes.size()), zs(t.boxes.size());
        auto addVertex = [&](const SkinnedVertex& v) {
            int best = -1; float bw = 0.0f;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) if (v.BoneIDs[k] >= 0 && v.Weights[k] > bw) { bw = v.Weights[k]; best = v.BoneIDs[k]; }
            if (best < 0) return;
            const char* blk = blockOf(s.NameOf(best), v.Position);
            auto it = index.find(blk);
            if (it == index.end()) it = index.find("Torso");
            const auto& b = t.boxes[it->second];
            const glm::vec3 q = glm::transpose(b.bindFrame) * (v.Position - s.Pos(b.startBone.c_str()));
            xs[it->second].push_back(q.x); ys[it->second].push_back(q.y); zs[it->second].push_back(q.z);
        };
        for (const auto& mesh : model.meshes) for (const auto& v : mesh.vertices) addVertex(v);
        for (const auto* m : extra) if (m) for (const auto& v : *m) addVertex(v);
        for (size_t i = 0; i < t.boxes.size(); ++i) {
            auto& b = t.boxes[i];
            if (xs[i].size() < 12) continue;
            b.lo = glm::vec3(Percentile(xs[i], 0.03f), Percentile(ys[i], 0.01f), Percentile(zs[i], 0.03f));
            b.hi = glm::vec3(Percentile(xs[i], 0.97f), Percentile(ys[i], 0.99f), Percentile(zs[i], 0.97f));
            b.valid = true;
        }
        t.valid = t.boxes[index["Torso"]].valid;
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
            glm::mat4 ms(1.0f), me(1.0f), mr(1.0f);
            if (!getBone(t.prefix + b.startBone, ms) || !getBone(t.prefix + b.endBone, me)) { skel.driven = false; return; }
            if (b.rotBone.empty() || !getBone(t.prefix + b.rotBone, mr)) mr = ms;
            const glm::vec3 a(ms[3]);
            const glm::vec3 e = a + (glm::vec3(me[3]) - a) * b.endScale;
            // assi: ruotano con l'osso di riferimento, poi l'asse lungo si riallinea al segmento
            const glm::mat3 delta = Ortho(glm::mat3(mr)) * glm::transpose(b.bindBoneRot);
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
        // blocchi senza misura (non c'e' l'osso nel modello): spenti, al centro del busto
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
