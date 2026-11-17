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
#include <functional>
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

        // Osso nuovo (senza vertici: li assegna chi lo crea), figlio di 'parent', in 'pivot' (spazio della mesh,
        // senza rotazione). Prima delle clip: le animazioni lo ereditano dalla gerarchia. Ritorna l'id (-1 = no).
        int AddBone(const char* name, const char* parent, const glm::vec3& pivot) {
            using namespace BodySculptDetail;
            const auto it = m_Bind.bySuffix.find(parent);
            if (it == m_Bind.bySuffix.end()) return -1;
            const std::string prefix = it->second.substr(0, it->second.size() - std::string(parent).size());
            const std::string full = prefix + name;
            if (auto bi = m_Model.boneInfoMap.find(full); bi != m_Model.boneInfoMap.end()) return bi->second.id;
            int maxId = -1;
            for (const auto& [n, b] : m_Model.boneInfoMap) maxId = std::max(maxId, b.id);
            if (maxId + 1 >= 100) return -1;   // ANIMATOR_MAX_BONES
            AssimpNodeData* p = FindNode(m_Model.rootNode, it->second);
            if (!p) return -1;
            const glm::mat4 meshBind = glm::translate(glm::mat4(1.0f), pivot);
            const glm::mat4 nodeGlobal = glm::inverse(m_Gi) * meshBind;
            AssimpNodeData node;
            node.name = full;
            node.transformation = glm::inverse(m_Bind.global[it->second]) * nodeGlobal;
            p->children.push_back(node);
            p->childrenCount = static_cast<int>(p->children.size());
            const int id = maxId + 1;
            m_Model.boneInfoMap[full] = { id, glm::inverse(meshBind) };
            m_Model.boneCounter = std::max(m_Model.boneCounter, id + 1);
            m_Bind.global[full] = nodeGlobal; m_Bind.parentGlobal[full] = m_Bind.global[it->second];
            m_Bind.bySuffix[name] = full; m_IdToName[id] = full;
            if (static_cast<int>(m_Disp.size()) <= id) m_Disp.resize(id + 1, glm::vec3(0.0f));
            return id;
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
        std::unique_ptr<SkinnedMesh> hooves, tail, mane;   // zoccoli, coda a ricciolo, cresta di setole sulla schiena
        SkinnedPartSet parts;
    };

    namespace BodySculptDetail {
        // Testa del cinghiale nello spazio della testa: origine all'osso Head, y in alto, z avanti, unita' = h
        // (altezza della testa). Grugno, disco del naso, arcate sopraccigliari, orecchie e cresta di setole.
        inline float BoarSkinSdf(const glm::vec3& q) {
            using namespace Sdf;
            float d = RoundCone(q, glm::vec3(0.0f, 0.28f, 0.10f), glm::vec3(0.0f, 0.13f, 0.72f), 0.27f, 0.17f);   // grugno lungo
            d = SUnion(d, Ellipsoid(q, glm::vec3(0.0f, 0.14f, 0.74f), glm::vec3(0.19f, 0.15f, 0.07f)), 0.04f);   // disco del naso
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
            for (float sx : { -1.0f, 1.0f }) d = std::max(d, -Sphere(q, glm::vec3(sx * 0.07f, 0.15f, 0.81f), 0.045f));
            return d;
        }
        inline float BoarTuskSdf(const glm::vec3& q) {
            using namespace Sdf;
            float d = 1e9f;
            for (float sx : { -1.0f, 1.0f }) {   // dalla mascella in avanti, poi su e indietro
                const glm::vec3 a(sx * 0.17f, 0.02f, 0.52f), b(sx * 0.27f, 0.17f, 0.68f), c(sx * 0.29f, 0.40f, 0.61f);
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
        glm::vec3 hips = s.Pos("Hips"), neck = s.Pos("Neck");
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
        // Proporzioni da cinghiale in piedi: cosce e stinchi corti e tozzi, braccia lunghe e pesanti. Il bacino
        // scende quanto si accorciano le gambe, cosi' i piedi restano a terra.
        if (s.Has("LeftLeg") && s.Has("RightLeg") && s.Has("LeftFoot") && s.Has("RightFoot")) {
            const float thigh = glm::length(s.Pos("LeftLeg") - s.Pos("LeftUpLeg")), shin = glm::length(s.Pos("LeftFoot") - s.Pos("LeftLeg"));
            const float dT = thigh * 0.16f, dS = shin * 0.10f;
            s.MoveBone("Hips", glm::vec3(0.0f, -(dT + dS), 0.0f));
            for (const char* k : { "LeftLeg", "RightLeg" }) s.MoveBone(k, glm::vec3(0.0f, dT, 0.0f));
            for (const char* f : { "LeftFoot", "RightFoot" }) s.MoveBone(f, glm::vec3(0.0f, dS, 0.0f));
        }
        for (const char* S : { "Left", "Right" }) {
            const std::string a = std::string(S) + "Arm", f = std::string(S) + "ForeArm", h = std::string(S) + "Hand";
            if (!s.Has(a.c_str()) || !s.Has(f.c_str()) || !s.Has(h.c_str())) continue;
            const glm::vec3 up = s.Pos(f.c_str()) - s.Pos(a.c_str()), lo = s.Pos(h.c_str()) - s.Pos(f.c_str());
            s.MoveBone(f.c_str(), up * 0.07f);
            s.MoveBone(h.c_str(), lo * 0.07f);
        }
        s.ApplyBoneMoves();
        hips = s.Pos("Hips"); neck = s.Pos("Neck");   // il bacino e' sceso con le gambe corte

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
        const float h = H * 0.46f;   // misura della testa (unita' della mesh): testa grossa da cinghiale
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
        head->snout = polygonize(&BoarSkinSdf, glm::vec3(-0.6f, -0.2f, -0.6f), glm::vec3(0.6f, 0.95f, 0.95f), 0.02f, false);
        head->tusks = polygonize(&BoarTuskSdf, glm::vec3(-0.4f, -0.1f, 0.38f), glm::vec3(0.45f, 0.5f, 0.8f), 0.012f, true);
        // Orecchie con le loro ossa (sbattono con la corsa e la testa: RaceMotion "ears")
        for (int k = 0; k < 2; ++k) {
            const float sx = k == 0 ? side : -side;   // sinistra del personaggio
            const int earId = s.AddBone(k == 0 ? "LeftEar" : "RightEar", "Head", headO + glm::vec3(sx * 0.26f, 0.52f, -0.02f) * h);
            if (earId < 0 || !head->snout) continue;
            for (auto& v : head->snout->vertices) {
                const glm::vec3 q = (v.Position - headO) / h;
                const float w = Smooth(0.2f, 0.32f, q.x * sx) * Smooth(0.46f, 0.6f, q.y);
                if (w < 0.01f) continue;
                for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) v.Weights[i] *= (1.0f - w);
                int slot = 0;
                for (int i = 1; i < MAX_BONE_INFLUENCE; ++i) if (v.Weights[i] < v.Weights[slot]) slot = i;
                v.BoneIDs[slot] = earId; v.Weights[slot] = w;
            }
        }
        if (head->snout) {   // pesi delle orecchie cambiati: mesh di nuovo sulla GPU
            SkinnedMesh& old = *head->snout;
            glDeleteVertexArrays(1, &old.VAO); glDeleteBuffers(1, &old.VBO); glDeleteBuffers(1, &old.EBO);
            std::vector<SkinnedVertex> v = std::move(old.vertices);
            std::vector<unsigned int> idx = std::move(old.indices);
            head->snout = std::make_unique<SkinnedMesh>(std::move(v), std::move(idx));
        }
        head->eyes = polygonize(&BoarEyesSdf, glm::vec3(-0.3f, 0.25f, 0.25f), glm::vec3(0.3f, 0.45f, 0.45f), 0.01f, false);

        // 3. Corpo da cinghiale: zoccoli, coda a ricciolo (con il suo osso), cresta di setole lungo la schiena
        auto meshFrom = [&](const std::function<float(const glm::vec3&)>& f, glm::vec3 lo, glm::vec3 hi, float cell,
                            const std::function<void(SkinnedVertex&)>& weigh) -> std::unique_ptr<SkinnedMesh> {
            SdfMesh m = SdfPolygonize(f, lo, hi, cell);
            if (m.indices.empty()) return nullptr;
            const std::vector<float> ao = SdfAmbientOcclusion(f, m, cell * 4.0f);
            std::vector<SkinnedVertex> verts(m.positions.size());
            for (size_t i = 0; i < verts.size(); ++i) {
                verts[i].Position = m.positions[i]; verts[i].Normal = m.normals[i];
                verts[i].TexCoords = glm::vec2(0.5f, i < ao.size() ? ao[i] : 1.0f);
                weigh(verts[i]);
            }
            return std::make_unique<SkinnedMesh>(std::move(verts), std::move(m.indices));
        };
        // vertice del corpo piu' vicino (pesi copiati): per la cresta, che deve piegarsi con la schiena
        std::vector<const SkinnedVertex*> bodyV;
        for (const auto& mesh : model.meshes) for (const auto& v : mesh.vertices) bodyV.push_back(&v);
        std::vector<const SkinnedVertex*> backV;   // solo la schiena (la cresta sta li')
        for (const auto* v : bodyV) if (v->Position.z < hips.z && std::fabs(v->Position.x - cx) < H * 0.35f && v->Position.y > hips.y + H * 0.3f) backV.push_back(v);
        auto copyNearest = [&](SkinnedVertex& out) {
            const SkinnedVertex* best = nullptr; float bd = 1e30f;
            for (const auto* v : backV) { const glm::vec3 d = v->Position - out.Position; const float dd = glm::dot(d, d); if (dd < bd) { bd = dd; best = v; } }
            if (!best) return;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) { out.BoneIDs[k] = best->BoneIDs[k]; out.Weights[k] = best->Weights[k]; }
        };
        {   // zoccoli: blocco fesso davanti che copre il piede (tutto il piede: sotto c'e' il piede umano)
            std::function<float(const glm::vec3&)> hoofF = [](const glm::vec3&) { return 1e9f; };
            glm::vec3 lo(1e9f), hi(-1e9f);
            struct Hoof { glm::vec3 a, b; float r, ground; glm::vec3 fwd; };
            std::vector<Hoof> hv;
            for (const char* S : { "Left", "Right" }) {
                const std::string fb = std::string(S) + "Foot", tb = std::string(S) + "ToeBase";
                if (!s.Has(fb.c_str()) || !s.Has(tb.c_str())) continue;
                const glm::vec3 F = s.Pos(fb.c_str()), T = s.Pos(tb.c_str());
                glm::vec3 fwd = T - F; fwd.y = 0.0f; fwd = glm::length(fwd) > 1e-6f ? glm::normalize(fwd) : glm::vec3(0, 0, 1);
                float r = 0.0f, ground = 1e9f, tipZ = -1e9f;
                for (const auto* v : bodyV) {
                    float w = 0.0f;
                    for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) if (v->BoneIDs[k] >= 0) { const std::string n = s.NameOf(v->BoneIDs[k]); if (n.rfind(fb, 0) == 0 || n.rfind(std::string(S) + "Toe", 0) == 0) w += v->Weights[k]; }
                    if (w < 0.5f) continue;
                    ground = std::min(ground, v->Position.y);
                    tipZ = std::max(tipZ, glm::dot(v->Position - F, fwd));
                    const glm::vec3 d = v->Position - F; const float side2 = glm::length(d - fwd * glm::dot(d, fwd) - glm::vec3(0, d.y, 0));
                    r = std::max(r, side2);
                }
                if (ground > 1e8f) continue;
                Hoof hf; hf.a = glm::vec3(F.x, ground, F.z) - fwd * r * 0.4f; hf.b = glm::vec3(F.x, ground, F.z) + fwd * (tipZ + r * 0.15f); hf.r = r * 1.15f; hf.ground = ground; hf.fwd = fwd;
                hv.push_back(hf);
                lo = glm::min(lo, glm::min(hf.a, hf.b) - glm::vec3(r * 2.0f)); hi = glm::max(hi, glm::max(hf.a, hf.b) + glm::vec3(r * 2.0f, (F.y - ground) * 1.3f + r, r * 2.0f));
            }
            if (!hv.empty()) {
                auto f = [hv](const glm::vec3& p) {
                    float d = 1e9f;
                    for (const auto& hf : hv) {
                        const float height = hf.r * 1.15f;
                        // cuneo: alto dietro (caviglia), basso e largo davanti
                        const glm::vec3 ab = hf.b - hf.a;
                        const float t = std::clamp(glm::dot(p - hf.a, ab) / glm::dot(ab, ab), 0.0f, 1.0f);
                        const glm::vec3 c = hf.a + ab * t + glm::vec3(0.0f, height * (0.55f - 0.35f * t), 0.0f);
                        const glm::vec3 rr(hf.r * (0.85f + 0.2f * t), height * (0.62f - 0.3f * t), hf.r * 0.9f);
                        float hd = Sdf::RoundBox(p, c, glm::vec3(rr.x, rr.y, glm::length(ab) * 0.06f + hf.r * 0.3f), hf.r * 0.35f);
                        hd = std::min(hd, Sdf::Capsule(p, hf.a + glm::vec3(0, height * 0.5f, 0), hf.b + glm::vec3(0, height * 0.18f, 0), hf.r * 0.78f));
                        hd = std::max(hd, hf.ground - p.y);                                           // suola piatta
                        // unghia fessa: taglio al centro nella meta' davanti
                        const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0, 1, 0), hf.fwd));
                        const float split = std::max(std::fabs(glm::dot(p - hf.a, side)) - hf.r * 0.05f, -(glm::dot(p - hf.a, hf.fwd) - glm::length(ab) * 0.6f));
                        hd = std::max(hd, -split);
                        d = std::min(d, hd);
                    }
                    return d;
                };
                head->hooves = meshFrom(f, lo, hi, hv[0].r * 0.09f, [&](SkinnedVertex& v) {
                    const bool l = (v.Position.x - cx) * side > 0.0f;
                    const int foot = s.IdOf(l ? "LeftFoot" : "RightFoot"), toe = s.IdOf(l ? "LeftToeBase" : "RightToeBase");
                    const Hoof& hf = hv[(l || hv.size() < 2) ? 0 : 1];
                    const float t = std::clamp(glm::dot(v.Position - hf.a, hf.fwd) / std::max(1e-4f, glm::length(hf.b - hf.a)), 0.0f, 1.0f);
                    const float wt = Smooth(0.45f, 0.75f, t);
                    if (toe >= 0 && wt > 0.01f) { v.AddBoneData(toe, wt); if (wt < 0.99f && foot >= 0) v.AddBoneData(foot, 1.0f - wt); }
                    else if (foot >= 0) v.AddBoneData(foot, 1.0f);
                });
            }
        }
        {   // coda: ricciolo che parte dall'alto dei glutei, con il suo osso (scodinzola correndo)
            float backZ = 1e9f;
            const glm::vec3 hp = s.Pos("Hips");
            for (const auto* v : bodyV) if (std::fabs(v->Position.x - cx) < H * 0.08f && std::fabs(v->Position.y - (hp.y + H * 0.02f)) < H * 0.05f) backZ = std::min(backZ, v->Position.z);
            if (backZ < 1e8f) {
                const glm::vec3 root(cx, hp.y + H * 0.02f, backZ + H * 0.02f);
                const int tailId = s.AddBone("Tail", "Hips", root);
                const float r = H * 0.03f;
                auto f = [root, r, H](const glm::vec3& p) {
                    float d = 1e9f;
                    glm::vec3 prev = root;
                    for (int i = 1; i <= 14; ++i) {   // esce all'indietro e si arriccia in alto
                        const float u = i / 14.0f, ang = u * 5.5f;
                        const glm::vec3 c = root + glm::vec3(std::sin(ang) * H * 0.05f * u, H * 0.05f * (1.0f - std::cos(ang)) * u + H * 0.06f * u, -H * 0.10f * u - std::cos(ang) * H * 0.02f * u);
                        d = std::min(d, Sdf::Capsule(p, prev, c, r * (1.0f - 0.7f * u)));
                        prev = c;
                    }
                    return d;
                };
                head->tail = meshFrom(f, root - glm::vec3(H * 0.15f), root + glm::vec3(H * 0.15f, H * 0.25f, H * 0.15f), r * 0.18f, [&](SkinnedVertex& v) {
                    if (tailId >= 0) v.AddBoneData(tailId, 1.0f); else v.AddBoneData(s.IdOf("Hips"), 1.0f);
                });
            }
        }
        {   // cresta di setole dalla nuca a meta' schiena: aculei che puntano indietro e in alto, sul colmo della gobba
            const float y0 = hips.y + H * 0.45f, y1 = neck.y + H * 0.08f;
            std::vector<glm::vec2> back;   // (y, z del dorso) lungo la linea mediana
            for (int i = 0; i <= 16; ++i) {
                const float y = glm::mix(y0, y1, i / 16.0f);
                float z = 1e9f;
                for (const auto* v : bodyV) if (std::fabs(v->Position.x - cx) < H * 0.05f && std::fabs(v->Position.y - y) < H * 0.03f) z = std::min(z, v->Position.z);
                if (z < 1e8f) back.push_back(glm::vec2(y, z));
            }
            if (back.size() > 4) {
                auto f = [back, cx, H](const glm::vec3& p) {
                    float d = 1e9f;
                    for (size_t i = 0; i < back.size(); ++i) {
                        const glm::vec3 base(cx, back[i].x, back[i].y + H * 0.01f);
                        const float len = H * (0.07f + 0.05f * std::sin(i * 1.7f) * std::sin(i * 1.7f)) * (0.6f + 0.4f * std::sin(3.14159f * i / back.size()));
                        const glm::vec3 tip = base + glm::normalize(glm::vec3(std::sin(i * 2.3f) * 0.25f, 0.55f, -1.0f)) * len;
                        d = std::min(d, Sdf::RoundCone(p, base, tip, H * 0.022f, H * 0.002f));
                    }
                    return d;
                };
                glm::vec3 lo(cx - H * 0.12f, y0 - H * 0.05f, 1e9f), hi(cx + H * 0.12f, y1 + H * 0.1f, -1e9f);
                for (const auto& b : back) { lo.z = std::min(lo.z, b.y - H * 0.18f); hi.z = std::max(hi.z, b.y + H * 0.05f); }
                head->mane = meshFrom(f, lo, hi, H * 0.006f, copyNearest);
            }
        }
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
        add(head->hooves, 0, glm::vec4(0.075f, 0.06f, 0.05f, 1.0f), 0.45f, 0, 1, 0.0f); // zoccoli di corno scuro
        add(head->tail, 1, glm::vec4(1.0f), 0.75f, 1, 1, 0.0f);                        // coda: pelle dell'unita'
        add(head->mane, 0, glm::vec4(0.09f, 0.07f, 0.06f, 1.0f), 0.9f, 0, 1, 0.0f);    // setole scure
    }

}
