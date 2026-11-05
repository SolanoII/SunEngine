#pragma once
// Tono e spessore dei muscoli dei corpi animati, calcolati una volta al caricamento.
//  - Geometria: la "forma dettagliata" del corpo (differenza fra il corpo e una sua versione levigata) viene
//    amplificata lungo la normale: i ventri muscolari (pettorali, deltoidi, bicipiti, quadricipiti, addominali)
//    escono di piu', i solchi fra un muscolo e l'altro si approfondiscono. In piu' un gonfiaggio per zona
//    (braccia, petto, gambe) da' spessore ai muscoli secondo la razza (ork e astartes piu' massicci).
//  - Ombreggiatura: nella coordinata texture del vertice restano la curvatura (x: 0.5 = piatto, sopra
//    convesso, sotto concavo) e un'occlusione ambientale approssimata (y: 1 = esposto). Lo shader della pelle
//    (detail 1 con u_Baked) ci disegna solchi piu' scuri e rossastri, ventri muscolari piu' chiari e lucidi,
//    vene in rilievo sulle razze muscolose.
// Le coordinate texture originali del corpo non servono: la pelle e i vestiti usano proiezioni in posa T.
#include "SkinnedModel.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace SunEngine {

    struct MuscleToneParams {
        float definition = 0.5f;   // amplificazione dei dettagli (0 = invariato)
        float bulkArms = 0.0f;     // gonfiaggio (frazione dell'altezza del corpo)
        float bulkChest = 0.0f;
        float bulkLegs = 0.0f;
        float maxShift = 0.006f;   // spostamento massimo per vertice (frazione dell'altezza)
    };

    // Parametri per razza (etichetta del corpo come in GameLayer::LoadAnimatedBody)
    inline MuscleToneParams RaceMuscleTone(const std::string& race) {
        MuscleToneParams p;
        if (race == "ork")            { p.definition = 0.9f; p.bulkArms = 0.0030f; p.bulkChest = 0.0022f; p.bulkLegs = 0.0012f; p.maxShift = 0.008f; }
        else if (race == "astartes")  { p.definition = 0.8f; p.bulkArms = 0.0024f; p.bulkChest = 0.0026f; p.bulkLegs = 0.0016f; p.maxShift = 0.007f; }
        else if (race == "neofita")   { p.definition = 0.6f; p.bulkArms = 0.0012f; p.bulkChest = 0.0012f; p.bulkLegs = 0.0008f; }
        else if (race == "ogryn")     { p.definition = 0.45f; p.bulkArms = 0.0022f; p.bulkChest = 0.0006f; p.bulkLegs = 0.0010f; p.maxShift = 0.006f; }
        else if (race == "gretchin")  { p.definition = 0.55f; p.maxShift = 0.005f; }   // nervoso e secco: niente massa
        else                          { p.definition = 0.45f; p.bulkArms = 0.0006f; p.bulkChest = 0.0004f; }   // umano
        return p;
    }

    // Stile della pelle per razza (u_SkinStyle): x chiazze, y verruche, z definizione muscolare, w vene
    inline glm::vec4 RaceSkinStyle(const std::string& race) {
        if (race == "ork")      return { 0.55f, 0.5f, 1.0f, 0.7f };
        if (race == "gretchin") return { 0.45f, 0.75f, 0.8f, 0.3f };
        if (race == "ogryn")    return { 0.15f, 0.3f, 0.75f, 0.45f };
        if (race == "astartes") return { 0.0f, 0.0f, 1.0f, 0.6f };
        if (race == "neofita")  return { 0.0f, 0.0f, 0.85f, 0.35f };
        return { 0.0f, 0.0f, 0.7f, 0.15f };
    }

    // Ritorna il numero di vertici modificati. Segna il modello come "pelle preparata" (bakedSkin).
    inline size_t ApplyMuscleTone(SkinnedModel& model, const MuscleToneParams& prm, bool gpuRebuild = true) {
        // altezza del corpo e ossa per zona
        float minY = 1e9f, maxY = -1e9f;
        for (const auto& m : model.meshes) for (const auto& v : m.vertices) { minY = std::min(minY, v.Position.y); maxY = std::max(maxY, v.Position.y); }
        const float H = std::max(1e-3f, maxY - minY);
        auto suffix = [](const std::string& n) { const auto p = n.rfind(':'); return p == std::string::npos ? n : n.substr(p + 1); };
        std::unordered_map<int, int> zoneOf;   // id osso -> 0 niente, 1 braccia, 2 petto/spalle, 3 gambe, 4 escluso (mani, testa, piedi)
        for (const auto& [name, info] : model.boneInfoMap) {
            const std::string s = suffix(name);
            int z = 0;
            if (s == "LeftArm" || s == "RightArm" || s == "LeftForeArm" || s == "RightForeArm") z = 1;
            else if (s == "Spine1" || s == "Spine2" || s == "LeftShoulder" || s == "RightShoulder" || s == "LeftPauldron" || s == "RightPauldron") z = 2;
            else if (s == "LeftUpLeg" || s == "RightUpLeg" || s == "LeftLeg" || s == "RightLeg") z = 3;
            else if (s.find("Hand") != std::string::npos || s == "Head" || s == "Jaw" || s.find("Ear") != std::string::npos || s.find("Eye") != std::string::npos ||
                     s.find("Toe") != std::string::npos || s.find("Foot") != std::string::npos || s == "HeadTop_End") z = 4;
            zoneOf[info.id] = z;
        }
        size_t changed = 0;
        for (auto& mesh : model.meshes) {
            auto& V = mesh.vertices;
            const auto& I = mesh.indices;
            const size_t n = V.size();
            if (n == 0) continue;
            // saldatura per posizione (cuciture UV)
            const float q = H * 1e-4f;
            std::unordered_map<uint64_t, int> weld;
            std::vector<int> wid(n);
            std::vector<glm::vec3> P;
            for (size_t i = 0; i < n; ++i) {
                const glm::vec3& p = V[i].Position;
                const int64_t x = std::llround(p.x / q), y = std::llround(p.y / q), z = std::llround(p.z / q);
                const uint64_t k = static_cast<uint64_t>((x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL));
                auto it = weld.find(k);
                if (it == weld.end()) { it = weld.emplace(k, static_cast<int>(P.size())).first; P.push_back(p); }
                wid[i] = it->second;
            }
            const size_t m = P.size();
            std::vector<std::vector<int>> adj(m);
            for (size_t t = 0; t + 2 < I.size(); t += 3) {
                const int a = wid[I[t]], b = wid[I[t + 1]], c = wid[I[t + 2]];
                auto link = [&](int u, int w) { if (u == w) return; auto& l = adj[u]; if (std::find(l.begin(), l.end(), w) == l.end()) l.push_back(w); };
                link(a, b); link(b, a); link(b, c); link(c, b); link(a, c); link(c, a);
            }
            // normali per vertice saldato
            auto normalsOf = [&](const std::vector<glm::vec3>& pts) {
                std::vector<glm::vec3> N(m, glm::vec3(0.0f));
                for (size_t t = 0; t + 2 < I.size(); t += 3) {
                    const int a = wid[I[t]], b = wid[I[t + 1]], c = wid[I[t + 2]];
                    const glm::vec3 fn = glm::cross(pts[b] - pts[a], pts[c] - pts[a]);
                    N[a] += fn; N[b] += fn; N[c] += fn;
                }
                for (auto& x : N) { const float l = glm::length(x); x = l > 1e-12f ? x / l : glm::vec3(0, 1, 0); }
                return N;
            };
            const std::vector<glm::vec3> N0 = normalsOf(P);
            // levigatura di Taubin (non restringe il corpo): scala piccola (muscoli) e grande (occlusione)
            auto smooth = [&](int iters) {
                std::vector<glm::vec3> S = P, T(m);
                for (int it = 0; it < iters; ++it) {
                    for (int pass = 0; pass < 2; ++pass) {
                        const float lambda = pass == 0 ? 0.6f : -0.63f;
                        for (size_t i = 0; i < m; ++i) {
                            if (adj[i].empty()) { T[i] = S[i]; continue; }
                            glm::vec3 avg(0.0f);
                            for (int j : adj[i]) avg += S[j];
                            avg /= static_cast<float>(adj[i].size());
                            T[i] = S[i] + lambda * (avg - S[i]);
                        }
                        std::swap(S, T);
                    }
                }
                return S;
            };
            const std::vector<glm::vec3> S1 = smooth(10), S2 = smooth(40);
            // zona del vertice saldato: media dei pesi delle ossa (dal primo vertice che lo usa)
            std::vector<glm::vec4> zoneW(m, glm::vec4(0.0f));   // braccia, petto, gambe, esclusi
            std::vector<char> seen(m, 0);
            for (size_t i = 0; i < n; ++i) {
                const int w = wid[i];
                if (seen[w]) continue;
                seen[w] = 1;
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                    if (V[i].BoneIDs[k] < 0) continue;
                    auto it = zoneOf.find(V[i].BoneIDs[k]);
                    const int z = it == zoneOf.end() ? 0 : it->second;
                    if (z >= 1) zoneW[w][z - 1] += V[i].Weights[k];
                }
            }
            std::vector<glm::vec3> Pn(m);
            std::vector<float> cav(m), occ(m);
            for (size_t i = 0; i < m; ++i) {
                const float detail = glm::dot(P[i] - S1[i], N0[i]);      // + ventre, - solco
                const float big = glm::dot(P[i] - S2[i], N0[i]);         // concavita' a grande scala (ascelle, inguine)
                const float keep = 1.0f - glm::clamp(zoneW[i].w, 0.0f, 1.0f);   // mani, testa, piedi: invariati
                float shift = detail * prm.definition * keep;
                shift += (zoneW[i].x * prm.bulkArms + zoneW[i].y * prm.bulkChest + zoneW[i].z * prm.bulkLegs) * H * keep * glm::clamp(0.6f + detail / (0.004f * H), 0.2f, 1.4f);
                shift = glm::clamp(shift, -prm.maxShift * H, prm.maxShift * H);
                Pn[i] = P[i] + N0[i] * shift;
                cav[i] = detail / H;
                occ[i] = big / H;
            }
            const std::vector<glm::vec3> N1 = normalsOf(Pn);
            for (size_t i = 0; i < n; ++i) {
                const int w = wid[i];
                if (glm::distance(V[i].Position, Pn[w]) > 1e-6f) ++changed;
                V[i].Position = Pn[w];
                V[i].Normal = N1[w];
                V[i].TexCoords = glm::vec2(glm::clamp(0.5f + cav[w] * 160.0f, 0.0f, 1.0f),
                                           glm::clamp(1.0f + std::min(0.0f, occ[w]) * 55.0f, 0.15f, 1.0f));
            }
        }
        model.bakedSkin = true;
        if (gpuRebuild) {
            std::vector<SkinnedMesh> rebuilt;
            rebuilt.reserve(model.meshes.size());
            for (auto& mm : model.meshes) {
                if (mm.VAO) { glDeleteVertexArrays(1, &mm.VAO); glDeleteBuffers(1, &mm.VBO); glDeleteBuffers(1, &mm.EBO); mm.VAO = mm.VBO = mm.EBO = 0; }
                rebuilt.emplace_back(std::move(mm.vertices), std::move(mm.indices));
            }
            model.meshes = std::move(rebuilt);
        }
        return changed;
    }
}
