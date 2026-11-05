#pragma once
// Corpi delle altre razze (ork, ogryn, astartes, aspirante, gretchin) ricavati dal corpo umano base.
//
// I modelli delle altre razze hanno proporzioni e volumi giusti ma una superficie grossolana (piedi a blocco,
// mani senza dita, muscoli a bolle). Il corpo umano (mixamo) ha invece anatomia, mani e piedi precisi.
// Qui si "veste" lo scheletro della razza con la mesh umana:
//   1) per ogni osso si confrontano le due ossature in posa T (origine, direzione, lunghezza) e si misura lo
//      spessore dei due corpi attorno all'osso, a fasce lungo l'osso e a spicchi attorno (rapporto di raggio);
//   2) ogni vertice umano viene portato sull'osso corrispondente della razza: allungato come l'osso e gonfiato
//      come il corpo della razza in quel punto (rapporti lisciati, cosi' resta la superficie umana e non le bolle);
//   3) la testa resta quella del modello della razza (zanne, orecchie, mascella), il resto e' umano adattato.
// Scheletro, ossa e animazioni restano quelli del modello della razza. Nessuna parte OpenGL: testabile offline.
#include "SkinnedVertex.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SunEngine {

    struct BodyFitBone {
        glm::vec3 origin{ 0.0f }, axis{ 0.0f, 1.0f, 0.0f }, ref{ 1.0f, 0.0f, 0.0f };
        float length = 1.0f;
        bool valid = false;
    };

    struct BodyFitOptions {
        float headWeight = 0.5f;      // vertici con almeno questo peso sulla testa: testa della razza
        float headOverlap = 0.35f;    // la testa della razza scende un po' sotto il taglio (niente buchi al collo)
        float minRatio = 0.55f, maxRatio = 3.5f;
        int smoothPasses = 2;         // lisciatura dei rapporti di spessore (piu' alta = meno "bolle" della razza)
        float bulk = 1.0f;            // gonfiaggio extra uniforme (es. 1.04 = muscoli un po' piu' pieni)
        float conform = 0.9f;         // quanto il corpo adattato si appoggia sulla sagoma della razza (0 = solo ossa)
        int conformSmooth = 40;       // lisciatura dello spostamento verso la sagoma (piu' alta = meno bozzi della razza)
    };

    namespace BodyFitDetail {
        inline std::string Suffix(const std::string& n) { const auto p = n.rfind(':'); return p == std::string::npos ? n : n.substr(p + 1); }
        constexpr int kBins = 6, kSectors = 8;
        constexpr float kTMin = -0.25f, kTMax = 1.25f;   // fasce lungo l'osso (frazione della lunghezza)

        struct Profile { float sum[kBins][kSectors] = {}; float cnt[kBins][kSectors] = {}; };
        // Tratto dell'osso coperto dai suoi vertici (in frazioni della lunghezza): le ossa corte della schiena
        // reggono una pancia o un torace che va ben oltre la loro lunghezza
        struct Range { float lo = kTMin, hi = kTMax; };

        inline void Locate(const BodyFitBone& b, const glm::vec3& v, float& tn, float& r, float& ang) {
            const glm::vec3 d = v - b.origin;
            const float t = glm::dot(d, b.axis);
            const glm::vec3 rv = d - t * b.axis;
            tn = t / std::max(1e-4f, b.length);
            r = glm::length(rv);
            const glm::vec3 e2 = glm::cross(b.axis, b.ref);
            ang = std::atan2(glm::dot(rv, e2), glm::dot(rv, b.ref));   // -pi..pi
        }
        inline int BinOf(float tn, const Range& rg) { return std::clamp(static_cast<int>((tn - rg.lo) / std::max(1e-3f, rg.hi - rg.lo) * kBins), 0, kBins - 1); }
        inline int SectorOf(float ang) { return (static_cast<int>(std::floor((ang + 3.14159265f) / 6.2831853f * kSectors)) % kSectors + kSectors) % kSectors; }
    }

    // Ossatura di un modello in posa T, nello spazio della mesh. boneOffsets: nome -> offset matrix (mesh -> osso).
    // nodeGlobals: nome -> trasformazione globale dei nodi (per le ossa finali senza pesi). children: nome -> figli.
    struct BodyFitSkeleton {
        std::unordered_map<std::string, BodyFitBone> bones;   // per nome senza prefisso
        std::unordered_map<std::string, int> ids;             // nome senza prefisso -> id dell'osso nel modello
        std::unordered_map<std::string, std::string> parent;  // nome senza prefisso -> padre (senza prefisso)
    };

    inline BodyFitSkeleton BodyFitBuildSkeleton(const std::unordered_map<std::string, glm::mat4>& boneOffsets,
                                                const std::unordered_map<std::string, int>& boneIds,
                                                const std::unordered_map<std::string, glm::mat4>& nodeGlobals,
                                                const std::unordered_map<std::string, std::vector<std::string>>& children,
                                                const std::unordered_map<std::string, std::string>& parents) {
        using namespace BodyFitDetail;
        BodyFitSkeleton sk;
        // Correzione nodo -> spazio mesh (uguale per tutte le ossa): presa da un osso con pesi
        glm::mat4 corr(1.0f);
        for (const auto& [name, off] : boneOffsets) {
            auto it = nodeGlobals.find(name);
            if (it != nodeGlobals.end()) { corr = glm::inverse(off) * glm::inverse(it->second); break; }
        }
        auto meshPos = [&](const std::string& full) -> glm::vec3 {
            auto ob = boneOffsets.find(full);
            if (ob != boneOffsets.end()) return glm::vec3(glm::inverse(ob->second)[3]);
            auto it = nodeGlobals.find(full);
            return it == nodeGlobals.end() ? glm::vec3(0.0f) : glm::vec3((corr * it->second)[3]);
        };
        for (const auto& [full, off] : boneOffsets) {
            const std::string name = Suffix(full);
            BodyFitBone b;
            const glm::mat4 G = glm::inverse(off);
            b.origin = glm::vec3(G[3]);
            // Figlio che continua l'osso: quello "centrale" se l'osso e' centrale, il dito medio per la mano
            std::string best;
            auto ch = children.find(full);
            if (ch != children.end() && !ch->second.empty()) {
                const bool sided = name.find("Left") != std::string::npos || name.find("Right") != std::string::npos;
                for (const auto& c : ch->second) {
                    const std::string cs = Suffix(c);
                    if (name.find("Hand") != std::string::npos && name.find("Hand") + 4 == name.size()) { if (cs.find("Middle1") != std::string::npos) { best = c; break; } continue; }
                    const bool cSided = cs.find("Left") != std::string::npos || cs.find("Right") != std::string::npos;
                    if (!sided && cSided) continue;
                    best = c; break;
                }
                if (best.empty()) best = ch->second.front();
            }
            glm::vec3 axis(0.0f);
            if (!best.empty()) { axis = meshPos(best) - b.origin; }
            if (glm::length(axis) < 1e-4f) {   // osso finale: continua la direzione del padre
                auto p = parents.find(full);
                if (p != parents.end()) axis = (b.origin - meshPos(p->second)) * 0.6f;
            }
            b.length = std::max(1e-3f, glm::length(axis));
            b.axis = glm::length(axis) > 1e-5f ? axis / glm::length(axis) : glm::vec3(0, 1, 0);
            // Riferimento per gli spicchi: l'asse X dell'osso proiettato sul piano perpendicolare
            glm::vec3 ref = glm::normalize(glm::vec3(G[0]));
            ref -= glm::dot(ref, b.axis) * b.axis;
            if (glm::length(ref) < 1e-3f) { ref = glm::vec3(G[2]); ref -= glm::dot(ref, b.axis) * b.axis; }
            b.ref = glm::normalize(ref);
            b.valid = true;
            sk.bones[name] = b;
            auto idIt = boneIds.find(full);
            if (idIt != boneIds.end()) sk.ids[name] = idIt->second;
            auto p = parents.find(full);
            if (p != parents.end()) sk.parent[name] = Suffix(p->second);
        }
        return sk;
    }

    // Ossatura da un modello caricato (SkinnedModel: boneInfoMap + rootNode)
    template <class Model>
    inline BodyFitSkeleton BodyFitSkeletonOf(const Model& m) {
        std::unordered_map<std::string, glm::mat4> offsets, globals;
        std::unordered_map<std::string, int> ids;
        std::unordered_map<std::string, std::vector<std::string>> children;
        std::unordered_map<std::string, std::string> parents;
        for (const auto& [n, info] : m.boneInfoMap) { offsets[n] = info.offsetMatrix; ids[n] = info.id; }
        struct Walk {
            static void Go(const decltype(m.rootNode)& node, const glm::mat4& parentG, const std::string& parentName,
                           std::unordered_map<std::string, glm::mat4>& g, std::unordered_map<std::string, std::vector<std::string>>& ch,
                           std::unordered_map<std::string, std::string>& par) {
                const glm::mat4 G = parentG * node.transformation;
                g[node.name] = G;
                if (!parentName.empty()) { ch[parentName].push_back(node.name); par[node.name] = parentName; }
                for (const auto& c : node.children) Go(c, G, node.name, g, ch, par);
            }
        };
        Walk::Go(m.rootNode, glm::mat4(1.0f), std::string(), globals, children, parents);
        return BodyFitBuildSkeleton(offsets, ids, globals, children, parents);
    }

    struct BodyFitMesh {
        const std::vector<SkinnedVertex>* vertices = nullptr;
        const std::vector<unsigned int>* indices = nullptr;
    };

    // Risultato: vertici e indici nello spazio mesh della razza, con gli id delle ossa della razza
    inline void BodyFit(const BodyFitSkeleton& human, const std::vector<BodyFitMesh>& humanMeshes,
                        const BodyFitSkeleton& race, const std::vector<BodyFitMesh>& raceMeshes,
                        const BodyFitOptions& opt,
                        std::vector<SkinnedVertex>& outVerts, std::vector<unsigned int>& outIdx) {
        using namespace BodyFitDetail;
        // Nome per id, nei due modelli
        std::unordered_map<int, std::string> hName, rName;
        for (const auto& [n, id] : human.ids) hName[id] = n;
        for (const auto& [n, id] : race.ids) rName[id] = n;
        // Osso umano -> osso della razza (se manca, il padre piu' vicino che c'e')
        auto mapBone = [&](std::string n) -> std::string {
            for (int guard = 0; guard < 8 && !n.empty(); ++guard) {
                if (race.ids.count(n) && race.bones.count(n)) return n;
                auto p = human.parent.find(n);
                if (p == human.parent.end()) break;
                n = p->second;
            }
            return std::string();
        };

        // 1) profili di spessore attorno a ogni osso, nei due corpi
        std::unordered_map<std::string, Profile> ph, pr;
        std::unordered_map<std::string, Range> ranges;
        {   // tratto coperto da ogni osso nel corpo umano
            std::unordered_map<std::string, std::vector<float>> ts;
            for (const auto& m : humanMeshes) for (const auto& v : *m.vertices) {
                int best = -1; float bw = 0.0f;
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) if (v.BoneIDs[k] >= 0 && v.Weights[k] > bw) { bw = v.Weights[k]; best = v.BoneIDs[k]; }
                if (best < 0 || bw < 0.5f) continue;
                auto nIt = hName.find(best);
                if (nIt == hName.end()) continue;
                const std::string bn = mapBone(nIt->second);
                if (bn.empty() || !human.bones.count(bn)) continue;
                float tn, r, ang; Locate(human.bones.at(bn), v.Position, tn, r, ang);
                ts[bn].push_back(tn);
            }
            for (auto& [bn, v] : ts) {
                if (v.size() < 8) continue;
                std::sort(v.begin(), v.end());
                Range rg; rg.lo = v[v.size() * 3 / 100]; rg.hi = v[v.size() * 97 / 100];
                const float pad = (rg.hi - rg.lo) * 0.08f + 0.05f;
                rg.lo -= pad; rg.hi += pad;
                ranges[bn] = rg;
            }
        }
        auto rangeOf = [&](const std::string& bn) { auto it = ranges.find(bn); return it == ranges.end() ? Range{} : it->second; };
        auto accumulate = [&](const std::vector<BodyFitMesh>& meshes, const std::unordered_map<int, std::string>& names,
                              const BodyFitSkeleton& sk, std::unordered_map<std::string, Profile>& prof, bool isHuman) {
            for (const auto& m : meshes) for (const auto& v : *m.vertices) {
                int best = -1; float bw = 0.0f;
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) if (v.BoneIDs[k] >= 0 && v.Weights[k] > bw) { bw = v.Weights[k]; best = v.BoneIDs[k]; }
                if (best < 0 || bw < 0.5f) continue;
                auto nIt = names.find(best);
                if (nIt == names.end()) continue;
                const std::string bn = isHuman ? mapBone(nIt->second) : nIt->second;
                if (bn.empty()) continue;
                auto bIt = sk.bones.find(bn);
                if (bIt == sk.bones.end()) continue;
                float tn, r, ang; Locate(bIt->second, v.Position, tn, r, ang);
                const Range rg = rangeOf(bn);
                if (tn < rg.lo - 0.1f || tn > rg.hi + 0.1f) continue;
                Profile& p = prof[bn];
                p.sum[BinOf(tn, rg)][SectorOf(ang)] += r; p.cnt[BinOf(tn, rg)][SectorOf(ang)] += 1.0f;
            }
        };
        accumulate(humanMeshes, hName, human, ph, true);
        accumulate(raceMeshes, rName, race, pr, false);

        // Rapporti di raggio (razza / umano) per osso, fascia e spicchio, lisciati
        struct Ratio { float v[kBins][kSectors]; };
        std::unordered_map<std::string, Ratio> ratios, humanRadius;   // rapporto e raggio umano medio
        for (auto& [bn, h] : ph) {
            auto rIt = pr.find(bn);
            if (rIt == pr.end()) continue;
            Profile& r = rIt->second;
            float hs = 0, hc = 0, rs = 0, rc = 0;
            for (int b = 0; b < kBins; ++b) for (int s = 0; s < kSectors; ++s) { hs += h.sum[b][s]; hc += h.cnt[b][s]; rs += r.sum[b][s]; rc += r.cnt[b][s]; }
            if (hc < 4 || rc < 4) continue;
            const float overall = std::clamp((rs / rc) / std::max(1e-4f, hs / hc), opt.minRatio, opt.maxRatio);
            Ratio ra, hr;
            for (int b = 0; b < kBins; ++b) for (int s = 0; s < kSectors; ++s)
                hr.v[b][s] = h.cnt[b][s] >= 3 ? h.sum[b][s] / h.cnt[b][s] : hs / hc;
            for (int b = 0; b < kBins; ++b) for (int s = 0; s < kSectors; ++s) {
                const bool ok = h.cnt[b][s] >= 3 && r.cnt[b][s] >= 3;
                ra.v[b][s] = ok ? std::clamp((r.sum[b][s] / r.cnt[b][s]) / std::max(1e-4f, h.sum[b][s] / h.cnt[b][s]), opt.minRatio, opt.maxRatio) : overall;
            }
            for (int pass = 0; pass < opt.smoothPasses; ++pass) {
                Ratio sm = ra;
                for (int b = 0; b < kBins; ++b) for (int s = 0; s < kSectors; ++s) {
                    float acc = ra.v[b][s] * 4.0f, w = 4.0f;
                    acc += ra.v[b][(s + 1) % kSectors] + ra.v[b][(s + kSectors - 1) % kSectors]; w += 2.0f;
                    if (b > 0) { acc += ra.v[b - 1][s]; w += 1.0f; }
                    if (b + 1 < kBins) { acc += ra.v[b + 1][s]; w += 1.0f; }
                    sm.v[b][s] = acc / w;
                }
                ra = sm;
            }
            ratios[bn] = ra;
            humanRadius[bn] = hr;
        }
        // Taglia di mani e piedi: rapporto medio dello spessore (mano e piede interi, senza fasce)
        std::unordered_map<std::string, float> handFootScale;
        for (const char* bn : { "LeftHand", "RightHand", "LeftFoot", "RightFoot" }) {
            auto h = ph.find(bn); auto r = pr.find(bn);
            if (h == ph.end() || r == pr.end()) continue;
            float hs = 0, hc = 0, rs = 0, rc = 0;
            for (int b = 0; b < kBins; ++b) for (int sct = 0; sct < kSectors; ++sct) { hs += h->second.sum[b][sct]; hc += h->second.cnt[b][sct]; rs += r->second.sum[b][sct]; rc += r->second.cnt[b][sct]; }
            if (hc > 4 && rc > 4) handFootScale[bn] = (rs / rc) / std::max(1e-4f, hs / hc);
        }
        auto sample = [&](const std::unordered_map<std::string, Ratio>& table, const std::string& bn, float tn, float ang, float fallback) -> float {
            auto it = table.find(bn);
            if (it == table.end()) return fallback;
            const Range rg = rangeOf(bn);
            const float fb = std::clamp((tn - rg.lo) / std::max(1e-3f, rg.hi - rg.lo) * kBins - 0.5f, 0.0f, static_cast<float>(kBins - 1));
            const float fs = (ang + 3.14159265f) / 6.2831853f * kSectors - 0.5f;
            const int b0 = static_cast<int>(std::floor(fb)), b1 = std::min(b0 + 1, kBins - 1);
            const float tb = fb - b0;
            const int s0 = (static_cast<int>(std::floor(fs)) % kSectors + kSectors) % kSectors, s1 = (s0 + 1) % kSectors;
            const float ts = fs - std::floor(fs);
            const auto& v = it->second.v;
            return glm::mix(glm::mix(v[b0][s0], v[b0][s1], ts), glm::mix(v[b1][s0], v[b1][s1], ts), tb);
        };

        // Rotazione osso umano -> osso della razza: allinea l'asse e poi il riferimento degli spicchi
        auto rotation = [&](const BodyFitBone& h, const BodyFitBone& r) {
            const glm::quat q1 = glm::rotation(h.axis, r.axis);
            glm::vec3 ref1 = q1 * h.ref;
            ref1 -= glm::dot(ref1, r.axis) * r.axis;
            float ang = 0.0f;
            if (glm::length(ref1) > 1e-4f) {
                ref1 = glm::normalize(ref1);
                ang = std::atan2(glm::dot(glm::cross(ref1, r.ref), r.axis), glm::dot(ref1, r.ref));
            }
            return glm::angleAxis(ang, r.axis) * q1;
        };
        std::unordered_map<std::string, glm::quat> rots;
        for (const auto& [n, hb] : human.bones) {
            const std::string rn = mapBone(n);
            if (rn.empty()) continue;
            rots[n] = rotation(hb, race.bones.at(rn));
        }

        auto headWeight = [&](const SkinnedVertex& v, const std::unordered_map<int, std::string>& names) {
            float w = 0.0f;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                if (v.BoneIDs[k] < 0) continue;
                auto it = names.find(v.BoneIDs[k]);
                if (it != names.end() && it->second.find("Head") == 0) w += v.Weights[k];   // Head, HeadTop_End
            }
            return w;
        };

        // 2) corpo umano adattato (senza testa)
        for (const auto& m : humanMeshes) {
            const auto& V = *m.vertices;
            std::vector<int> remap(V.size(), -1);
            std::vector<char> keep(V.size(), 0);
            for (size_t i = 0; i < V.size(); ++i) keep[i] = headWeight(V[i], hName) < opt.headWeight;
            auto emit = [&](unsigned int i) -> unsigned int {
                if (remap[i] >= 0) return static_cast<unsigned int>(remap[i]);
                const SkinnedVertex& src = V[i];
                SkinnedVertex out = src;
                glm::vec3 pos(0.0f); float wsum = 0.0f;
                // pesi azzerati tutti prima: gli id umani e quelli della razza sono numeri diversi per le stesse ossa
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) { out.BoneIDs[k] = -1; out.Weights[k] = 0.0f; }
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                    if (src.BoneIDs[k] < 0 || src.Weights[k] <= 0.0f) continue;
                    auto nIt = hName.find(src.BoneIDs[k]);
                    if (nIt == hName.end()) continue;
                    const std::string rn = mapBone(nIt->second);   // stesso osso, o il padre piu' vicino che la razza ha
                    if (rn.empty() || !human.bones.count(rn) || !rots.count(rn)) continue;
                    const BodyFitBone& hb = human.bones.at(rn);
                    const BodyFitBone& rb = race.bones.at(rn);
                    float tn, r, ang; Locate(hb, src.Position, tn, r, ang);
                    const glm::vec3 d = src.Position - hb.origin;
                    const float t = glm::dot(d, hb.axis);
                    const glm::vec3 rv = d - t * hb.axis;
                    // lungo l'osso: come la lunghezza dell'osso della razza; attorno: come il suo spessore
                    // Spessore: il rapporto vale per la superficie attorno all'osso. Per i vertici lontani dall'asse
                    // (es. petto pesato in parte sul braccio) diventa uno spostamento fisso invece di un fattore,
                    // altrimenti verrebbero sparati via.
                    // Il corpo prende forma dall'appoggio sulla sagoma (2b); qui lo spessore conta solo per mani e piedi
                    // (esclusi dall'appoggio): mano e piede della razza, presi interi, danno la taglia.
                    const bool ext = rn.find("Hand") != std::string::npos || rn.find("Foot") != std::string::npos || rn.find("Toe") != std::string::npos;
                    float ratio = opt.bulk;
                    if (ext) {
                        const std::string base = rn.find("Hand") != std::string::npos ? (rn.find("Left") == 0 ? "LeftHand" : "RightHand")
                                                                                        : (rn.find("Left") == 0 ? "LeftFoot" : "RightFoot");
                        ratio *= std::clamp(handFootScale.count(base) ? handFootScale.at(base) : 1.0f, 0.75f, 1.8f);
                    }
                    else {
                        // Braccia e gambe: lo spessore della razza per osso (lisciato), altrimenti l'appoggio da solo
                        // non riempirebbe un braccio d'ogryn partendo da uno umano. Il busto lo modella l'appoggio.
                        const bool limb = rn.find("Arm") != std::string::npos || rn.find("Leg") != std::string::npos;
                        // coscia: limitata (le ossa della coscia della razza reggono anche un pezzo di pancia)
                        if (limb || opt.conform <= 0.0f) ratio *= rn.find("UpLeg") != std::string::npos ? std::min(2.0f, sample(ratios, rn, tn, ang, 1.0f)) : sample(ratios, rn, tn, ang, 1.0f);
                    }
                    const float meanR = sample(humanRadius, rn, tn, ang, r);
                    const float radial = r > 1e-5f ? 1.0f + (ratio - 1.0f) * std::min(1.0f, meanR / r) : 1.0f;
                    const glm::vec3 scaled = t * (rb.length / hb.length) * hb.axis + rv * radial;
                    pos += (rb.origin + rots.at(rn) * scaled) * src.Weights[k];
                    wsum += src.Weights[k];
                    // id dell'osso nella razza (pesi sommati se due ossa umane finiscono sullo stesso)
                    const int rid = race.ids.at(rn);
                    bool merged = false;
                    for (int q = 0; q < MAX_BONE_INFLUENCE; ++q) if (out.BoneIDs[q] == rid) { out.Weights[q] += src.Weights[k]; merged = true; break; }
                    if (!merged) for (int q = 0; q < MAX_BONE_INFLUENCE; ++q) if (out.BoneIDs[q] < 0) { out.BoneIDs[q] = rid; out.Weights[q] = src.Weights[k]; break; }
                }
                if (wsum > 1e-4f) {
                    out.Position = pos / wsum;
                    for (int q = 0; q < MAX_BONE_INFLUENCE; ++q) out.Weights[q] /= wsum;
                }
                remap[i] = static_cast<int>(outVerts.size());
                outVerts.push_back(out);
                return static_cast<unsigned int>(remap[i]);
            };
            const auto& I = *m.indices;
            for (size_t t = 0; t + 2 < I.size(); t += 3) {
                if (!keep[I[t]] || !keep[I[t + 1]] || !keep[I[t + 2]]) continue;
                outIdx.push_back(emit(I[t])); outIdx.push_back(emit(I[t + 1])); outIdx.push_back(emit(I[t + 2]));
            }
        }

        // 2b) Appoggio sulla sagoma della razza: ogni vertice viene spinto verso il punto piu' vicino del vecchio
        // corpo (gobba, pancia, trapezi dell'ork...). Lo spostamento viene lisciato sulla superficie, cosi' arriva la
        // forma della razza ma restano i dettagli umani (ombelico, muscoli, pieghe). Mani e piedi esclusi: quelli
        // della razza sono blocchi, restano quelli umani portati dalle ossa.
        const size_t humanCount = outVerts.size();
        if (opt.conform > 0.0f && humanCount > 0) {
            // triangoli della razza in una griglia
            std::vector<glm::vec3> tri;
            std::vector<char> triHead;   // triangolo della testa: sagoma solo per il collo
            glm::vec3 mn(1e9f), mx(-1e9f);
            for (const auto& m : raceMeshes) {
                const auto& V = *m.vertices; const auto& I = *m.indices;
                for (size_t t = 0; t + 2 < I.size(); t += 3) {
                    // la testa della razza non fa da sagoma (la mascella dell'ork pende sul petto: tirerebbe su il torace)
                    triHead.push_back(std::max({ headWeight(V[I[t]], rName), headWeight(V[I[t + 1]], rName), headWeight(V[I[t + 2]], rName) }) >= opt.headWeight);
                    for (int k = 0; k < 3; ++k) { tri.push_back(V[I[t + k]].Position); mn = glm::min(mn, V[I[t + k]].Position); mx = glm::max(mx, V[I[t + k]].Position); }
                }
            }
            const float cell = std::max(1e-3f, (mx.y - mn.y) / 48.0f);
            auto cellOf = [&](const glm::vec3& p) { return glm::ivec3(glm::floor((p - mn) / cell)); };
            auto ckey = [](const glm::ivec3& c) { return (static_cast<uint64_t>(c.x + 4096) << 42) | (static_cast<uint64_t>(c.y + 4096) << 21) | static_cast<uint64_t>(c.z + 4096); };
            std::unordered_map<uint64_t, std::vector<int>> grid;
            for (int t = 0; t < static_cast<int>(tri.size() / 3); ++t) {
                const glm::vec3 a = tri[t * 3], b = tri[t * 3 + 1], c = tri[t * 3 + 2];
                const glm::ivec3 lo = cellOf(glm::min(a, glm::min(b, c))), hi = cellOf(glm::max(a, glm::max(b, c)));
                for (int x = lo.x; x <= hi.x; ++x) for (int y = lo.y; y <= hi.y; ++y) for (int z = lo.z; z <= hi.z; ++z) grid[ckey({ x, y, z })].push_back(t);
            }
            auto closestOnTri = [](const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
                const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
                const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
                if (d1 <= 0 && d2 <= 0) return a;
                const glm::vec3 bp = p - b; const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
                if (d3 >= 0 && d4 <= d3) return b;
                const float vc = d1 * d4 - d3 * d2;
                if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
                const glm::vec3 cp = p - c; const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
                if (d6 >= 0 && d5 <= d6) return c;
                const float vb = d5 * d2 - d1 * d6;
                if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
                const float va = d3 * d6 - d5 * d4;
                if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
                const float den = 1.0f / (va + vb + vc);
                return a + ab * (vb * den) + ac * (vc * den);
            };
            auto nearest = [&](const glm::vec3& p, bool allowHead) {
                const glm::ivec3 c0 = cellOf(p);
                glm::vec3 best = p; float bd = 1e30f;
                for (int ring = 0; ring <= 6; ++ring) {
                    for (int x = -ring; x <= ring; ++x) for (int y = -ring; y <= ring; ++y) for (int z = -ring; z <= ring; ++z) {
                        if (std::max({ std::abs(x), std::abs(y), std::abs(z) }) != ring) continue;
                        auto it = grid.find(ckey(c0 + glm::ivec3(x, y, z)));
                        if (it == grid.end()) continue;
                        for (int t : it->second) {
                            if (triHead[t] && !allowHead) continue;
                            const glm::vec3 q = closestOnTri(p, tri[t * 3], tri[t * 3 + 1], tri[t * 3 + 2]);
                            const float d = glm::dot(q - p, q - p);
                            if (d < bd) { bd = d; best = q; }
                        }
                    }
                    if (bd < (ring * cell) * (ring * cell)) break;   // niente di piu' vicino negli anelli dopo
                }
                return best;
            };
            // vertici saldati per posizione (niente crepe sulle cuciture)
            const float qk = cell * 1e-3f;
            auto pkey = [&](const glm::vec3& p) {
                const int64_t x = static_cast<int64_t>(std::llround(p.x / qk)), y = static_cast<int64_t>(std::llround(p.y / qk)), z = static_cast<int64_t>(std::llround(p.z / qk));
                return static_cast<uint64_t>((x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL));
            };
            std::unordered_map<uint64_t, int> weldIdx;
            std::vector<int> weldOf(humanCount);
            std::vector<glm::vec3> wpos; std::vector<float> walpha; std::vector<char> wneck;
            for (size_t i = 0; i < humanCount; ++i) {
                const uint64_t k = pkey(outVerts[i].Position);
                auto it = weldIdx.find(k);
                if (it == weldIdx.end()) {
                    it = weldIdx.emplace(k, static_cast<int>(wpos.size())).first;
                    wpos.push_back(outVerts[i].Position);
                    // mani e piedi (e dita) restano solo alle ossa
                    float ext = 0.0f, neck = 0.0f;
                    for (int q = 0; q < MAX_BONE_INFLUENCE; ++q) {
                        if (outVerts[i].BoneIDs[q] < 0) continue;
                        auto n = rName.find(outVerts[i].BoneIDs[q]);
                        if (n == rName.end()) continue;
                        const std::string& bn = n->second;
                        if (bn.find("Hand") != std::string::npos || bn.find("Foot") != std::string::npos || bn.find("Toe") != std::string::npos) ext += outVerts[i].Weights[q];
                        if (bn == "Neck" || bn.find("Head") == 0) neck += outVerts[i].Weights[q];
                    }
                    walpha.push_back(std::clamp(1.0f - ext * 1.5f, 0.0f, 1.0f));
                    wneck.push_back(neck > 0.4f);   // il collo si attacca anche alla testa della razza
                }
                weldOf[i] = it->second;
            }
            const size_t W = wpos.size();
            // adiacenza saldata
            std::vector<std::vector<int>> adj(W);
            for (size_t t = 0; t + 2 < outIdx.size(); t += 3) {
                const int a = weldOf[outIdx[t]], b = weldOf[outIdx[t + 1]], c = weldOf[outIdx[t + 2]];
                auto link = [&](int x, int y) { if (x != y) { adj[x].push_back(y); adj[y].push_back(x); } };
                link(a, b); link(b, c); link(c, a);
            }
            for (auto& l : adj) { std::sort(l.begin(), l.end()); l.erase(std::unique(l.begin(), l.end()), l.end()); }
            std::vector<glm::vec3> disp(W);
            for (size_t w = 0; w < W; ++w) disp[w] = (nearest(wpos[w], wneck[w]) - wpos[w]) * walpha[w];
            for (int it = 0; it < opt.conformSmooth; ++it) {
                std::vector<glm::vec3> nd(W);
                for (size_t w = 0; w < W; ++w) {
                    glm::vec3 acc = disp[w]; float cnt = 1.0f;
                    for (int n : adj[w]) { acc += disp[n]; cnt += 1.0f; }
                    nd[w] = acc / cnt;
                }
                disp.swap(nd);
            }
            for (size_t i = 0; i < humanCount; ++i) outVerts[i].Position += disp[weldOf[i]] * opt.conform;

            // Collo umano un po' dentro: il collo della testa della razza lo copre senza sfarfallii ne' fessure
            if (race.bones.count("Neck")) {
                const BodyFitBone& nb = race.bones.at("Neck");
                for (size_t i = 0; i < humanCount; ++i) {
                    float w = 0.0f;
                    for (int q = 0; q < MAX_BONE_INFLUENCE; ++q) {
                        if (outVerts[i].BoneIDs[q] < 0) continue;
                        auto n = rName.find(outVerts[i].BoneIDs[q]);
                        if (n != rName.end() && (n->second == "Neck" || n->second.find("Head") == 0)) w += outVerts[i].Weights[q];
                    }
                    if (w < 0.3f) continue;
                    const glm::vec3 d = outVerts[i].Position - nb.origin;
                    const glm::vec3 axisPt = nb.origin + nb.axis * glm::dot(d, nb.axis);
                    outVerts[i].Position = glm::mix(outVerts[i].Position, axisPt, 0.1f * std::min(1.0f, (w - 0.3f) / 0.3f));
                }
            }
        }

        // 3) testa della razza (scende un po' sotto il taglio per coprire il collo)
        glm::vec3 neckCenter(0.0f); float neckReach = 0.0f;
        if (race.bones.count("Neck") && race.bones.count("Head")) {
            neckCenter = race.bones.at("Neck").origin;
            neckReach = 1.1f * glm::distance(race.bones.at("Neck").origin, race.bones.at("Head").origin) + 0.5f * race.bones.at("Head").length;
        }
        for (const auto& m : raceMeshes) {
            const auto& V = *m.vertices;
            const auto& I = *m.indices;
            std::vector<int> remap(V.size(), -1);
            for (size_t t = 0; t + 2 < I.size(); t += 3) {
                const float w = std::max({ headWeight(V[I[t]], rName), headWeight(V[I[t + 1]], rName), headWeight(V[I[t + 2]], rName) });
                if (w < opt.headWeight - opt.headOverlap) continue;
                // la fascia sotto il taglio solo attorno al collo (non pezzi di spalla pesati un po' sulla testa)
                if (w < opt.headWeight && neckReach > 0.0f) {
                    const glm::vec3 c = (V[I[t]].Position + V[I[t + 1]].Position + V[I[t + 2]].Position) / 3.0f;
                    if (glm::distance(c, neckCenter) > neckReach) continue;
                }
                for (int k = 0; k < 3; ++k) {
                    const unsigned int i = I[t + k];
                    if (remap[i] < 0) { remap[i] = static_cast<int>(outVerts.size()); outVerts.push_back(V[i]); }
                    outIdx.push_back(static_cast<unsigned int>(remap[i]));
                }
            }
        }

        // Normali ricalcolate (saldate per posizione: niente spigoli sulle cuciture UV)
        std::unordered_map<uint64_t, glm::vec3> acc;
        float minY = 1e9f, maxY = -1e9f;
        for (const auto& v : outVerts) { minY = std::min(minY, v.Position.y); maxY = std::max(maxY, v.Position.y); }
        const float q = std::max(1e-4f, (maxY - minY) * 1e-5f);
        auto key = [&](const glm::vec3& p) {
            const int64_t x = static_cast<int64_t>(std::llround(p.x / q)), y = static_cast<int64_t>(std::llround(p.y / q)), z = static_cast<int64_t>(std::llround(p.z / q));
            return static_cast<uint64_t>((x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL));
        };
        for (size_t t = 0; t + 2 < outIdx.size(); t += 3) {
            const glm::vec3& a = outVerts[outIdx[t]].Position, & b = outVerts[outIdx[t + 1]].Position, & c = outVerts[outIdx[t + 2]].Position;
            const glm::vec3 n = glm::cross(b - a, c - a);   // pesata per area
            for (int k = 0; k < 3; ++k) acc[key(outVerts[outIdx[t + k]].Position)] += n;
        }
        for (auto& v : outVerts) {
            const glm::vec3 n = acc[key(v.Position)];
            if (glm::length(n) > 1e-8f) v.Normal = glm::normalize(n);
        }
    }
}
