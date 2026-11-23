#pragma once
// Il corpo come ingombro e come portaoggetti.
//
// 1. Ingombro: il tronco, la testa e le gambe misurati sulla mesh in posa di riposo, come ellissi lungo le ossa
//    (larghezza, davanti e dietro separati: la pancia e la gobba di un Vrepigh non sono quelle di un fante).
//    Le armi in mano e quelle appese vengono spinte fuori da questi volumi: niente canne nel petto o nella testa.
// 2. Portaoggetti: dove si appendono le cose che non sono in mano. Fucile di traverso sulla schiena, pistola nella
//    fondina sulla coscia destra, arma bianca sul fianco sinistro, granate e attrezzi sulla cintura, armi lunghe
//    in piu' di traverso sulla schiena. Ogni posto e' legato a un osso, quindi segue il corpo in ogni animazione.
//
// Tutto si calcola una volta per modello (ComputeBodyGear) e si usa con le matrici delle ossa del fotogramma.
// Spazio del modello del gioco: +Y alto; il davanti si ricava dai piedi. Niente OpenGL qui.
#include "SkinnedModel.h"
#include "SkinnedMaterial.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace SunEngine {

    struct BodyGearSegment {
        std::string a, b;              // ossa agli estremi (nomi completi)
        float rSide = 0.1f;            // mezza larghezza
        float rFront = 0.1f;           // davanti (pancia, petto)
        float rBack = 0.1f;            // dietro (schiena, gobba)
        bool round = false;            // gambe: sezione tonda (rSide)
        float tailExtra = 0.0f;        // allungamento oltre 'b' (testa: fino alla sommita')
    };

    enum class GearMount { Back = 0, BackCross, HolsterRight, HipLeft, BeltFrontLeft, BeltFrontRight, BeltBackLeft, BeltBackRight, Pack, Count };

    struct BodyGearInfo {
        bool valid = false;
        std::vector<BodyGearSegment> segs;
        glm::vec3 fwd{ 0.0f, 0.0f, 1.0f }, right{ -1.0f, 0.0f, 0.0f }, up{ 0.0f, 1.0f, 0.0f };   // posa di riposo
        // posto di ogni oggetto appeso: osso, punto in posa di riposo, normale verso fuori e direzione in cui punta
        // la canna/lama, piu' il "su" dell'oggetto
        struct Mount { std::string bone; glm::vec3 point{ 0.0f }, out{ 0.0f }, along{ 0.0f }, top{ 0.0f }; float surface = 0.1f; bool ok = false; };
        std::array<Mount, static_cast<int>(GearMount::Count)> mounts;
        float height = 1.8f;
    };

    namespace BodyGearDetail {
        inline std::string Suffix(const std::string& n) { const auto p = n.rfind(':'); return p == std::string::npos ? n : n.substr(p + 1); }
        inline float Percentile(std::vector<float>& v, float q, float def) {
            if (v.size() < 8) return def;
            const size_t k = std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1)));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return v[k];
        }
        // Pelle: nei modelli d'autore solo i materiali di pelle (niente setole, zanne, orecchie, code)
        inline bool IsSkinMesh(const SkinnedModel& m, const SkinnedMesh& mesh) {
            if (!m.authored) return true;
            if (mesh.materialIndex < 0 || mesh.materialIndex >= static_cast<int>(m.materials.size())) return true;
            const SkinnedMaterial& mat = m.materials[mesh.materialIndex];
            std::string n = mat.name;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return mat.kind == SkinMat_Skin && n.find("ear") == std::string::npos && n.find("tail") == std::string::npos;
        }
    }

    // Misura il corpo in posa di riposo. Il prefisso delle ossa si trova da solo (mixamorig1:, mixamorig:, niente).
    inline BodyGearInfo ComputeBodyGear(const SkinnedModel& model) {
        using namespace BodyGearDetail;
        BodyGearInfo g;
        std::unordered_map<std::string, std::string> full;      // suffisso -> nome completo
        std::unordered_map<std::string, glm::vec3> pos;          // suffisso -> posizione in posa di riposo
        std::unordered_map<int, std::string> nameOf;
        for (const auto& [name, bi] : model.boneInfoMap) {
            const std::string s = Suffix(name);
            full[s] = name;
            pos[s] = glm::vec3(glm::inverse(bi.offsetMatrix)[3]);
            nameOf[bi.id] = s;
        }
        for (const char* need : { "Hips", "Spine", "Spine1", "Spine2", "Neck", "Head", "LeftUpLeg", "RightUpLeg", "LeftLeg", "RightLeg" })
            if (!pos.count(need)) return g;
        // assi: davanti dai piedi, destra = davanti x su
        glm::vec3 fwd(0.0f);
        for (const char* side : { "Left", "Right" })
            if (pos.count(std::string(side) + "Foot") && pos.count(std::string(side) + "ToeBase"))
                fwd += pos[std::string(side) + "ToeBase"] - pos[std::string(side) + "Foot"];
        fwd.y = 0.0f;
        g.fwd = glm::length(fwd) > 1e-6f ? glm::normalize(fwd) : glm::vec3(0.0f, 0.0f, 1.0f);
        g.up = glm::vec3(0.0f, 1.0f, 0.0f);
        g.right = glm::normalize(glm::cross(g.fwd, g.up));
        if (pos.count("LeftArm") && glm::dot(pos["LeftArm"] - pos["Spine2"], g.right) > 0.0f) g.right = -g.right;   // la destra e' dalla parte del braccio destro

        // vertici di pelle con l'osso che pesa di piu'
        struct V { glm::vec3 p; std::string bone; };
        std::vector<V> verts;
        float lo = 1e9f, hi = -1e9f;
        for (const auto& mesh : model.meshes) {
            if (!IsSkinMesh(model, mesh)) continue;
            for (const auto& v : mesh.vertices) {
                int best = -1; float bw = 0.0f;
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) if (v.BoneIDs[k] >= 0 && v.Weights[k] > bw) { bw = v.Weights[k]; best = v.BoneIDs[k]; }
                auto it = nameOf.find(best);
                verts.push_back({ v.Position, it == nameOf.end() ? std::string() : it->second });
                lo = std::min(lo, v.Position.y); hi = std::max(hi, v.Position.y);
            }
        }
        if (verts.empty()) return g;
        g.height = hi - lo;

        auto measure = [&](const char* a, const char* b, std::initializer_list<const char*> bones, float extra, bool round) {
            BodyGearSegment s;
            s.a = full[a]; s.b = full[b]; s.round = round; s.tailExtra = extra;
            const glm::vec3 A = pos[a], B0 = pos[b];
            const glm::vec3 dir = glm::normalize(B0 - A);
            const glm::vec3 B = B0 + dir * extra;
            const glm::vec3 ab = B - A;
            const float ab2 = std::max(1e-8f, glm::dot(ab, ab));
            std::vector<float> side, front, back, rad;
            for (const auto& v : verts) {
                bool mine = false;
                for (const char* bn : bones) if (v.bone == bn) { mine = true; break; }
                if (!mine) continue;
                const float t = glm::dot(v.p - A, ab) / ab2;
                if (t < -0.05f || t > 1.05f) continue;
                const glm::vec3 d = v.p - (A + ab * std::clamp(t, 0.0f, 1.0f));
                if (round) { rad.push_back(glm::length(d)); continue; }
                side.push_back(std::abs(glm::dot(d, g.right)));
                const float f = glm::dot(d, g.fwd);
                (f >= 0.0f ? front : back).push_back(std::abs(f));
            }
            const float def = 0.12f * g.height / 1.8f;
            if (round) { s.rSide = s.rFront = s.rBack = Percentile(rad, 0.9f, def * 0.6f); }
            else {
                s.rSide = Percentile(side, 0.95f, def);
                s.rFront = Percentile(front, 0.92f, def * 0.8f);
                s.rBack = Percentile(back, 0.92f, def * 0.8f);
            }
            g.segs.push_back(s);
        };
        measure("Hips", "Spine", { "Hips", "Spine" }, 0.0f, false);
        measure("Spine", "Spine1", { "Spine", "Spine1" }, 0.0f, false);
        measure("Spine1", "Spine2", { "Spine1", "Spine2" }, 0.0f, false);
        measure("Spine2", "Neck", { "Spine2", "Neck" }, 0.0f, false);
        {
            // testa: dal collo alla sommita' (osso Head piu' meta' della sua distanza dal collo, almeno 12 cm del corpo)
            const float extra = std::max(glm::distance(pos["Head"], pos["Neck"]) * 1.4f, 0.1f * g.height);
            measure("Neck", "Head", { "Neck", "Head", "Jaw", "LeftEar", "RightEar" }, extra, false);
        }
        for (const char* side : { "Left", "Right" }) {
            const std::string s(side);
            measure((s + "UpLeg").c_str(), (s + "Leg").c_str(), { (s + "UpLeg").c_str() }, 0.0f, true);
        }

        // --- posti degli oggetti ---
        auto seg = [&](const std::string& a) -> const BodyGearSegment* {
            for (const auto& s : g.segs) if (Suffix(s.a) == a) return &s;
            return nullptr;
        };
        auto set = [&](GearMount m, const std::string& bone, glm::vec3 p, glm::vec3 out, glm::vec3 along, glm::vec3 top, float surface) {
            auto& mt = g.mounts[static_cast<int>(m)];
            mt.bone = full[bone]; mt.point = p; mt.out = glm::normalize(out); mt.along = glm::normalize(along);
            mt.top = glm::normalize(top - mt.along * glm::dot(top, mt.along)); mt.surface = surface; mt.ok = !mt.bone.empty();
        };
        const BodyGearSegment* chest = seg("Spine1");
        const BodyGearSegment* hips = seg("Hips");
        const glm::vec3 backPoint = glm::mix(pos["Spine1"], pos["Spine2"], 0.55f);
        const glm::vec3 diag = glm::normalize(g.up * 0.82f - g.right * 0.55f);       // canna in alto verso la spalla sinistra
        const glm::vec3 diag2 = glm::normalize(g.up * 0.82f + g.right * 0.55f);
        if (chest) {
            // di piatto sulla schiena: il fianco dell'arma (asse X) contro la schiena, il "su" dell'arma di lato
            set(GearMount::Back, "Spine2", backPoint, -g.fwd, diag, glm::cross(diag, -g.fwd), chest->rBack);
            set(GearMount::BackCross, "Spine2", backPoint - g.up * (0.04f * g.height / 1.8f), -g.fwd, diag2, glm::cross(-g.fwd, diag2), chest->rBack);
            set(GearMount::Pack, "Spine1", glm::mix(pos["Spine"], pos["Spine1"], 0.5f), -g.fwd, g.right, g.up, seg("Spine") ? seg("Spine")->rBack : chest->rBack);
        }
        for (const char* side : { "Right", "Left" }) {
            const std::string s(side);
            const BodyGearSegment* leg = seg(s + "UpLeg");
            if (!leg) continue;
            const glm::vec3 legDir = glm::normalize(pos[s + "Leg"] - pos[s + "UpLeg"]);
            const glm::vec3 outward = (s == "Right") ? g.right : -g.right;
            const glm::vec3 p = glm::mix(pos[s + "UpLeg"], pos[s + "Leg"], 0.22f);
            if (s == "Right") set(GearMount::HolsterRight, s + "UpLeg", p, outward, legDir, g.fwd, leg->rSide);
            else set(GearMount::HipLeft, s + "UpLeg", glm::mix(pos[s + "UpLeg"], pos[s + "Leg"], 0.08f), outward,
                     glm::normalize(legDir - g.fwd * 0.35f), g.fwd, leg->rSide);
        }
        if (hips) {
            const glm::vec3 belt = glm::mix(pos["Hips"], pos["Spine"], 0.6f);
            struct B { GearMount m; float side, front; };
            for (const B b : { B{ GearMount::BeltFrontLeft, -0.55f, 0.83f }, B{ GearMount::BeltFrontRight, 0.55f, 0.83f },
                               B{ GearMount::BeltBackLeft, -0.6f, -0.8f }, B{ GearMount::BeltBackRight, 0.6f, -0.8f } }) {
                const glm::vec3 out = glm::normalize(g.right * b.side + g.fwd * b.front);
                // distanza dalla pelle lungo 'out' sull'ellisse dei fianchi
                const float rf = b.front > 0.0f ? hips->rFront : hips->rBack;
                const float cx = b.side / hips->rSide, cz = b.front / rf;
                const float surf = 1.0f / std::sqrt(cx * cx + cz * cz);
                set(b.m, "Hips", belt, out, glm::cross(g.up, out), g.up, surf);   // oggetti dritti sulla cintura
            }
        }
        g.valid = true;
        return g;
    }

    // Matrice finale di un osso (posa del fotogramma * inversa della posa di riposo): porta un punto della posa di
    // riposo dove si trova ora, nello spazio del modello.
    inline bool BodyGearBoneFinal(const SkinnedModel& model, const glm::mat4& boneGlobal, const std::string& bone, glm::mat4& out) {
        auto it = model.boneInfoMap.find(bone);
        if (it == model.boneInfoMap.end()) return false;
        out = boneGlobal * it->second.offsetMatrix;
        return true;
    }

    // Spinge fuori dal corpo un insieme di punti rigidi (es. gli spigoli di un'arma). 'segs' sono le ellissi gia'
    // portate nel mondo: estremi A, B e assi (destra, davanti) del fotogramma. Ritorna lo spostamento da applicare.
    struct BodyGearWorldSeg { glm::vec3 A, B, right, fwd; float rSide, rFront, rBack; };

    inline glm::vec3 BodyGearPushOut(const std::vector<BodyGearWorldSeg>& segs, const std::vector<glm::vec3>& pts, float margin) {
        glm::vec3 total(0.0f);
        for (int iter = 0; iter < 3; ++iter) {
            glm::vec3 best(0.0f); float bestLen = 0.0f;
            for (const auto& s : segs) {
                const glm::vec3 ab = s.B - s.A;
                const float ab2 = std::max(1e-8f, glm::dot(ab, ab));
                const glm::vec3 axis = ab / std::sqrt(ab2);
                for (const glm::vec3& p0 : pts) {
                    const glm::vec3 p = p0 + total;
                    const float t = glm::dot(p - s.A, ab) / ab2;
                    if (t < -0.02f || t > 1.02f) continue;
                    const glm::vec3 c = s.A + ab * std::clamp(t, 0.0f, 1.0f);
                    glm::vec3 d = p - c;
                    d -= axis * glm::dot(d, axis);
                    const float x = glm::dot(d, s.right), z = glm::dot(d, s.fwd);
                    const float rx = s.rSide + margin, rz = (z >= 0.0f ? s.rFront : s.rBack) + margin;
                    const float e = std::sqrt((x * x) / (rx * rx) + (z * z) / (rz * rz));
                    if (e >= 1.0f) continue;
                    // fuori lungo la direzione radiale fino al bordo dell'ellisse
                    glm::vec3 dir = glm::length(d) > 1e-5f ? glm::normalize(d) : s.fwd;
                    const float need = (e > 1e-4f ? glm::length(d) / e : std::min(rx, rz)) - glm::length(d);
                    if (need > bestLen) { bestLen = need; best = dir * need; }
                }
            }
            if (bestLen < 1e-4f) break;
            total += best;
        }
        return total;
    }
}
