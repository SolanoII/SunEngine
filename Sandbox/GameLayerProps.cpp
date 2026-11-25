// Prop delle mappe (Tools/ModelForge/props, Models/Props/<id>.glb).
//
// Le mappe sono fatte di blocchi (collisioni, navmesh, materiali procedurali). Qui i blocchi di certi materiali
// vengono "vestiti" con mesh vere fatte in Blender, che si disegnano al posto del cubo (il cubo resta per le
// collisioni e la navmesh, solo nascosto):
//   - sacchi di sabbia       -> sandbag_wall (pezzi da 2 m, adattati al blocco)
//   - terrapieni di trincea  -> trench_berm  (pezzi da 3 m)
//   - tronchi (+ chiome)     -> dead_tree    (albero morto spezzato; la chioma a cubo sparisce)
// Poi sul terreno libero si spargono filo spinato, cavalli di frisia e croci di legno (detriti: si spengono con
// l'opzione grafica dei detriti). Sempre uguali per la stessa mappa (seme dal nome).
#include "GameLayer.h"
#include "Engine/Renderer/AuthoredModel.h"
#include "Engine/Renderer/SurfaceMaterials.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

extern std::string FindValidDir(const std::string& targetDirName);

namespace SunEngine {

    namespace {
        struct MapPropTag {};   // pezzi che vestono i blocchi: si rifanno a ogni caricamento della mappa

        // Misure dei prop come escono da Blender (lungo X, alto Y, profondo Z), per adattarli ai blocchi
        struct PropFit { const char* id; glm::vec3 size; };
        constexpr PropFit kSandbag{ "sandbag_wall", { 2.0f, 1.0f, 0.8f } };
        constexpr PropFit kBerm{ "trench_berm", { 3.0f, 1.3f, 1.0f } };
        constexpr float kTreeHeight = 7.4f;
    }

