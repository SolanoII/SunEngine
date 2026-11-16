#pragma once
// Armatura teutonica (versione media) dell'Ordo Domus Humilitatis: costruita sul corpo, non disegnata a mano.
//
// Ogni pezzo e' un "garment" procedurale ("procedural": "teutonic_..." nel JSON dell'armatura): GarmentLibrary
// lo chiede qui con il corpo su cui va indossato, e il pezzo viene misurato su quel corpo (ossa in posa a T e
// vertici della mesh). Cosi' la stessa armatura veste qualsiasi Vanguard di taglia media (stesso scheletro e
// corpo a V di BodySculpt.h) e, quando arrivera' il modello dell'artista, si rifara' sulla sua mesh.
//
// Pezzi (riferimento: cavaliere teutonico, piastre brunite quasi nere, sopravveste bianca con la croce nera):
//   teutonic_helm          grande elmo a secchio: cima bombata, feritoia per gli occhi, croce di rinforzo
//                          (fascia sopra la feritoia e costola verticale), fori di respirazione
//   teutonic_helm_eyes     luce viola degli occhi dietro la feritoia
//   teutonic_gorget        goletta a due lame fra elmo e spalle
//   teutonic_tabard        sopravveste: davanti dal petto alle ginocchia, orlo consumato
//   teutonic_tabard_cross  croce nera sulla sopravveste
//   teutonic_cape          mantello dalle spalle ai polpacci, che cade staccato dalla schiena
//   teutonic_belt          cinturone con fibbia e borse
//   teutonic_tassets       falda: tre file di lame sui fianchi e dietro (il davanti e' della sopravveste)
//   teutonic_pauldron_l/r  spallacci tondi con due lame sotto e bordo rialzato
//   teutonic_couter_l/r    cubitiere (gomito)
//   teutonic_cuff_l/r      polsini svasati delle manopole
//   teutonic_knee_l/r      ginocchielli con l'ala sul lato esterno
// Le piastre attaccate al corpo (cotta, braccia, gambe, manopole, scarpe) restano gusci dal corpo (GarmentShell.h).
//
// Pesi delle ossa: i pezzi rigidi vanno sulle loro ossa (elmo sulla testa, ginocchiello fra coscia e gamba...);
// stoffa e falde copiano i pesi del punto del corpo piu' vicino, e sotto le anche si dividono fra bacino e cosce
// (la sopravveste si apre un po' camminando invece di strapparsi in mezzo).
#include "SkinnedModel.h"
#include "SdfMesher.h"
#include "BodySculpt.h"
#include "GarmentShell.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SunEngine {

    namespace TeutonicDetail {
        inline float Sm(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

        enum Group { G_Torso = 0, G_Neck, G_Head, G_ArmL, G_ArmR, G_LegL, G_LegR, G_Count };

        inline int GroupOf(const std::string& b) {
            auto starts = [&](const char* p) { return b.rfind(p, 0) == 0; };
            if (b == "Neck") return G_Neck;
            if (starts("Head") || b == "Jaw" || b.find("Ear") != std::string::npos || b.find("Eye") != std::string::npos) return G_Head;
            if (starts("LeftArm") || starts("LeftForeArm") || starts("LeftHand")) return G_ArmL;
            if (starts("RightArm") || starts("RightForeArm") || starts("RightHand")) return G_ArmR;
            if (starts("LeftUpLeg") || b == "LeftLeg" || starts("LeftFoot") || starts("LeftToe")) return G_LegL;
            if (starts("RightUpLeg") || b == "RightLeg" || starts("RightFoot") || starts("RightToe")) return G_LegR;
            return G_Torso;   // anche, colonna, clavicole, spallacci, pancia
        }

        using Weights = std::vector<std::pair<int, float>>;

        // Corpo misurato in posa a T (spazio della mesh): ossa, vertici con il loro gruppo, ricerca del vicino
        struct Body {
            const SkinnedModel* model = nullptr;
            std::unordered_map<std::string, int> id;          // "LeftArm" -> id dell'osso
            std::unordered_map<std::string, glm::vec3> pos;   // "LeftArm" -> posizione
            std::vector<const SkinnedVertex*> verts;
            std::vector<int> group;
            float minY = 0.0f, maxY = 1.0f, Hb = 1.0f;        // altezza del corpo
            float side = 1.0f;                                // +1 se la sinistra del personaggio sta su +x
            float cx = 0.0f, cz = 0.0f;                       // asse del busto
            // griglia per il vertice piu' vicino
            float cell = 1.0f;
            std::unordered_map<long long, std::vector<int>> grid;

            bool Has(const char* b) const { return pos.count(b) > 0; }
            glm::vec3 P(const char* b) const { auto it = pos.find(b); return it == pos.end() ? glm::vec3(0.0f) : it->second; }
            int Id(const char* b) const { auto it = id.find(b); return it == id.end() ? -1 : it->second; }
            long long Key(int x, int y, int z) const { return (static_cast<long long>(x) * 73856093LL) ^ (static_cast<long long>(y) * 19349663LL) ^ (static_cast<long long>(z) * 83492791LL); }

            bool Init(const SkinnedModel& m) {
                model = &m;
                const BodySculptDetail::Bind bind = BodySculptDetail::ReadBind(m);
                const glm::mat4 gi = m.m_GlobalInverseTransform;
                for (const auto& [suffix, full] : bind.bySuffix) {
                    pos[suffix] = glm::vec3((gi * bind.global.at(full))[3]);
                    auto bi = m.boneInfoMap.find(full);
                    if (bi != m.boneInfoMap.end()) id[suffix] = bi->second.id;
                }
                if (!Has("Hips") || !Has("Neck") || !Has("Head") || !Has("LeftArm") || !Has("RightArm") || !Has("LeftUpLeg") || !Has("LeftLeg")) return false;
                std::unordered_map<int, int> groupOfId;
                for (const auto& [name, info] : m.boneInfoMap) groupOfId[info.id] = GroupOf(BodySculptDetail::Suffix(name));
                minY = 1e9f; maxY = -1e9f;
                for (const auto& mesh : m.meshes) {
                    for (const auto& v : mesh.vertices) {
                        float best = 0.0f; int g = G_Torso;
                        float acc[G_Count] = {};
                        for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                            if (v.BoneIDs[k] < 0 || v.Weights[k] <= 0.0f) continue;
                            auto it = groupOfId.find(v.BoneIDs[k]);
                            if (it != groupOfId.end()) acc[it->second] += v.Weights[k];
                        }
                        for (int k = 0; k < G_Count; ++k) if (acc[k] > best) { best = acc[k]; g = k; }
                        verts.push_back(&v); group.push_back(g);
                        minY = std::min(minY, v.Position.y); maxY = std::max(maxY, v.Position.y);
                    }
                }
                if (verts.empty()) return false;
                Hb = std::max(1e-3f, maxY - minY);
                side = P("LeftArm").x > P("RightArm").x ? 1.0f : -1.0f;
                cx = P("Hips").x; cz = P("Hips").z;
                cell = Hb * 0.03f;
                for (size_t i = 0; i < verts.size(); ++i) {
                    const glm::vec3& p = verts[i]->Position;
                    grid[Key(static_cast<int>(std::floor(p.x / cell)), static_cast<int>(std::floor(p.y / cell)), static_cast<int>(std::floor(p.z / cell)))].push_back(static_cast<int>(i));
                }
                return true;
            }

            // Vertice del corpo piu' vicino fra quelli dei gruppi ammessi (maschera di bit sui gruppi)
            int Nearest(const glm::vec3& p, unsigned mask) const {
                const int x0 = static_cast<int>(std::floor(p.x / cell)), y0 = static_cast<int>(std::floor(p.y / cell)), z0 = static_cast<int>(std::floor(p.z / cell));
                int best = -1; float bestD = 1e30f;
                for (int r = 1; r <= 8 && best < 0; ++r) {
                    for (int dx = -r; dx <= r; ++dx) for (int dy = -r; dy <= r; ++dy) for (int dz = -r; dz <= r; ++dz) {
                        auto it = grid.find(Key(x0 + dx, y0 + dy, z0 + dz));
                        if (it == grid.end()) continue;
                        for (int i : it->second) {
                            if (!(mask & (1u << group[i]))) continue;
                            const glm::vec3 d = verts[i]->Position - p;
                            const float dd = glm::dot(d, d);
                            if (dd < bestD) { bestD = dd; best = i; }
                        }
                    }
                }
                return best;
            }
            Weights WeightsOf(int vi) const {
                Weights w;
                if (vi < 0) return w;
                const SkinnedVertex& v = *verts[vi];
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) if (v.BoneIDs[k] >= 0 && v.Weights[k] > 0.0f) w.push_back({ v.BoneIDs[k], v.Weights[k] });
                return w;
            }

            // Ingombro dei vertici di un gruppo (percentili, per non farsi ingannare da qualche vertice isolato)
            void Box(int g, glm::vec3& lo, glm::vec3& hi) const {
                std::vector<float> xs, ys, zs;
                for (size_t i = 0; i < verts.size(); ++i) if (group[i] == g) { xs.push_back(verts[i]->Position.x); ys.push_back(verts[i]->Position.y); zs.push_back(verts[i]->Position.z); }
                if (xs.empty()) { lo = hi = P("Head"); return; }
                auto pct = [](std::vector<float>& v, float q) { const size_t k = std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1))); std::nth_element(v.begin(), v.begin() + k, v.end()); return v[k]; };
                lo = glm::vec3(pct(xs, 0.01f), pct(ys, 0.005f), pct(zs, 0.01f));
                hi = glm::vec3(pct(xs, 0.99f), pct(ys, 0.995f), pct(zs, 0.99f));
            }
            // Raggio di un arto attorno al segmento a->b, fra le frazioni t0 e t1 del segmento
            float LimbRadius(int g, const glm::vec3& a, const glm::vec3& b, float t0, float t1) const {
                const glm::vec3 ab = b - a;
                const float l2 = std::max(1e-8f, glm::dot(ab, ab));
                std::vector<float> r;
                for (size_t i = 0; i < verts.size(); ++i) {
                    if (group[i] != g) continue;
                    const glm::vec3 p = verts[i]->Position;
                    const float t = glm::dot(p - a, ab) / l2;
                    if (t < t0 || t > t1) continue;
                    r.push_back(glm::length(p - (a + ab * t)));
                }
                if (r.empty()) return Hb * 0.04f;
                const size_t k = static_cast<size_t>(0.92f * (r.size() - 1));
                std::nth_element(r.begin(), r.begin() + k, r.end());
                return r[k];
            }
        };

        inline Weights Mix(const Weights& a, float wa, const Weights& b, float wb) {
            Weights out;
            auto add = [&](int id, float w) {
                if (w <= 0.0f || id < 0) return;
                for (auto& e : out) if (e.first == id) { e.second += w; return; }
                out.push_back({ id, w });
            };
            for (const auto& e : a) add(e.first, e.second * wa);
            for (const auto& e : b) add(e.first, e.second * wb);
            return out;
        }
        inline Weights One(int id) { Weights w; if (id >= 0) w.push_back({ id, 1.0f }); return w; }
        inline Weights Two(int a, float wa, int b, float wb) { return Mix(One(a), wa, One(b), wb); }

        inline void SetWeights(SkinnedVertex& v, Weights w) {
            std::sort(w.begin(), w.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
            if (w.size() > MAX_BONE_INFLUENCE) w.resize(MAX_BONE_INFLUENCE);
            float s = 0.0f; for (const auto& e : w) s += e.second;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) { v.BoneIDs[k] = -1; v.Weights[k] = 0.0f; }
            if (s <= 1e-6f) return;
            for (size_t k = 0; k < w.size(); ++k) { v.BoneIDs[k] = w[k].first; v.Weights[k] = w[k].second / s; }
        }

        // Spigoli vivi -> TexCoords.x piccolo: lo shader dell'armatura ci mette il metallo consumato
        inline void MarkEdges(GarmentShell& out, size_t firstV, size_t firstI, float Hb) {
            const size_t n = out.vertices.size() - firstV;
            std::vector<float> dev(n, 0.0f);
            for (size_t t = firstI; t + 2 < out.indices.size(); t += 3) {
                for (int k = 0; k < 3; ++k) {
                    const unsigned a = out.indices[t + k], b = out.indices[t + (k + 1) % 3];
                    const float d = 1.0f - glm::dot(out.vertices[a].Normal, out.vertices[b].Normal);
                    dev[a - firstV] = std::max(dev[a - firstV], d); dev[b - firstV] = std::max(dev[b - firstV], d);
                }
            }
            for (size_t i = 0; i < n; ++i) out.vertices[firstV + i].TexCoords = glm::vec2(0.04f * (1.0f - Sm(0.12f, 0.55f, dev[i])), 1.0f);   // solo gli spigoli veri
            (void)Hb;
        }

        // Pezzo da una SDF: superficie, pesi, spigoli
        inline void AddSdf(GarmentShell& out, const std::function<float(const glm::vec3&)>& f, glm::vec3 lo, glm::vec3 hi, float cell,
                           const std::function<Weights(const glm::vec3&)>& weights, float Hb) {
            SdfMesh m = SdfPolygonize(f, lo, hi, cell);
            if (m.indices.empty()) return;
            const size_t firstV = out.vertices.size(), firstI = out.indices.size();
            for (size_t i = 0; i < m.positions.size(); ++i) {
                SkinnedVertex v;
                v.Position = m.positions[i];
                v.Normal = m.normals[i];
                SetWeights(v, weights(v.Position));
                out.vertices.push_back(v);
            }
            for (unsigned idx : m.indices) out.indices.push_back(static_cast<unsigned>(firstV) + idx);
            MarkEdges(out, firstV, firstI, Hb);
        }

        // Campo d'altezza in z del corpo (davanti: z massima, dietro: z minima) su una griglia (x, y)
        struct ZField {
            float x0 = 0, y0 = 0, dx = 1, dy = 1; int nx = 0, ny = 0;
            std::vector<float> z;
            std::vector<char> filled;   // celle con corpo vero (prima di riempire i buchi)
            float At(float x, float y) const {
                const float fx = std::clamp((x - x0) / dx, 0.0f, static_cast<float>(nx - 1) - 1e-3f), fy = std::clamp((y - y0) / dy, 0.0f, static_cast<float>(ny - 1) - 1e-3f);
                const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
                const float tx = fx - ix, ty = fy - iy;
                auto Z = [&](int a, int b) { return z[static_cast<size_t>(b) * nx + a]; };
                return glm::mix(glm::mix(Z(ix, iy), Z(ix + 1, iy), tx), glm::mix(Z(ix, iy + 1), Z(ix + 1, iy + 1), tx), ty);
            }
        };

        // front = +1: superficie davanti (z massima); -1: dietro. hangBelow: sotto questa altezza la stoffa cade dritta
        inline ZField BuildZField(const Body& B, float front, float halfW, float yLo, float yHi, int nx, int ny, unsigned mask, float hangBelow) {
            ZField f;
            f.nx = nx; f.ny = ny; f.x0 = B.cx - halfW; f.y0 = yLo; f.dx = 2.0f * halfW / (nx - 1); f.dy = (yHi - yLo) / (ny - 1);
            f.z.assign(static_cast<size_t>(nx) * ny, -1e9f);
            f.filled.assign(static_cast<size_t>(nx) * ny, 0);
            for (size_t i = 0; i < B.verts.size(); ++i) {
                if (!(mask & (1u << B.group[i]))) continue;
                const glm::vec3& p = B.verts[i]->Position;
                const int ix = static_cast<int>(std::lround((p.x - f.x0) / f.dx)), iy = static_cast<int>(std::lround((p.y - f.y0) / f.dy));
                for (int ox = -1; ox <= 1; ++ox) for (int oy = -1; oy <= 1; ++oy) {
                    const int a = ix + ox, b = iy + oy;
                    if (a < 0 || b < 0 || a >= nx || b >= ny) continue;
                    float& zz = f.z[static_cast<size_t>(b) * nx + a];
                    zz = std::max(zz, p.z * front);
                    f.filled[static_cast<size_t>(b) * nx + a] = 1;
                }
            }
            // buchi (fuori dal corpo, fra le gambe): dal vicino, piu' volte
            for (int it = 0; it < nx + ny; ++it) {
                bool any = false;
                std::vector<float> nz = f.z;
                for (int b = 0; b < ny; ++b) for (int a = 0; a < nx; ++a) {
                    const size_t k = static_cast<size_t>(b) * nx + a;
                    if (f.z[k] > -1e8f) continue;
                    float s = 0.0f; int c = 0;
                    for (int ox = -1; ox <= 1; ++ox) for (int oy = -1; oy <= 1; ++oy) {
                        const int x = a + ox, y = b + oy;
                        if (x < 0 || y < 0 || x >= nx || y >= ny) continue;
                        const float v = f.z[static_cast<size_t>(y) * nx + x];
                        if (v > -1e8f) { s += v; ++c; }
                    }
                    if (c > 0) nz[k] = s / c; else any = true;
                }
                f.z.swap(nz);
                if (!any) break;
            }
            for (auto& v : f.z) if (v < -1e8f) v = (front > 0 ? B.cz : -B.cz);
            // stoffa che cade: dall'alto in basso non rientra mai sotto le parti che sporgono (pancia, glutei, cosce)
            for (int a = 0; a < nx; ++a) {
                float run = -1e9f;
                for (int b = ny - 1; b >= 0; --b) {
                    const float y = f.y0 + b * f.dy;
                    float& v = f.z[static_cast<size_t>(b) * nx + a];
                    if (y < hangBelow) v = std::max(v, run);   // sopra hangBelow la stoffa segue il corpo
                    run = v;
                }
            }
            // liscia (la stoffa non segue ogni muscolo)
            for (int pass = 0; pass < 3; ++pass) {
                std::vector<float> nz = f.z;
                for (int b = 0; b < ny; ++b) for (int a = 0; a < nx; ++a) {
                    float s = 0.0f, w = 0.0f;
                    for (int ox = -2; ox <= 2; ++ox) for (int oy = -2; oy <= 2; ++oy) {
                        const int x = std::clamp(a + ox, 0, nx - 1), y = std::clamp(b + oy, 0, ny - 1);
                        const float k = 1.0f / (1.0f + ox * ox + oy * oy);
                        s += f.z[static_cast<size_t>(y) * nx + x] * k; w += k;
                    }
                    // la superficie lisciata resta fuori dal corpo
                    nz[static_cast<size_t>(b) * nx + a] = std::max(s / w, f.z[static_cast<size_t>(b) * nx + a]);
                }
                f.z.swap(nz);
            }
            for (auto& v : f.z) v *= front;   // di nuovo in z vera
            return f;
        }

        // Pannello di stoffa con spessore, su una griglia che segue il contorno: ogni riga va da xRange(y).first a
        // .second (bordi lisci, niente scalini), l'orlo in basso si sposta di hem(u). cut(x, y) = buco (scollo).
        // surf(x, y) = z della faccia interna, thick = spessore verso l'esterno (front: +1 avanti, -1 dietro).
        inline void AddPanel(GarmentShell& out, float yLo, float yHi, int nu, int nv,
                             const std::function<std::pair<float, float>(float)>& xRange, const std::function<float(float)>& hem,
                             const std::function<bool(float, float)>& cut, const std::function<float(float, float)>& surf,
                             float thick, float front, const std::function<Weights(const glm::vec3&)>& weights, float Hb) {
            const int NU = nu + 1, NV = nv + 1;
            auto XY = [&](int i, int j) {
                const float u = static_cast<float>(i) / nu, v = static_cast<float>(j) / nv;
                float y = glm::mix(yLo, yHi, v);
                y += hem(u) * (1.0f - v) * (1.0f - v);                     // l'orlo frastagliato si smorza salendo
                const auto r = xRange(y);
                return glm::vec2(glm::mix(r.first, r.second, u), y);
            };
            std::vector<char> keep(static_cast<size_t>(nu) * nv, 1);
            for (int j = 0; j < nv; ++j) for (int i = 0; i < nu; ++i) {
                const glm::vec2 c = (XY(i, j) + XY(i + 1, j + 1)) * 0.5f;
                if (cut(c.x, c.y)) keep[static_cast<size_t>(j) * nu + i] = 0;
            }
            auto K = [&](int i, int j) { return i >= 0 && j >= 0 && i < nu && j < nv && keep[static_cast<size_t>(j) * nu + i]; };
            // distanza dal bordo in celle (per orli e cuciture)
            std::vector<float> dist(static_cast<size_t>(NU) * NV, 1e9f);
            auto D = [&](int i, int j) -> float& { return dist[static_cast<size_t>(j) * NU + i]; };
            for (int j = 0; j < NV; ++j) for (int i = 0; i < NU; ++i)
                if (!(K(i - 1, j - 1) && K(i, j - 1) && K(i - 1, j) && K(i, j))) D(i, j) = 0.0f;
            for (int pass = 0; pass < 2; ++pass)
                for (int j = 0; j < NV; ++j) for (int i = 0; i < NU; ++i) {
                    const int ii = pass ? NU - 1 - i : i, jj = pass ? NV - 1 - j : j;
                    for (int ox = -1; ox <= 1; ++ox) for (int oy = -1; oy <= 1; ++oy) {
                        const int a = ii + ox, b = jj + oy;
                        if (a < 0 || b < 0 || a >= NU || b >= NV) continue;
                        D(ii, jj) = std::min(D(ii, jj), D(a, b) + std::sqrt(static_cast<float>(ox * ox + oy * oy)));
                    }
                }
            const glm::vec2 c00 = XY(0, 0), c11 = XY(nu, nv);
            const float cellLen = std::min(std::fabs(c11.x - c00.x) / nu, std::fabs(c11.y - c00.y) / nv);
            std::vector<int> vo(static_cast<size_t>(NU) * NV, -1), vn(vo.size(), -1);
            auto node = [&](int i, int j, bool outer) {
                int& slot = (outer ? vo : vn)[static_cast<size_t>(j) * NU + i];
                if (slot >= 0) return slot;
                const glm::vec2 q = XY(i, j);
                SkinnedVertex v;
                v.Position = glm::vec3(q.x, q.y, surf(q.x, q.y) + (outer ? thick * front : 0.0f));
                SetWeights(v, weights(v.Position));
                v.TexCoords = glm::vec2(std::min(0.04f, D(i, j) * cellLen / Hb), 1.0f);
                slot = static_cast<int>(out.vertices.size());
                out.vertices.push_back(v);
                return slot;
            };
            auto tri = [&](int a, int b, int c) { out.indices.push_back(a); out.indices.push_back(b); out.indices.push_back(c); };
            for (int j = 0; j < nv; ++j) for (int i = 0; i < nu; ++i) {
                if (!K(i, j)) continue;
                const int o00 = node(i, j, true), o10 = node(i + 1, j, true), o01 = node(i, j + 1, true), o11 = node(i + 1, j + 1, true);
                const int n00 = node(i, j, false), n10 = node(i + 1, j, false), n01 = node(i, j + 1, false), n11 = node(i + 1, j + 1, false);
                if (front > 0) { tri(o00, o10, o11); tri(o00, o11, o01); tri(n00, n11, n10); tri(n00, n01, n11); }
                else { tri(o00, o11, o10); tri(o00, o01, o11); tri(n00, n10, n11); tri(n00, n11, n01); }
                auto wall = [&](int a0, int a1, int b0, int b1) { tri(a0, a1, b1); tri(a0, b1, b0); tri(a0, b1, a1); tri(a0, b0, b1); };
                if (!K(i, j - 1)) wall(o00, o10, n00, n10);
                if (!K(i, j + 1)) wall(o01, o11, n01, n11);
                if (!K(i - 1, j)) wall(o00, o01, n00, n01);
                if (!K(i + 1, j)) wall(o10, o11, n10, n11);
            }
        }

        // Normali dai triangoli per i vertici da firstV in poi
        inline void FaceNormals(GarmentShell& out, size_t firstV, size_t firstI) {
            std::vector<glm::vec3> n(out.vertices.size() - firstV, glm::vec3(0.0f));
            for (size_t t = firstI; t + 2 < out.indices.size(); t += 3) {
                const unsigned a = out.indices[t], b = out.indices[t + 1], c = out.indices[t + 2];
                if (a < firstV || b < firstV || c < firstV) continue;
                const glm::vec3 f = glm::cross(out.vertices[b].Position - out.vertices[a].Position, out.vertices[c].Position - out.vertices[a].Position);
                n[a - firstV] += f; n[b - firstV] += f; n[c - firstV] += f;
            }
            for (size_t i = 0; i < n.size(); ++i) out.vertices[firstV + i].Normal = glm::dot(n[i], n[i]) > 1e-20f ? glm::normalize(n[i]) : glm::vec3(0.0f, 1.0f, 0.0f);
        }

        // Sezione orizzontale del corpo: raggio massimo per angolo (attorno a cx, cz) a ogni altezza
        struct RingTable {
            float y0 = 0, dy = 1; int ny = 0, na = 0;
            std::vector<float> r;
            float cx = 0, cz = 0;
            float At(float ang, float y) const {
                const float fa = (ang / 6.2831853f + 0.5f) * na;
                const float fy = std::clamp((y - y0) / dy, 0.0f, static_cast<float>(ny - 1) - 1e-3f);
                const int iy = static_cast<int>(fy);
                const float ty = fy - iy;
                const float fl = std::floor(fa);
                const float ta = fa - fl;
                const int a0 = ((static_cast<int>(fl) % na) + na) % na, a1 = (a0 + 1) % na;
                auto R = [&](int a, int b) { return r[static_cast<size_t>(b) * na + a]; };
                return glm::mix(glm::mix(R(a0, iy), R(a1, iy), ta), glm::mix(R(a0, iy + 1), R(a1, iy + 1), ta), ty);
            }
        };
        inline float Ang(float x, float z, float cx, float cz) { return std::atan2(x - cx, z - cz); }   // 0 davanti (+z)

        inline RingTable BuildRings(const Body& B, float cx, float cz, float yLo, float yHi, int ny, int na, unsigned mask) {
            RingTable t; t.cx = cx; t.cz = cz; t.y0 = yLo; t.ny = ny; t.na = na; t.dy = (yHi - yLo) / (ny - 1);
            t.r.assign(static_cast<size_t>(ny) * na, 0.0f);
            for (size_t i = 0; i < B.verts.size(); ++i) {
                if (!(mask & (1u << B.group[i]))) continue;
                const glm::vec3& p = B.verts[i]->Position;
                const int iy = static_cast<int>(std::lround((p.y - yLo) / t.dy));
                if (iy < -1 || iy > ny) continue;
                const float a = Ang(p.x, p.z, cx, cz);
                const int ia = ((static_cast<int>(std::floor((a / 6.2831853f + 0.5f) * na)) % na) + na) % na;
                const float rr = std::hypot(p.x - cx, p.z - cz);
                for (int oy = -1; oy <= 1; ++oy) {
                    const int b = iy + oy;
                    if (b < 0 || b >= ny) continue;
                    float& v = t.r[static_cast<size_t>(b) * na + ia];
                    v = std::max(v, rr);
                }
            }
            // buchi: dal vicino nello stesso anello
            for (int b = 0; b < ny; ++b)
                for (int pass = 0; pass < na; ++pass) {
                    bool any = false;
                    for (int a = 0; a < na; ++a) {
                        float& v = t.r[static_cast<size_t>(b) * na + a];
                        if (v > 0.0f) continue;
                        const float l = t.r[static_cast<size_t>(b) * na + (a + na - 1) % na], rr = t.r[static_cast<size_t>(b) * na + (a + 1) % na];
                        if (l > 0.0f || rr > 0.0f) v = std::max(l, rr); else any = true;
                    }
                    if (!any) break;
                }
            // inviluppo morbido: prima si tiene il massimo (resta fuori dal corpo), poi si liscia davvero
            // (i raggi per spicchio sono rumorosi: senza lisciare le piastre verrebbero bitorzolute)
            for (int pass = 0; pass < 3; ++pass) {
                std::vector<float> nr = t.r;
                for (int b = 0; b < ny; ++b) for (int a = 0; a < na; ++a) {
                    const float l = t.r[static_cast<size_t>(b) * na + (a + na - 1) % na], c = t.r[static_cast<size_t>(b) * na + a], rr = t.r[static_cast<size_t>(b) * na + (a + 1) % na];
                    const float up = t.r[static_cast<size_t>(std::min(b + 1, ny - 1)) * na + a], dn = t.r[static_cast<size_t>(std::max(b - 1, 0)) * na + a];
                    nr[static_cast<size_t>(b) * na + a] = std::max(c, (l + rr + up + dn + c * 2.0f) / 6.0f);
                }
                t.r.swap(nr);
            }
            for (int pass = 0; pass < 10; ++pass) {
                std::vector<float> nr = t.r;
                for (int b = 0; b < ny; ++b) for (int a = 0; a < na; ++a) {
                    const float l = t.r[static_cast<size_t>(b) * na + (a + na - 1) % na], c = t.r[static_cast<size_t>(b) * na + a], rr = t.r[static_cast<size_t>(b) * na + (a + 1) % na];
                    const float up = t.r[static_cast<size_t>(std::min(b + 1, ny - 1)) * na + a], dn = t.r[static_cast<size_t>(std::max(b - 1, 0)) * na + a];
                    nr[static_cast<size_t>(b) * na + a] = (l + rr + up + dn + c * 4.0f) / 8.0f;
                }
                t.r.swap(nr);
            }
            return t;
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Costruisce il pezzo 'kind' sul corpo. false = pezzo sconosciuto o corpo senza le ossa che servono.
    inline bool BuildTeutonicGarment(const SkinnedModel& model, const std::string& kind, GarmentShell& out) {
        using namespace TeutonicDetail;
        using namespace Sdf;
        Body B;
        if (!B.Init(model)) return false;
        const float Hb = B.Hb, sd = B.side;
        const glm::vec3 hips = B.P("Hips"), neck = B.P("Neck"), head = B.P("Head");
        const float H = std::max(1e-3f, neck.y - hips.y);   // dalle anche al collo
        const float yBelt = hips.y + H * 0.16f;
        const float yKnee = (B.P("LeftLeg").y + B.P("RightLeg").y) * 0.5f;
        const unsigned torsoMask = (1u << G_Torso) | (1u << G_Neck);
        const unsigned clothMask = torsoMask | (1u << G_LegL) | (1u << G_LegR);
        const int idHips = B.Id("Hips"), idUpL = B.Id("LeftUpLeg"), idUpR = B.Id("RightUpLeg");
        const size_t firstV = out.vertices.size(), firstI = out.indices.size();

        // Stoffa e falde: pesi del corpo sopra le anche, sotto divisi fra bacino e cosce
        // split: larghezza del passaggio fra le due meta' (piccola = spacco davanti, come le sopravvesti da cavallo)
        auto clothWeights = [&](const glm::vec3& p, float legShare, float split = 0.35f) {
            const Weights top = B.WeightsOf(B.Nearest(glm::vec3(p.x, std::max(p.y, hips.y), p.z), torsoMask));
            if (p.y >= hips.y) return top;
            const float s = Sm(hips.y, yKnee, p.y) * legShare;
            const float l = Sm(-split, split, (p.x - B.cx) * sd / std::max(1e-3f, H * 0.25f));   // 1 = gamba sinistra
            return Mix(top, 1.0f - s, Two(idUpL, l, idUpR, 1.0f - l), s);
        };

        // --- Elmo ---------------------------------------------------------------------------------------------
        if (kind == "teutonic_helm" || kind == "teutonic_helm_eyes") {
            glm::vec3 lo, hi; B.Box(G_Head, lo, hi);
            const glm::vec3 c = (lo + hi) * 0.5f, e = (hi - lo) * 0.5f;
            const float m = Hb * 0.012f;
            const float rx = e.x * 1.06f + m, rz = e.z * 1.04f + m;
            const float yBot = lo.y - e.y * 0.12f, yTop = hi.y + Hb * 0.012f;
            const float yEye = c.y + e.y * 0.18f;
            const float t = Hb * 0.007f;      // spessore della piastra
            const int idHead = B.Id("Head");
            auto outer = [=](const glm::vec3& p) {
                // secchio leggermente svasato in basso, cima bombata
                const float flare = 1.0f + 0.05f * std::clamp((c.y - p.y) / e.y, 0.0f, 1.5f);
                const glm::vec2 q((p.x - c.x) / (rx * flare), (p.z - c.z) / (rz * flare));
                float d = (glm::length(q) - 1.0f) * std::min(rx, rz);
                const float crown = Ellipsoid(p, glm::vec3(c.x, yTop - rx * 1.5f, c.z), glm::vec3(rx * 1.22f, rx * 1.5f, rz * 1.22f));
                d = SInter(d, crown, Hb * 0.004f);
                return std::max(d, yBot - p.y);
            };
            if (kind == "teutonic_helm_eyes") {
                // due occhi viola dietro la feritoia
                auto eyes = [=](const glm::vec3& p) {
                    float d = 1e9f;
                    for (float s : { -1.0f, 1.0f }) d = std::min(d, Ellipsoid(p, glm::vec3(c.x + s * rx * 0.30f, yEye, c.z + rz * 0.80f), glm::vec3(rx * 0.17f, e.y * 0.045f, rz * 0.06f)));
                    return d;
                };
                AddSdf(out, eyes, c - glm::vec3(rx, e.y * 0.2f, -rz * 0.5f), c + glm::vec3(rx, e.y * 0.5f, rz * 1.1f), Hb * 0.0025f, [&](const glm::vec3&) { return One(idHead); }, Hb);
                return !out.indices.empty();
            }
            auto helm = [=](const glm::vec3& p) {
                const float o = outer(p);
                float d = std::max(o, -(o + t));                                       // guscio
                d = std::max(d, -(p.y - yBot + t));                                    // aperto sotto
                const bool frontHalf = p.z > c.z;
                // croce di rinforzo: fascia sopra la feritoia e costola verticale (rialzate)
                if (frontHalf) {
                    const float band = std::max(std::fabs(p.y - (yEye + e.y * 0.13f)) - e.y * 0.055f, std::max(o - t * 0.9f, -(o + t)));
                    const float rib = std::max(std::max(std::fabs(p.x - c.x) - rx * 0.075f, std::max(p.y - (yEye + e.y * 0.2f), yBot + e.y * 0.05f - p.y)), std::max(o - t * 0.9f, -(o + t)));
                    d = std::min(d, std::min(band, rib));
                }
                // cerchio rialzato attorno alla cima e orlo in basso
                d = std::min(d, std::max(std::fabs(p.y - (yTop - e.y * 0.32f)) - e.y * 0.035f, std::max(o - t * 0.7f, -(o + t))));
                d = std::min(d, std::max(std::fabs(p.y - (yBot + e.y * 0.04f)) - e.y * 0.04f, std::max(o - t * 0.7f, -(o + t))));
                // feritoia per gli occhi (taglia anche la costola)
                const float slit = RoundBox(p, glm::vec3(c.x, yEye, c.z + rz), glm::vec3(rx * 0.62f, e.y * 0.035f, rz * 0.65f), e.y * 0.01f);
                d = std::max(d, -slit);
                // fori di respirazione sul lato destro, sotto la feritoia
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
                    const glm::vec3 h(c.x - sd * rx * (0.28f + 0.13f * i), yEye - e.y * (0.32f + 0.14f * j), c.z + rz);
                    d = std::max(d, -Capsule(p, h - glm::vec3(0, 0, rz * 0.3f), h + glm::vec3(0, 0, rz * 0.2f), e.y * 0.022f));
                }
                return d;
            };
            const glm::vec3 L(c.x - rx * 1.3f, yBot - Hb * 0.01f, c.z - rz * 1.3f), Hh(c.x + rx * 1.3f, yTop + Hb * 0.01f, c.z + rz * 1.3f);
            AddSdf(out, helm, L, Hh, Hb * 0.0028f, [&](const glm::vec3&) { return One(idHead); }, Hb);
            return !out.indices.empty();
        }

        // --- Goletta -------------------------------------------------------------------------------------------
        if (kind == "teutonic_gorget") {
            const float y0 = neck.y - H * 0.16f, y1 = neck.y + H * 0.12f;
            const RingTable rt = BuildRings(B, neck.x, neck.z, y0, y1, 12, 32, torsoMask);
            const float t = Hb * 0.008f, off = Hb * 0.03f;   // sopra la corazza
            auto f = [=](const glm::vec3& p) {
                const float a = Ang(p.x, p.z, rt.cx, rt.cz);
                const float rad = std::hypot(p.x - rt.cx, p.z - rt.cz);
                // due lame sovrapposte: la bassa piu' larga, che scende sulle spalle
                float d = 1e9f;
                for (int k = 0; k < 2; ++k) {
                    const float ya = k == 0 ? y0 + H * 0.02f : neck.y - H * 0.07f, yb = k == 0 ? neck.y - H * 0.03f : y1;
                    const float yy = std::clamp(p.y, ya, yb);
                    const float rc = rt.At(a, yy) + off + (k == 0 ? t * 1.2f : 0.0f) + (yb - yy) * 0.25f;   // svasata in basso
                    const float slab = std::max(std::fabs(rad - rc) - t * 0.5f, std::max(ya - p.y, p.y - yb));
                    d = std::min(d, slab);
                }
                return d;
            };
            const float R = Hb * 0.16f;
            AddSdf(out, f, glm::vec3(neck.x - R, y0 - Hb * 0.01f, neck.z - R), glm::vec3(neck.x + R, y1 + Hb * 0.01f, neck.z + R), Hb * 0.0036f,
                   [&](const glm::vec3& p) { return B.WeightsOf(B.Nearest(p, torsoMask)); }, Hb);
            return !out.indices.empty();
        }

        // --- Sopravveste e croce ------------------------------------------------------------------------------
        // Sopra la corazza: le piastre del busto sono gusci gonfiati fino a ~3% dell'altezza, la stoffa sta fuori
        const float overPlate = Hb * 0.034f;
        if (kind == "teutonic_tabard" || kind == "teutonic_tabard_cross") {
            const glm::vec3 shL = B.P("LeftArm"), shR = B.P("RightArm");
            const float shoulderHalf = std::fabs(shL.x - shR.x) * 0.5f;
            const float yTop = neck.y - H * 0.10f, yHem = yKnee + H * 0.04f;
            const float halfW = shoulderHalf * 1.1f;
            const ZField zf = BuildZField(B, 1.0f, halfW, yHem - H * 0.1f, yTop + H * 0.05f, 48, 96, clothMask, hips.y + H * 0.05f);
            const float thick = Hb * 0.0035f;
            // largo sul petto, stretto in vita, svasato sotto la cintura
            auto halfWidth = [=](float y) {
                const float t = (y - hips.y) / H;
                float w = glm::mix(0.40f, 0.58f, Sm(0.2f, 0.7f, t));
                w = glm::mix(w, 0.50f, Sm(0.75f, 1.0f, t));
                if (y < hips.y) w = glm::mix(0.40f, 0.52f, Sm(hips.y, yHem, y));
                return w * shoulderHalf * 1.25f;
            };
            auto range = [=](float y) { const float w = halfWidth(y); return std::make_pair(B.cx - w, B.cx + w); };
            auto hem = [=](float u) { return (std::sin(u * 23.0f) * 0.5f + std::sin(u * 61.0f + 1.3f) * 0.35f) * Hb * 0.006f; };
            auto neckHole = [=](float x, float y) { return glm::length(glm::vec2((x - B.cx) / (H * 0.17f), (y - yTop) / (H * 0.14f))) < 1.0f; };
            auto surf = [&](float x, float y) {
                // sopra la corazza; sotto la cintura un filo piu' staccata (cade davanti alle cosce)
                return zf.At(x, y) + overPlate + Hb * 0.022f * Sm(hips.y, yHem, y);
            };
            auto w = [&](const glm::vec3& p) { return clothWeights(p, 1.0f, 0.04f); };   // spaccata davanti: ogni meta' segue la sua coscia
            if (kind == "teutonic_tabard") {
                AddPanel(out, yHem, yTop, 40, 120, range, hem, neckHole, surf, thick, 1.0f, w, Hb);
            }
            else {
                // croce nera: braccio verticale dal collo fin sotto la cintura, braccio orizzontale sul petto
                const float yc = hips.y + H * 0.72f, bw = shoulderHalf * 0.17f;
                auto onTop = [&](float x, float y) { return surf(x, y) + thick * 0.9f; };
                AddPanel(out, hips.y - H * 0.08f, yTop - H * 0.13f, 6, 90,
                         [=](float) { return std::make_pair(B.cx - bw * 0.5f, B.cx + bw * 0.5f); }, [](float) { return 0.0f; },
                         [=](float, float y) { return std::fabs(y - yc) < bw * 0.5f; }, onTop, thick * 0.6f, 1.0f, w, Hb);
                AddPanel(out, yc - bw * 0.5f, yc + bw * 0.5f, 60, 6,
                         [=](float y) { const float hw = halfWidth(y) * 0.86f; return std::make_pair(B.cx - hw, B.cx + hw); }, [](float) { return 0.0f; },
                         [](float, float) { return false; }, onTop, thick * 0.6f, 1.0f, w, Hb);
            }
            FaceNormals(out, firstV, firstI);
            return !out.indices.empty();
        }

        // --- Mantello ------------------------------------------------------------------------------------------
        if (kind == "teutonic_cape") {
            const glm::vec3 shL = B.P("LeftArm"), shR = B.P("RightArm");
            const float shoulderHalf = std::fabs(shL.x - shR.x) * 0.5f;
            const float yTop = neck.y - H * 0.06f;
            const float yHem = B.P("LeftLeg").y - (B.P("LeftLeg").y - B.P("LeftFoot").y) * 0.45f;   // a meta' polpaccio
            const float halfW = shoulderHalf * 1.4f;
            const ZField zb = BuildZField(B, -1.0f, halfW, yHem - H * 0.1f, yTop + H * 0.05f, 48, 96, clothMask, neck.y - H * 0.35f);
            const float thick = Hb * 0.004f;
            auto halfWidth = [=](float y) {
                const float t = std::clamp((yTop - y) / (yTop - yHem), 0.0f, 1.0f);
                return shoulderHalf * glm::mix(0.95f, 1.22f, Sm(0.0f, 0.7f, t));
            };
            auto range = [=](float y) { const float w = halfWidth(y); return std::make_pair(B.cx - w, B.cx + w); };
            auto hem = [=](float u) { return (std::sin(u * 19.0f) * 0.5f + std::sin(u * 47.0f + 0.7f) * 0.4f + std::sin(u * 113.0f) * 0.2f) * Hb * 0.008f; };
            auto surf = [=](float x, float y) {
                const float t = std::clamp((yTop - y) / (yTop - yHem), 0.0f, 1.0f);
                float z = zb.At(x, y) - overPlate - Hb * (0.004f + 0.03f * t * t);       // sopra la corazza, si stacca scendendo
                // ai lati gira verso il davanti (avvolge le spalle)
                const float over = std::max(0.0f, std::fabs(x - B.cx) - shoulderHalf * 0.7f);
                z += over * over / (shoulderHalf * 1.2f);
                // pieghe verticali morbide che si aprono verso il basso
                z -= (0.5f + 0.5f * std::sin((x - B.cx) / shoulderHalf * 6.0f)) * Hb * 0.008f * Sm(yTop, yTop - H * 0.6f, y);
                return z;
            };
            AddPanel(out, yHem, yTop, 56, 130, range, hem, [](float, float) { return false; }, surf, thick, -1.0f,
                     [&](const glm::vec3& p) { return clothWeights(p, 0.45f); }, Hb);
            FaceNormals(out, firstV, firstI);
            return !out.indices.empty();
        }

        // --- Cinturone e falda ---------------------------------------------------------------------------------
        if (kind == "teutonic_belt" || kind == "teutonic_tassets") {
            const float y0 = hips.y - H * 0.45f, y1 = yBelt + H * 0.08f;
            const RingTable rt = BuildRings(B, B.cx, B.cz, y0, y1, 24, 36, clothMask);
            const float R = Hb * 0.2f;
            const glm::vec3 L(B.cx - R, y0 - Hb * 0.01f, B.cz - R), Hh(B.cx + R, y1 + Hb * 0.01f, B.cz + R);
            if (kind == "teutonic_belt") {
                const float hb = H * 0.045f, t = Hb * 0.0045f, off = overPlate + Hb * 0.012f;   // sopra la sopravveste
                auto f = [=](const glm::vec3& p) {
                    const float a = Ang(p.x, p.z, rt.cx, rt.cz);
                    const float rad = std::hypot(p.x - rt.cx, p.z - rt.cz);
                    const float rc = rt.At(a, yBelt) + off;
                    float d = std::max(std::fabs(rad - rc) - t, std::fabs(p.y - yBelt) - hb);
                    // fibbia davanti, borse ai fianchi
                    const glm::vec3 fr(rt.cx, yBelt, rt.cz + rt.At(0.0f, yBelt) + off + t);
                    const float buckle = std::max(RoundBox(p, fr, glm::vec3(hb * 1.1f, hb * 1.25f, t * 1.4f), t * 0.5f),
                                                  -RoundBox(p, fr, glm::vec3(hb * 0.6f, hb * 0.75f, t * 3.0f), t * 0.3f));
                    d = std::min(d, buckle);
                    for (float s : { -1.0f, 1.0f }) {
                        const float pa = s * 1.15f;
                        const float pr = rt.At(pa, yBelt - hb * 1.6f) + off + hb * 0.55f;
                        const glm::vec3 pc(rt.cx + std::sin(pa) * pr, yBelt - hb * 1.4f, rt.cz + std::cos(pa) * pr);
                        d = std::min(d, Ellipsoid(p, pc, glm::vec3(hb * 0.95f, hb * 1.5f, hb * 0.95f)));
                    }
                    return d;
                };
                AddSdf(out, f, L, Hh, Hb * 0.0032f, [&](const glm::vec3& p) { return clothWeights(p, 0.3f); }, Hb);
            }
            else {
                // tre file di lame, ognuna sopra la successiva; niente davanti (c'e' la sopravveste)
                const float rowH = H * 0.12f, t = Hb * 0.0075f, off = overPlate * 0.9f;        // sopra i cosciali
                auto f = [=](const glm::vec3& p) {
                    const float a = Ang(p.x, p.z, rt.cx, rt.cz);
                    const float rad = std::hypot(p.x - rt.cx, p.z - rt.cz);
                    const float aa = std::fabs(a);
                    if (aa < 0.62f) return 1e9f;                                 // davanti libero
                    float d = 1e9f;
                    for (int k = 0; k < 3; ++k) {
                        const float yt = yBelt - H * 0.05f - k * rowH * 0.78f, yb = yt - rowH;
                        const float yy = std::clamp(p.y, yb, yt);
                        const float rc = rt.At(a, yy) + off + k * t * 1.5f + (yt - yy) * 0.18f;   // svasate in basso
                        float s = std::max(std::fabs(rad - rc) - t * 0.5f, std::max(yb - p.y, p.y - yt));
                        s = std::max(s, 0.66f - aa);                                              // bordo davanti
                        s = std::max(s, -(3.1f - aa) + 0.02f);                                    // spacco dietro
                        d = std::min(d, s);
                    }
                    return d;
                };
                AddSdf(out, f, L, Hh, Hb * 0.003f, [&](const glm::vec3& p) { return clothWeights(p, 0.55f); }, Hb);
            }
            return !out.indices.empty();
        }

        // --- Spallacci, cubitiere, polsini --------------------------------------------------------------------
        const bool left = kind.size() > 2 && kind.compare(kind.size() - 2, 2, "_l") == 0;
        const bool right = kind.size() > 2 && kind.compare(kind.size() - 2, 2, "_r") == 0;
        if (!left && !right) return false;
        const std::string S = left ? "Left" : "Right";
        const float ss = left ? sd : -sd;      // segno x del lato
        const int gArm = left ? G_ArmL : G_ArmR, gLeg = left ? G_LegL : G_LegR;
        auto bn = [&](const char* b) { return S + b; };
        auto P = [&](const char* b) { return B.P((S + b).c_str()); };
        auto I = [&](const char* b) { return B.Id((S + b).c_str()); };

        if (kind.rfind("teutonic_pauldron", 0) == 0) {
            const glm::vec3 sh = P("Arm"), el = P("ForeArm");
            const float rA = B.LimbRadius(gArm, sh, el, 0.0f, 0.35f);
            const glm::vec3 out3(ss, 0.0f, 0.0f);
            const glm::vec3 C = sh + out3 * rA * 0.25f + glm::vec3(0.0f, rA * 0.35f, 0.0f);
            const glm::vec3 R(rA * 1.55f, rA * 1.25f, rA * 1.55f);
            const float t = rA * 0.14f;
            auto f = [=](const glm::vec3& p) {
                const glm::vec3 q = p - C;
                const float ox = q.x * ss;                                              // verso l'esterno
                // calotta: guscio d'ellissoide tagliato sotto da un piano inclinato (piu' basso fuori)
                const float o = Ellipsoid(p, C, R);
                float d = std::max(o, -(o + t));
                d = std::max(d, -(q.y + rA * 0.55f + ox * 0.35f));
                d = std::max(d, -(ox + rA * 0.95f));                                    // non entra nel collo
                // bordo rialzato lungo il taglio
                const float lip = std::max(std::fabs(q.y + rA * 0.55f + ox * 0.35f - rA * 0.07f) - rA * 0.07f, std::max(o - t * 0.8f, -(o + t * 1.6f)));
                d = std::min(d, std::max(lip, -(ox + rA * 0.9f)));
                // due lame sotto, solo all'esterno
                for (int k = 0; k < 2; ++k) {
                    const glm::vec3 Ck = C + glm::vec3(ss * rA * 0.12f * (k + 1), -rA * (0.55f + 0.4f * k), 0.0f);
                    const glm::vec3 Rk = R * (1.0f + 0.04f * (k + 1));
                    const float ok = Ellipsoid(p, Ck, Rk);
                    float lk = std::max(ok, -(ok + t));
                    const float yk = q.y + rA * (0.55f + 0.4f * k) + ox * 0.35f;
                    lk = std::max(lk, std::fabs(yk + rA * 0.22f) - rA * 0.25f);
                    lk = std::max(lk, -(ox - rA * 0.15f));
                    d = std::min(d, lk);
                }
                return d;
            };
            const int idArm = I("Arm"), idPd = B.Id((S + "Pauldron").c_str()) >= 0 ? B.Id((S + "Pauldron").c_str()) : B.Id("Spine2");
            AddSdf(out, f, C - R * 1.6f, C + R * 1.6f, rA * 0.075f, [&](const glm::vec3& p) {
                const float low = Sm(C.y, C.y - rA * 0.9f, p.y);                         // le lame seguono di piu' il braccio
                return Two(idArm, 0.55f + 0.3f * low, idPd, 0.45f - 0.3f * low);
            }, Hb);
            (void)bn;
            return !out.indices.empty();
        }
        if (kind.rfind("teutonic_couter", 0) == 0) {
            const glm::vec3 sh = P("Arm"), el = P("ForeArm"), wr = P("Hand");
            const float rF = B.LimbRadius(gArm, el, wr, 0.0f, 0.2f);
            const glm::vec3 a = glm::normalize(el - sh);
            const glm::vec3 back(0.0f, 0.0f, -1.0f);                                    // il gomito punta indietro in posa a T
            const glm::vec3 C = el + back * rF * 0.35f;
            const float t = rF * 0.16f;
            auto f = [=](const glm::vec3& p) {
                const glm::vec3 q = p - C;
                const float along = glm::dot(q, a);
                const glm::vec3 r3(std::fabs(a.x) * rF * 0.95f + (1.0f - std::fabs(a.x)) * rF * 1.15f, rF * 1.15f, rF * 1.0f);
                const float o = Ellipsoid(p, C, r3);
                float d = std::max(o, -(o + t));
                d = std::max(d, q.z - rF * 0.15f);                                      // solo dietro
                // rosetta tonda sul lato esterno del gomito
                const glm::vec3 rc = C + glm::vec3(0.0f, rF * 0.95f, -rF * 0.1f);
                (void)along;
                return std::min(d, Ellipsoid(p, rc, glm::vec3(rF * 0.75f, rF * 0.16f, rF * 0.75f)));
            };
            AddSdf(out, f, C - glm::vec3(rF * 2.0f), C + glm::vec3(rF * 2.0f), rF * 0.07f, [&](const glm::vec3&) { return Two(I("Arm"), 0.5f, I("ForeArm"), 0.5f); }, Hb);
            return !out.indices.empty();
        }
        if (kind.rfind("teutonic_cuff", 0) == 0) {
            const glm::vec3 el = P("ForeArm"), wr = P("Hand");
            const float L = glm::length(el - wr);
            const float rW = B.LimbRadius(gArm, wr, el, 0.08f, 0.25f);
            const glm::vec3 dir = glm::normalize(el - wr);
            const glm::vec3 a = wr + dir * L * 0.02f, b = wr + dir * L * 0.36f;
            const float t = rW * 0.14f;
            auto f = [=](const glm::vec3& p) {
                const float o = RoundCone(p, a, b, rW * 1.22f, rW * 1.62f);
                float d = std::max(o, -(o + t));
                d = std::max(d, -glm::dot(p - a, dir));                                  // aperto verso la mano
                d = std::max(d, glm::dot(p - b, dir));                                   // e verso il gomito
                // nervatura sul bordo
                const float rib = std::max(std::fabs(glm::dot(p - b, dir) + L * 0.03f) - L * 0.022f, std::max(o - t * 0.8f, -(o + t * 1.6f)));
                return std::min(d, std::max(rib, std::max(-glm::dot(p - a, dir), glm::dot(p - b, dir))));
            };
            const glm::vec3 lo = glm::min(a, b) - glm::vec3(rW * 2.2f), hi = glm::max(a, b) + glm::vec3(rW * 2.2f);
            AddSdf(out, f, lo, hi, rW * 0.08f, [&](const glm::vec3& p) {
                const float k = std::clamp(glm::dot(p - wr, dir) / L, 0.0f, 1.0f);
                return Two(I("Hand"), 0.8f - k, I("ForeArm"), 0.2f + k);
            }, Hb);
            return !out.indices.empty();
        }
        if (kind.rfind("teutonic_knee", 0) == 0) {
            const glm::vec3 up = P("UpLeg"), kn = P("Leg"), ft = P("Foot");
            const float rL = B.LimbRadius(gLeg, kn, ft, 0.0f, 0.15f);
            const glm::vec3 C = kn + glm::vec3(0.0f, 0.0f, rL * 0.3f);
            const float t = rL * 0.15f;
            auto f = [=](const glm::vec3& p) {
                const glm::vec3 q = p - C;
                const float o = Ellipsoid(p, C, glm::vec3(rL * 1.2f, rL * 1.35f, rL * 1.15f));
                float d = std::max(o, -(o + t));
                d = std::max(d, -(q.z + rL * 0.15f));                                   // solo davanti
                // ala a cuore sul lato esterno
                const glm::vec3 wc = C + glm::vec3(ss * rL * 1.05f, 0.0f, -rL * 0.35f);
                const float wing = Ellipsoid(p, wc, glm::vec3(rL * 0.14f, rL * 0.95f, rL * 0.8f));
                d = std::min(d, wing);
                // costola centrale
                const float rib = std::max(std::fabs(q.x) - rL * 0.06f, std::max(o - t * 0.8f, -(o + t * 1.6f)));
                return std::min(d, std::max(rib, -(q.z + rL * 0.1f)));
            };
            AddSdf(out, f, C - glm::vec3(rL * 2.0f), C + glm::vec3(rL * 2.0f), rL * 0.07f, [&](const glm::vec3& p) {
                const float k = Sm(C.y + rL, C.y - rL, p.y);
                return Two(I("UpLeg"), 0.65f - 0.35f * k, I("Leg"), 0.35f + 0.35f * k);
            }, Hb);
            (void)up;
            return !out.indices.empty();
        }
        return false;
    }

    // Pezzi costruiti sul corpo, per nome ("procedural" nei JSON delle armature)
    inline bool BuildProceduralGarment(const SkinnedModel& body, const std::string& kind, GarmentShell& out) {
        if (kind.rfind("teutonic_", 0) == 0) return BuildTeutonicGarment(body, kind, out);
        return false;
    }
}
