#pragma once
// Sagome delle razze, scolpite sul corpo base in posa di riposo (prima delle animazioni):
//
//   Vanguard Armor  umano piu' grande a V: spalle larghissime (spalline da "power ranger"), dorsali aperti,
//                   petto alto, vita e bacino stretti. L'ombra frontale e' una V.
//   Vrepigh         uomo-cinghiale: gobba di muscoli fra le spalle che sale fino alla nuca, collo corto e
//                   spesso, testa piu' bassa e in avanti, braccia grosse e lunghe, gambe tozze. Sulla testa un
//                   grugno da cinghiale con il disco del naso, zanne che salgono dalla mascella, orecchie a punta
//                   e una cresta di setole (SdfMesher.h): pelle del colore dell'unita', zanne d'avorio.
//
// Le ossa si spostano davvero (spalle in fuori, testa in avanti): nodo della gerarchia e matrice di offset
// insieme, cosi' le clip (che si appoggiano alla posa di riposo del modello, vedi Animation::Evaluate) e la
// pelle restano coerenti. Va chiamato prima di caricare le clip. Nessuna parte OpenGL tranne la creazione
// delle mesh delle parti (SkinnedMesh).
//
// Le sagome sono un segnaposto per il modello dell'artista: un modello d'autore (AuthoredModel.h) le salta.
#include "SkinnedModel.h"
#include "SkinnedParts.h"
#include "SdfMesher.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace SunEngine {

    namespace BodySculptDetail {
        inline std::string Suffix(const std::string& n) {
            const size_t c = n.find_last_of(":|");
            return c == std::string::npos ? n : n.substr(c + 1);
        }
        inline float Smooth(float a, float b, float x) {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // Posa di riposo: per ogni nodo la trasformazione globale (spazio dei nodi) e quella del padre
        struct Bind {
            std::unordered_map<std::string, glm::mat4> global, parentGlobal;
            std::unordered_map<std::string, std::string> parent;
            std::unordered_map<std::string, std::vector<std::string>> children;
            std::unordered_map<std::string, std::string> bySuffix;   // "LeftArm" -> "mixamorig1:LeftArm"
        };
        inline void Walk(const AssimpNodeData& n, const glm::mat4& pg, const std::string& pname, Bind& b) {
            const glm::mat4 g = pg * n.transformation;
            b.global[n.name] = g; b.parentGlobal[n.name] = pg; b.parent[n.name] = pname;
            if (!pname.empty()) b.children[pname].push_back(n.name);
            b.bySuffix.emplace(Suffix(n.name), n.name);
            for (const auto& c : n.children) Walk(c, g, n.name, b);
        }
        inline Bind ReadBind(const SkinnedModel& m) { Bind b; Walk(m.rootNode, glm::mat4(1.0f), std::string(), b); return b; }
        inline AssimpNodeData* FindNode(AssimpNodeData& n, const std::string& name) {
            if (n.name == name) return &n;
            for (auto& c : n.children) if (AssimpNodeData* r = FindNode(c, name)) return r;
            return nullptr;
        }
    }

    class BodySculptor {
    public:
        explicit BodySculptor(SkinnedModel& m) : m_Model(m) {
            m_Bind = BodySculptDetail::ReadBind(m);
            m_Gi = m.m_GlobalInverseTransform;
            m_Disp.assign(std::max(1, m.boneCounter) + 8, glm::vec3(0.0f));
            for (const auto& [name, info] : m.boneInfoMap) m_IdToName[info.id] = name;
        }

        bool Has(const char* suffix) const { return m_Bind.bySuffix.count(suffix) > 0; }
        // Posizione dell'osso in posa di riposo, spazio della mesh
        glm::vec3 Pos(const char* suffix) const {
            const auto it = m_Bind.bySuffix.find(suffix);
            if (it == m_Bind.bySuffix.end()) return glm::vec3(0.0f);
            return glm::vec3((m_Gi * m_Bind.global.at(it->second))[3]);
        }
        std::string NameOf(int boneId) const { auto it = m_IdToName.find(boneId); return it == m_IdToName.end() ? std::string() : BodySculptDetail::Suffix(it->second); }
        int IdOf(const char* suffix) const {
            const auto it = m_Bind.bySuffix.find(suffix);
            if (it == m_Bind.bySuffix.end()) return -1;
            const auto bi = m_Model.boneInfoMap.find(it->second);
            return bi == m_Model.boneInfoMap.end() ? -1 : bi->second.id;
        }

        // Sposta l'osso (e con lui tutti i figli) di d nello spazio della mesh. La pelle segue quando si chiama
        // ApplyBoneMoves (i vertici si spostano con i pesi delle loro ossa).
        void MoveBone(const char* suffix, const glm::vec3& d) {
            using namespace BodySculptDetail;
            const auto it = m_Bind.bySuffix.find(suffix);
            if (it == m_Bind.bySuffix.end()) return;
            AssimpNodeData* node = FindNode(m_Model.rootNode, it->second);
            if (!node) return;
            const glm::mat4 G = m_Bind.global[it->second], Gp = m_Bind.parentGlobal[it->second];
            const glm::mat4 Gnew = glm::inverse(m_Gi) * glm::translate(glm::mat4(1.0f), d) * m_Gi * G;
            node->transformation = glm::inverse(Gp) * Gnew;
            // l'osso e i discendenti: offset spostati, globali di riposo aggiornati
            std::vector<std::string> stack{ it->second };
            while (!stack.empty()) {
                const std::string n = stack.back(); stack.pop_back();
                const glm::mat4 shift = glm::inverse(m_Gi) * glm::translate(glm::mat4(1.0f), d) * m_Gi;
                m_Bind.global[n] = shift * m_Bind.global[n];
                if (n != it->second) m_Bind.parentGlobal[n] = shift * m_Bind.parentGlobal[n];
                if (auto bi = m_Model.boneInfoMap.find(n); bi != m_Model.boneInfoMap.end()) {
                    bi->second.offsetMatrix = bi->second.offsetMatrix * glm::translate(glm::mat4(1.0f), -d);
                    if (bi->second.id >= 0 && bi->second.id < static_cast<int>(m_Disp.size())) m_Disp[bi->second.id] += d;
                }
                if (auto ch = m_Bind.children.find(n); ch != m_Bind.children.end()) for (const auto& c : ch->second) stack.push_back(c);
            }
        }

        // La pelle segue gli spostamenti delle ossa (media pesata)
        void ApplyBoneMoves() {
            for (auto& mesh : m_Model.meshes) {
                for (auto& v : mesh.vertices) {
                    glm::vec3 d(0.0f);
                    for (int k = 0; k < MAX_BONE_INFLUENCE; ++k)
                        if (v.BoneIDs[k] >= 0 && v.BoneIDs[k] < static_cast<int>(m_Disp.size())) d += m_Disp[v.BoneIDs[k]] * v.Weights[k];
                    v.Position += d;
                }
            }
            std::fill(m_Disp.begin(), m_Disp.end(), glm::vec3(0.0f));
        }

        // Peso delle ossa del vertice che soddisfano il filtro
        template <typename F> float Weight(const SkinnedVertex& v, F&& pred) const {
            float w = 0.0f;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) if (v.BoneIDs[k] >= 0 && v.Weights[k] > 0.0f && pred(NameOf(v.BoneIDs[k]))) w += v.Weights[k];
            return w;
        }

        // Fine: mesh ricaricata sulla GPU e normali ricalcolate
        void Finish() {
            for (auto& mesh : m_Model.meshes) {
                RecomputeNormals(mesh.vertices, mesh.indices);
                glDeleteVertexArrays(1, &mesh.VAO); glDeleteBuffers(1, &mesh.VBO); glDeleteBuffers(1, &mesh.EBO);
                std::vector<SkinnedVertex> v = std::move(mesh.vertices);
                std::vector<unsigned int> idx = std::move(mesh.indices);
                mesh = SkinnedMesh(std::move(v), std::move(idx));
            }
        }

        static void RecomputeNormals(std::vector<SkinnedVertex>& v, const std::vector<unsigned int>& idx) {
            std::vector<glm::vec3> n(v.size(), glm::vec3(0.0f));
            for (size_t i = 0; i + 2 < idx.size(); i += 3) {
                const unsigned a = idx[i], b = idx[i + 1], c = idx[i + 2];
                if (a >= v.size() || b >= v.size() || c >= v.size()) continue;
                const glm::vec3 f = glm::cross(v[b].Position - v[a].Position, v[c].Position - v[a].Position);
                n[a] += f; n[b] += f; n[c] += f;
            }
            // vertici con la stessa posizione (cuciture delle UV): normale comune, niente spigoli finti
            std::unordered_map<long long, glm::vec3> acc;
            auto key = [](const glm::vec3& p) {
                const long long x = std::llround(p.x * 2000.0), y = std::llround(p.y * 2000.0), z = std::llround(p.z * 2000.0);
                return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL);
            };
            for (size_t i = 0; i < v.size(); ++i) acc[key(v[i].Position)] += n[i];
            for (size_t i = 0; i < v.size(); ++i) {
                const glm::vec3 s = acc[key(v[i].Position)];
                if (glm::dot(s, s) > 1e-20f) v[i].Normal = glm::normalize(s);
            }
        }

        SkinnedModel& Model() { return m_Model; }

    private:
        SkinnedModel& m_Model;
        BodySculptDetail::Bind m_Bind;
        glm::mat4 m_Gi{ 1.0f };
        std::vector<glm::vec3> m_Disp;
        std::unordered_map<int, std::string> m_IdToName;
    };

    // ---------------------------------------------------------------------------------------------------------
    // Gruppi di ossa
    namespace BodySculptDetail {
        inline bool IsTorsoBone(const std::string& b) {
            return b == "Hips" || b.rfind("Spine", 0) == 0 || b == "LeftShoulder" || b == "RightShoulder" || b == "Belly";
        }
        inline bool IsArmBone(const std::string& b, bool left) {
            const char* s = left ? "Left" : "Right";
            return b.rfind(std::string(s) + "Arm", 0) == 0 || b.rfind(std::string(s) + "ForeArm", 0) == 0 || b.rfind(std::string(s) + "Hand", 0) == 0;
        }
        inline bool IsLegBone(const std::string& b) {
            return b.find("UpLeg") != std::string::npos || b == "LeftLeg" || b == "RightLeg" || b.find("Foot") != std::string::npos || b.find("Toe") != std::string::npos;
        }
        inline bool IsHeadBone(const std::string& b) { return b == "Neck" || b.rfind("Head", 0) == 0 || b == "Jaw" || b.find("Ear") != std::string::npos; }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Vanguard Armor: V
    inline void SculptVanguardBody(SkinnedModel& model) {
        using namespace BodySculptDetail;
        BodySculptor s(model);
        if (!s.Has("Hips") || !s.Has("Neck") || !s.Has("LeftArm") || !s.Has("RightArm")) return;
        const glm::vec3 hips = s.Pos("Hips"), neck = s.Pos("Neck");
        const glm::vec3 armL = s.Pos("LeftArm"), armR = s.Pos("RightArm");
        const float H = std::max(1e-3f, neck.y - hips.y);           // dalle anche al collo
        const float width = std::fabs(armL.x - armR.x);
        const float side = armL.x > armR.x ? 1.0f : -1.0f;           // da che parte sta la sinistra

        // 1. spalle in fuori e un po' in alto: le braccia partono piu' larghe (spalline)
        const float out = width * 0.26f;
        s.MoveBone("LeftArm", glm::vec3(side * out, H * 0.035f, 0.0f));
        s.MoveBone("RightArm", glm::vec3(-side * out, H * 0.035f, 0.0f));
        if (s.Has("LeftPauldron")) s.MoveBone("LeftPauldron", glm::vec3(side * out, H * 0.035f, 0.0f));
        if (s.Has("RightPauldron")) s.MoveBone("RightPauldron", glm::vec3(-side * out, H * 0.035f, 0.0f));
        s.ApplyBoneMoves();

        // 2. busto a V: largo in alto (dorsali, petto, trapezi), stretto in vita e al bacino; deltoidi grossi
        const glm::vec3 armL2 = s.Pos("LeftArm"), armR2 = s.Pos("RightArm"), foreL = s.Pos("LeftForeArm"), foreR = s.Pos("RightForeArm");
        const glm::vec3 upL = s.Pos("LeftUpLeg"), upR = s.Pos("RightUpLeg"), kneeL = s.Pos("LeftLeg"), kneeR = s.Pos("RightLeg");
        const glm::vec3 footL = s.Pos("LeftFoot"), footR = s.Pos("RightFoot");
        const float cx = hips.x, cz = hips.z;
        for (auto& mesh : model.meshes) {
            for (auto& v : mesh.vertices) {
                const float wT = s.Weight(v, [](const std::string& b) { return IsTorsoBone(b); });
                const float wL = s.Weight(v, [](const std::string& b) { return IsArmBone(b, true); });
                const float wR = s.Weight(v, [](const std::string& b) { return IsArmBone(b, false); });
                const float wLeg = s.Weight(v, [](const std::string& b) { return IsLegBone(b); });
                const float t = (v.Position.y - hips.y) / H;            // 0 anche, 1 collo
                glm::vec3& p = v.Position;
                if (wT > 0.0f) {
                    // larghezza: 0.94 al bacino, 0.86 in vita, 1.30 sotto le ascelle, 1.22 alle spalle, poi il collo
                    float sx = 0.94f;
                    sx = glm::mix(sx, 0.86f, Smooth(0.05f, 0.30f, t));
                    sx = glm::mix(sx, 1.30f, Smooth(0.32f, 0.78f, t));
                    sx = glm::mix(sx, 1.18f, Smooth(0.80f, 1.0f, t));
                    sx = glm::mix(sx, 1.0f, Smooth(1.02f, 1.15f, t));
                    // profondita': petto alto in avanti, schiena larga dietro
                    const bool front = p.z > cz;
                    float sz = glm::mix(0.92f, front ? 1.16f : 1.10f, Smooth(0.35f, 0.80f, t));
                    sz = glm::mix(sz, 1.0f, Smooth(1.02f, 1.15f, t));
                    const float k = std::min(1.0f, wT);
                    p.x = cx + (p.x - cx) * glm::mix(1.0f, sx, k);
                    p.z = cz + (p.z - cz) * glm::mix(1.0f, sz, k);
                    p.y += H * 0.03f * Smooth(0.7f, 1.0f, t) * k;          // trapezi alti
                }
                // deltoidi e bicipiti piu' grossi attorno all'osso del braccio
                auto thicken = [&](float w, const glm::vec3& a, const glm::vec3& b) {
                    if (w <= 0.0f) return;
                    const glm::vec3 ab = b - a;
                    const float l2 = std::max(1e-8f, glm::dot(ab, ab));
                    const float h = std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f);
                    const glm::vec3 axis = a + ab * h;
                    const float k = (1.35f - 0.25f * h) - 1.0f;            // +35% alla spalla, +10% al gomito
                    p += (p - axis) * k * w;
                };
                thicken(wL, armL2, foreL);
                thicken(wR, armR2, foreR);
                // cosce piene (un uomo piu' grosso, non un manichino a gambe sottili); la V resta dal busto
                if (wLeg > 0.0f) {
                    const bool leftSide = (p.x - cx) * side > 0.0f;
                    const glm::vec3 a = leftSide ? upL : upR, b = leftSide ? kneeL : kneeR, f = leftSide ? footL : footR;
                    auto around = [&](const glm::vec3& a0, const glm::vec3& b0, float k0, float k1) {
                        const glm::vec3 ab = b0 - a0;
                        const float l2 = std::max(1e-8f, glm::dot(ab, ab));
                        const float hh = glm::dot(p - a0, ab) / l2;
                        if (hh < -0.1f || hh > 1.05f) return;
                        const glm::vec3 axis = a0 + ab * std::clamp(hh, 0.0f, 1.0f);
                        glm::vec3 r = p - axis; r -= ab * (glm::dot(r, ab) / l2);   // solo di traverso all'osso
                        // gonfio a meta' osso, quasi niente all'attacco (il bacino resta stretto: la V non si perde)
                        const float c = std::clamp(hh, 0.0f, 1.0f);
                        p += r * (glm::mix(k0, k1, c) - 1.0f) * std::sin(3.14159f * std::clamp(c * 0.85f + 0.15f, 0.0f, 1.0f)) * wLeg;
                    };
                    if (p.y > b.y) around(a, b, 1.16f, 1.10f);   // coscia
                    else around(b, f, 1.08f, 1.0f);             // polpaccio
                }
            }
        }
        s.Finish();
    }

    // ---------------------------------------------------------------------------------------------------------
    // Vrepigh: uomo-cinghiale
    struct BoarHead {
        std::unique_ptr<SkinnedMesh> snout, tusks, eyes;
        SkinnedPartSet parts;
    };

    namespace BodySculptDetail {
        // Testa del cinghiale nello spazio della testa: origine all'osso Head, y in alto, z avanti, unita' = h
        // (altezza della testa). Grugno, disco del naso, arcate sopraccigliari, orecchie e cresta di setole.
        inline float BoarSkinSdf(const glm::vec3& q) {
            using namespace Sdf;
            float d = RoundCone(q, glm::vec3(0.0f, 0.28f, 0.10f), glm::vec3(0.0f, 0.14f, 0.62f), 0.26f, 0.17f);   // grugno
            d = SUnion(d, Ellipsoid(q, glm::vec3(0.0f, 0.15f, 0.64f), glm::vec3(0.19f, 0.15f, 0.07f)), 0.04f);   // disco del naso
            d = SUnion(d, Ellipsoid(q, glm::vec3(0.0f, 0.44f, 0.30f), glm::vec3(0.30f, 0.08f, 0.12f)), 0.10f);   // arcate
            d = SUnion(d, Ellipsoid(q, glm::vec3(0.0f, 0.04f, 0.32f), glm::vec3(0.24f, 0.12f, 0.24f)), 0.10f);   // mascella pesante
            for (float sx : { -1.0f, 1.0f }) {   // orecchie a punta, all'indietro e in fuori
                d = SUnion(d, RoundCone(q, glm::vec3(sx * 0.26f, 0.52f, -0.02f), glm::vec3(sx * 0.48f, 0.80f, -0.20f), 0.10f, 0.02f), 0.05f);
            }
            // cresta di setole: lama sottile dalla fronte alla nuca, frastagliata
            const float ridge = RoundCone(q, glm::vec3(0.0f, 0.62f, 0.20f), glm::vec3(0.0f, 0.55f, -0.45f), 0.07f, 0.03f);
            const float teeth = std::sin(q.z * 55.0f) * 0.025f;
            d = SUnion(d, std::max(ridge + teeth, std::fabs(q.x) - 0.05f), 0.03f);
            // narici
            for (float sx : { -1.0f, 1.0f }) d = std::max(d, -Sphere(q, glm::vec3(sx * 0.07f, 0.16f, 0.71f), 0.045f));
            return d;
        }
        inline float BoarTuskSdf(const glm::vec3& q) {
            using namespace Sdf;
            float d = 1e9f;
            for (float sx : { -1.0f, 1.0f }) {   // dalla mascella in avanti, poi su e indietro
                const glm::vec3 a(sx * 0.17f, 0.02f, 0.44f), b(sx * 0.26f, 0.16f, 0.58f), c(sx * 0.27f, 0.36f, 0.52f);
                d = std::min(d, RoundCone(q, a, b, 0.055f, 0.042f));
                d = std::min(d, RoundCone(q, b, c, 0.042f, 0.008f));
            }
            return d;
        }
        inline float BoarEyesSdf(const glm::vec3& q) {
            float d = 1e9f;
            for (float sx : { -1.0f, 1.0f }) d = std::min(d, Sdf::Sphere(q, glm::vec3(sx * 0.17f, 0.36f, 0.35f), 0.05f));
            return d;
        }
    }

    inline void SculptBoarBody(SkinnedModel& model, BoarHead* head) {
        using namespace BodySculptDetail;
        BodySculptor s(model);
        if (!s.Has("Hips") || !s.Has("Neck") || !s.Has("Head") || !s.Has("LeftArm")) return;
        const glm::vec3 hips = s.Pos("Hips"), neck = s.Pos("Neck");
        const glm::vec3 armL = s.Pos("LeftArm"), armR = s.Pos("RightArm");
        const float H = std::max(1e-3f, neck.y - hips.y);
        const float width = std::fabs(armL.x - armR.x);
        const float side = armL.x > armR.x ? 1.0f : -1.0f;

        // 1. ossa: spalle in fuori; testa sopra il collo. Nel modello base la testa e' bassa e avanti, quasi alla
        //    altezza del collo: con le clip (schiena piegata in avanti) finiva dentro il petto e il vrepigh
        //    sembrava senza testa. Ora sta sopra le spalle, col grugno che sporge in avanti.
        s.MoveBone("LeftArm", glm::vec3(side * width * 0.10f, 0.0f, 0.0f));
        s.MoveBone("RightArm", glm::vec3(-side * width * 0.10f, 0.0f, 0.0f));
        s.MoveBone("Head", glm::vec3(0.0f, H * 0.18f, -H * 0.10f));
        s.ApplyBoneMoves();

        const glm::vec3 headP = s.Pos("Head");
        const glm::vec3 armL2 = s.Pos("LeftArm"), armR2 = s.Pos("RightArm"), foreL = s.Pos("LeftForeArm"), foreR = s.Pos("RightForeArm");
        const float cx = hips.x, cz = hips.z;
        for (auto& mesh : model.meshes) {
            for (auto& v : mesh.vertices) {
                const float wT = s.Weight(v, [](const std::string& b) { return IsTorsoBone(b) || b == "Neck"; });
                const float wL = s.Weight(v, [](const std::string& b) { return IsArmBone(b, true); });
                const float wR = s.Weight(v, [](const std::string& b) { return IsArmBone(b, false); });
                const float t = (v.Position.y - hips.y) / H;
                glm::vec3& p = v.Position;
                if (wT > 0.0f) {
                    // gobba: massa di muscoli dietro fra le spalle che sale fino alla nuca
                    const float back = Smooth(0.0f, -0.25f * H, p.z - cz);              // 0 davanti, 1 dietro
                    const float band = Smooth(0.55f, 0.85f, t) * (1.0f - Smooth(1.10f, 1.30f, t));
                    const float mid = std::exp(-std::pow((p.x - cx) / (width * 0.45f), 2.0f));
                    p.z -= H * 0.20f * back * band * mid * wT;
                    p.y += H * 0.10f * back * band * mid * wT;
                    // busto piu' massiccio: largo al petto, pancia piena
                    float sx = glm::mix(1.05f, 1.18f, Smooth(0.35f, 0.85f, t));
                    sx = glm::mix(sx, 1.0f, Smooth(1.05f, 1.25f, t));
                    p.x = cx + (p.x - cx) * glm::mix(1.0f, sx, std::min(1.0f, wT));
                    // collo spesso
                    if (t > 0.95f) p.x = cx + (p.x - cx) * glm::mix(1.0f, 1.25f, Smooth(0.95f, 1.1f, t) * wT);
                }
                auto thicken = [&](float w, const glm::vec3& a, const glm::vec3& b, float k0, float k1) {
                    if (w <= 0.0f) return;
                    const glm::vec3 ab = b - a;
                    const float l2 = std::max(1e-8f, glm::dot(ab, ab));
                    const float h = std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f);
                    p += (p - (a + ab * h)) * (glm::mix(k0, k1, h) - 1.0f) * w;
                };
                thicken(wL, armL2, foreL, 1.35f, 1.20f);
                thicken(wR, armR2, foreR, 1.35f, 1.20f);
            }
        }
        s.Finish();

        // 2. testa da cinghiale: grugno, zanne e occhi come parti in piu' sull'osso della testa
        if (!head) return;
        const int headId = s.IdOf("Head");
        const int jawId = s.IdOf("Jaw");
        if (headId < 0) return;
        const float h = H * 0.42f;   // misura della testa (unita' della mesh)
        // Il grugno parte dalla faccia del modello (non dall'osso, che sta dentro il cranio): punta dei vertici della
        // testa in avanti, cosi' sporge davvero invece di restare nascosto nel viso
        glm::vec3 headO = headP;
        {
            std::vector<float> zs;
            for (const auto& mesh : model.meshes)
                for (const auto& v : mesh.vertices)
                    if (s.Weight(v, [](const std::string& b) { return b.rfind("Head", 0) == 0 || b == "Jaw"; }) > 0.5f &&
                        std::fabs(v.Position.x - headP.x) < h * 0.3f) zs.push_back(v.Position.z);
            if (zs.size() > 20) {
                std::sort(zs.begin(), zs.end());
                const float faceZ = zs[static_cast<size_t>(zs.size() * 0.97f)];
                headO.z = std::max(headP.z, faceZ - h * 0.30f);
            }
        }
        // Orientamento: la testa guarda +z della mesh (vedi ProceduralPose.h)
        auto polygonize = [&](float (*f)(const glm::vec3&), const glm::vec3& lo, const glm::vec3& hi, float cell, bool jawPart) {
            const auto field = [&](const glm::vec3& p) { return f((p - headO) / h) * h; };
            SdfMesh m = SdfPolygonize(field, headO + lo * h, headO + hi * h, cell * h);
            std::vector<SkinnedVertex> verts(m.positions.size());
            const std::vector<float> ao = SdfAmbientOcclusion(field, m, 0.06f * h);
            for (size_t i = 0; i < verts.size(); ++i) {
                verts[i].Position = m.positions[i];
                verts[i].Normal = m.normals[i];
                const glm::vec3 q = (m.positions[i] - headO) / h;
                verts[i].TexCoords = glm::vec2(0.5f + 0.25f * std::clamp(q.y - 0.3f, -1.0f, 1.0f), i < ao.size() ? ao[i] : 1.0f);   // curvatura finta, occlusione
                // mascella: la parte bassa del grugno (e le zanne) seguono la mascella
                const float jw = (jawId >= 0) ? (jawPart ? 1.0f : 1.0f - Smooth(0.02f, 0.18f, q.y)) * Smooth(0.15f, 0.40f, q.z) : 0.0f;
                if (jw > 0.01f) { verts[i].AddBoneData(jawId, jw); if (jw < 0.99f) verts[i].AddBoneData(headId, 1.0f - jw); }
                else verts[i].AddBoneData(headId, 1.0f);
            }
            return std::make_unique<SkinnedMesh>(std::move(verts), std::move(m.indices));
        };
        head->snout = polygonize(&BoarSkinSdf, glm::vec3(-0.6f, -0.2f, -0.6f), glm::vec3(0.6f, 0.95f, 0.85f), 0.02f, false);
        head->tusks = polygonize(&BoarTuskSdf, glm::vec3(-0.4f, -0.1f, 0.3f), glm::vec3(0.4f, 0.45f, 0.7f), 0.012f, true);
        head->eyes = polygonize(&BoarEyesSdf, glm::vec3(-0.3f, 0.25f, 0.25f), glm::vec3(0.3f, 0.45f, 0.45f), 0.01f, false);
        head->parts.parts.clear();
        auto add = [&](const std::unique_ptr<SkinnedMesh>& m, int detail, glm::vec4 col, float rough, int colorSource, int baked, float emissive) {
            if (!m) return;
            GarmentDraw g;
            g.mesh = m.get(); g.detail = detail; g.color = col; g.roughness = rough; g.colorSource = colorSource; g.baked = baked; g.emissive = emissive;
            g.invHeight = 1.0f / std::max(1e-3f, h * 4.2f);
            head->parts.parts.push_back(g);
        };
        add(head->snout, 1, glm::vec4(1.0f), 0.75f, 1, 1, 0.0f);                       // pelle del colore dell'unita'
        add(head->tusks, 6, glm::vec4(0.86f, 0.80f, 0.66f, 1.0f), 0.4f, 0, 1, 0.0f);   // avorio sporco
        add(head->eyes, 0, glm::vec4(0.35f, 0.05f, 0.03f, 1.0f), 0.1f, 0, 0, 0.25f);   // piccoli occhi rossi e lucidi
    }

}
