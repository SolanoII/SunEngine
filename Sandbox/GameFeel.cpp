#include "GameFeel.h"
#include "Engine/ECS/Components.h"
#include "Engine/Core/Random.h"
#include "Engine/Renderer/Pipeline/DeferredRenderer.h"
#include "Engine/Renderer/Pipeline/Light.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace SunEngine {

    namespace {
        // Come si sente ogni tipo di arma. L'ordine e' quello di WeaponFx.
        struct ShotFeel {
            const char* file;
            float volume;       // volume dello strato vicino
            float range;        // distanza a cui lo strato vicino sparisce (m)
            float trauma;       // scossone a chi spara
            float fov;          // pugno di FOV a chi spara (gradi)
            int tail;           // coda lontana: 0 nessuna, 1 leggera, 2 pesante, 3 energia
            glm::vec3 light;    // colore del lampo (x < 0 = colore dell'arma)
            float lightRange, lightIntensity;
            int casing;         // 0 nessuno, 1 bossolo, 2 bossolo grosso
        };
        const ShotFeel k_Shot[12] = {
            { "shot_laser",     0.80f,  70.0f, 0.16f, 0.5f, 3, { -1.0f, 0.0f, 0.0f },   5.0f, 3.5f, 0 },   // Laser
            { "shot_hotshot",   0.95f,  80.0f, 0.24f, 0.9f, 3, { -1.0f, 0.0f, 0.0f },   6.5f, 5.0f, 0 },   // HotShot
            { "shot_longlas",   1.10f, 110.0f, 0.48f, 2.0f, 3, { -1.0f, 0.0f, 0.0f },   9.0f, 7.0f, 0 },   // LongLas
            { "shot_ballistic", 0.90f,  85.0f, 0.22f, 0.7f, 1, { 1.0f, 0.72f, 0.38f },  6.0f, 4.5f, 1 },   // Ballistic
            { "shot_shotgun",   1.15f,  95.0f, 0.52f, 2.2f, 2, { 1.0f, 0.68f, 0.32f },  8.0f, 6.5f, 2 },   // Shotgun
            { "shot_sniper",    1.20f, 130.0f, 0.55f, 2.4f, 2, { 1.0f, 0.75f, 0.45f },  8.0f, 6.0f, 2 },   // Sniper
            { "shot_bolt",      1.20f, 110.0f, 0.40f, 1.5f, 2, { 1.0f, 0.62f, 0.25f },  8.5f, 7.0f, 2 },   // Bolt
            { "shot_plasma",    1.00f,  90.0f, 0.40f, 1.6f, 3, { 0.45f, 0.75f, 1.0f },  9.0f, 7.5f, 0 },   // Plasma
            { "shot_melta",     0.80f,  60.0f, 0.12f, 0.2f, 0, { 1.0f, 0.5f, 0.15f },   7.0f, 5.0f, 0 },   // Melta
            { "shot_flamer",    0.75f,  55.0f, 0.08f, 0.1f, 0, { 1.0f, 0.55f, 0.2f },   7.0f, 4.0f, 0 },   // Flamer
            { "shot_launcher",  1.00f,  80.0f, 0.45f, 1.6f, 2, { 1.0f, 0.7f, 0.4f },    6.0f, 4.0f, 0 },   // Explosive
            { "shot_grav",      1.00f,  80.0f, 0.36f, 1.4f, 3, { 0.65f, 0.4f, 1.0f },   8.0f, 6.0f, 0 },   // Grav
        };
        bool IsEnergy(WeaponFx fx) {
            return fx == WeaponFx::Laser || fx == WeaponFx::HotShot || fx == WeaponFx::LongLas || fx == WeaponFx::Plasma ||
                   fx == WeaponFx::Melta || fx == WeaponFx::Grav;
        }
        float Smooth01(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
        const float kSoundSpeed = 340.0f;
        entt::entity Ent(std::uint32_t id) { return static_cast<entt::entity>(id); }
    }

    const FeelProfile& GameFeel::ProfileOf(int body) {
        //                               preso  dato  tono   vol   tonfo scossa arma   stop   suss.  forza
        static const FeelProfile k[6] = {
            /* Human    */ { 1.00f, 1.00f, 1.00f, 1.00f, 0.00f, 0.00f, 1.00f, 1.00f, 1.00f, 1.00f },
            /* Ork      */ { 0.75f, 0.90f, 0.86f, 1.05f, 0.40f, 0.00f, 0.95f, 1.25f, 0.60f, 1.15f },
            /* Ogryn    */ { 0.50f, 0.60f, 0.72f, 1.10f, 0.90f, 0.45f, 0.90f, 1.50f, 0.35f, 1.30f },
            /* Neophyte */ { 0.80f, 0.85f, 0.92f, 1.00f, 0.35f, 0.00f, 0.96f, 1.10f, 0.70f, 1.10f },
            /* Astartes */ { 0.40f, 0.55f, 0.78f, 1.10f, 1.00f, 0.35f, 0.90f, 1.60f, 0.25f, 1.40f },
            /* Gretchin */ { 1.90f, 1.50f, 1.45f, 0.60f, 0.00f, 0.00f, 1.12f, 0.60f, 1.80f, 0.80f },
        };
        return k[(body >= 0 && body < 6) ? body : 0];
    }

    void GameFeel::Init(ma_engine* engine, ma_sound_group* sfxGroup, const std::string& audioDir) {
        std::string dir = audioDir.empty() ? std::string("GameData/Audio/") : audioDir;
        if (dir.back() != '/' && dir.back() != '\\') dir += "/";
        m_Bank.Init(engine, sfxGroup, dir + "Feel/");
        if (!engine) return;
        for (int i = 0; i < 12; ++i) m_ShotSnd[i] = m_Bank.Load(k_Shot[i].file, 4);
        m_TailLight = m_Bank.Load("tail_light", 4); m_TailHeavy = m_Bank.Load("tail_heavy", 4); m_TailEnergy = m_Bank.Load("tail_energy", 4);
        m_Whiz = m_Bank.Load("whiz", 2); m_WhizSnap = m_Bank.Load("whiz_snap", 2); m_WhizEnergy = m_Bank.Load("whiz_energy", 2);
        m_ImpFlesh = m_Bank.Load("imp_flesh", 2); m_ImpArmor = m_Bank.Load("imp_armor", 2); m_ImpHard = m_Bank.Load("imp_hard", 2);
        m_ImpDirt = m_Bank.Load("imp_dirt", 2); m_ImpEnergy = m_Bank.Load("imp_energy", 2);
        m_Expl = m_Bank.Load("expl", 2); m_ExplFar = m_Bank.Load("expl_far", 3); m_ExplSmall = m_Bank.Load("expl_small", 3);
        m_SwingLight = m_Bank.Load("swing_light", 3); m_SwingMid = m_Bank.Load("swing_mid", 3); m_SwingHeavy = m_Bank.Load("swing_heavy", 3);
        m_MeleeHit = m_Bank.Load("melee_hit", 2); m_MeleeHeavy = m_Bank.Load("melee_hit_heavy", 3); m_Chain = m_Bank.Load("chain_rev", 3);
        m_Parry = m_Bank.Load("parry", 3); m_GuardBreak = m_Bank.Load("guard_break", 2);
        m_HitTick = m_Bank.Load("hit_tick", 4); m_HitArmorTick = m_Bank.Load("hit_armor_tick", 4); m_Kill = m_Bank.Load("kill_confirm", 2); m_Hurt = m_Bank.Load("hurt", 2);
        m_CasingSnd = m_Bank.Load("casing", 2); m_CasingHeavy = m_Bank.Load("casing_heavy", 2);
        const char* steps[4] = { "../Footsteps/step_concrete", "../Footsteps/step_dirt", "../Footsteps/step_metal", "../Footsteps/step_wood" };
        for (int i = 0; i < 4; ++i) m_StepSnd[i] = m_Bank.Load(steps[i], 5);
        m_StepHeavy = m_Bank.Load("step_heavy", 3); m_StepLight = m_Bank.Load("step_light", 3); m_Land = m_Bank.Load("land", 2);

        // Senza i file (cartella Feel mancante) restano i suoni di prima del motore
        m_AudioReady = m_Bank.Has(m_ShotSnd[static_cast<int>(WeaponFx::Laser)]) && m_Bank.Has(m_ShotSnd[static_cast<int>(WeaponFx::Ballistic)]) &&
                       m_Bank.Has(m_StepSnd[0]);
        FeelAudioActive() = m_AudioReady;
    }

    void GameFeel::Shutdown() {
        FeelAudioActive() = false;
        m_AudioReady = false;
        m_Bank.Shutdown();
    }

    void GameFeel::Reset() {
        m_Pending.clear(); m_Lights.clear(); m_Casings.clear(); m_Marks.clear(); m_Flinch.clear(); m_LoopCooldown.clear();
        m_Trauma = m_FovPunch = m_KickPitch = m_KickYaw = m_Dip = m_HitStop = 0.0f;
        m_HavePrev = false;
        FeelEventQueue().clear();
    }

    void GameFeel::AddLight(const glm::vec3& pos, const glm::vec3& color, float range, float intensity, float life) {
        if (glm::distance(pos, m_CamPos) > 90.0f) return;
        const FlashLight l{ pos, color, range, intensity, life, life };
        if (m_Lights.size() < 20) { m_Lights.push_back(l); return; }
        auto it = std::min_element(m_Lights.begin(), m_Lights.end(), [](const FlashLight& a, const FlashLight& b) { return a.life * a.intensity < b.life * b.intensity; });
        *it = l;   // troppe luci: al posto della piu' debole
    }

    void GameFeel::AddFlinch(Scene& scene, entt::entity target, const glm::vec3& dir, float strength) {
        auto& reg = scene.GetRegistry();
        if (!reg.valid(target)) return;
        const auto* t = reg.try_get<TransformComponent>(target);
        if (!t) return;
        const float yaw = glm::radians(t->AimYaw);
        const glm::vec3 fwd(-std::sin(yaw), 0.0f, -std::cos(yaw));
        const glm::vec3 right(-fwd.z, 0.0f, fwd.x);
        glm::vec3 d(dir.x, 0.0f, dir.z);
        if (glm::dot(d, d) < 1e-6f) d = -fwd; else d = glm::normalize(d);
        Flinch& f = m_Flinch[target];
        f.t = 0.0f;
        f.strength = (std::min)(1.6f, (std::max)(f.strength * 0.5f, strength));
        f.local = glm::vec2(glm::dot(d, right), glm::dot(d, fwd));
    }

    // Sussulto: busto e testa spinti nella direzione del colpo, poi tornano
    void GameFeel::ApplyFlinch(entt::entity e, ProceduralPose& pose) const {
        auto it = m_Flinch.find(e);
        if (it == m_Flinch.end()) return;
        const Flinch& f = it->second;
        const float a = PoseLib::Bell(f.t / 0.32f, 0.18f, 0.3f) * f.strength;
        if (a <= 0.001f) return;
        const float pitch = 13.0f * a * f.local.y, roll = 10.0f * a * f.local.x;
        pose.Rotate("Spine1", PoseLib::Rx(pitch * 0.6f) * PoseLib::Rz(-roll * 0.6f));
        pose.Rotate("Spine2", PoseLib::Rx(pitch * 0.5f) * PoseLib::Rz(-roll * 0.5f));
        pose.Rotate("Head", PoseLib::Rx(-pitch * 0.7f) * PoseLib::Rz(roll * 0.4f));   // la testa resta indietro
    }

    void GameFeel::Update(float realDt, bool paused, Scene& scene, entt::entity player, const glm::vec3& camPos, const BodyFn& bodyOf) {
        auto& reg = scene.GetRegistry();
        realDt = std::clamp(realDt, 0.0f, 0.1f);
        m_CamPos = camPos;
        const bool hasPlayer = reg.valid(player) && reg.all_of<TransformComponent>(player);
        if (hasPlayer) { m_PlayerPos = reg.get<TransformComponent>(player).Position; m_PlayerBody = bodyOf ? bodyOf(player) : 0; }
        else m_PlayerPos = camPos;
        const FeelProfile& me = ProfileOf(m_PlayerBody);

        m_Bank.Update(realDt);
        m_HitStop = (std::max)(0.0f, m_HitStop - realDt);
        const float dt = paused ? 0.0f : realDt * TimeScale();
        m_Time += realDt;
        m_TickCooldown -= realDt; m_WhizCooldown -= realDt; m_CasingSndCooldown -= realDt;
        m_ImpactBudget = 5.0f;
        for (auto it = m_LoopCooldown.begin(); it != m_LoopCooldown.end();) { it->second -= realDt; if (it->second < -1.0f) it = m_LoopCooldown.erase(it); else ++it; }

        // --- eventi nuovi: subito, oppure in attesa (proiettile in volo) ---
        auto& q = FeelEventQueue();
        if (paused) q.clear();
        for (size_t i = 0; i < q.size(); ++i) {
            const FeelEvent e = q[i];
            if (e.kind == FeelKind::Impact) {
                // Proiettile che passa vicino alla testa: fischio (e un sussulto, tanto piu' forte quanto piu' sei piccolo)
                const entt::entity src = Ent(e.source);
                if (hasPlayer && src != player && Ent(e.target) != player && reg.valid(src) && reg.all_of<TransformComponent>(src) && m_WhizCooldown <= 0.0f) {
                    const glm::vec3 a = reg.get<TransformComponent>(src).Position + glm::vec3(0.0f, 1.3f, 0.0f);
                    const glm::vec3 ear = m_PlayerPos + glm::vec3(0.0f, 1.4f, 0.0f);
                    const glm::vec3 ab = e.pos - a;
                    const float len2 = glm::dot(ab, ab);
                    if (len2 > 16.0f) {
                        const float t = glm::dot(ear - a, ab) / len2;
                        const glm::vec3 c = a + ab * std::clamp(t, 0.0f, 1.0f);
                        const float miss = glm::distance(c, ear);
                        if (t > 0.06f && t < 1.0f && miss < 2.4f) {
                            const float k = 1.0f - miss / 2.4f;
                            const int snd = IsEnergy(e.fx) ? m_WhizEnergy : ((e.fx == WeaponFx::Sniper || e.fx == WeaponFx::Bolt) ? m_WhizSnap : m_Whiz);
                            m_Bank.PlayAt(snd, c, (0.55f + 0.6f * k) * (0.7f + 0.3f * me.shakeTaken), 12.0f, Random::Range(0.9f, 1.15f), e.delay * t);
                            AddTrauma(0.26f * k * me.shakeTaken);
                            m_WhizCooldown = 0.06f;
                        }
                    }
                }
                if (e.delay > 0.01f) { if (m_Pending.size() < 512) m_Pending.push_back({ e, e.delay }); continue; }
            }
            Handle(e, scene, player, bodyOf);
        }
        q.clear();
        for (size_t i = 0; i < m_Pending.size();) {
            m_Pending[i].t -= dt;
            if (m_Pending[i].t <= 0.0f) {
                const FeelEvent e = m_Pending[i].e;
                m_Pending[i] = m_Pending.back(); m_Pending.pop_back();
                Handle(e, scene, player, bodyOf);
            }
            else ++i;
        }

        // --- atterraggio del giocatore: tonfo e telecamera che si abbassa ---
        if (hasPlayer && dt > 1e-5f) {
            const float y = m_PlayerPos.y;
            if (m_HavePrev) {
                const float vy = (y - m_PrevPlayerY) / dt;
                if (m_PrevVy < -4.5f && vy > -0.8f) {
                    const float k = std::clamp((-m_PrevVy - 4.5f) / 8.0f, 0.15f, 1.0f);
                    m_Bank.Play2D(m_Land, (0.45f + 0.5f * k) * (0.6f + 0.6f * me.stepHeavy), me.stepPitch);
                    if (me.stepHeavy > 0.5f) m_Bank.Play2D(m_StepHeavy, 0.8f, 0.9f);
                    AddTrauma((0.22f + 0.3f * k) * (0.6f + 0.5f * me.stepHeavy));
                    m_Dip += 0.07f + 0.12f * k;
                }
                m_PrevVy = vy;
            }
            m_PrevPlayerY = y; m_HavePrev = true;
        }
        else if (!hasPlayer) m_HavePrev = false;

        // --- luci lampo ---
        for (size_t i = 0; i < m_Lights.size();) {
            m_Lights[i].life -= dt;
            if (m_Lights[i].life <= 0.0f) { m_Lights[i] = m_Lights.back(); m_Lights.pop_back(); } else ++i;
        }

        // --- bossoli: volano, rimbalzano una volta, restano a terra ---
        for (size_t i = 0; i < m_Casings.size();) {
            Casing& c = m_Casings[i];
            c.life -= dt;
            if (c.life <= 0.0f) { m_Casings[i] = m_Casings.back(); m_Casings.pop_back(); continue; }
            if (!c.rested) {
                c.vel.y -= 9.81f * dt;
                c.pos += c.vel * dt;
                c.spin += c.spinVel * dt;
                if (c.pos.y <= c.groundY && c.vel.y < 0.0f) {
                    c.pos.y = c.groundY;
                    if (!c.sounded) {
                        c.sounded = true;
                        if (m_CasingSndCooldown <= 0.0f) {
                            m_Bank.PlayAt(c.size > 1.3f ? m_CasingHeavy : m_CasingSnd, c.pos, 0.5f, 14.0f, Random::Range(0.9f, 1.15f));
                            m_CasingSndCooldown = 0.05f;
                        }
                    }
                    if (std::abs(c.vel.y) > 1.2f) { c.vel = glm::vec3(c.vel.x * 0.45f, -c.vel.y * 0.32f, c.vel.z * 0.45f); c.spinVel *= 0.5f; }
                    else { c.rested = true; c.spin = 0.0f; }
                }
            }
            ++i;
        }

        // --- segni dei colpi ---
        for (size_t i = 0; i < m_Marks.size();) {
            m_Marks[i].age += dt;
            m_Marks[i].heat *= std::exp(-dt * 1.6f);
            if (m_Marks[i].age > 40.0f) m_Marks.erase(m_Marks.begin() + i); else ++i;
        }

        // --- sussulti ---
        for (auto it = m_Flinch.begin(); it != m_Flinch.end();) {
            it->second.t += dt;
            if (it->second.t > 0.32f || !reg.valid(it->first)) it = m_Flinch.erase(it); else ++it;
        }

        // --- telecamera: trauma (scossone morbido, non rumore a scatti), calci direzionali, pugno di FOV ---
        m_Trauma = (std::max)(0.0f, m_Trauma - realDt * 1.9f);
        const float decay = std::exp(-realDt * 10.0f);
        m_KickPitch *= decay; m_KickYaw *= decay; m_FovPunch *= std::exp(-realDt * 9.0f); m_Dip *= std::exp(-realDt * 7.0f);
        const float s = m_Trauma * m_Trauma;
        const float t = m_Time;
        const float nx = std::sin(t * 37.0f) * 0.6f + std::sin(t * 61.3f + 1.7f) * 0.4f;
        const float ny = std::sin(t * 43.1f + 0.5f) * 0.6f + std::sin(t * 71.7f + 2.9f) * 0.4f;
        const float nz = std::sin(t * 29.3f + 4.1f);
        m_Kick.pitch = ny * s * 2.4f + m_KickPitch * shakeScale;
        m_Kick.yaw = nx * s * 2.4f + m_KickYaw * shakeScale;
        m_Kick.fov = m_FovPunch * (std::min)(1.0f, shakeScale);
        m_Kick.offset = glm::vec3(nx * s * 0.05f, nz * s * 0.04f - m_Dip * (std::min)(1.0f, shakeScale), 0.0f);
    }

    void GameFeel::Handle(const FeelEvent& e, Scene& scene, entt::entity player, const BodyFn& bodyOf) {
        const FeelProfile& me = ProfileOf(m_PlayerBody);
        switch (e.kind) {
        case FeelKind::Shot: OnShot(e, scene, player, bodyOf); break;
        case FeelKind::Impact: OnImpact(e, scene, player, bodyOf); break;
        case FeelKind::Explosion: OnExplosion(e.pos, e.scale); break;
        case FeelKind::MeleeSwing: {
            const entt::entity src = Ent(e.source);
            const float pitch = Random::Range(0.93f, 1.08f) * (bodyOf && scene.GetRegistry().valid(src) ? ProfileOf(bodyOf(src)).gunPitch : 1.0f);
            m_Bank.PlayAt(e.variant == 2 ? m_SwingHeavy : (e.variant == 0 ? m_SwingLight : m_SwingMid), e.pos, 0.75f, 24.0f, pitch);
            if (e.flag) m_Bank.PlayAt(m_Chain, e.pos, 0.7f, 35.0f, Random::Range(0.92f, 1.08f));
            break;
        }
        case FeelKind::MeleeHit: OnMeleeHit(e, scene, player, bodyOf); break;
        case FeelKind::Parry:
        case FeelKind::GuardBreak: {
            const bool brk = e.kind == FeelKind::GuardBreak;
            m_Bank.PlayAt(brk ? m_GuardBreak : m_Parry, e.pos, 1.0f, 40.0f, Random::Range(0.95f, 1.06f));
            AddLight(e.pos, glm::vec3(1.0f, 0.85f, 0.55f), 4.0f, 4.0f, 0.08f);
            const float d = glm::distance(e.pos, m_PlayerPos + glm::vec3(0.0f, 1.0f, 0.0f));
            if (d < 3.0f) { AddTrauma((brk ? 0.5f : 0.3f) * (0.5f + 0.5f * me.shakeTaken)); HitStop(brk ? 0.08f : 0.05f); }
            break;
        }
        case FeelKind::Step: OnStep(e, scene, player, bodyOf); break;
        }
    }

    void GameFeel::OnShot(const FeelEvent& e, Scene& scene, entt::entity player, const BodyFn& bodyOf) {
        auto& reg = scene.GetRegistry();
        const int fi = std::clamp(static_cast<int>(e.fx), 0, 11);
        const ShotFeel& sf = k_Shot[fi];
        const entt::entity src = Ent(e.source);
        const bool mine = reg.valid(player) && src == player;
        const bool srcValid = reg.valid(src);
        const FeelProfile& who = ProfileOf((bodyOf && srcValid) ? bodyOf(src) : 0);

        float gain = 1.0f;
        if (e.flag) {   // getto continuo (melta, lanciafiamme): un suono ogni tanto, non a ogni fotogramma
            float& cd = m_LoopCooldown[e.source];
            if (cd > 0.0f) return;
            cd = 0.17f; gain = 0.8f;
        }
        const float dist = glm::distance(e.pos, m_CamPos);
        const float pitch = Random::Range(0.95f, 1.05f) * who.gunPitch;
        const int snd = m_Bank.Has(m_ShotSnd[fi]) ? m_ShotSnd[fi] : m_ShotSnd[static_cast<int>(WeaponFx::Ballistic)];
        const int tail = sf.tail == 1 ? m_TailLight : (sf.tail == 2 ? m_TailHeavy : (sf.tail == 3 ? m_TailEnergy : -1));
        if (mine) {
            m_Bank.Play2D(snd, sf.volume * gain, pitch);
            if (who.stepHeavy > 0.6f && sf.tail == 2) m_Bank.Play2D(m_TailHeavy, 0.35f, 0.8f);   // corpi enormi: piu' basso nella propria arma
            AddTrauma(sf.trauma * who.shakeGiven);
            m_FovPunch += sf.fov * (0.5f + 0.5f * who.shakeGiven);
        }
        else {
            const float lag = dist / kSoundSpeed;
            m_Bank.PlayAt(snd, e.pos, sf.volume * gain, sf.range, pitch, lag);
            // da lontano resta solo il rimbombo
            if (tail >= 0) m_Bank.PlayAt(tail, e.pos, sf.volume * 0.9f * Smooth01(10.0f, 45.0f, dist), sf.range * 3.0f, Random::Range(0.92f, 1.08f), lag);
        }

        // lampo di luce alla bocca
        if (dist < 80.0f) {
            glm::vec3 col = sf.light;
            if (col.x < 0.0f) { col = glm::vec3(e.color); const float m = (std::max)(col.r, (std::max)(col.g, col.b)); col = m > 0.05f ? col / m : glm::vec3(1.0f, 0.3f, 0.2f); }
            AddLight(e.pos + e.dir * 0.3f, col, sf.lightRange, sf.lightIntensity * gain, e.flag ? 0.12f : 0.07f);
        }

        // bossolo espulso a destra
        if (sf.casing && dist < 35.0f && m_Casings.size() < 120) {
            glm::vec3 right = glm::cross(e.dir, glm::vec3(0.0f, 1.0f, 0.0f));
            right = glm::dot(right, right) > 1e-5f ? glm::normalize(right) : glm::vec3(1.0f, 0.0f, 0.0f);
            Casing c{};
            c.pos = e.pos - e.dir * 0.38f + right * 0.05f;
            c.vel = right * Random::Range(1.6f, 2.6f) + glm::vec3(0.0f, Random::Range(1.6f, 2.6f), 0.0f) - e.dir * Random::Range(0.2f, 0.9f);
            c.groundY = (srcValid && reg.all_of<TransformComponent>(src) ? reg.get<TransformComponent>(src).Position.y : e.pos.y - 1.3f) + 0.012f;
            c.spin = Random::Range(0.0f, 6.28f); c.spinVel = Random::Range(18.0f, 40.0f); c.yaw = Random::Range(0.0f, 6.28f);
            c.life = 9.0f; c.size = sf.casing == 2 ? 1.6f : 1.0f;
            m_Casings.push_back(c);
        }
    }

    void GameFeel::OnExplosion(const glm::vec3& pos, float radius) {
        const FeelProfile& me = ProfileOf(m_PlayerBody);
        radius = std::clamp(radius, 0.5f, 20.0f);
        const float dist = glm::distance(pos, m_CamPos);
        const float lag = dist / kSoundSpeed;
        const bool small = radius < 2.2f;
        m_Bank.PlayAt(small ? m_ExplSmall : m_Expl, pos, small ? 1.0f : 1.35f, small ? 110.0f : 170.0f, Random::Range(0.9f, 1.08f), lag);
        m_Bank.PlayAt(m_ExplFar, pos, 1.1f * Smooth01(25.0f, 80.0f, dist), 450.0f, Random::Range(0.9f, 1.1f), lag);
        const float reach = radius * 6.0f + 8.0f;
        if (dist < reach) {
            const float k = 1.0f - dist / reach;
            AddTrauma(k * k * 1.1f * (0.45f + 0.55f * me.shakeTaken));
            m_FovPunch += k * 3.0f;
        }
        AddLight(pos + glm::vec3(0.0f, 0.4f, 0.0f), glm::vec3(1.0f, 0.62f, 0.26f), radius * 5.0f + 5.0f, 16.0f, 0.38f);
    }

    void GameFeel::OnImpact(const FeelEvent& e, Scene& scene, entt::entity player, const BodyFn& bodyOf) {
        auto& reg = scene.GetRegistry();
        if (e.fx == WeaponFx::Explosive) { OnExplosion(e.pos, e.scale); return; }
        const FeelProfile& me = ProfileOf(m_PlayerBody);
        const entt::entity src = Ent(e.source), tgt = Ent(e.target);
        const bool mine = reg.valid(player) && src == player;
        const bool onMe = reg.valid(player) && tgt == player && e.hitUnit;
        const bool energy = IsEnergy(e.fx);
        const bool pellets = e.variant > 1;
        const float dist = glm::distance(e.pos, m_CamPos);

        // segno sul muro / a terra (prima del suono: serve la faccia colpita)
        int axis = 1; float axisSign = 1.0f;
        if (!e.hitUnit) {
            glm::vec3 n = -e.dir;
            if (reg.valid(tgt) && reg.all_of<TransformComponent, ColliderComponent>(tgt)) {
                const auto& tt = reg.get<TransformComponent>(tgt); const auto& col = reg.get<ColliderComponent>(tgt);
                const glm::vec3 half = glm::max(col.halfExtents * tt.Scale, glm::vec3(1e-3f));
                n = (e.pos - GetColliderWorldCenter(tt, col)) / half;   // la faccia piu' vicina al punto colpito
            }
            const glm::vec3 an = glm::abs(n);
            axis = (an.x > an.y && an.x > an.z) ? 0 : (an.y > an.z ? 1 : 2);
            axisSign = n[axis] < 0.0f ? -1.0f : 1.0f;
            if (dist < 90.0f) {
                Mark m{};
                m.pos = e.pos; m.axis = axis; m.age = 0.0f;
                switch (e.fx) {
                case WeaponFx::Bolt: m.size = 0.17f; m.heat = 0.6f; m.glow = glm::vec3(1.0f, 0.5f, 0.15f); break;
                case WeaponFx::Plasma: m.size = 0.24f; m.heat = 1.0f; m.glow = glm::vec3(0.5f, 0.8f, 1.0f); break;
                case WeaponFx::Melta: m.size = 0.2f; m.heat = 1.0f; m.glow = glm::vec3(1.0f, 0.45f, 0.1f); break;
                case WeaponFx::Sniper: m.size = 0.1f; break;
                case WeaponFx::Shotgun: m.size = 0.045f; break;
                case WeaponFx::LongLas: m.size = 0.11f; m.heat = 1.0f; m.glow = glm::vec3(e.color); break;
                case WeaponFx::Laser: case WeaponFx::HotShot: m.size = 0.06f; m.heat = 0.9f; m.glow = glm::vec3(e.color); break;
                case WeaponFx::Grav: m.size = 0.2f; break;
                default: m.size = 0.065f; break;
                }
                m.size *= Random::Range(0.85f, 1.2f);
                if (m_Marks.size() >= 220) m_Marks.erase(m_Marks.begin());
                m_Marks.push_back(m);
            }
        }

        // suono dell'impatto (i pallettoni non suonano tutti)
        if (m_ImpactBudget > 0.0f && (!pellets || Random::Chance(0.4f))) {
            m_ImpactBudget -= 1.0f;
            const float lag = dist / kSoundSpeed, pitch = Random::Range(0.92f, 1.1f);
            if (e.hitUnit) {
                if (!onMe) m_Bank.PlayAt(e.armored ? m_ImpArmor : m_ImpFlesh, e.pos, 0.85f, 45.0f, pitch, lag);
                if (energy) m_Bank.PlayAt(m_ImpEnergy, e.pos, 0.45f, 40.0f, pitch, lag);
            }
            else if (energy) m_Bank.PlayAt(m_ImpEnergy, e.pos, 0.7f, 45.0f, pitch, lag);
            else m_Bank.PlayAt((axis == 1 && axisSign > 0.0f && Random::Chance(0.6f)) ? m_ImpDirt : m_ImpHard, e.pos, 0.8f, 45.0f, pitch, lag);
            if (e.fx == WeaponFx::Bolt) m_Bank.PlayAt(m_ExplSmall, e.pos, 0.5f, 70.0f, Random::Range(1.15f, 1.35f), lag);   // la cartuccia esplode nel bersaglio
        }
        if (e.fx == WeaponFx::Bolt) AddLight(e.pos, glm::vec3(1.0f, 0.6f, 0.25f), 5.0f, 6.0f, 0.1f);
        else if (energy && !pellets) { glm::vec3 c(e.color); const float m = (std::max)(c.r, (std::max)(c.g, c.b)); AddLight(e.pos - e.dir * 0.15f, m > 0.05f ? c / m : glm::vec3(1.0f), 3.5f, 3.0f, 0.08f); }

        if (e.hitUnit && !e.killed) AddFlinch(scene, tgt, e.dir, std::clamp(0.4f + e.scale / 60.0f, 0.4f, 1.0f) * ProfileOf(bodyOf && reg.valid(tgt) ? bodyOf(tgt) : 0).flinch);

        if (onMe) {   // colpito: botta sorda, scossone, la mira salta dalla parte del colpo
            m_Bank.Play2D(e.armored ? m_ImpArmor : m_Hurt, 0.9f, Random::Range(0.92f, 1.08f));
            if (e.armored) m_Bank.Play2D(m_Hurt, 0.5f, 0.9f);
            const float k = std::clamp(0.5f + e.scale / 80.0f, 0.5f, 1.2f) * (pellets ? 0.45f : 1.0f);
            AddTrauma(0.42f * k * me.shakeTaken);
            const float yaw = glm::radians(reg.get<TransformComponent>(player).AimYaw);
            const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
            m_KickYaw += (glm::dot(e.dir, right) > 0.0f ? 1.0f : -1.0f) * 0.9f * k * me.shakeTaken;
            m_KickPitch += 0.7f * k * me.shakeTaken;
        }
        if (mine && e.hitUnit) {   // conferma: ticchettio a ogni colpo a segno, nota piena all'uccisione
            if (e.killed) {
                m_Bank.Play2D(m_Kill, 0.8f * me.power, Random::Range(0.97f, 1.04f) * (me.stepHeavy > 0.6f ? 0.88f : 1.0f));
                m_FovPunch += 1.0f * me.power;
                AddTrauma(0.12f * me.power);
            }
            else if (m_TickCooldown <= 0.0f) {
                m_Bank.Play2D(e.armored ? m_HitArmorTick : m_HitTick, 0.5f, Random::Range(0.96f, 1.05f));
                m_TickCooldown = 0.045f;
            }
        }
    }

    void GameFeel::OnMeleeHit(const FeelEvent& e, Scene& scene, entt::entity player, const BodyFn& bodyOf) {
        auto& reg = scene.GetRegistry();
        const FeelProfile& me = ProfileOf(m_PlayerBody);
        const entt::entity src = Ent(e.source), tgt = Ent(e.target);
        const bool mine = reg.valid(player) && src == player, onMe = reg.valid(player) && tgt == player;
        const int v = std::clamp(e.variant, 0, 2);
        const FeelProfile& who = ProfileOf(bodyOf && reg.valid(src) ? bodyOf(src) : 0);
        const float pitch = Random::Range(0.93f, 1.07f) * who.gunPitch;
        m_Bank.PlayAt(v == 2 ? m_MeleeHeavy : m_MeleeHit, e.pos, v == 0 ? 0.8f : 1.0f, 40.0f, pitch);
        if (who.stepHeavy > 0.6f && v != 2) m_Bank.PlayAt(m_MeleeHeavy, e.pos, 0.55f, 40.0f, pitch);   // i pugni di un gigante pesano sempre
        if (e.armored) m_Bank.PlayAt(m_ImpArmor, e.pos, 0.6f, 40.0f, Random::Range(0.8f, 0.95f));
        if (e.flag) m_Bank.PlayAt(m_Chain, e.pos, 0.9f, 40.0f, Random::Range(1.05f, 1.2f));
        if (!e.killed) AddFlinch(scene, tgt, e.dir, (v == 2 ? 1.2f : (v == 1 ? 0.95f : 0.75f)) * ProfileOf(bodyOf && reg.valid(tgt) ? bodyOf(tgt) : 0).flinch);

        static const float kStop[3] = { 0.045f, 0.065f, 0.10f }, kTrauma[3] = { 0.30f, 0.40f, 0.60f }, kFov[3] = { 0.6f, 1.0f, 2.0f };
        if (mine) {   // il colpo "si pianta": un attimo di fermo-immagine, poi lo scossone
            HitStop(kStop[v] * me.hitStop + (e.killed ? 0.04f : 0.0f));
            AddTrauma(kTrauma[v] * (0.6f + 0.4f * me.power));
            m_FovPunch += kFov[v] * me.power;
            if (e.killed) m_Bank.Play2D(m_Kill, 0.8f * me.power, me.stepHeavy > 0.6f ? 0.88f : 1.0f);
        }
        if (onMe) {
            m_Bank.Play2D(m_Hurt, 1.0f, Random::Range(0.9f, 1.05f));
            HitStop(0.04f);
            AddTrauma((0.45f + 0.15f * v) * me.shakeTaken);
            const float yaw = glm::radians(reg.get<TransformComponent>(player).AimYaw);
            const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
            m_KickYaw += (glm::dot(e.dir, right) > 0.0f ? 1.0f : -1.0f) * (1.2f + 0.6f * v) * me.shakeTaken;
            m_KickPitch += 1.0f * me.shakeTaken;
        }
    }

    void GameFeel::OnStep(const FeelEvent& e, Scene& scene, entt::entity player, const BodyFn& bodyOf) {
        auto& reg = scene.GetRegistry();
        const entt::entity src = Ent(e.source);
        if (!reg.valid(src)) return;
        const bool mine = src == player;
        const FeelProfile& who = ProfileOf(bodyOf ? bodyOf(src) : 0);
        const FeelProfile& me = ProfileOf(m_PlayerBody);
        const int surf = std::clamp(e.variant, 0, 3);
        const int snd = m_Bank.Has(m_StepSnd[surf]) ? m_StepSnd[surf] : m_StepSnd[0];
        const float pitch = who.stepPitch * Random::Range(0.94f, 1.07f);
        const float base = (mine ? 0.6f : 0.75f) * who.stepVolume;
        const bool tiny = who.stepPitch > 1.3f;
        m_Bank.PlayAt(snd, e.pos, base * (tiny ? 0.45f : 1.0f), 26.0f, pitch);
        if (tiny) m_Bank.PlayAt(m_StepLight, e.pos, base * 1.1f, 20.0f, Random::Range(0.9f, 1.2f));
        if (who.stepHeavy > 0.01f) {
            // stivali corazzati: si sentono arrivare da lontano
            m_Bank.PlayAt(m_StepHeavy, e.pos, (mine ? 0.55f : 0.9f) * who.stepHeavy, 20.0f + 35.0f * who.stepHeavy, Random::Range(0.92f, 1.06f));
            if (mine) { AddTrauma(0.16f * who.stepShake); m_Dip += 0.012f * who.stepShake; }
            else {
                // un gigante che ti cammina accanto: piu' sei piccolo, piu' la terra trema
                const float d = glm::distance(e.pos, m_PlayerPos);
                if (d < 8.0f && who.stepHeavy > 0.6f) AddTrauma((1.0f - d / 8.0f) * 0.2f * who.stepHeavy * (std::max)(0.0f, me.shakeTaken - 0.3f));
            }
        }
    }

    void GameFeel::SubmitLights(DeferredRenderer& renderer) const {
        for (const auto& l : m_Lights) {
            const float k = l.life / l.maxLife;
            renderer.AddLight(Light::Point(l.pos, l.range, l.color, l.intensity * k * k));
        }
    }

    void GameFeel::Draw(DeferredRenderer& renderer) const {
        // bossoli d'ottone
        for (const auto& c : m_Casings) {
            if (glm::distance(c.pos, m_CamPos) > 40.0f) continue;
            glm::mat4 m = glm::translate(glm::mat4(1.0f), c.pos);
            m = glm::rotate(m, c.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            m = glm::rotate(m, c.spin, glm::vec3(1.0f, 0.0f, 0.0f));
            m = glm::scale(m, glm::vec3(0.012f, 0.012f, 0.034f) * c.size * (c.life < 1.0f ? c.life : 1.0f));
            renderer.DrawCube(m, glm::vec4(0.80f, 0.58f, 0.22f, 1.0f), 0.32f, 1.0f, 0.0f, 0);
        }
        // segni dei colpi: piastrina scura sulla faccia colpita; le armi a energia lasciano il punto incandescente
        for (const auto& k : m_Marks) {
            if (glm::distance(k.pos, m_CamPos) > 90.0f) continue;
            const float fade = k.age > 36.0f ? (40.0f - k.age) / 4.0f : 1.0f;
            glm::vec3 sc(k.size * fade);
            sc[k.axis] = 0.014f;
            const glm::mat4 at = glm::translate(glm::mat4(1.0f), k.pos);
            const float h = std::clamp(k.heat, 0.0f, 1.0f);
            const glm::vec3 col = glm::mix(glm::vec3(0.03f, 0.027f, 0.025f), k.glow, h);
            glm::vec3 n(0.0f); n[k.axis] = 1.0f;
            // due piastrine incrociate a 45 gradi: una macchia tondeggiante invece di un quadrato
            renderer.DrawCube(glm::scale(at, sc), glm::vec4(col, 1.0f), 0.95f, 0.0f, h * 5.0f, 0);
            glm::vec3 sc2 = sc * 0.92f; sc2[k.axis] = 0.016f;
            renderer.DrawCube(glm::scale(glm::rotate(at, 0.785398f, n), sc2), glm::vec4(col, 1.0f), 0.95f, 0.0f, h * 5.0f, 0);
        }
    }
}
