// Armi e oggetti sul corpo: niente compenetrazioni, tutto quello che non e' in mano resta visibile addosso
// (fucile sulla schiena, pistola nella fondina, arma bianca sul fianco, granate e attrezzi alla cintura) e
// sfoderare costa tempo, tanto piu' quanto l'arma pesa e quanto e' scomoda da raggiungere. Vedi BodyGear.h.
#include "GameLayer.h"
#include "Engine/Renderer/BodyGear.h"
#include <glm/gtx/euler_angles.hpp>

namespace SunEngine {

    const BodyGearInfo& GameLayer::BodyGearOf(const SkinnedModel& model) {
        auto it = m_BodyGearCache.find(&model);
        if (it != m_BodyGearCache.end()) return it->second;
        BodyGearInfo g = ComputeBodyGear(model);
        if (g.valid) {
            const auto& s = g.segs;
            const float k = 100.0f / std::max(1e-4f, g.height);   // in percento dell'altezza del corpo
            SUN_CORE_INFO("Corpo: ingombro misurato ({0} tratti; petto largo {1:.0f}%, davanti {2:.0f}%, dietro {3:.0f}% dell'altezza)", s.size(),
                s.size() > 2 ? s[2].rSide * 2.0f * k : 0.0f, s.size() > 2 ? s[2].rFront * k : 0.0f, s.size() > 2 ? s[2].rBack * k : 0.0f);
        }
        return m_BodyGearCache.emplace(&model, std::move(g)).first->second;
    }

    // Ellissi del corpo portate nel mondo con la posa del fotogramma
    void GameLayer::BodyGearWorldSegs(const SkinnedModel& model, RifleAnimationController& ctrl, const glm::mat4& visual,
                                      std::vector<BodyGearWorldSeg>& out) {
        out.clear();
        const BodyGearInfo& g = BodyGearOf(model);
        if (!g.valid) return;
        const float scale = glm::length(glm::vec3(visual[0]));
        for (const auto& s : g.segs) {
            glm::mat4 ga(1.0f), gb(1.0f), fa(1.0f);
            if (!ctrl.GetBoneModelTransform(s.a, ga) || !ctrl.GetBoneModelTransform(s.b, gb)) continue;
            if (!BodyGearBoneFinal(model, ga, s.a, fa)) continue;
            BodyGearWorldSeg w;
            w.A = glm::vec3((visual * ga)[3]);
            w.B = glm::vec3((visual * gb)[3]);
            if (s.tailExtra > 0.0f && glm::length(w.B - w.A) > 1e-5f) w.B += glm::normalize(w.B - w.A) * (s.tailExtra * scale);
            const glm::mat3 R(visual * fa);
            w.right = glm::normalize(R * g.right);
            w.fwd = glm::normalize(R * g.fwd);
            w.rSide = s.rSide * scale; w.rFront = s.rFront * scale; w.rBack = s.rBack * scale;
            if (s.round) w.rFront = w.rBack = w.rSide;
            out.push_back(w);
        }
    }

    // Punti di controllo di un oggetto: gli spigoli dei suoi blocchi (un po' rientrati) e i loro centri
    const std::vector<glm::vec3>& GameLayer::GearSamplePoints(const BlockModel& m) {
        auto it = m_GearSamples.find(&m);
        if (it != m_GearSamples.end()) return it->second;
        std::vector<glm::vec3> pts;
        for (const auto& p : m.parts) {
            if (p.name.find("rust") != std::string::npos) continue;
            const glm::mat4 pm = BlockPartMatrix(glm::mat4(1.0f), p);
            pts.push_back(glm::vec3(pm[3]));
            for (int c = 0; c < 8; ++c)
                pts.push_back(glm::vec3(pm * glm::vec4((c & 1) ? 0.4f : -0.4f, (c & 2) ? 0.4f : -0.4f, (c & 4) ? 0.4f : -0.4f, 1.0f)));
        }
        return m_GearSamples.emplace(&m, std::move(pts)).first->second;
    }

