// Vetrina del menu: le razze affiancate (o una sola unita', aperta dall'albero di ricerca) con il loro modo di
// muoversi (RaceMotion.h), le ossa in piu' (ExtraBones.h), vestiti e armature della dotazione base e l'arma in mano.
// Pannello per scegliere l'animazione (idle, mira, camminata, corsa, fuoco, colpi in mischia, parata, stordito,
// urlo di guerra), confrontare con lo stile di razza spento e girare la camera.
#include "GameLayer.h"
#include "Engine/ECS/CombatVFX.h"
#include "Engine/ECS/WeaponSystem.h"
#include "Engine/ECS/DamageResolver.h"
#include "Engine/ECS/VfxEvents.h"
#include <imgui.h>
#include <glm/gtx/euler_angles.hpp>
#include <cmath>
#include <algorithm>

namespace SunEngine {

    namespace {
        const char* kShowcaseAnimNames[] = { "Idle", "Mira", "Camminata", "Corsa", "Fuoco", "Colpo normale", "Colpo basso",
                                             "Colpo pesante", "Parata", "Stordito", "Urlo di guerra" };

        // Le razze della vetrina (una per corpo animato)
        const char* kShowcaseLineup[] = { "imperium_guardsman_recruit", "spacemarines_scout", "spacemarines_battle_brother",
                                          "orks_boy", "orks_gretchin", "imperium_ogryn" };

        struct RaceNote { const char* race; const char* moves; const char* bones; };
        RaceNote NoteFor(RaceMotionKind k) {
            switch (k) {
            case RaceMotionKind::Ork: return { "Ork", "Gobbo e spavaldo: spalle avanti, braccia larghe, rolla e torce il busto a ogni passo, urla in carica", "Mascella (ringhio, urlo)" };
            case RaceMotionKind::Gretchin: return { "Gretchin", "Furtivo e nervoso: accucciato, passi corti e rapidi, scatti della testa, si guarda le spalle", "Orecchie (sbattono a molla), mascella (chiacchiera)" };
            case RaceMotionKind::Astartes: return { "Space Marine", "Eretto, petto in fuori, passo pesante con affondo a ogni appoggio, immobile quando mira", "Spallacci montati sul busto" };
            case RaceMotionKind::Neophyte: return { "Scout / Aspirante", "Agile e attento: chino in avanti, passo rapido e leggero, si guarda attorno spesso", "-" };
            case RaceMotionKind::Ogryn: return { "Ogryn", "Montagna lenta: braccia larghe, gambe divaricate, ondeggia pesante, testa inclinata", "Mascella (bocca socchiusa), pancia (ballonzola)" };
            default: return { "Umano", "Soldato addestrato: dritto, passo regolare, si guarda attorno con calma", "-" };
            }
        }

        // Colore della pelle per corpo (indice di GameLayer::AnimatedBodyKind: 0 umano, 1 ork, 2 ogryn, 3 aspirante, 4 astartes, 5 gretchin)
        glm::vec3 SkinFor(int kind) {
            switch (kind) {
            case 1: return { 0.24f, 0.33f, 0.15f };
            case 5: return { 0.34f, 0.40f, 0.21f };
            case 2: return { 0.74f, 0.58f, 0.48f };
            default: return { 0.78f, 0.6f, 0.5f };
            }
        }
    }

    void GameLayer::OpenShowcasePreview(const std::string& unitId) {
        m_GameState = GameState::MainMenu;
        m_CurrentTab = MenuTab::Showcase;
        m_ShowcaseFocusUnit = unitId;
        m_ShowcaseFromResearch = true;
        m_ShowcaseZoom = 1.0f;
    }

    void GameLayer::ClearShowcase() {
        auto& reg = m_Scene.GetRegistry();
        for (auto& a : m_ShowcaseActors) {
            if (reg.valid(a.visual)) m_Scene.DestroyEntity(a.visual);
            if (reg.valid(a.weapon)) m_Scene.DestroyEntity(a.weapon);
        }
        m_ShowcaseActors.clear();
        m_ShowcaseBuiltFor = "#";
        m_ParticleSystem.Clear();
    }

