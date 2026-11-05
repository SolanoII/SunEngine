#pragma once
// Ossa in piu' per razza, aggiunte al modello al caricamento (dopo l'eventuale BodyFit, prima delle clip):
//   gretchin: orecchie (sinistra/destra) e mascella  -> orecchie che sbattono, chiacchiericcio nervoso
//   ork:      mascella                              -> ringhio, urlo in carica e durante la WAAAGH!
//   ogryn:    mascella e pancia                     -> bocca socchiusa da tonto, pancia che ballonzola
//   astartes: spallacci (sinistro/destro)           -> spallacci montati sul busto: non si deformano col braccio
//
// Ogni osso nuovo e' figlio di un osso esistente, ha un perno nello spazio della mesh e prende i vertici di una
// zona del corpo (pesi sfumati ai bordi, tolti in proporzione alle ossa che li avevano). Le clip lo ereditano
// gia' dalla gerarchia del modello (Animation copia rootNode e boneInfoMap); lo muove la posa procedurale per
// nome ("LeftEar", "Jaw", "Belly", "LeftPauldron"...). I vestiti, costruiti dal corpo, lo seguono.
#include "SkinnedModel.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SunEngine {

    struct ExtraBoneDef {
        std::string name;                       // senza prefisso (es. "Jaw")
        std::string parent;                     // osso padre, senza prefisso (es. "Head")
        std::vector<std::string> region;        // ossa (prefissi del nome) che "possiedono" la zona: es. {"Head"}
        float minShare = 0.5f;                  // peso minimo dei vertici su quelle ossa
        glm::vec3 pivot{ 0.0f };                // perno nello spazio della mesh
        bool pivotFromRegion = false;           // perno: x fisso, y/z = baricentro della zona
        // peso del vertice per l'osso nuovo (0..1), dalla posizione in posa T
        std::function<float(const glm::vec3&)> weight;
    };

    namespace ExtraBonesDetail {
        inline float SmoothStep(float e0, float e1, float x) {
            const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }
        inline std::string Suffix(const std::string& n) { const auto p = n.rfind(':'); return p == std::string::npos ? n : n.substr(p + 1); }
        inline AssimpNodeData* FindNode(AssimpNodeData& n, const std::string& name, const glm::mat4& parentGlobal, glm::mat4& outGlobal) {
            const glm::mat4 g = parentGlobal * n.transformation;
            if (n.name == name) { outGlobal = g; return &n; }
            for (auto& c : n.children) if (auto* r = FindNode(c, name, g, outGlobal)) return r;
            return nullptr;
        }
    }

    // Definizioni per razza (coordinate dei modelli in GameData/Models/Characters, posa T)
    inline std::vector<ExtraBoneDef> RaceExtraBones(const std::string& race) {
        using ExtraBonesDetail::SmoothStep;
        std::vector<ExtraBoneDef> out;
        if (race == "gretchin") {
            // Orecchie enormi ai lati della testa (|x| oltre 9, fra y 78 e 91)
            for (int side : { 1, -1 }) {
                ExtraBoneDef d;
                d.name = side > 0 ? "LeftEar" : "RightEar"; d.parent = "Head"; d.region = { "Head" };
                d.pivot = glm::vec3(side * 8.0f, 84.0f, 6.0f); d.pivotFromRegion = true;
                d.weight = [side](const glm::vec3& p) {
                    if (p.x * side <= 0.0f) return 0.0f;
                    return SmoothStep(7.0f, 13.5f, std::fabs(p.x)) * SmoothStep(76.0f, 80.0f, p.y) * SmoothStep(93.0f, 88.5f, p.y);
                };
                out.push_back(d);
            }
            ExtraBoneDef jaw;
            jaw.name = "Jaw"; jaw.parent = "Head"; jaw.region = { "Head" };
            jaw.pivot = glm::vec3(0.0f, 77.0f, 6.0f);
            jaw.weight = [](const glm::vec3& p) { return SmoothStep(76.0f, 73.5f, p.y) * SmoothStep(8.0f, 11.0f, p.z) * SmoothStep(8.0f, 6.0f, std::fabs(p.x)); };
            out.push_back(jaw);
        }
        else if (race == "ork") {
            // Mascellone: la parte bassa e avanzata della testa (y < 141, z > 20), cerniera dietro la bocca
            ExtraBoneDef jaw;
            jaw.name = "Jaw"; jaw.parent = "Head"; jaw.region = { "Head" };
            jaw.pivot = glm::vec3(0.0f, 143.0f, 20.0f);
            jaw.weight = [](const glm::vec3& p) { return SmoothStep(142.5f, 138.5f, p.y) * SmoothStep(19.0f, 24.0f, p.z); };
            out.push_back(jaw);
        }
        else if (race == "ogryn") {
            ExtraBoneDef jaw;
            jaw.name = "Jaw"; jaw.parent = "Head"; jaw.region = { "Head" };
            jaw.pivot = glm::vec3(0.0f, 256.0f, 8.0f);
            jaw.weight = [](const glm::vec3& p) { return SmoothStep(255.0f, 250.5f, p.y) * SmoothStep(9.0f, 14.0f, p.z); };
            out.push_back(jaw);
            // Pancia: la parte davanti del busto basso (z > 18), fra le anche e il torace
            ExtraBoneDef belly;
            belly.name = "Belly"; belly.parent = "Spine"; belly.region = { "Spine", "Hips" }; belly.minShare = 0.35f;
            belly.pivot = glm::vec3(0.0f, 182.0f, 6.0f);
            belly.weight = [](const glm::vec3& p) { return SmoothStep(16.0f, 30.0f, p.z) * SmoothStep(148.0f, 162.0f, p.y) * SmoothStep(212.0f, 196.0f, p.y); };
            out.push_back(belly);
        }
        else if (race == "astartes") {
            // Spallacci: la calotta della spalla (y > 182, 16 < |x| < 40) passa al busto
            for (int side : { 1, -1 }) {
                ExtraBoneDef d;
                d.name = side > 0 ? "LeftPauldron" : "RightPauldron"; d.parent = "Spine2";
                d.region = { side > 0 ? "LeftArm" : "RightArm", side > 0 ? "LeftShoulder" : "RightShoulder", "Spine2" }; d.minShare = 0.3f;
                d.pivot = glm::vec3(side * 24.0f, 186.0f, -3.0f);
                d.weight = [side](const glm::vec3& p) {
                    if (p.x * side <= 0.0f) return 0.0f;
                    const float ax = std::fabs(p.x);
                    return SmoothStep(181.0f, 186.0f, p.y) * SmoothStep(14.0f, 19.0f, ax) * SmoothStep(42.0f, 36.0f, ax);
                };
                out.push_back(d);
            }
        }
        return out;
    }

    // Aggiunge le ossa al modello e ripesa i vertici. Ritorna quante ossa sono state aggiunte.
    // gpuRebuild = true: ricrea le mesh sulla GPU (serve un contesto OpenGL).
    inline int AddExtraBones(SkinnedModel& model, const std::vector<ExtraBoneDef>& defs, bool gpuRebuild = true) {
        using namespace ExtraBonesDetail;
        if (defs.empty() || model.meshes.empty()) return 0;
        // prefisso delle ossa (es. "mixamorig1:") e id per nome breve
        std::string prefix;
        int maxId = -1;
        std::unordered_map<int, std::string> nameOf;
        for (const auto& [n, b] : model.boneInfoMap) {
            maxId = std::max(maxId, b.id);
            nameOf[b.id] = Suffix(n);
            const auto p = n.rfind(':');
            if (p != std::string::npos) prefix = n.substr(0, p + 1);
        }
        std::vector<std::vector<SkinnedVertex>> verts;
        for (const auto& m : model.meshes) verts.push_back(m.vertices);

        int added = 0;
        for (const auto& d : defs) {
            if (maxId + 1 >= 100) break;   // ANIMATOR_MAX_BONES
            glm::mat4 parentGlobal(1.0f);
            AssimpNodeData* parent = FindNode(model.rootNode, prefix + d.parent, glm::mat4(1.0f), parentGlobal);
            if (!parent || model.boneInfoMap.count(prefix + d.name)) continue;
            const int id = maxId + 1;

            // pesi dei vertici
            auto share = [&](const SkinnedVertex& v) {
                float s = 0.0f;
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                    if (v.BoneIDs[k] < 0) continue;
                    auto it = nameOf.find(v.BoneIDs[k]);
                    if (it == nameOf.end()) continue;
                    for (const auto& r : d.region) if (it->second.rfind(r, 0) == 0) { s += v.Weights[k]; break; }
                }
                return s;
            };
            glm::vec3 centroid(0.0f); float wsum = 0.0f; int touched = 0;
            std::vector<std::vector<float>> w(verts.size());
            for (size_t m = 0; m < verts.size(); ++m) {
                w[m].assign(verts[m].size(), 0.0f);
                for (size_t i = 0; i < verts[m].size(); ++i) {
                    const SkinnedVertex& v = verts[m][i];
                    const float s = share(v);
                    if (s < d.minShare) continue;
                    const float k = std::clamp(d.weight(v.Position), 0.0f, 1.0f) * std::min(1.0f, s / std::max(0.01f, d.minShare));
                    if (k < 0.01f) continue;
                    w[m][i] = k; centroid += v.Position * k; wsum += k; ++touched;
                }
            }
            if (touched < 8) continue;   // zona vuota: niente osso
            glm::vec3 pivot = d.pivot;
            if (d.pivotFromRegion) { const glm::vec3 c = centroid / wsum; pivot.y = c.y; pivot.z = c.z; }

            for (size_t m = 0; m < verts.size(); ++m) for (size_t i = 0; i < verts[m].size(); ++i) {
                const float k = w[m][i];
                if (k <= 0.0f) continue;
                SkinnedVertex& v = verts[m][i];
                for (int q = 0; q < MAX_BONE_INFLUENCE; ++q) if (v.BoneIDs[q] >= 0) v.Weights[q] *= (1.0f - k);
                int slot = -1;
                for (int q = 0; q < MAX_BONE_INFLUENCE; ++q) if (v.BoneIDs[q] < 0) { slot = q; break; }
                if (slot < 0) {   // tutte piene: rimpiazza la piu' leggera
                    slot = 0;
                    for (int q = 1; q < MAX_BONE_INFLUENCE; ++q) if (v.Weights[q] < v.Weights[slot]) slot = q;
                }
                v.BoneIDs[slot] = id; v.Weights[slot] = k;
                float tot = 0.0f;
                for (int q = 0; q < MAX_BONE_INFLUENCE; ++q) if (v.BoneIDs[q] >= 0) tot += v.Weights[q];
                if (tot > 1e-6f) for (int q = 0; q < MAX_BONE_INFLUENCE; ++q) v.Weights[q] /= tot;
            }

            // osso nella gerarchia: in posa T sta nel perno, senza rotazione (spazio della mesh)
            const glm::mat4 meshBind = glm::translate(glm::mat4(1.0f), pivot);
            const glm::mat4 nodeGlobal = glm::inverse(model.m_GlobalInverseTransform) * meshBind;
            AssimpNodeData node;
            node.name = prefix + d.name;
            node.transformation = glm::inverse(parentGlobal) * nodeGlobal;
            parent->children.push_back(node);
            parent->childrenCount = static_cast<int>(parent->children.size());
            model.boneInfoMap[node.name] = { id, glm::inverse(meshBind) };
            nameOf[id] = d.name;
            maxId = id;
            ++added;
        }
        if (added == 0) return 0;
        model.boneCounter = std::max(model.boneCounter, maxId + 1);
        for (size_t m = 0; m < model.meshes.size(); ++m) {
            if (gpuRebuild) {
                SkinnedMesh& old = model.meshes[m];
                glDeleteVertexArrays(1, &old.VAO); glDeleteBuffers(1, &old.VBO); glDeleteBuffers(1, &old.EBO);
                std::vector<unsigned int> idx = old.indices;
                model.meshes[m] = SkinnedMesh(std::move(verts[m]), std::move(idx));
            }
            else model.meshes[m].vertices = std::move(verts[m]);
        }
        return added;
    }

}
