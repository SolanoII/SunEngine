#pragma once
// Modello dello squig (compagno dei gretchin): palla di carne che e' quasi tutta bocca, zanne storte, occhietti
// sulla cupola, spine sulla schiena, due zampe tozze con gli artigli. Scolpito con una SDF (SdfMesher.h) e
// skinnato sui cinque blocchi dello scheletro a cubi (Skeletons/squig.json: Torso, Head, Jaw, Leg_L, Leg_R),
// che restano la hitbox: il modello segue l'animazione dei blocchi (saltelli, morso) e anche il ragdoll.
// Coordinate dell'entita' in metri, origine a terra, muso verso -Z.
#include "SdfMesher.h"
#include "SkinnedModel.h"
#include "SkinnedParts.h"
#include <algorithm>
#include <functional>
#include "../Physics/HitboxBlock.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace SunEngine {

    class SquigModel {
        enum class PartMaterial { Tooth, Eye };
        static float SmoothStep(float a, float b, float x) { const float t = glm::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
    public:
        // Costruisce il modello (una volta) per lo scheletro dato. false se lo scheletro non ha i blocchi attesi.
        bool Build(const SkeletonData& skel) {
            if (m_Built) return m_Ok;
            m_Built = true;
            for (int i = 0; i < 5; ++i) m_Block[i] = -1;
            static const char* names[5] = { "Torso", "Head", "Jaw", "Leg_L", "Leg_R" };
            for (size_t b = 0; b < skel.blocks.size(); ++b)
                for (int i = 0; i < 5; ++i) if (skel.blocks[b].name == names[i]) m_Block[i] = static_cast<int>(b);
            for (int i = 0; i < 5; ++i) if (m_Block[i] < 0) return false;
            // posa di riposo dei blocchi (senza animazione e senza la scala del cubo)
            SkeletonData rest = skel;
            for (auto& b : rest.blocks) b.jointRotation = glm::vec3(0.0f);
            const auto mats = rest.ComputeWorldTransforms(glm::vec3(0.0f), glm::vec3(0.0f), 1.0f);
            m_InvRest.assign(skel.blocks.size(), glm::mat4(1.0f));
            for (size_t b = 0; b < mats.size(); ++b) m_InvRest[b] = glm::inverse(NoScale(mats[b]));
            // pelle
            const auto skin = [](const glm::vec3& p) { return SkinSdf(p); };
            SdfMesh m = SdfPolygonize(skin, glm::vec3(-0.32f, -0.02f, -0.36f), glm::vec3(0.32f, 0.56f, 0.34f), 0.0075f);
            if (m.indices.empty()) return false;
            const std::vector<float> ao = SdfAmbientOcclusion(skin, m, 0.04f);
            std::vector<SkinnedVertex> verts(m.positions.size());
            for (size_t i = 0; i < verts.size(); ++i) {
                const glm::vec3& p = m.positions[i];
                SkinnedVertex& v = verts[i];
                v.Position = p; v.Normal = m.normals[i];
                const float h = 0.01f;
                float lap = -6.0f * skin(p);
                lap += skin(p + glm::vec3(h, 0, 0)) + skin(p - glm::vec3(h, 0, 0)) + skin(p + glm::vec3(0, h, 0)) + skin(p - glm::vec3(0, h, 0)) + skin(p + glm::vec3(0, 0, h)) + skin(p - glm::vec3(0, 0, h));
                v.TexCoords = glm::vec2(glm::clamp(0.5f + lap / h * 0.04f, 0.0f, 1.0f), ao[i]);
                Weights(p, v);
                // canali: macchie scure sulla schiena (pittura) e interno della bocca (rossore)
                const float spots = Sdf::Noise(p * 22.0f) * 0.7f + Sdf::Noise(p * 50.0f) * 0.3f;
                const float back = SmoothStep(-0.05f, 0.1f, p.z) + SmoothStep(0.33f, 0.42f, p.y);
                v.Weights[2] = (spots > 0.58f && back > 0.3f) ? 2.0f + SmoothStep(0.58f, 0.64f, spots) * 0.8f : 0.0f;
                const float cav = Sdf::Ellipsoid(p, glm::vec3(0.0f, 0.215f, -0.12f), glm::vec3(0.17f, 0.065f, 0.15f));
                v.Weights[3] = (cav < 0.01f && p.z > -0.27f) ? 1.6f : 0.0f;
            }
            float minY = 1e9f, maxY = -1e9f;
            for (const auto& v : verts) { minY = std::min(minY, v.Position.y); maxY = std::max(maxY, v.Position.y); }
            m_InvHeight = 1.0f / std::max(0.01f, maxY - minY);   // come il corpo (RenderSystem misura l'altezza del modello)
            m_Model.meshes.emplace_back(std::move(verts), std::move(m.indices));
            m_Model.bakedSkin = true;
            // denti e occhi
            AddPart(PartMaterial::Tooth, [](const glm::vec3& p) { return TeethSdf(p); }, glm::vec3(-0.24f, 0.08f, -0.36f), glm::vec3(0.24f, 0.34f, 0.0f), 0.004f,
                    [&](const glm::vec3& p) { return std::min(SkinSdf(p), TeethSdf(p)); });
            AddPart(PartMaterial::Eye, [](const glm::vec3& p) { return EyesSdf(p); }, glm::vec3(-0.16f, 0.32f, -0.2f), glm::vec3(0.16f, 0.48f, -0.02f), 0.0035f,
                    [&](const glm::vec3& p) { return std::min(SkinSdf(p), EyesSdf(p)); });
            m_Ok = true;
            return true;
        }

        bool Ready() const { return m_Ok; }
        SkinnedModel* Model() { return m_Ok ? &m_Model : nullptr; }
        const SkinnedPartSet* Parts() const { return m_Ok ? &m_Parts : nullptr; }

        // Matrici delle ossa dai blocchi (worldBlocks: matrici dei blocchi nel mondo, con la scala del cubo;
        // entityMat: trasformazione dell'entita' con cui si disegna il modello)
        void ComputeBones(const std::vector<glm::mat4>& worldBlocks, const glm::mat4& entityMat, std::vector<glm::mat4>& out) const {
            out.assign(8, glm::mat4(1.0f));
            if (!m_Ok) return;
            const glm::mat4 invEntity = glm::inverse(entityMat);
            for (int i = 0; i < 5; ++i) {
                const int b = m_Block[i];
                if (b < 0 || b >= static_cast<int>(worldBlocks.size()) || b >= static_cast<int>(m_InvRest.size())) continue;
                out[i] = invEntity * NoScale(worldBlocks[b]) * m_InvRest[b];
            }
        }

    private:
        static glm::mat4 NoScale(glm::mat4 m) {
            for (int c = 0; c < 3; ++c) { const float l = glm::length(glm::vec3(m[c])); if (l > 1e-6f) m[c] /= l; }
            return m;
        }

        // ---- forma
        static float SkinSdf(const glm::vec3& p) {
            using namespace Sdf;
            const glm::vec3 m(std::abs(p.x), p.y, p.z);
            // corpo-testa: palla schiacciata, piu' larga davanti (la bocca)
            float d = Ellipsoid(p, glm::vec3(0.0f, 0.27f, 0.03f), glm::vec3(0.21f, 0.165f, 0.21f));
            d = SUnion(d, Ellipsoid(p, glm::vec3(0.0f, 0.25f, -0.09f), glm::vec3(0.22f, 0.13f, 0.17f)), 0.06f);
            // labbro inferiore enorme e sporgente
            d = SUnion(d, Ellipsoid(p, glm::vec3(0.0f, 0.165f, -0.15f), glm::vec3(0.195f, 0.065f, 0.15f)), 0.05f);
            // cupola con le orbite
            d = SUnion(d, Ellipsoid(m, glm::vec3(0.085f, 0.385f, -0.08f), glm::vec3(0.06f, 0.05f, 0.06f)), 0.04f);
            // bocca: fessura larga e cavita'
            const float slit = RoundBox(p, glm::vec3(0.0f, 0.215f, -0.22f), glm::vec3(0.185f, 0.012f, 0.12f), 0.006f);
            const float cav = Ellipsoid(p, glm::vec3(0.0f, 0.215f, -0.12f), glm::vec3(0.16f, 0.055f, 0.14f));
            d = SSub(d, std::min(slit, cav), 0.012f);
            // orbite
            d = SSub(d, Sphere(m, glm::vec3(0.085f, 0.405f, -0.115f), 0.036f), 0.01f);
            // spine sulla schiena
            for (int i = 0; i < 4; ++i) {
                const float t = i / 3.0f;
                const glm::vec3 base(0.0f, 0.42f - t * 0.1f, 0.0f + t * 0.17f);
                const glm::vec3 tip = base + glm::vec3(0.0f, 0.08f - t * 0.02f, 0.05f);
                d = SUnion(d, RoundCone(p, base, tip, 0.03f, 0.004f), 0.02f);
            }
            // zampe e piedi con tre artigli
            float leg = Capsule(m, glm::vec3(0.12f, 0.17f, 0.04f), glm::vec3(0.135f, 0.07f, 0.0f), 0.055f);
            leg = SUnion(leg, Ellipsoid(m, glm::vec3(0.135f, 0.035f, -0.03f), glm::vec3(0.06f, 0.035f, 0.09f)), 0.03f);
            for (int k = -1; k <= 1; ++k)
                leg = SUnion(leg, RoundCone(m, glm::vec3(0.135f + k * 0.03f, 0.03f, -0.09f), glm::vec3(0.135f + k * 0.04f, 0.012f, -0.15f), 0.016f, 0.003f), 0.01f);
            d = SUnion(d, leg, 0.035f);
            // pelle bitorzoluta
            d -= 0.004f * (Noise(p * 40.0f) - 0.5f) + 0.002f * (Noise(p * 95.0f) - 0.5f);
            return d;
        }
        static float TeethSdf(const glm::vec3& p) {
            using namespace Sdf;
            float d = 1e9f;
            for (int i = -6; i <= 6; ++i) {
                const float a = i / 6.0f;                       // lungo l'arco della bocca
                const float x = a * 0.165f;
                const float z = -0.255f + 0.13f * a * a;
                const float l = 0.035f + 0.025f * Hash(i, 2, 9) + (std::abs(i) == 4 ? 0.02f : 0.0f);
                const float tilt = (Hash(i, 7, 3) - 0.5f) * 0.03f;
                // superiori (verso il basso) e inferiori (verso l'alto), sfalsati
                d = std::min(d, RoundCone(p, glm::vec3(x, 0.238f, z + 0.012f), glm::vec3(x + tilt, 0.238f - l, z + 0.004f), 0.013f, 0.002f));
                const float x2 = x + 0.0135f;
                if (x2 < 0.17f) d = std::min(d, RoundCone(p, glm::vec3(x2, 0.19f, z + 0.015f), glm::vec3(x2 - tilt, 0.19f + l * 0.85f, z + 0.006f), 0.012f, 0.002f));
            }
            return d;
        }
        static float EyesSdf(const glm::vec3& p) {
            const glm::vec3 m(std::abs(p.x), p.y, p.z);
            return Sdf::Sphere(m, glm::vec3(0.085f, 0.405f, -0.11f), 0.03f);
        }

        // ossa: 0 corpo, 1 cupola, 2 mascella, 3 zampa sinistra (x < 0), 4 zampa destra
        static void Weights(const glm::vec3& p, SkinnedVertex& v) {
            float wJaw = SmoothStep(0.215f, 0.19f, p.y) * SmoothStep(-0.02f, -0.08f, p.z) * SmoothStep(0.09f, 0.13f, p.y);
            float wHead = SmoothStep(0.33f, 0.37f, p.y) * SmoothStep(0.06f, -0.02f, p.z);
            float wLeg = SmoothStep(0.15f, 0.11f, p.y) * SmoothStep(0.05f, 0.08f, std::abs(p.x));
            wJaw *= 1.0f - wLeg;
            float wTorso = std::max(0.0f, 1.0f - wJaw - wHead - wLeg);
            std::pair<float, int> w[4] = { { wTorso, 0 }, { wHead, 1 }, { wJaw, 2 }, { wLeg, p.x < 0.0f ? 3 : 4 } };
            std::sort(std::begin(w), std::end(w), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (int k = 0; k < 4; ++k) { v.BoneIDs[k] = -1; v.Weights[k] = 0.0f; }
            float tot = 0.0f; int n = 0;
            for (const auto& [wt, id] : w) { if (n >= 2 || wt <= 0.001f) continue; v.BoneIDs[n] = id; v.Weights[n] = wt; tot += wt; ++n; }
            if (n == 0) { v.BoneIDs[0] = 0; v.Weights[0] = 1.0f; tot = 1.0f; }
            for (int k = 0; k < n; ++k) v.Weights[k] /= tot;
        }

        void AddPart(PartMaterial mat, const std::function<float(const glm::vec3&)>& f, const glm::vec3& lo, const glm::vec3& hi, float cell,
                     const std::function<float(const glm::vec3&)>& aoField) {
            SdfMesh m = SdfPolygonize(f, lo, hi, cell);
            if (m.indices.empty()) return;
            const std::vector<float> ao = SdfAmbientOcclusion(aoField, m, 0.03f);
            std::vector<SkinnedVertex> verts(m.positions.size());
            for (size_t i = 0; i < verts.size(); ++i) {
                SkinnedVertex& v = verts[i];
                v.Position = m.positions[i]; v.Normal = m.normals[i];
                v.TexCoords = glm::vec2(0.5f, ao[i]);
                Weights(m.positions[i], v);
                if (mat == PartMaterial::Tooth) {   // denti inferiori con la mascella
                    v.BoneIDs[0] = m.positions[i].y < 0.214f ? 2 : 0; v.Weights[0] = 1.0f; v.BoneIDs[1] = -1; v.Weights[1] = 0.0f;
                }
                if (mat == PartMaterial::Eye) {
                    v.BoneIDs[0] = 1; v.Weights[0] = 1.0f; v.BoneIDs[1] = -1; v.Weights[1] = 0.0f;
                    const glm::vec3 c(m.positions[i].x > 0.0f ? 0.085f : -0.085f, 0.405f, -0.11f);
                    const glm::vec3 q = (m.positions[i] - c) / 0.03f;
                    glm::vec2 uv(3.0f);
                    if (q.z < 0.0f) uv = glm::vec2(q.x, q.y + 0.1f) / 0.6f;   // guarda avanti (-Z), pupilla grande
                    v.BoneIDs[2] = -1; v.Weights[2] = uv.x; v.BoneIDs[3] = -1; v.Weights[3] = uv.y;
                }
            }
            auto mesh = std::make_unique<SkinnedMesh>(std::move(verts), std::move(m.indices));
            GarmentDraw d;
            d.mesh = mesh.get();
            d.baked = 1;
            d.invHeight = m_InvHeight;
            if (mat == PartMaterial::Tooth) { d.detail = 6; d.color = glm::vec4(0.85f, 0.78f, 0.55f, 1.0f); d.roughness = 0.4f; }
            else { d.detail = 5; d.color = glm::vec4(0.95f, 0.55f, 0.05f, 1.0f); d.emissive = 0.4f; }
            m_Parts.parts.push_back(d);
            m_Meshes.push_back(std::move(mesh));
        }

        bool m_Built = false, m_Ok = false;
        int m_Block[5]{};
        std::vector<glm::mat4> m_InvRest;
        float m_InvHeight = 1.0f;
        SkinnedModel m_Model;
        SkinnedPartSet m_Parts;
        std::vector<std::unique_ptr<SkinnedMesh>> m_Meshes;
    };
}