    void GameLayer::BuildShowcase() {
        ClearShowcase();
        auto& reg = m_Scene.GetRegistry();
        std::vector<std::string> ids;
        if (!m_ShowcaseFocusUnit.empty()) ids.push_back(m_ShowcaseFocusUnit);
        else for (const char* id : kShowcaseLineup) ids.push_back(id);

        // Centro: dove sta il giocatore nel garage (li' c'e' il pavimento), ai piedi
        glm::vec3 base(0.0f);
        if (const auto* pt = reg.try_get<TransformComponent>(m_PlayerEntity)) {
            base = pt->Position;
            if (const auto* col = reg.try_get<ColliderComponent>(m_PlayerEntity)) base.y += (col->localCenterOffset.y - col->halfExtents.y) * pt->Scale.y;
        }
        // Larghezza di ogni posto in fila (l'ogryn ne occupa di piu')
        std::vector<float> widths;
        for (const auto& id : ids) {
            const AnimatedBodyKind k = BodyKindForUnit(id);
            widths.push_back(k == AnimatedBodyKind::Ogryn ? 2.4f : (k == AnimatedBodyKind::Gretchin ? 1.3f : 1.7f));
        }
        float total = 0.0f;
        for (float w : widths) total += w;
        float x = -total * 0.5f;

        for (size_t i = 0; i < ids.size(); ++i) {
            const std::string& id = ids[i];
            const auto* bp = BlueprintManager::Get().GetUnit(id);
            ShowcaseActor a;
            a.unitId = id;
            a.kind = BodyKindForUnit(id);
            if (!AnimatedBodyReady(a.kind)) a.kind = AnimatedBodyKind::Human;
            a.ctrl = std::make_unique<RifleAnimationController>();
            a.ctrl->ShareClipsFrom(AnimatedBodyController(a.kind));
            a.scale = (bp && bp->model_scale > 0.05f) ? bp->model_scale : 1.0f;
            a.pos = base + glm::vec3(x + widths[i] * 0.5f, 0.0f, 0.0f);
            x += widths[i];
            a.facing = 180.0f;   // guarda verso +Z, dove parte la camera
            a.motion.seed = 977u * static_cast<unsigned int>(i + 1);

            // Dotazione base: armi e vestiti
            TrainedUnit tu;
            tu.blueprintId = id;
            tu.ApplyDefaultLoadout();
            if (!bp || !bp->has_default_loadout) {   // unita' senza dotazione definita (es. ogryn): niente armi ne' armature prese a prestito
                tu.primaryWeaponId.clear(); tu.secondaryWeaponId.clear(); tu.meleeWeaponId.clear();
                for (auto& e : tu.equipmentSlots) e.clear();
                tu.armorHeadId.clear(); tu.armorTorsoId.clear(); tu.armorLeftArmId.clear(); tu.armorRightArmId.clear();
                tu.armorLeftLegId.clear(); tu.armorRightLegId.clear();
            }
            a.weaponId = !tu.primaryWeaponId.empty() ? tu.primaryWeaponId : tu.secondaryWeaponId;
            a.meleeId = tu.meleeWeaponId;

            a.visual = m_Scene.CreateEntity();
            auto& vt = reg.emplace<TransformComponent>(a.visual);
            vt.Position = a.pos;
            vt.Rotation = glm::vec3(0.0f, a.facing + m_AnimatedYawOffset, 0.0f);
            vt.AimYaw = a.facing;
            vt.Scale = glm::vec3(m_AnimatedModelScale * a.scale * AnimatedBodyModel(a.kind).unitScale);
            reg.emplace<ColorComponent>(a.visual);
            auto& sk = reg.emplace<SkinnedModelInstanceComponent>(a.visual);
            sk.sourceModel = &AnimatedBodyModel(a.kind);
            sk.boneMatrices = &a.ctrl->GetFinalBoneMatrices();
            sk.albedo = glm::vec4(SkinFor(static_cast<int>(a.kind)), 1.0f);
            // vestiti e armature: la stessa skin che avrebbe in partita
            const entt::entity holder = m_Scene.CreateEntity();
            ApplyArmorSkin(holder, tu);
            if (const auto* skin = reg.try_get<SkinComponent>(holder)) {
                for (const auto& d : skin->garments) {
                    GarmentDraw draw;
                    if (m_Garments.Get(*sk.sourceModel, d, draw)) sk.garments.push_back(draw);
                }
            }
            m_Scene.DestroyEntity(holder);

            a.weapon = m_Scene.CreateEntity();
            reg.emplace<TransformComponent>(a.weapon);
            reg.emplace<MeshComponent>(a.weapon, 0u);
            auto& wc = reg.emplace<ColorComponent>(a.weapon);
            wc.Color = wc.BaseColor = glm::vec4(0.25f, 0.25f, 0.27f, 1.0f);
            SetWeaponBlockModel(a.weapon, a.weaponId);
            m_ShowcaseActors.push_back(std::move(a));
        }
        m_ShowcaseCenter = base;
        m_ShowcaseExtent = std::max(2.5f, total);
        m_ShowcaseBuiltFor = m_ShowcaseFocusUnit;
    }