    glm::vec3 GameLayer::KeepOutOfBody(const SkinnedModel& body, RifleAnimationController& ctrl, const glm::mat4& visual,
                                       const BlockModel& item, const glm::mat4& itemWorld) {
        BodyGearWorldSegs(body, ctrl, visual, m_GearSegsScratch);
        if (m_GearSegsScratch.empty()) return glm::vec3(0.0f);
        const auto& local = GearSamplePoints(item);
        m_GearPtsScratch.resize(local.size());
        for (size_t i = 0; i < local.size(); ++i) m_GearPtsScratch[i] = glm::vec3(itemWorld * glm::vec4(local[i], 1.0f));
        const float scale = glm::length(glm::vec3(visual[0]));
        return BodyGearPushOut(m_GearSegsScratch, m_GearPtsScratch, 0.006f * scale);
    }

    // Posto di un oggetto sul corpo: rotazione e posizione dell'origine del suo modello nel mondo
    bool GameLayer::GearMountWorld(const SkinnedModel& body, RifleAnimationController& ctrl, const glm::mat4& visual,
                                   GearMount mount, const BlockModel& item, glm::mat4& out) {
        const BodyGearInfo& g = BodyGearOf(body);
        if (!g.valid) return false;
        const auto& mt = g.mounts[static_cast<int>(mount)];
        if (!mt.ok) return false;
        glm::mat4 gb(1.0f), fb(1.0f);
        if (!ctrl.GetBoneModelTransform(mt.bone, gb) || !BodyGearBoneFinal(body, gb, mt.bone, fb)) return false;
        const glm::mat4 W = visual * fb;
        const float scale = glm::length(glm::vec3(visual[0]));
        const glm::mat3 R(W);
        const glm::vec3 p = glm::vec3(W * glm::vec4(mt.point, 1.0f));
        const glm::vec3 outDir = glm::normalize(R * mt.out);
        glm::vec3 along = glm::normalize(R * mt.along);
        glm::vec3 top = R * mt.top;
        if (mount == GearMount::Back || mount == GearMount::BackCross) {
            // di traverso sulla schiena rispetto al mondo, non alla curva della schiena: anche sui corpi gobbi la
            // canna sale verso la spalla, e l'arma resta di piatto sulla schiena
            const glm::vec3 rightW = glm::normalize(R * g.right);
            along = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) * 0.82f + rightW * (mount == GearMount::Back ? -0.55f : 0.55f));
            along = glm::normalize(along - outDir * glm::dot(along, outDir));
            top = glm::cross(along, outDir) * (mount == GearMount::Back ? 1.0f : -1.0f);
        }
        top = glm::normalize(top - along * glm::dot(top, along));
        // base dell'oggetto: canna/lama (-Z) lungo 'along', alto (+Y) verso 'top'
        const glm::vec3 z = -along, y = top, x = glm::normalize(glm::cross(y, z));
        glm::mat4 basis(1.0f);
        basis[0] = glm::vec4(x, 0.0f); basis[1] = glm::vec4(y, 0.0f); basis[2] = glm::vec4(z, 0.0f);
        // mezzo spessore dell'oggetto verso fuori: si appoggia sulla pelle
        const glm::vec3 half = (item.boundsMax - item.boundsMin) * 0.5f;
        const glm::vec3 centre = (item.boundsMax + item.boundsMin) * 0.5f;
        const float thick = std::abs(glm::dot(outDir, x)) * half.x + std::abs(glm::dot(outDir, y)) * half.y + std::abs(glm::dot(outDir, z)) * half.z;
        const glm::vec3 target = p + outDir * (mt.surface * scale + thick + 0.008f * scale);
        out = basis;
        out[3] = glm::vec4(target - glm::vec3(basis * glm::vec4(centre, 0.0f)), 1.0f);
        // niente compenetrazioni con il resto del corpo (gobbe, pance, teste)
        out[3] += glm::vec4(KeepOutOfBody(body, ctrl, visual, item, out), 0.0f);
        return true;
    }

    // Dove va appeso un oggetto
    GearMount GameLayer::DefaultMountOf(const std::string& id, bool& show) {
        show = false;
        const BlockModel* m = m_BlockModels.Find(id);
        const WeaponBlueprint* bp = BlueprintManager::Get().GetWeapon(id);
        if (!m || !bp) return GearMount::Count;
        const glm::vec3 ext = m->boundsMax - m->boundsMin;
        const float len = std::max(ext.x, std::max(ext.y, ext.z));
        if (len > 1.3f) return GearMount::Count;                        // armi enormi: non si portano addosso
        if (bp->is_equipment) {
            // granate e piccoli attrezzi alla cintura, casse e zaini sulla schiena; muri, barricate e stendardi no
            if (len <= 0.17f) { show = true; return GearMount::BeltFrontRight; }
            if (len <= 0.6f) { show = true; return GearMount::Pack; }
            return GearMount::Count;
        }
        show = true;
        if (bp->is_melee) return (bp->hand_type == HandType::TwoHanded || len > 0.75f) ? GearMount::BackCross : GearMount::HipLeft;
        if (bp->hand_type == HandType::OneHanded && len < 0.45f) return GearMount::HolsterRight;
        return GearMount::Back;
    }

    // Quello che si vede addosso: tutto tranne cio' che e' in mano. ids: primaria, secondaria, mischia, granata, 3 attrezzi
    std::vector<GameLayer::GearItem> GameLayer::GearItemsFor(const std::string* ids, int count, const std::string& heldR, const std::string& heldL) {
        std::vector<GearItem> items;
        bool used[static_cast<int>(GearMount::Count)] = {};
        static const GearMount belts[] = { GearMount::BeltFrontLeft, GearMount::BeltFrontRight, GearMount::BeltBackLeft, GearMount::BeltBackRight };
        auto place = [&](const std::string& id, GearMount want) {
            auto freeOf = [&](GearMount m) { return !used[static_cast<int>(m)]; };
            if (want == GearMount::Back && !freeOf(GearMount::Back)) want = GearMount::BackCross;
            if (want == GearMount::BackCross && !freeOf(GearMount::BackCross)) want = freeOf(GearMount::Back) ? GearMount::Back : GearMount::Count;
            if (want == GearMount::HolsterRight && !freeOf(want)) want = GearMount::BeltBackRight;
            if (want == GearMount::HipLeft && !freeOf(want)) want = GearMount::BackCross;
            if (want == GearMount::BeltFrontRight || want == GearMount::BeltFrontLeft || want == GearMount::BeltBackLeft || want == GearMount::BeltBackRight) {
                if (!freeOf(want)) { want = GearMount::Count; for (GearMount b : belts) if (freeOf(b)) { want = b; break; } }
            }
            if (want == GearMount::Pack && !freeOf(want)) want = GearMount::Count;
            if (want == GearMount::Count || !freeOf(want)) return;
            used[static_cast<int>(want)] = true;
            items.push_back({ id, want });
        };
        bool heldOnce[8] = {};
        for (int i = 0; i < count; ++i) {
            const std::string& id = ids[i];
            if (id.empty()) continue;
            if ((id == heldR || id == heldL)) {                        // in mano: solo la prima copia (due pistole uguali: una resta)
                bool skip = false;
                for (int j = 0; j < i; ++j) if (ids[j] == id) skip = true;
                if (!skip) continue;
            }
            bool show = false;
            GearMount m = DefaultMountOf(id, show);
            if (!show) continue;
            if (i == 3) m = GearMount::BeltFrontLeft;                  // granata: davanti a sinistra
            place(id, m);
        }
        (void)heldOnce;
        return items;
    }

    // Tiene in pari le entita' degli oggetti appesi e le mette al loro posto
    void GameLayer::UpdateGearRack(GearRack& rack, const SkinnedModel* body, RifleAnimationController* ctrl, const TransformComponent& visualT,
                                   const std::vector<GearItem>& items, bool visible) {
        auto& reg = m_Scene.GetRegistry();
        while (rack.ents.size() < items.size()) {
            const entt::entity e = m_Scene.CreateEntity();
            reg.emplace<TransformComponent>(e);
            reg.emplace<MeshComponent>(e, 0u);
            reg.emplace<ColorComponent>(e);
            rack.ents.push_back(e);
        }
        const glm::mat4 visual = visualT.GetTransform();
        for (size_t i = 0; i < rack.ents.size(); ++i) {
            const entt::entity e = rack.ents[i];
            if (!reg.valid(e)) continue;
            auto& t = reg.get<TransformComponent>(e);
            const BlockModel* m = i < items.size() ? m_BlockModels.Find(items[i].id) : nullptr;
            glm::mat4 w(1.0f);
            if (!visible || !m || !body || !ctrl || visualT.Scale.x < 1e-4f || !GearMountWorld(*body, *ctrl, visual, items[i].mount, *m, w)) {
                t.Scale = glm::vec3(0.0f);
                continue;
            }
            SetWeaponBlockModel(e, items[i].id);
            float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
            glm::extractEulerAngleYXZ(glm::mat4(glm::mat3(w)), yaw, pitch, roll);
            t.Position = glm::vec3(w[3]);
            t.Rotation = glm::degrees(glm::vec3(pitch, yaw, roll));
            t.Scale = glm::vec3(1.0f);   // misure reali, come le armi in mano
        }
    }

    void GameLayer::DestroyGearRack(GearRack& rack) {
        auto& reg = m_Scene.GetRegistry();
        for (auto e : rack.ents) if (reg.valid(e)) m_Scene.DestroyEntity(e);
        rack.ents.clear();
    }

    // Tempo per sfoderare: da dove sta l'arma (fondina rapida, schiena lenta) e da quanto pesa; i forti fanno prima
    float GameLayer::DrawTimeOf(const std::string& id, float strength) {
        const WeaponBlueprint* bp = BlueprintManager::Get().GetWeapon(id);
        if (!bp) return 0.3f;
        bool show = false;
        const GearMount m = DefaultMountOf(id, show);
        float base = 0.45f;
        switch (m) {
        case GearMount::HolsterRight: base = 0.32f; break;
        case GearMount::HipLeft: base = 0.38f; break;
        case GearMount::Back: case GearMount::BackCross: base = 0.62f; break;
        case GearMount::Pack: base = 0.8f; break;
        default: base = bp->is_equipment ? 0.4f : 0.45f; break;
        }
        const float w = std::max(0.0f, bp->weight_kg);
        const float strong = std::clamp(1.18f - strength / 280.0f, 0.8f, 1.18f);
        return (base + 0.075f * w) * strong;
    }

    // Sfoderare: l'arma parte dal suo posto sul corpo e arriva in mano. s = avanzamento (0 sul corpo, 1 in mano)
    void GameLayer::BlendFromMount(const SkinnedModel& body, RifleAnimationController& ctrl, const TransformComponent& visualT,
                                   const BlockModel& item, float s, TransformComponent& weaponT) {
        if (s >= 0.999f) return;
        bool show = false;
        const GearMount m = DefaultMountOf(item.id, show);
        if (!show) return;
        glm::mat4 from(1.0f);
        if (!GearMountWorld(body, ctrl, visualT.GetTransform(), m, item, from)) return;
        const float k = s * s * (3.0f - 2.0f * s);
        const glm::mat4 to = weaponT.GetTransform();
        const glm::quat qa = glm::quat_cast(glm::mat3(from)), qb = glm::quat_cast(glm::mat3(glm::scale(to, glm::vec3(1.0f) / glm::max(weaponT.Scale, glm::vec3(1e-4f)))));
        const glm::quat q = glm::slerp(qa, qb, k);
        // a meta' strada l'arma passa un poco fuori dal corpo (arco del braccio)
        const glm::vec3 pa = glm::vec3(from[3]), pb = weaponT.Position;
        const glm::vec3 mid = (pa + pb) * 0.5f + glm::normalize(glm::vec3(visualT.GetTransform() * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f))) * 0.12f;
        const glm::vec3 p = glm::mix(glm::mix(pa, mid, k), glm::mix(mid, pb, k), k);
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
        glm::extractEulerAngleYXZ(glm::mat4(q), yaw, pitch, roll);
        weaponT.Position = p;
        weaponT.Rotation = glm::degrees(glm::vec3(pitch, yaw, roll));
    }
}
