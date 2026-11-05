#pragma once
// Da una funzione di distanza con segno (SDF: < 0 dentro, > 0 fuori) a una mesh di triangoli.
// Algoritmo "surface nets": un vertice per ogni cella della griglia attraversata dalla superficie (media dei
// punti in cui la superficie taglia gli spigoli della cella, poi riportato sulla superficie col gradiente),
// un quadrilatero per ogni spigolo della griglia che cambia segno. Superfici lisce e organiche, adatte a
// creature e parti organiche (vedi SquigModel.h).
//
// La griglia fine viene valutata solo vicino alla superficie: prima una griglia grossolana (passo x4), poi
// solo i blocchi in cui la distanza grossolana dice che la superficie puo' passare. Calcolo su piu' thread.
#include <glm/glm.hpp>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <functional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace SunEngine {

    // ---------------------------- primitive e operatori ----------------------------
    namespace Sdf {
        inline float Sphere(const glm::vec3& p, const glm::vec3& c, float r) { return glm::length(p - c) - r; }
        // Ellissoide (approssimazione con distanza quasi esatta vicino alla superficie)
        inline float Ellipsoid(const glm::vec3& p, const glm::vec3& c, const glm::vec3& r) {
            const glm::vec3 q = p - c;
            const float k0 = glm::length(q / r);
            const float k1 = glm::length(q / (r * r));
            return k1 > 1e-8f ? k0 * (k0 - 1.0f) / k1 : -std::min(r.x, std::min(r.y, r.z));
        }
        // Capsula fra a e b
        inline float Capsule(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, float r) {
            const glm::vec3 pa = p - a, ba = b - a;
            const float h = glm::clamp(glm::dot(pa, ba) / std::max(glm::dot(ba, ba), 1e-8f), 0.0f, 1.0f);
            return glm::length(pa - ba * h) - r;
        }
        // Cono arrotondato: raggio ra in a, rb in b
        inline float RoundCone(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, float ra, float rb) {
            const glm::vec3 ba = b - a;
            const float l2 = glm::dot(ba, ba);
            if (l2 < 1e-10f) return glm::length(p - a) - std::max(ra, rb);
            const float rr = ra - rb, a2 = l2 - rr * rr, il2 = 1.0f / l2;
            const glm::vec3 pa = p - a;
            const float y = glm::dot(pa, ba), z = y - l2;
            const glm::vec3 xv = pa * l2 - ba * y;
            const float x2 = glm::dot(xv, xv), y2 = y * y * l2, z2 = z * z * l2;
            const float k = (rr > 0.0f ? 1.0f : -1.0f) * rr * rr * x2;
            if ((z > 0.0f ? 1.0f : -1.0f) * a2 * z2 > k) return std::sqrt(x2 + z2) * il2 - rb;
            if ((y > 0.0f ? 1.0f : -1.0f) * a2 * y2 < k) return std::sqrt(x2 + y2) * il2 - ra;
            return (std::sqrt(x2 * a2 * il2) + y * rr) * il2 - ra;
        }
        // Scatola arrotondata (semi-lati b, raggio degli spigoli r)
        inline float RoundBox(const glm::vec3& p, const glm::vec3& c, const glm::vec3& b, float r) {
            const glm::vec3 q = glm::abs(p - c) - b + r;
            return glm::length(glm::max(q, glm::vec3(0.0f))) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f) - r;
        }
        // Toro sul piano XZ (asse Y) centrato in c
        inline float Torus(const glm::vec3& p, const glm::vec3& c, float R, float r) {
            const glm::vec3 q = p - c;
            const glm::vec2 t(glm::length(glm::vec2(q.x, q.z)) - R, q.y);
            return glm::length(t) - r;
        }
        // Unione, sottrazione, intersezione morbide (k = raggio del raccordo)
        inline float SUnion(float a, float b, float k) {
            if (k <= 0.0f) return std::min(a, b);
            const float h = glm::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
            return glm::mix(b, a, h) - k * h * (1.0f - h);
        }
        inline float SSub(float a, float b, float k) {   // a meno b
            if (k <= 0.0f) return std::max(a, -b);
            const float h = glm::clamp(0.5f - 0.5f * (a + b) / k, 0.0f, 1.0f);
            return glm::mix(a, -b, h) + k * h * (1.0f - h);
        }
        inline float SInter(float a, float b, float k) {
            if (k <= 0.0f) return std::max(a, b);
            const float h = glm::clamp(0.5f - 0.5f * (b - a) / k, 0.0f, 1.0f);
            return glm::mix(b, a, h) + k * h * (1.0f - h);
        }
        // Rotazioni (radianti) per orientare le primitive
        inline glm::vec3 RotX(const glm::vec3& p, float a) { const float c = std::cos(a), s = std::sin(a); return { p.x, c * p.y - s * p.z, s * p.y + c * p.z }; }
        inline glm::vec3 RotY(const glm::vec3& p, float a) { const float c = std::cos(a), s = std::sin(a); return { c * p.x + s * p.z, p.y, -s * p.x + c * p.z }; }
        inline glm::vec3 RotZ(const glm::vec3& p, float a) { const float c = std::cos(a), s = std::sin(a); return { c * p.x - s * p.y, s * p.x + c * p.y, p.z }; }
        // Rumore liscio deterministico (per irregolarita' della pelle, crepe, verruche)
        inline float Hash(int x, int y, int z) {
            uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(z) * 2147483647u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return ((h ^ (h >> 16)) & 0xFFFFFFu) / 16777215.0f;
        }
        inline float Noise(const glm::vec3& p) {
            const glm::vec3 f = glm::floor(p), t = p - f, s = t * t * (3.0f - 2.0f * t);
            const int x = static_cast<int>(f.x), y = static_cast<int>(f.y), z = static_cast<int>(f.z);
            auto h = [&](int a, int b, int c) { return Hash(x + a, y + b, z + c); };
            const float a0 = glm::mix(glm::mix(h(0, 0, 0), h(1, 0, 0), s.x), glm::mix(h(0, 1, 0), h(1, 1, 0), s.x), s.y);
            const float a1 = glm::mix(glm::mix(h(0, 0, 1), h(1, 0, 1), s.x), glm::mix(h(0, 1, 1), h(1, 1, 1), s.x), s.y);
            return glm::mix(a0, a1, s.z);
        }
    }

    struct SdfMesh {
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> normals;
        std::vector<unsigned int> indices;
    };

    // f: distanza nel punto. [lo, hi]: volume da coprire. cell: passo della griglia fine.
    inline SdfMesh SdfPolygonize(const std::function<float(const glm::vec3&)>& f, glm::vec3 lo, glm::vec3 hi, float cell,
                                 int threads = 0) {
        SdfMesh out;
        if (cell <= 0.0f) return out;
        lo -= glm::vec3(cell * 2.0f); hi += glm::vec3(cell * 2.0f);
        const int B = 4;   // celle fini per blocco grossolano
        glm::ivec3 nb = glm::max(glm::ivec3(1), glm::ivec3(glm::ceil((hi - lo) / (cell * B))));
        const glm::ivec3 n = nb * B + 1;   // punti della griglia fine
        const size_t total = static_cast<size_t>(n.x) * n.y * n.z;
        if (total > 60'000'000ull) return out;   // griglia assurda: niente mesh
        std::vector<float> v(total, 1e9f);
        auto idx = [&](int x, int y, int z) { return (static_cast<size_t>(z) * n.y + y) * n.x + x; };
        auto pos = [&](int x, int y, int z) { return lo + glm::vec3(x, y, z) * cell; };

        unsigned hw = threads > 0 ? static_cast<unsigned>(threads) : std::max(1u, std::thread::hardware_concurrency());
        hw = std::min(hw, 16u);
        auto parallel = [&](int count, const std::function<void(int)>& job) {
            std::vector<std::thread> pool;
            const int per = (count + static_cast<int>(hw) - 1) / static_cast<int>(hw);
            for (unsigned t = 0; t < hw; ++t) {
                const int a = static_cast<int>(t) * per, b = std::min(count, a + per);
                if (a >= b) break;
                pool.emplace_back([&, a, b] { for (int i = a; i < b; ++i) job(i); });
            }
            for (auto& th : pool) th.join();
        };

        // 1) blocchi: distanza al centro di ogni blocco; un punto fine si valuta davvero solo se uno dei blocchi
        //    che lo toccano e' vicino alla superficie, altrimenti prende la distanza grossolana (basta il segno)
        const float blockRadius = cell * B * 0.8660254f;
        std::vector<float> coarse(static_cast<size_t>(nb.x) * nb.y * nb.z);
        auto bidx = [&](int x, int y, int z) { return (static_cast<size_t>(z) * nb.y + y) * nb.x + x; };
        parallel(nb.z, [&](int bz) {
            for (int by = 0; by < nb.y; ++by)
                for (int bx = 0; bx < nb.x; ++bx)
                    coarse[bidx(bx, by, bz)] = f(pos(bx * B, by * B, bz * B) + glm::vec3(cell * B * 0.5f));
        });
        auto nearBlock = [&](int bx, int by, int bz) {
            if (bx < 0 || by < 0 || bz < 0 || bx >= nb.x || by >= nb.y || bz >= nb.z) return false;
            return std::abs(coarse[bidx(bx, by, bz)]) < blockRadius * 1.6f + cell;
        };
        parallel(n.z, [&](int z) {
            for (int y = 0; y < n.y; ++y)
                for (int x = 0; x < n.x; ++x) {
                    const int bx = std::min(x / B, nb.x - 1), by = std::min(y / B, nb.y - 1), bz = std::min(z / B, nb.z - 1);
                    bool near = false;
                    for (int dz = (z % B == 0 && z > 0) ? -1 : 0; dz <= 0 && !near; ++dz)
                        for (int dy = (y % B == 0 && y > 0) ? -1 : 0; dy <= 0 && !near; ++dy)
                            for (int dx = (x % B == 0 && x > 0) ? -1 : 0; dx <= 0 && !near; ++dx)
                                near = nearBlock(bx + dx, by + dy, bz + dz);
                    v[idx(x, y, z)] = near ? f(pos(x, y, z)) : coarse[bidx(bx, by, bz)];
                }
        });
        // 2) vertici: uno per cella attraversata
        const glm::ivec3 nc = n - 1;
        std::vector<int> cellVert(static_cast<size_t>(nc.x) * nc.y * nc.z, -1);
        auto cidx = [&](int x, int y, int z) { return (static_cast<size_t>(z) * nc.y + y) * nc.x + x; };
        static const int E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
        std::vector<std::vector<std::pair<size_t, glm::vec3>>> perSlab(nc.z);
        parallel(nc.z, [&](int z) {
            auto& list = perSlab[z];
            for (int y = 0; y < nc.y; ++y)
                for (int x = 0; x < nc.x; ++x) {
                    float d[8]; int mask = 0;
                    for (int k = 0; k < 8; ++k) {
                        d[k] = v[idx(x + (k & 1), y + ((k >> 1) & 1), z + ((k >> 2) & 1))];
                        if (d[k] < 0.0f) mask |= 1 << k;
                    }
                    if (mask == 0 || mask == 255) continue;
                    glm::vec3 acc(0.0f); int cnt = 0;
                    for (const auto& e : E) {
                        const float a = d[e[0]], b = d[e[1]];
                        if ((a < 0.0f) == (b < 0.0f)) continue;
                        const float t = a / (a - b);
                        const glm::vec3 pa(e[0] & 1, (e[0] >> 1) & 1, (e[0] >> 2) & 1), pb(e[1] & 1, (e[1] >> 1) & 1, (e[1] >> 2) & 1);
                        acc += glm::mix(pa, pb, t); ++cnt;
                    }
                    glm::vec3 p = pos(x, y, z) + (acc / static_cast<float>(cnt)) * cell;
                    // sulla superficie: due passi di Newton col gradiente, restando nella cella
                    const glm::vec3 cmin = pos(x, y, z), cmax = cmin + glm::vec3(cell);
                    for (int it = 0; it < 2; ++it) {
                        const float h = cell * 0.25f;
                        const float fp = f(p);
                        const glm::vec3 g((f(p + glm::vec3(h, 0, 0)) - f(p - glm::vec3(h, 0, 0))),
                                          (f(p + glm::vec3(0, h, 0)) - f(p - glm::vec3(0, h, 0))),
                                          (f(p + glm::vec3(0, 0, h)) - f(p - glm::vec3(0, 0, h))));
                        const float g2 = glm::dot(g, g) / (4.0f * h * h);
                        if (g2 < 1e-10f) break;
                        p -= (g / (2.0f * h)) * (fp / g2);
                        p = glm::clamp(p, cmin, cmax);
                    }
                    list.push_back({ cidx(x, y, z), p });
                }
        });
        for (int z = 0; z < nc.z; ++z)
            for (const auto& [ci, p] : perSlab[z]) { cellVert[ci] = static_cast<int>(out.positions.size()); out.positions.push_back(p); }
        perSlab.clear();
        // normali dal gradiente
        out.normals.resize(out.positions.size());
        parallel(static_cast<int>(out.positions.size() / 1024 + 1), [&](int chunk) {
            const size_t a = static_cast<size_t>(chunk) * 1024, b = std::min(out.positions.size(), a + 1024);
            const float h = cell * 0.5f;
            for (size_t i = a; i < b; ++i) {
                const glm::vec3 p = out.positions[i];
                glm::vec3 g(f(p + glm::vec3(h, 0, 0)) - f(p - glm::vec3(h, 0, 0)),
                            f(p + glm::vec3(0, h, 0)) - f(p - glm::vec3(0, h, 0)),
                            f(p + glm::vec3(0, 0, h)) - f(p - glm::vec3(0, 0, h)));
                const float l = glm::length(g);
                out.normals[i] = l > 1e-8f ? g / l : glm::vec3(0, 1, 0);
            }
        });
        // 3) quadrilateri: uno per spigolo della griglia con cambio di segno
        auto quad = [&](int a, int b, int c, int d, bool flip) {
            if (a < 0 || b < 0 || c < 0 || d < 0) return;
            if (flip) std::swap(b, d);
            // diagonale piu' corta
            const float d1 = glm::distance(out.positions[a], out.positions[c]), d2 = glm::distance(out.positions[b], out.positions[d]);
            const unsigned A = a, Bv = b, C = c, D = d;
            if (d1 <= d2) out.indices.insert(out.indices.end(), { A, Bv, C, A, C, D });
            else out.indices.insert(out.indices.end(), { A, Bv, D, Bv, C, D });
        };
        auto cv = [&](int x, int y, int z) { return cellVert[cidx(x, y, z)]; };
        for (int z = 0; z < n.z; ++z)
            for (int y = 0; y < n.y; ++y)
                for (int x = 0; x < n.x; ++x) {
                    const bool in0 = v[idx(x, y, z)] < 0.0f;
                    if (x < nc.x && y >= 1 && y < nc.y && z >= 1 && z < nc.z && in0 != (v[idx(x + 1, y, z)] < 0.0f))
                        quad(cv(x, y - 1, z - 1), cv(x, y, z - 1), cv(x, y, z), cv(x, y - 1, z), in0);
                    if (y < nc.y && x >= 1 && x < nc.x && z >= 1 && z < nc.z && in0 != (v[idx(x, y + 1, z)] < 0.0f))
                        quad(cv(x - 1, y, z - 1), cv(x - 1, y, z), cv(x, y, z), cv(x, y, z - 1), in0);
                    if (z < nc.z && x >= 1 && x < nc.x && y >= 1 && y < nc.y && in0 != (v[idx(x, y, z + 1)] < 0.0f))
                        quad(cv(x - 1, y - 1, z), cv(x, y - 1, z), cv(x, y, z), cv(x - 1, y, z), in0);
                }
        // verso dei triangoli: concorde con la normale (gradiente) del primo vertice
        for (size_t t = 0; t + 2 < out.indices.size(); t += 3) {
            const glm::vec3& a = out.positions[out.indices[t]], & b = out.positions[out.indices[t + 1]], & c = out.positions[out.indices[t + 2]];
            const glm::vec3 fn = glm::cross(b - a, c - a);
            const glm::vec3 vn = out.normals[out.indices[t]] + out.normals[out.indices[t + 1]] + out.normals[out.indices[t + 2]];
            if (glm::dot(fn, vn) < 0.0f) std::swap(out.indices[t + 1], out.indices[t + 2]);
        }
        return out;
    }

    // Occlusione ambientale per vertice stimata dalla SDF: quanto la superficie e' "chiusa" intorno
    // (pieghe, orbite, narici, interno della bocca). 1 = esposto, 0 = in fondo a una cavita'.
    inline std::vector<float> SdfAmbientOcclusion(const std::function<float(const glm::vec3&)>& f, const SdfMesh& m, float scale) {
        std::vector<float> ao(m.positions.size(), 1.0f);
        for (size_t i = 0; i < m.positions.size(); ++i) {
            float occ = 0.0f, w = 1.0f;
            for (int k = 1; k <= 5; ++k) {
                const float h = scale * static_cast<float>(k) * 0.2f;
                occ += w * std::max(0.0f, h - f(m.positions[i] + m.normals[i] * h));
                w *= 0.6f;
            }
            ao[i] = glm::clamp(1.0f - 2.2f * occ / scale, 0.0f, 1.0f);
        }
        return ao;
    }
}