    void GameLayer::UpdateShowcaseActor(ShowcaseActor& a, float dt) {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(a.visual)) return;
        a.time += dt;
        if (auto* sk = reg.try_get<SkinnedModelInstanceComponent>(a.visual)) ApplySkinStyle(*sk, static_cast<int>(a.kind));
        const ShowcaseAnim mode = static_cast<ShowcaseAnim>(m_ShowcaseAnim);
        const bool meleeMode = mode == ShowcaseAnim::MeleeNormal || mode == ShowcaseAnim::MeleeLow || mode == ShowcaseAnim::MeleeHeavy ||
                               mode == ShowcaseAnim::Parry || mode == ShowcaseAnim::Stunned;

        // Arma in mano: da fuoco o da mischia secondo l'animazione
        const std::string handId = (meleeMode && !a.meleeId.empty()) ? a.meleeId : a.weaponId;
        SetWeaponBlockModel(a.weapon, handId);

        RifleAnimInput in;
        float speed = 0.0f;
        const float metersPerUnitSpeed = a.kind == AnimatedBodyKind::Ogryn ? 1.2f : (a.kind == AnimatedBodyKind::Gretchin ? 0.9f : 1.0f);
        switch (mode) {
        case ShowcaseAnim::Aim: in.aiming = true; break;
        case ShowcaseAnim::Walk: speed = 1.5f * metersPerUnitSpeed; in.isMoving = true; in.forward = true; break;
        case ShowcaseAnim::Run: speed = 4.2f * metersPerUnitSpeed; in.isMoving = true; in.forward = true; in.sprinting = true; break;
        case ShowcaseAnim::Fire: {
            in.aiming = true;
            a.shotTimer -= dt;
            const auto* wbp = BlueprintManager::Get().GetWeapon(a.weaponId);
            if (a.shotTimer <= 0.0f && wbp && !wbp->is_melee) {
                a.shotTimer = std::clamp(wbp->rate_of_fire > 0.0f ? 60.0f / wbp->rate_of_fire : 0.4f, 0.12f, 0.6f);
                in.firing = true; in.shotFired = true;
                // lampo e colpo dalla bocca dell'arma, dritto davanti
                const auto& wt = reg.get<TransformComponent>(a.weapon);
                glm::vec3 muzzle = wt.Position;
                if (const BlockModel* m = m_BlockModels.Find(a.weaponId); m && m->hasMuzzle) muzzle = glm::vec3(wt.GetTransform() * glm::vec4(m->muzzle, 1.0f));
                const float yr = glm::radians(a.facing);
                const glm::vec3 fwd(-std::sin(yr), 0.0f, -std::cos(yr));
                const WeaponFx fx = WeaponSystem::FxFor(*wbp);
                VfxEvent mz; mz.kind = fx == WeaponFx::Flamer ? VfxKind::FlamerJet : VfxKind::Muzzle; mz.fx = fx; mz.pos = muzzle; mz.dir = fwd;
                mz.color = wbp->primary_color; mz.scale = wbp->range_effective;
                EmitVfx(mz);
                if (fx != WeaponFx::Flamer) {
                    const entt::entity te = m_Scene.CreateEntity();
                    auto& tr = reg.emplace<TrailRequestComponent>(te);
                    tr.start = muzzle; tr.end = muzzle + fwd * 25.0f; tr.color = wbp->primary_color; tr.fx = static_cast<int>(fx);
                    tr.travelTime = 0.12f; tr.totalLife = 0.14f;
                }
            }
            break;
        }
        default: break;
        }
        in.groundSpeed = speed;