    void GameLayer::LoadProps() {
        m_PropsLoaded = true;
        const std::string dir = FindValidDir("Models") + "Props/";
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) { SUN_CORE_WARN("Prop delle mappe: cartella {0} assente", dir); return; }
        for (const auto& f : std::filesystem::directory_iterator(dir, ec)) {
            if (f.path().extension() != ".glb") continue;
            const std::string id = f.path().stem().string();
            auto mesh = std::make_unique<SkinnedModel>();
            if (!LoadAuthoredProp(f.path().string(), *mesh) || mesh->meshes.empty()) { SUN_CORE_WARN("Prop {0} non caricato", id); continue; }
            auto bm = std::make_unique<BlockModel>();
            bm->id = id;
            bm->mesh = mesh.get();
            glm::vec3 lo(1e9f), hi(-1e9f);
            for (const auto& m : mesh->meshes) for (const auto& v : m.vertices) { lo = glm::min(lo, v.Position); hi = glm::max(hi, v.Position); }
            bm->boundsMin = lo; bm->boundsMax = hi;
            m_PropModels[id] = std::move(bm);
            m_PropMeshes[id] = std::move(mesh);
        }
        SUN_CORE_INFO("Prop delle mappe caricati: {0}", m_PropModels.size());
    }

    void GameLayer::DressMap(const std::string& mapId) {
        if (!m_PropsLoaded) LoadProps();
        auto& reg = m_Scene.GetRegistry();
        {   // via i pezzi della mappa precedente
            std::vector<entt::entity> old;
            for (auto e : reg.view<MapPropTag>()) old.push_back(e);
            for (auto e : old) m_Scene.DestroyEntity(e);
        }
        if (m_PropModels.empty()) return;
        auto model = [&](const char* id) -> const BlockModel* { auto it = m_PropModels.find(id); return it != m_PropModels.end() ? it->second.get() : nullptr; };

        uint32_t seed = 2166136261u;
        for (char c : mapId) { seed ^= static_cast<uint8_t>(c); seed *= 16777619u; }
        auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFFFF) / 16777215.0f; };

        auto spawn = [&](const BlockModel* bm, const glm::vec3& basePos, float yawDeg, const glm::vec3& scale, bool decor) {
            auto e = m_Scene.CreateEntity();
            auto& t = reg.emplace<TransformComponent>(e);
            t.Position = basePos; t.Rotation = glm::vec3(0.0f, yawDeg, 0.0f); t.Scale = scale;
            reg.emplace<MeshComponent>(e, 0u);
            auto& c = reg.emplace<ColorComponent>(e); c.Color = c.BaseColor = glm::vec4(1.0f);
            reg.emplace<BlockModelComponent>(e, BlockModelComponent{ bm });
            if (decor) reg.emplace<MapDecorTag>(e); else reg.emplace<MapPropTag>(e);
        };

        // Blocchi della mappa (quelli con collisione e materiale)
        struct Blk { entt::entity e; glm::vec3 c, s; float yaw; int mat; };
        std::vector<Blk> blocks;
        glm::vec3 terrainC(0.0f), terrainS(0.0f);
        for (auto e : reg.view<TransformComponent, SurfaceMaterialComponent, ColliderComponent, MeshComponent>()) {
            if (reg.any_of<MapDecorTag, BlockModelComponent>(e)) continue;
            const auto& t = reg.get<TransformComponent>(e);
            const int mat = reg.get<SurfaceMaterialComponent>(e).id;
            if (mat == Surface_Mud) {   // il terreno: il blocco di fango piu' grande
                if (t.Scale.x * t.Scale.z > terrainS.x * terrainS.z) { terrainC = t.Position; terrainS = t.Scale; }
                continue;
            }
            blocks.push_back({ e, t.Position, t.Scale, t.Rotation.y, mat });
        }

        int dressed = 0;
        // Pezzi uguali messi in fila lungo il lato lungo del blocco, stirati per riempirlo esattamente
        auto dressRow = [&](const Blk& b, const PropFit& fit, const BlockModel* bm) {
            const bool alongX = b.s.x >= b.s.z;
            const float len = alongX ? b.s.x : b.s.z, depth = alongX ? b.s.z : b.s.x;
            const int n = std::max(1, static_cast<int>(std::lround(len / fit.size.x)));
            const float seg = len / n;
            const float yaw = b.yaw + (alongX ? 0.0f : 90.0f);
            const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(yaw), glm::vec3(0, 1, 0));
            const glm::vec3 axis = glm::vec3(rot * glm::vec4(1, 0, 0, 0));
            const float baseY = b.c.y - b.s.y * 0.5f;
            for (int i = 0; i < n; ++i) {
                const float off = -len * 0.5f + seg * (i + 0.5f);
                const float flip = (rnd() < 0.5f) ? 180.0f : 0.0f;   // pezzi girati a caso: la fila non si ripete
                spawn(bm, glm::vec3(b.c.x, baseY, b.c.z) + axis * off, yaw + flip,
                    glm::vec3(seg / fit.size.x * 1.01f, b.s.y / fit.size.y, depth / fit.size.z), false);
            }
            reg.emplace_or_replace<HideProceduralVisualTag>(b.e);
            ++dressed;
        };

        const BlockModel* sandbag = model(kSandbag.id);
        const BlockModel* berm = model(kBerm.id);
        const BlockModel* tree = model("dead_tree");
        std::vector<glm::vec3> trunks;
        for (const auto& b : blocks) {
            if (b.mat == Surface_Sandbags && sandbag && b.s.y <= 2.0f) dressRow(b, kSandbag, sandbag);
            else if (b.mat == Surface_Earthworks && berm && b.s.y <= 2.2f && std::min(b.s.x, b.s.z) <= 2.5f) dressRow(b, kBerm, berm);
            else if (b.mat == Surface_Wood && tree && b.s.x < 1.2f && b.s.z < 1.2f && b.s.y > 1.8f) {
                const float h = b.s.y + 2.5f;   // il tronco della mappa + la chioma che c'era sopra
                const float k = std::clamp(h / kTreeHeight, 0.6f, 1.6f);
                spawn(tree, glm::vec3(b.c.x, b.c.y - b.s.y * 0.5f, b.c.z), rnd() * 360.0f, glm::vec3(k), false);
                reg.emplace_or_replace<HideProceduralVisualTag>(b.e);
                trunks.push_back(b.c);
                ++dressed;
            }
        }
        // chiome a cubo sopra i tronchi: spariscono (l'albero morto ha i rami al loro posto)
        for (const auto& b : blocks) {
            if (b.mat != Surface_Foliage || b.c.y - b.s.y * 0.5f < 1.5f) continue;
            for (const auto& t : trunks)
                if (std::abs(t.x - b.c.x) < b.s.x * 0.5f + 0.5f && std::abs(t.z - b.c.z) < b.s.z * 0.5f + 0.5f) { reg.emplace_or_replace<HideProceduralVisualTag>(b.e); break; }
        }

        // Terreno libero: filo spinato, cavalli di frisia, croci
        int scattered = 0;
        if (terrainS.x > 10.0f && terrainS.z > 10.0f) {
            const float groundY = terrainC.y + terrainS.y * 0.5f;
            auto freeAt = [&](const glm::vec3& p, float r) {
                for (const auto& b : blocks) {
                    const glm::vec2 d = glm::abs(glm::vec2(p.x - b.c.x, p.z - b.c.z)) - glm::vec2(b.s.x, b.s.z) * 0.5f;
                    if (d.x < r && d.y < r) return false;
                }
                for (const auto& sp : m_CurrentMapData.playerSpawnPoints) if (glm::length(glm::vec2(p.x - sp.x, p.z - sp.z)) < 7.0f) return false;
                for (const auto& sp : m_CurrentMapData.enemySpawnPoints) if (glm::length(glm::vec2(p.x - sp.x, p.z - sp.z)) < 7.0f) return false;
                return true;
            };
            const float area = terrainS.x * terrainS.z;
            struct Kind { const char* id; int count; float radius; };
            const Kind kinds[] = { { "barbed_wire", static_cast<int>(area / 520.0f), 2.0f },
                                   { "czech_hedgehog", static_cast<int>(area / 900.0f), 1.3f },
                                   { "grave_cross", static_cast<int>(area / 1300.0f), 1.0f } };
            for (const auto& k : kinds) {
                const BlockModel* bm = model(k.id);
                if (!bm) continue;
                for (int i = 0, tries = 0; i < k.count && tries < k.count * 20; ++tries) {
                    const glm::vec3 p(terrainC.x + (rnd() - 0.5f) * (terrainS.x - 8.0f), groundY, terrainC.z + (rnd() - 0.5f) * (terrainS.z - 8.0f));
                    if (!freeAt(p, k.radius)) continue;
                    const float s = 0.9f + rnd() * 0.25f;
                    spawn(bm, p - glm::vec3(0.0f, 0.03f, 0.0f), rnd() * 360.0f, glm::vec3(s), true);
                    ++i; ++scattered;
                }
            }
        }
        SUN_CORE_INFO("Mappa {0}: {1} blocchi vestiti con i prop, {2} prop sparsi", mapId, dressed, scattered);
    }
}