        // --- posa: stile della razza, poi l'azione ---
        a.pose.Clear();
        if (m_ShowcaseStyle) {
            RaceMotionInput rin;
            rin.dt = dt; rin.speed = speed; rin.legLen = a.legLen;
            rin.modelUnitsPerMeter = 1.0f / std::max(1e-4f, m_AnimatedModelScale * a.scale * AnimatedBodyModel(a.kind).unitScale);
            rin.aiming = in.aiming; rin.sprinting = in.sprinting; rin.yaw = a.facing;
            rin.roar = mode == ShowcaseAnim::Roar || mode == ShowcaseAnim::MeleeHeavy || (mode == ShowcaseAnim::Run && a.kind != AnimatedBodyKind::Human);
            a.ctrl->SetStyleRate(ApplyRaceMotion(a.pose, GetRaceMotionProfile(RaceMotionOf(a.kind)), a.motion, rin));
        }
        else a.ctrl->SetStyleRate(1.0f);

        // Segnali e stato della mischia sull'entita' visiva (ParticleSystem disegna cariche, parata, stelline)
        auto clearMelee = [&]() { if (reg.all_of<MeleeStateComponent>(a.visual)) reg.remove<MeleeStateComponent>(a.visual); };
        if (mode == ShowcaseAnim::MeleeNormal || mode == ShowcaseAnim::MeleeLow || mode == ShowcaseAnim::MeleeHeavy) {
            const int type = mode == ShowcaseAnim::MeleeLow ? 0 : (mode == ShowcaseAnim::MeleeHeavy ? 2 : 1);
            const MeleeAttackType atk = type == 0 ? MeleeAttackType::Light : (type == 2 ? MeleeAttackType::Heavy : MeleeAttackType::Normal);
            const std::string nodeKey = type == 0 ? "low_1" : (type == 2 ? "heavy_1" : "normal_1");
            MeleeTiming tm;
            const auto* mbp = BlueprintManager::Get().GetWeapon(handId);
            const MeleeNode* node = nullptr;
            if (mbp && mbp->hasMoveset) {
                auto it = mbp->moveset.nodes.find(nodeKey);
                if (it == mbp->moveset.nodes.end()) it = mbp->moveset.nodes.find("normal_1");
                if (it != mbp->moveset.nodes.end()) node = &it->second;
            }
            if (node) tm = DamageResolver::EffectiveTiming(*node, atk, nodeKey);
            else tm = MeleeTiming{ type == 2 ? 0.5f : 0.24f, 0.15f, type == 2 ? 0.45f : 0.25f };
            const float cycle = tm.windup + tm.active + tm.recovery + 0.7f;
            const float t = std::fmod(a.time, cycle);
            int phase = 3; float frac = 0.0f;
            if (t < tm.windup) { phase = 0; frac = t / tm.windup; }
            else if (t < tm.windup + tm.active) { phase = 1; frac = (t - tm.windup) / tm.active; }
            else if (t < tm.windup + tm.active + tm.recovery) { phase = 2; frac = (t - tm.windup - tm.active) / tm.recovery; }
            const auto* hbp = BlueprintManager::Get().GetWeapon(handId);
            if (phase <= 2) PoseLib::Melee(a.pose, type, phase, frac, hbp && hbp->hand_type == HandType::TwoHanded);
            auto& ms = reg.get_or_emplace<MeleeStateComponent>(a.visual);
            ms.currentAttack = atk; ms.currentAttackNode = nodeKey;
            ms.currentPhase = phase == 0 ? MeleePhase::Windup : (phase == 1 ? MeleePhase::Active : (phase == 2 ? MeleePhase::Recovery : MeleePhase::None));
            ms.stateTimer = phase == 0 ? t : 0.0f;
            if (phase == 1 && a.lastMeleePhase != 1) {   // scia del fendente
                TransformComponent tt;
                tt.Position = a.pos + glm::vec3(0.0f, 0.95f * a.scale, 0.0f);
                tt.AimYaw = a.facing;
                MeleeStateComponent tmp; tmp.currentAttack = atk; tmp.currentAttackNode = nodeKey;
                CombatVFX::SpawnSlashTrail(m_Scene, a.visual, tt, tmp, node);
            }
            a.lastMeleePhase = phase;
        }
        else if (mode == ShowcaseAnim::Parry) {
            const float t = std::fmod(a.time, 1.6f);
            const float amount = t < 0.1f ? t / 0.1f : (t < 1.0f ? 1.0f : std::max(0.0f, 1.0f - (t - 1.0f) / 0.25f));
            PoseLib::Parry(a.pose, amount);
            auto& ms = reg.get_or_emplace<MeleeStateComponent>(a.visual);
            ms.currentAttack = MeleeAttackType::Parry;
            ms.currentPhase = amount > 0.05f ? MeleePhase::Active : MeleePhase::None;
            ms.stateTimer = t;
        }
        else if (mode == ShowcaseAnim::Stunned) {
            PoseLib::Stunned(a.pose, 1.0f);
            auto& ms = reg.get_or_emplace<MeleeStateComponent>(a.visual);
            ms.currentPhase = MeleePhase::Stunned; ms.stunTimer = a.time;
        }
        else clearMelee();
        if (mode == ShowcaseAnim::Roar) {   // urlo di guerra: petto in fuori, braccia aperte e alzate, testa indietro
            const float k = PoseLib::Smooth(std::min(1.0f, std::fmod(a.time, 3.0f) / 0.4f)) * (std::fmod(a.time, 3.0f) < 2.4f ? 1.0f : 0.0f);
            a.pose.Rotate("Spine1", PoseLib::Rx(-10.0f * k));
            a.pose.Rotate("Spine2", PoseLib::Rx(-8.0f * k));
            a.pose.Rotate("Head", PoseLib::Rx(-18.0f * k));
            a.pose.Rotate("LeftArm", PoseLib::Rz(40.0f * k) * PoseLib::Rx(-20.0f * k));
            a.pose.Rotate("RightArm", PoseLib::Rz(-40.0f * k) * PoseLib::Rx(-20.0f * k));
        }

        a.ctrl->SetProceduralPose(&a.pose);
        a.ctrl->Update(dt, in);

        // Lunghezza della gamba per il molleggio (dalla posa appena calcolata)
        {
            glm::mat4 A, K, F;
            if (a.ctrl->GetBoneModelTransform("mixamorig1:LeftUpLeg", A) && a.ctrl->GetBoneModelTransform("mixamorig1:LeftLeg", K) &&
                a.ctrl->GetBoneModelTransform("mixamorig1:LeftFoot", F))
                a.legLen = glm::distance(glm::vec3(A[3]), glm::vec3(K[3])) + glm::distance(glm::vec3(K[3]), glm::vec3(F[3]));
        }

        auto& vt = reg.get<TransformComponent>(a.visual);
        vt.Position = a.pos;
        vt.Rotation = glm::vec3(0.0f, a.facing + m_AnimatedYawOffset, 0.0f);
        vt.AimYaw = a.facing;
        vt.Scale = glm::vec3(m_AnimatedModelScale * a.scale * AnimatedBodyModel(a.kind).unitScale);

        // Arma fra le mani, puntata davanti; poi le mani sulle impugnature
        auto& wt = reg.get<TransformComponent>(a.weapon);
        const BlockModel* model = m_BlockModels.Find(handId);
        glm::mat4 boneR(1.0f), boneL(1.0f);
        if (!model || !a.ctrl->GetBoneModelTransform("mixamorig1:RightHand", boneR)) { wt.Scale = glm::vec3(0.0f); return; }
        const glm::vec3 handR = glm::vec3((vt.GetTransform() * boneR)[3]);
        const float yr = glm::radians(a.facing);
        glm::vec3 aimDir(-std::sin(yr), 0.0f, -std::cos(yr));
        glm::vec3 anchorWorld = handR, anchorLocal = model->hasGripRight ? model->gripRight : glm::vec3(0.0f);
        const bool twoHands = !meleeMode && model->hasGripRight && model->hasGripLeft && a.ctrl->GetBoneModelTransform("mixamorig1:LeftHand", boneL);
        if (twoHands) {
            anchorWorld = (handR + glm::vec3((vt.GetTransform() * boneL)[3])) * 0.5f;
            anchorLocal = (model->gripRight + model->gripLeft) * 0.5f;
        }
        else if (meleeMode) {   // arma bianca: segue l'avambraccio (dal polso verso la mano)
            glm::mat4 fore(1.0f);
            if (a.ctrl->GetBoneModelTransform("mixamorig1:RightForeArm", fore)) {
                const glm::vec3 wrist = glm::vec3((vt.GetTransform() * fore)[3]);
                if (glm::length(handR - wrist) > 1e-3f) aimDir = glm::normalize(handR - wrist);
            }
        }
        const glm::vec3 zAxis = -aimDir;
        glm::vec3 xAxis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), zAxis);
        if (glm::length(xAxis) < 1e-3f) xAxis = glm::vec3(1.0f, 0.0f, 0.0f);
        xAxis = glm::normalize(xAxis);
        const glm::vec3 yAxis = glm::cross(zAxis, xAxis);
        glm::mat4 basis(1.0f);
        basis[0] = glm::vec4(xAxis, 0.0f); basis[1] = glm::vec4(yAxis, 0.0f); basis[2] = glm::vec4(zAxis, 0.0f);
        wt.Position = anchorWorld - glm::vec3(basis * glm::vec4(anchorLocal, 0.0f));
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
        glm::extractEulerAngleYXZ(basis, yaw, pitch, roll);
        wt.Rotation = glm::degrees(glm::vec3(pitch, yaw, roll));
        wt.Scale = glm::vec3(1.0f);
        if (!meleeMode && model->hasGripRight) {
            const glm::vec3 gR = wt.Position + glm::vec3(basis * glm::vec4(model->gripRight, 0.0f));
            const glm::vec3 gL = wt.Position + glm::vec3(basis * glm::vec4(model->gripLeft, 0.0f));
            ApplyHandIK(*a.ctrl, a.pose, vt, &gR, twoHands ? &gL : nullptr);
        }
    }

    void GameLayer::UpdateShowcase(float deltaTime) {
        auto& reg = m_Scene.GetRegistry();
        if (m_ShowcaseBuiltFor != m_ShowcaseFocusUnit || m_ShowcaseActors.empty()) BuildShowcase();

        // Il giocatore del garage sparisce mentre c'e' la vetrina
        for (entt::entity e : { m_AnimatedPlayerEntity, m_WeaponEntity, m_LeftWeaponEntity })
            if (reg.valid(e)) if (auto* t = reg.try_get<TransformComponent>(e)) t->Scale = glm::vec3(0.0f);

        // Animazioni in sequenza (per la presentazione)
        if (m_ShowcaseCycle) {
            m_ShowcaseCycleTimer += deltaTime;
            if (m_ShowcaseCycleTimer >= 4.0f) {
                m_ShowcaseCycleTimer = 0.0f;
                m_ShowcaseAnim = (m_ShowcaseAnim + 1) % static_cast<int>(ShowcaseAnim::Count);
            }
        }

        // Camera: gira da sola o col mouse (destro trascinato), rotellina per avvicinarsi
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantCaptureMouse) {
            if (Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT) || Input::IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)) {
                const glm::vec2 d = Input::GetMouseDelta();
                m_ShowcaseYaw -= d.x * 0.3f;
                m_ShowcasePitch = std::clamp(m_ShowcasePitch - d.y * 0.2f, -40.0f, 15.0f);
                m_ShowcaseAutoRotate = false;
            }
            if (io.MouseWheel != 0.0f) m_ShowcaseZoom = std::clamp(m_ShowcaseZoom * (1.0f - io.MouseWheel * 0.1f), 0.3f, 2.5f);
        }
        if (m_ShowcaseAutoRotate) m_ShowcaseYaw += deltaTime * 10.0f;
        const bool single = m_ShowcaseActors.size() == 1;
        float height = 1.9f;
        if (single) height = m_ShowcaseActors[0].kind == AnimatedBodyKind::Ogryn ? 2.8f : (m_ShowcaseActors[0].kind == AnimatedBodyKind::Gretchin ? 1.0f : 2.1f);
        const float dist = (single ? height * 1.6f : m_ShowcaseExtent * 0.75f + 1.5f) * m_ShowcaseZoom;
        m_Camera.SetViewportSize(m_WindowWidth, m_WindowHeight);
        m_Camera.SetCameraOffsets(0.0f, 0.0f, dist);
        m_Camera.SetYawOffset(0.0f);
        m_Camera.SetTarget(m_ShowcaseCenter + glm::vec3(0.0f, single ? height * 0.55f : 1.1f, 0.0f), m_ShowcaseYaw);
        m_Camera.SetPitch(m_ShowcasePitch);
        m_Camera.Update(glm::vec3(0.0f), 1.0f);
        Renderer::SetViewProjection(m_Camera.GetViewProjection());

        for (auto& a : m_ShowcaseActors) UpdateShowcaseActor(a, deltaTime);
        m_ParticleSystem.Update(m_Scene, deltaTime);
    }

    void GameLayer::RenderShowcaseUI() {
        ImGuiIO& io = ImGui::GetIO();
        auto& reg = m_Scene.GetRegistry();
        const bool single = !m_ShowcaseFocusUnit.empty();

        ImGui::SetNextWindowPos(ImVec2(20.0f, 80.0f));
        ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("VETRINA", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
        if (single) {
            const auto* bp = BlueprintManager::Get().GetUnit(m_ShowcaseFocusUnit);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", bp ? bp->displayName.c_str() : m_ShowcaseFocusUnit.c_str());
            if (m_ShowcaseFromResearch && ImGui::Button("< Torna alla ricerca", ImVec2(-1, 30))) {
                m_CurrentTab = MenuTab::Research;
                m_ShowcaseFocusUnit.clear();
                m_ShowcaseFromResearch = false;
            }
            if (ImGui::Button("Tutte le razze", ImVec2(-1, 26))) { m_ShowcaseFocusUnit.clear(); m_ShowcaseFromResearch = false; }
        }
        else ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Le razze di Project Trazyn");
        ImGui::Separator();
        ImGui::TextDisabled("Animazione");
        for (int i = 0; i < static_cast<int>(ShowcaseAnim::Count); ++i) {
            if (ImGui::RadioButton(kShowcaseAnimNames[i], m_ShowcaseAnim == i)) { m_ShowcaseAnim = i; m_ShowcaseCycle = false; }
            if (i % 2 == 0 && i + 1 < static_cast<int>(ShowcaseAnim::Count)) ImGui::SameLine(160.0f);
        }
        ImGui::Separator();
        ImGui::Checkbox("Tutte in sequenza (4 s l'una)", &m_ShowcaseCycle);
        ImGui::Checkbox("Stile di movimento della razza", &m_ShowcaseStyle);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Spento: tutte le razze usano le stesse clip, senza postura, passo e ossa in piu'");
        ImGui::Checkbox("Rotazione automatica", &m_ShowcaseAutoRotate);
        ImGui::TextDisabled("Trascina col mouse per girare, rotellina per lo zoom");
        ImGui::Separator();
        // Note di razza
        std::vector<RaceMotionKind> shown;
        for (const auto& a : m_ShowcaseActors) {
            const RaceMotionKind k = RaceMotionOf(a.kind);
            if (std::find(shown.begin(), shown.end(), k) != shown.end()) continue;
            shown.push_back(k);
            const RaceNote n = NoteFor(k);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", n.race);
            ImGui::PushTextWrapPos(310.0f);
            ImGui::TextWrapped("%s", n.moves);
            if (std::string(n.bones) != "-") ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Ossa in piu': %s", n.bones);
            ImGui::PopTextWrapPos();
        }
        ImGui::End();

        // Nomi sopra le unita'
        const glm::mat4 vp = m_Camera.GetViewProjection();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        for (const auto& a : m_ShowcaseActors) {
            const auto* bp = BlueprintManager::Get().GetUnit(a.unitId);
            const float h = a.kind == AnimatedBodyKind::Ogryn ? 2.9f : (a.kind == AnimatedBodyKind::Gretchin ? 1.15f : 2.15f);
            const glm::vec4 clip = vp * glm::vec4(a.pos + glm::vec3(0.0f, h, 0.0f), 1.0f);
            if (clip.w <= 0.05f) continue;
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            const ImVec2 sp((ndc.x * 0.5f + 0.5f) * io.DisplaySize.x, (1.0f - (ndc.y * 0.5f + 0.5f)) * io.DisplaySize.y);
            const char* name = bp ? bp->displayName.c_str() : a.unitId.c_str();
            const ImVec2 ts = ImGui::CalcTextSize(name);
            dl->AddText(ImVec2(sp.x - ts.x * 0.5f + 1, sp.y + 1), IM_COL32(0, 0, 0, 200), name);
            dl->AddText(ImVec2(sp.x - ts.x * 0.5f, sp.y), IM_COL32(255, 230, 160, 255), name);
        }
        (void)reg;
    }

}
