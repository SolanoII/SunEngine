#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "../Scene/Scene.h"
#include "../Renderer/Renderer.h"
#include "../Renderer/VfxRenderer.h"
#include "../Core/Random.h"
#include "Components.h"
#include "VfxEvents.h"
#include "DamageResolver.h"

namespace SunEngine {

    // Effetti visivi del combattimento:
    //  - colpi in volo (TrailRequestComponent) disegnati in base al tipo di arma (laser, proiettile, bolt, plasma...)
    //    con impatto diverso su un'unita' o sul mondo quando arrivano;
    //  - particelle (lampi alla bocca, scintille, fumo, sangue, fiamme, esplosioni) create dagli eventi VfxEvents;
    //  - scie dei fendenti, colpi in mischia, parate, guardie rotte, interruzioni;
    //  - segnali di carica degli attacchi in mischia (anello che si stringe fino al momento del colpo: si legge
    //    se arriva un normale, un basso o un pesante) e scritte che salgono.
    class ParticleSystem {
    public:
        struct FloatText { glm::vec3 pos; std::string text; glm::vec4 color; float age = 0.0f, life = 1.0f; };

    private:
        using VR = VfxRenderer;
        struct Particle {
            glm::vec3 pos{ 0.0f }, vel{ 0.0f };
            float age = 0.0f, life = 1.0f;
            float size0 = 0.1f, size1 = 0.1f;
            glm::vec4 col0{ 1.0f }, col1{ 1.0f, 1.0f, 1.0f, 0.0f };
            float gravity = 0.0f, drag = 0.0f;
            int shape = VR::Glow;
            bool additive = true;
            float streak = 0.0f;           // >0: disegnata come striscia lunga velocita' x streak (scintille)
            float rot = 0.0f, rotVel = 0.0f, seed = 0.0f;
            bool flat = false;             // steso a terra (onde d'urto, bruciature, pozze)
        };
        struct Slash {
            glm::vec3 p0{ 0.0f }, ctrl{ 0.0f }, p1{ 0.0f };
            glm::vec4 color{ 1.0f };
            float width = 0.3f, duration = 0.15f, fade = 0.35f, age = 0.0f;
            bool chain = false;
            int variant = 1;
        };

        std::vector<Particle> m_Particles;
        std::vector<Slash> m_Slashes;
        std::vector<FloatText> m_Texts;
        static constexpr size_t kMaxParticles = 5000;

        static float R(float a, float b) { return Random::Range(a, b); }
        static glm::vec3 RandUnit() {
            const float z = R(-1.0f, 1.0f), a = R(0.0f, 6.2831853f), r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            return glm::vec3(r * std::cos(a), z, r * std::sin(a));
        }
        // Direzione a caso in un cono attorno a dir (semiapertura in radianti)
        static glm::vec3 RandCone(const glm::vec3& dir, float halfAngle) {
            glm::vec3 d = glm::length(dir) > 1e-5f ? glm::normalize(dir) : glm::vec3(0, 1, 0);
            const glm::vec3 up = std::abs(d.y) > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 x = glm::normalize(glm::cross(up, d)), y = glm::cross(d, x);
            const float c = std::cos(halfAngle), z = R(c, 1.0f), r = std::sqrt(std::max(0.0f, 1.0f - z * z)), a = R(0.0f, 6.2831853f);
            return d * z + x * (r * std::cos(a)) + y * (r * std::sin(a));
        }
        Particle& Add() {
            if (m_Particles.size() >= kMaxParticles) m_Particles.erase(m_Particles.begin(), m_Particles.begin() + 500);
            m_Particles.emplace_back();
            Particle& p = m_Particles.back();
            p.seed = R(0.0f, 100.0f);
            return p;
        }
        void Glow(const glm::vec3& pos, float s0, float s1, const glm::vec4& c0, float life, const glm::vec4& c1 = glm::vec4(-1)) {
            Particle& p = Add();
            p.pos = pos; p.size0 = s0; p.size1 = s1; p.col0 = c0; p.col1 = c1.x < 0 ? glm::vec4(glm::vec3(c0), 0.0f) : c1; p.life = life;
        }
        void Ring(const glm::vec3& pos, float s0, float s1, const glm::vec4& c0, float life) {
            Particle& p = Add();
            p.pos = pos; p.size0 = s0; p.size1 = s1; p.col0 = c0; p.col1 = glm::vec4(glm::vec3(c0), 0.0f); p.life = life; p.shape = VR::Ring;
        }
        void Sparks(const glm::vec3& pos, const glm::vec3& dir, int n, float cone, float speed0, float speed1, const glm::vec4& col,
                    float life = 0.35f, float gravity = 9.0f, float streak = 0.035f, float width = 0.025f) {
            for (int i = 0; i < n; ++i) {
                Particle& p = Add();
                p.pos = pos; p.vel = RandCone(dir, cone) * R(speed0, speed1);
                p.life = life * R(0.6f, 1.3f); p.gravity = gravity; p.drag = 1.5f;
                p.col0 = col; p.col1 = glm::vec4(glm::vec3(col) * glm::vec3(1.0f, 0.5f, 0.3f), 0.0f);
                p.shape = VR::Spark; p.streak = streak; p.size0 = width; p.size1 = width * 0.5f;
            }
        }
        void Smoke(const glm::vec3& pos, int n, float s0, float s1, const glm::vec4& col, float life, const glm::vec3& drift = glm::vec3(0, 0.5f, 0), float spread = 0.3f) {
            for (int i = 0; i < n; ++i) {
                Particle& p = Add();
                p.pos = pos + RandUnit() * (spread * 0.3f); p.vel = drift + RandUnit() * spread;
                p.life = life * R(0.7f, 1.3f); p.drag = 1.2f;
                p.size0 = s0 * R(0.8f, 1.2f); p.size1 = s1 * R(0.8f, 1.3f);
                p.col0 = col; p.col1 = glm::vec4(glm::vec3(col) * 1.1f, 0.0f);
                p.shape = VR::Smoke; p.additive = false; p.rot = R(0.0f, 6.28f); p.rotVel = R(-0.8f, 0.8f);
            }
        }
        void Blood(const glm::vec3& pos, const glm::vec3& dir, int drops, float mist) {
            const glm::vec4 red(0.45f, 0.02f, 0.02f, 0.9f);
            Smoke(pos, std::max(1, static_cast<int>(mist * 2.0f)), 0.12f * mist, 0.45f * mist, glm::vec4(0.5f, 0.03f, 0.03f, 0.55f), 0.45f, dir * 0.6f, 0.25f);
            for (int i = 0; i < drops; ++i) {
                Particle& p = Add();
                p.pos = pos; p.vel = RandCone(dir, 0.7f) * R(1.5f, 4.5f);
                p.life = R(0.35f, 0.7f); p.gravity = 9.8f; p.drag = 0.5f;
                p.size0 = R(0.03f, 0.06f); p.size1 = p.size0 * 0.6f;
                p.col0 = red; p.col1 = glm::vec4(0.3f, 0.0f, 0.0f, 0.0f);
                p.shape = VR::Disc; p.additive = false;
            }
        }
        static glm::vec4 Hdr(const glm::vec4& c, float a) { return glm::vec4(glm::vec3(c), a); }
        // Segno lasciato a terra: resta qualche secondo e sbiadisce
        void Decal(const glm::vec3& groundPos, float size, const glm::vec4& col, float life, int shape = VR::Smoke) {
            Particle& p = Add();
            p.pos = groundPos; p.size0 = size * 0.7f; p.size1 = size; p.life = life;
            p.col0 = col; p.col1 = glm::vec4(glm::vec3(col), 0.0f);
            p.shape = shape; p.additive = false; p.flat = true; p.rot = R(0.0f, 6.28f);
        }

        // ---------- effetti ----------
        void SpawnMuzzle(const VfxEvent& e) {
            const glm::vec3 d = glm::length(e.dir) > 1e-5f ? glm::normalize(e.dir) : glm::vec3(0, 0, -1);
            const glm::vec3 p = e.pos;
            const glm::vec4 wc = e.color.a > 0.01f ? e.color : glm::vec4(1.0f, 0.3f, 0.2f, 1.0f);
            const glm::vec4 fire(1.0f, 0.75f, 0.35f, 1.0f);
            switch (e.fx) {
            case WeaponFx::Laser:
                Glow(p, 0.22f, 0.32f, Hdr(wc, 0.9f), 0.06f); Glow(p, 0.08f, 0.1f, glm::vec4(1.0f), 0.04f); break;
            case WeaponFx::HotShot:
                Glow(p, 0.32f, 0.45f, Hdr(wc, 1.0f), 0.07f); Glow(p, 0.12f, 0.12f, glm::vec4(1.0f), 0.05f);
                Sparks(p, d, 3, 0.4f, 3.0f, 6.0f, wc, 0.15f, 0.0f); break;
            case WeaponFx::LongLas:
                Glow(p, 0.35f, 0.5f, Hdr(wc, 1.0f), 0.1f); Ring(p, 0.15f, 0.6f, Hdr(wc, 0.8f), 0.15f); break;
            case WeaponFx::Ballistic:
            case WeaponFx::Shotgun:
            case WeaponFx::Sniper: {
                const float k = e.fx == WeaponFx::Shotgun ? 1.6f : (e.fx == WeaponFx::Sniper ? 1.4f : 1.0f);
                Glow(p + d * 0.06f, 0.3f * k, 0.38f * k, fire, 0.05f);
                for (int i = 0; i < (e.fx == WeaponFx::Shotgun ? 5 : 3); ++i) {   // petali del lampo
                    Particle& s = Add();
                    s.pos = p; s.vel = RandCone(d, e.fx == WeaponFx::Shotgun ? 0.6f : 0.35f) * (6.0f * k); s.life = 0.045f;
                    s.shape = VR::Spark; s.streak = 0.05f; s.size0 = 0.07f * k; s.size1 = 0.05f;
                    s.col0 = glm::vec4(1.0f, 0.85f, 0.5f, 1.0f); s.col1 = glm::vec4(1.0f, 0.5f, 0.2f, 0.0f);
                }
                if (e.fx == WeaponFx::Sniper) {   // freno di bocca: lampi laterali
                    const glm::vec3 side = glm::normalize(glm::cross(d, glm::vec3(0, 1, 0)) + glm::vec3(0.0001f));
                    Sparks(p, side, 2, 0.15f, 4.0f, 5.0f, fire, 0.06f, 0.0f, 0.04f, 0.06f);
                    Sparks(p, -side, 2, 0.15f, 4.0f, 5.0f, fire, 0.06f, 0.0f, 0.04f, 0.06f);
                }
                Smoke(p + d * 0.1f, e.fx == WeaponFx::Ballistic ? 1 : 3, 0.12f * k, 0.45f * k, glm::vec4(0.6f, 0.58f, 0.55f, 0.35f), 0.8f, d * 0.6f + glm::vec3(0, 0.4f, 0), 0.15f);
                break;
            }
            case WeaponFx::Bolt:
                Glow(p + d * 0.08f, 0.45f, 0.55f, glm::vec4(1.0f, 0.7f, 0.3f, 1.0f), 0.06f);
                Sparks(p, d, 4, 0.35f, 5.0f, 9.0f, fire, 0.1f, 0.0f, 0.05f, 0.05f);
                Smoke(p + d * 0.15f, 3, 0.15f, 0.6f, glm::vec4(0.55f, 0.53f, 0.5f, 0.4f), 1.0f, d * 0.8f + glm::vec3(0, 0.3f, 0), 0.2f);
                break;
            case WeaponFx::Plasma:
                Glow(p, 0.45f, 0.7f, glm::vec4(0.45f, 0.8f, 1.0f, 1.0f), 0.12f); Glow(p, 0.15f, 0.2f, glm::vec4(1.0f), 0.08f);
                Ring(p, 0.1f, 0.7f, glm::vec4(0.5f, 0.85f, 1.0f, 0.7f), 0.18f);
                Smoke(p, 2, 0.1f, 0.35f, glm::vec4(0.7f, 0.85f, 1.0f, 0.18f), 0.6f, glm::vec3(0, 0.6f, 0), 0.15f);
                break;
            case WeaponFx::Melta:
                Glow(p, 0.5f, 0.7f, glm::vec4(1.0f, 0.55f, 0.15f, 1.0f), 0.1f); Glow(p, 0.18f, 0.2f, glm::vec4(1.0f, 0.95f, 0.8f, 1.0f), 0.07f);
                break;
            case WeaponFx::Explosive:
                Glow(p, 0.4f, 0.6f, fire, 0.08f);
                Smoke(p - d * 0.4f, 4, 0.2f, 0.9f, glm::vec4(0.55f, 0.53f, 0.5f, 0.5f), 1.2f, -d * 1.5f + glm::vec3(0, 0.3f, 0), 0.4f);   // vampata posteriore
                break;
            case WeaponFx::Grav:
                Glow(p, 0.35f, 0.5f, glm::vec4(0.7f, 0.4f, 1.0f, 1.0f), 0.1f); Ring(p, 0.1f, 0.5f, glm::vec4(0.75f, 0.5f, 1.0f, 0.8f), 0.15f);
                break;
            case WeaponFx::Flamer:
                Glow(p, 0.15f, 0.2f, glm::vec4(0.4f, 0.6f, 1.0f, 0.8f), 0.05f); break;   // fiammella pilota
            }
        }

        void SpawnFlamerJet(const VfxEvent& e) {
            const glm::vec3 d = glm::normalize(e.dir);
            const float range = std::max(3.0f, e.scale);
            for (int i = 0; i < 7; ++i) {
                Particle& p = Add();
                p.pos = e.pos + d * R(0.0f, 0.3f);
                p.vel = RandCone(d, 0.16f) * (range / 0.45f) * R(0.85f, 1.1f);
                p.life = R(0.4f, 0.55f); p.drag = 1.6f; p.gravity = -1.5f;   // le fiamme salgono
                p.size0 = R(0.08f, 0.16f); p.size1 = R(0.9f, 1.4f);
                p.col0 = glm::vec4(1.0f, 0.85f, 0.45f, 1.0f); p.col1 = glm::vec4(0.75f, 0.12f, 0.02f, 0.0f);
                p.shape = VR::Flame; p.rot = R(0.0f, 6.28f); p.rotVel = R(-3.0f, 3.0f);
            }
            Particle& s = Add();   // fumo nero in coda
            s.pos = e.pos + d * (range * 0.6f); s.vel = d * 2.0f + glm::vec3(0, 1.2f, 0);
            s.life = 1.2f; s.drag = 1.0f; s.size0 = 0.5f; s.size1 = 1.6f;
            s.col0 = glm::vec4(0.12f, 0.1f, 0.09f, 0.35f); s.col1 = glm::vec4(0.2f, 0.18f, 0.16f, 0.0f);
            s.shape = VR::Smoke; s.additive = false; s.rot = R(0.0f, 6.28f);
        }

        void SpawnExplosion(const glm::vec3& c, float radius) {
            const float k = std::clamp(radius / 3.5f, 0.5f, 2.0f);
            Glow(c, 0.5f * k, 3.0f * k, glm::vec4(1.0f, 0.95f, 0.8f, 1.0f), 0.12f);           // lampo
            for (int i = 0; i < 7; ++i) {                                                     // palla di fuoco
                Particle& p = Add();
                p.pos = c + RandUnit() * (0.3f * k); p.vel = RandUnit() * R(1.0f, 3.0f) * k + glm::vec3(0, 1.0f, 0);
                p.life = R(0.35f, 0.6f); p.drag = 3.0f;
                p.size0 = R(0.6f, 1.0f) * k; p.size1 = R(1.8f, 2.6f) * k;
                p.col0 = glm::vec4(1.0f, 0.75f, 0.3f, 1.0f); p.col1 = glm::vec4(0.6f, 0.1f, 0.02f, 0.0f);
                p.shape = VR::Flame; p.rot = R(0.0f, 6.28f);
            }
            {   // onda d'urto stesa a terra
                Particle& p = Add(); p.pos = c - glm::vec3(0, 0.25f, 0); p.size0 = 0.4f; p.size1 = radius * 2.4f; p.life = 0.3f;
                p.col0 = glm::vec4(1.0f, 0.85f, 0.6f, 0.8f); p.col1 = glm::vec4(1.0f, 0.6f, 0.3f, 0.0f); p.shape = VR::Ring; p.flat = true;
            }
            Decal(c - glm::vec3(0, 0.25f, 0), radius * 1.3f, glm::vec4(0.06f, 0.05f, 0.04f, 0.75f), 10.0f);   // bruciatura
            Sparks(c, glm::vec3(0, 1, 0), 22, 1.4f, 6.0f, 14.0f, glm::vec4(1.0f, 0.75f, 0.35f, 1.0f), 0.7f, 9.8f, 0.03f, 0.035f);
            Smoke(c + glm::vec3(0, 0.4f, 0), 9, 0.8f * k, 3.0f * k, glm::vec4(0.16f, 0.14f, 0.13f, 0.6f), 2.4f, glm::vec3(0, 1.2f, 0), 1.6f * k);
            for (int i = 0; i < 10; ++i) {                                                    // detriti
                Particle& p = Add();
                p.pos = c; p.vel = RandCone(glm::vec3(0, 1, 0), 1.2f) * R(4.0f, 9.0f);
                p.life = R(0.6f, 1.1f); p.gravity = 12.0f;
                p.size0 = R(0.05f, 0.1f); p.size1 = p.size0;
                p.col0 = glm::vec4(0.15f, 0.12f, 0.1f, 1.0f); p.col1 = glm::vec4(0.15f, 0.12f, 0.1f, 0.0f);
                p.shape = VR::Disc; p.additive = false;
            }
        }

        void SpawnImpact(WeaponFx fx, const glm::vec3& pos, const glm::vec3& dirIn, bool unit, const glm::vec4& wc) {
            const glm::vec3 back = -glm::normalize(dirIn);
            const glm::vec4 spark(1.0f, 0.8f, 0.45f, 1.0f);
            switch (fx) {
            case WeaponFx::Laser: case WeaponFx::HotShot: case WeaponFx::LongLas: {
                const float k = fx == WeaponFx::Laser ? 1.0f : 1.5f;
                Glow(pos, 0.2f * k, 0.45f * k, Hdr(wc, 1.0f), 0.12f);
                Glow(pos, 0.12f * k, 0.15f * k, glm::vec4(1.0f, 0.9f, 0.7f, 1.0f), 0.3f, glm::vec4(0.6f, 0.15f, 0.05f, 0.0f));   // bruciatura che si spegne
                Sparks(pos, back, unit ? 2 : 5, 0.9f, 2.0f, 5.0f, Hdr(wc, 1.0f), 0.25f);
                Smoke(pos, 1, 0.08f, 0.3f, glm::vec4(0.35f, 0.33f, 0.3f, 0.35f), 0.7f);
                if (unit) Blood(pos, back, 3, 0.5f);
                break;
            }
            case WeaponFx::Ballistic: case WeaponFx::Shotgun: case WeaponFx::Sniper: {
                const float k = fx == WeaponFx::Sniper ? 1.8f : (fx == WeaponFx::Shotgun ? 0.6f : 1.0f);
                if (unit) { Blood(pos, back, static_cast<int>(5 * k), k); Sparks(pos, back, 2, 0.8f, 2.0f, 4.0f, spark, 0.2f); }
                else {
                    Sparks(pos, back, static_cast<int>(6 * k) + 1, 0.9f, 3.0f, 8.0f, spark, 0.3f);
                    Smoke(pos, fx == WeaponFx::Shotgun ? 1 : 2, 0.1f * k, 0.55f * k, glm::vec4(0.5f, 0.45f, 0.38f, 0.45f), 0.9f, back * 0.8f + glm::vec3(0, 0.2f, 0), 0.3f);
                    Glow(pos, 0.12f * k, 0.15f * k, spark, 0.05f);
                }
                break;
            }
            case WeaponFx::Bolt:
                Glow(pos, 0.3f, 1.1f, glm::vec4(1.0f, 0.8f, 0.4f, 1.0f), 0.12f);
                for (int i = 0; i < 3; ++i) { Particle& p = Add(); p.pos = pos + RandUnit() * 0.08f; p.vel = RandUnit() * 1.2f; p.life = R(0.25f, 0.35f); p.drag = 3.0f;
                    p.size0 = 0.25f; p.size1 = 0.75f; p.col0 = glm::vec4(1.0f, 0.7f, 0.3f, 1.0f); p.col1 = glm::vec4(0.6f, 0.1f, 0.0f, 0.0f); p.shape = VR::Flame; p.rot = R(0, 6.28f); }
                Sparks(pos, back, 10, 1.1f, 4.0f, 10.0f, spark, 0.4f);
                Smoke(pos, 3, 0.2f, 0.9f, glm::vec4(0.25f, 0.23f, 0.21f, 0.55f), 1.4f, back * 0.5f + glm::vec3(0, 0.6f, 0), 0.5f);
                if (unit) Blood(pos, back, 8, 1.4f);
                break;
            case WeaponFx::Plasma:
                Glow(pos, 0.4f, 1.3f, glm::vec4(0.45f, 0.8f, 1.0f, 1.0f), 0.18f);
                Glow(pos, 0.2f, 0.25f, glm::vec4(1.0f), 0.1f);
                Ring(pos, 0.2f, 1.6f, glm::vec4(0.5f, 0.85f, 1.0f, 0.8f), 0.22f);
                Sparks(pos, back, 10, 1.3f, 3.0f, 8.0f, glm::vec4(0.6f, 0.9f, 1.0f, 1.0f), 0.35f, 4.0f);
                Smoke(pos, 2, 0.15f, 0.6f, glm::vec4(0.6f, 0.75f, 0.85f, 0.25f), 0.9f);
                if (unit) Blood(pos, back, 3, 0.6f);
                break;
            case WeaponFx::Melta:
                Glow(pos, 0.35f, 0.9f, glm::vec4(1.0f, 0.6f, 0.2f, 1.0f), 0.15f);
                Sparks(pos, back, 6, 1.0f, 2.0f, 6.0f, glm::vec4(1.0f, 0.65f, 0.25f, 1.0f), 0.5f, 6.0f, 0.03f, 0.04f);   // gocce fuse
                Smoke(pos, 1, 0.15f, 0.6f, glm::vec4(0.4f, 0.38f, 0.36f, 0.4f), 1.0f, glm::vec3(0, 1.0f, 0));
                break;
            case WeaponFx::Grav:
                Ring(pos, 0.2f, 1.0f, glm::vec4(0.7f, 0.45f, 1.0f, 0.9f), 0.25f); Glow(pos, 0.3f, 0.6f, glm::vec4(0.7f, 0.4f, 1.0f, 0.9f), 0.15f);
                break;
            default: break;   // lanciafiamme ed esplosivi: lo fanno i loro eventi
            }
        }

        static glm::vec4 SlashColor(int variant) {
            if (variant == 0) return glm::vec4(0.35f, 0.75f, 1.0f, 1.0f);    // basso: azzurro
            if (variant == 2) return glm::vec4(1.0f, 0.3f, 0.15f, 1.0f);     // pesante: rosso
            return glm::vec4(1.0f, 0.9f, 0.55f, 1.0f);                       // normale: giallo chiaro
        }
        static glm::vec3 Bezier(const Slash& s, float t) {
            const float u = 1.0f - t;
            return u * u * s.p0 + 2.0f * u * t * s.ctrl + t * t * s.p1;
        }

        void Consume() {
            auto& q = VfxEventQueue();
            for (const auto& e : q) {
                switch (e.kind) {
                case VfxKind::Muzzle: SpawnMuzzle(e); break;
                case VfxKind::FlamerJet: SpawnFlamerJet(e); break;
                case VfxKind::Explosion: SpawnExplosion(e.pos, e.scale); break;
                case VfxKind::MeleeSlash: {
                    Slash s; s.p0 = e.pos; s.p1 = e.pos2; s.ctrl = e.dir; s.variant = e.variant; s.chain = e.flag;
                    s.duration = std::clamp(e.scale, 0.06f, 0.5f);
                    s.width = e.variant == 2 ? 0.42f : (e.variant == 0 ? 0.3f : 0.32f);
                    s.fade = e.variant == 2 ? 0.45f : 0.32f;
                    s.color = SlashColor(e.variant);
                    m_Slashes.push_back(s);
                    break;
                }
                case VfxKind::MeleeHit: {
                    const glm::vec3 d = glm::length(e.dir) > 1e-5f ? glm::normalize(e.dir) : glm::vec3(0, 0, -1);
                    const float k = e.variant == 2 ? 1.6f : 1.0f;
                    if (e.pos2.y > -9000.0f) Decal(e.pos2 + d * 0.4f, 0.6f * k, glm::vec4(0.32f, 0.02f, 0.02f, 0.7f), 7.0f, VR::Smoke);   // sangue a terra
                    Glow(e.pos, 0.2f * k, 0.7f * k, glm::vec4(1.0f, 0.95f, 0.85f, 0.9f), 0.1f);
                    Ring(e.pos, 0.1f, 0.6f * k, Hdr(SlashColor(e.variant), 0.8f), 0.16f);
                    Blood(e.pos, d, static_cast<int>(8 * k), 1.2f * k);
                    if (e.flag) Sparks(e.pos, d, static_cast<int>(14 * k), 1.0f, 3.0f, 8.0f, glm::vec4(1.0f, 0.8f, 0.4f, 1.0f), 0.35f);   // denti della catena
                    else Sparks(e.pos, d, 3, 0.8f, 2.0f, 5.0f, glm::vec4(1.0f, 0.85f, 0.6f, 1.0f), 0.2f);
                    break;
                }
                case VfxKind::Parry:
                    Glow(e.pos, 0.3f, 1.2f, glm::vec4(1.0f, 0.95f, 0.75f, 1.0f), 0.15f);
                    Ring(e.pos, 0.2f, 1.4f, glm::vec4(1.0f, 0.9f, 0.6f, 0.9f), 0.2f);
                    Sparks(e.pos, glm::vec3(0, 0.3f, 0) + e.dir, 20, 1.5f, 3.0f, 9.0f, glm::vec4(1.0f, 0.9f, 0.55f, 1.0f), 0.4f);
                    break;
                case VfxKind::GuardBreak:
                    Glow(e.pos, 0.4f, 1.6f, glm::vec4(1.0f, 0.25f, 0.1f, 1.0f), 0.2f);
                    Ring(e.pos, 0.2f, 2.0f, glm::vec4(1.0f, 0.3f, 0.15f, 0.9f), 0.3f);
                    for (int i = 0; i < 12; ++i) {   // schegge della guardia
                        Particle& p = Add(); p.pos = e.pos; p.vel = RandUnit() * R(2.0f, 5.0f) + glm::vec3(0, 1.5f, 0);
                        p.life = R(0.4f, 0.7f); p.gravity = 9.0f; p.size0 = R(0.04f, 0.08f); p.size1 = 0.02f;
                        p.col0 = glm::vec4(1.0f, 0.5f, 0.3f, 1.0f); p.col1 = glm::vec4(1.0f, 0.2f, 0.1f, 0.0f); p.shape = VR::Disc;
                    }
                    break;
                case VfxKind::Interrupt:
                    Glow(e.pos, 0.2f, 0.8f, glm::vec4(0.5f, 0.8f, 1.0f, 0.9f), 0.15f);
                    Ring(e.pos, 0.15f, 0.9f, glm::vec4(0.6f, 0.85f, 1.0f, 0.8f), 0.2f);
                    break;
                case VfxKind::Text: {
                    FloatText t; t.pos = e.pos; t.text = e.text; t.color = e.color; t.life = 1.1f;
                    m_Texts.push_back(t);
                    if (m_Texts.size() > 40) m_Texts.erase(m_Texts.begin());
                    break;
                }
                }
            }
            q.clear();
        }

        // Proiettile in volo, disegnato in base al tipo di arma
        void DrawProjectile(const TrailRequestComponent& t, const glm::vec3& pos) {
            const glm::vec3 d = t.end - t.start;
            const float total = glm::length(d);
            if (total < 1e-3f) return;
            const glm::vec3 dir = d / total;
            const bool flying = t.currentAge <= t.travelTime;
            const float travelled = glm::length(pos - t.start);
            const glm::vec4 wc = t.color.a > 0.01f ? t.color : glm::vec4(1.0f, 0.2f, 0.1f, 1.0f);
            const float after = std::max(0.0f, t.currentAge - t.travelTime);
            const float lingerLife = std::max(0.01f, t.totalLife - t.travelTime);
            switch (static_cast<WeaponFx>(t.fx)) {
            case WeaponFx::Laser: case WeaponFx::HotShot: {
                if (!flying) return;
                const bool hot = t.fx == static_cast<int>(WeaponFx::HotShot);
                const float len = std::min(hot ? 3.2f : 2.4f, travelled);
                const float w = hot ? 0.11f : 0.075f;
                VR::Streak(pos - dir * len, pos, w, Hdr(wc, 0.0f), Hdr(wc, 1.0f));
                VR::Billboard(pos, hot ? 0.32f : 0.24f, Hdr(wc, 0.9f), VR::Glow);
                break;
            }
            case WeaponFx::LongLas: {   // raggio istantaneo che si assottiglia e sparisce
                const float k = std::clamp(t.currentAge / std::max(0.01f, t.totalLife), 0.0f, 1.0f);
                const float a = 1.0f - k;
                VR::Streak(t.start, t.end, 0.14f * (1.0f - 0.6f * k), Hdr(wc, a), Hdr(wc, a));
                VR::Streak(t.start, t.end, 0.5f * (1.0f - 0.5f * k), Hdr(wc, 0.25f * a), Hdr(wc, 0.25f * a), VR::Glow);
                break;
            }
            case WeaponFx::Ballistic: case WeaponFx::Shotgun: {
                if (!flying) return;
                const bool pellet = t.fx == static_cast<int>(WeaponFx::Shotgun);
                const float len = std::min(pellet ? 0.6f : 1.4f, travelled);
                if (len < 0.05f) return;
                VR::Streak(pos - dir * len, pos, pellet ? 0.03f : 0.045f, glm::vec4(1.0f, 0.7f, 0.3f, 0.0f), glm::vec4(1.0f, 0.92f, 0.7f, 1.0f));
                break;
            }
            case WeaponFx::Sniper: {
                // tracciante brillante + scia d'aria che resta e svanisce
                const float fadeA = flying ? 0.35f : 0.35f * (1.0f - std::clamp(after / lingerLife, 0.0f, 1.0f));
                VR::Streak(t.start, flying ? pos : t.end, 0.05f, glm::vec4(0.9f, 0.9f, 0.88f, fadeA * 0.5f), glm::vec4(0.9f, 0.9f, 0.88f, fadeA), VR::Ribbon, false);
                if (flying) {
                    const float len = std::min(2.5f, travelled);
                    VR::Streak(pos - dir * len, pos, 0.06f, glm::vec4(1.0f, 0.75f, 0.4f, 0.0f), glm::vec4(1.0f, 0.95f, 0.8f, 1.0f));
                }
                break;
            }
            case WeaponFx::Bolt: {
                const float smokeA = flying ? 0.45f : 0.45f * (1.0f - std::clamp(after / lingerLife, 0.0f, 1.0f));
                const float smokeLen = std::min(4.0f, travelled);
                const glm::vec3 head = flying ? pos : t.end;
                if (smokeLen > 0.1f) VR::Streak(head - dir * smokeLen, head - dir * 0.12f, 0.09f, glm::vec4(0.6f, 0.58f, 0.55f, 0.0f), glm::vec4(0.6f, 0.58f, 0.55f, smokeA), VR::Ribbon, false);
                if (flying) {
                    VR::Streak(pos - dir * 0.12f, pos + dir * 0.06f, 0.07f, glm::vec4(0.7f, 0.55f, 0.25f, 1.0f), glm::vec4(0.75f, 0.6f, 0.3f, 1.0f), VR::Disc, false);   // cartuccia
                    VR::Billboard(pos - dir * 0.16f, 0.22f, glm::vec4(1.0f, 0.65f, 0.2f, 1.0f), VR::Glow);                                                          // razzo
                    VR::Streak(pos - dir * 0.6f, pos - dir * 0.14f, 0.07f, glm::vec4(1.0f, 0.4f, 0.1f, 0.0f), glm::vec4(1.0f, 0.75f, 0.35f, 0.9f));                  // fiamma del razzo
                }
                break;
            }
            case WeaponFx::Plasma: {
                if (!flying) return;
                const float len = std::min(1.2f, travelled);
                const glm::vec4 c(0.45f, 0.8f, 1.0f, 1.0f);
                VR::Streak(pos - dir * len, pos, 0.18f, Hdr(c, 0.0f), Hdr(c, 0.8f));
                VR::Billboard(pos, 0.5f, Hdr(c, 0.9f), VR::Glow);
                VR::Billboard(pos, 0.2f, glm::vec4(1.0f), VR::Glow);
                break;
            }
            case WeaponFx::Melta: {   // raggio di calore: spesso, nucleo bianco, aria che trema
                const float k = std::clamp(t.currentAge / std::max(0.01f, t.totalLife), 0.0f, 1.0f);
                const float a = 1.0f - k * k;
                const glm::vec4 c(1.0f, 0.5f, 0.12f, 1.0f);
                VR::Streak(t.start, t.end, 0.22f, Hdr(c, a), Hdr(c, a * 0.8f));
                VR::Streak(t.start, t.end, 0.7f, Hdr(c, 0.18f * a), Hdr(c, 0.12f * a), VR::Glow);
                const int n = std::min(12, static_cast<int>(total / 1.2f) + 1);
                for (int i = 0; i < n; ++i) {
                    const float u = (i + 0.5f) / n;
                    VR::Billboard(glm::mix(t.start, t.end, u) + glm::vec3(0, 0.1f * std::sin(t.currentAge * 40.0f + i), 0), 0.45f,
                        glm::vec4(1.0f, 0.6f, 0.3f, 0.12f * a), VR::Flame, true, t.currentAge * 6.0f + i, static_cast<float>(i));
                }
                break;
            }
            case WeaponFx::Explosive: {
                const glm::vec3 head = flying ? pos : t.end;
                const float smokeA = flying ? 0.5f : 0.5f * (1.0f - std::clamp(after / lingerLife, 0.0f, 1.0f));
                const float len = std::min(6.0f, travelled);
                if (len > 0.1f) VR::Streak(head - dir * len, head, 0.25f, glm::vec4(0.55f, 0.53f, 0.5f, 0.0f), glm::vec4(0.55f, 0.53f, 0.5f, smokeA), VR::Ribbon, false);
                if (flying) { VR::Billboard(pos, 0.35f, glm::vec4(1.0f, 0.65f, 0.25f, 1.0f), VR::Glow); VR::Billboard(pos, 0.12f, glm::vec4(1.0f), VR::Glow); }
                break;
            }
            case WeaponFx::Grav: {
                if (!flying) return;
                VR::Billboard(pos, 0.5f, glm::vec4(0.7f, 0.45f, 1.0f, 0.8f), VR::Ring, true, t.currentAge * 10.0f);
                VR::Billboard(pos, 0.25f, glm::vec4(0.8f, 0.6f, 1.0f, 0.9f), VR::Glow);
                break;
            }
            default: break;
            }
        }

        // Vecchie scie (raggi continui, torrette): striscia luminosa del loro colore
        static void DrawLegacy(const TrailRequestComponent& t, const glm::vec3& pos) {
            const float k = std::clamp(t.currentAge / std::max(0.01f, t.totalLife), 0.0f, 1.0f);
            const glm::vec4 c = t.color;
            if (t.isBeam) {
                const float w = std::max(0.05f, t.projectileWidth);
                VR::Streak(t.start, pos, w, Hdr(c, c.a * (1.0f - k)), Hdr(c, c.a * (1.0f - k)));
                if (w > 0.08f) VR::Streak(t.start, pos, w * 3.0f, Hdr(c, 0.2f * (1.0f - k)), Hdr(c, 0.2f * (1.0f - k)), VR::Glow);
            }
            else VR::Billboard(pos, std::max(0.1f, t.projectileWidth * 2.0f), c, VR::Glow);
        }

        // Segnali della mischia: carica dell'attacco (anello che si stringe fino al momento del colpo, colore =
        // tipo di attacco), parata alzata, stordimento.
        void DrawMeleeTells(Scene& scene) {
            auto& reg = scene.GetRegistry();
            auto view = reg.view<MeleeStateComponent, TransformComponent>(entt::exclude<CorpseComponent>);
            for (auto e : view) {
                const auto& m = view.get<MeleeStateComponent>(e);
                if (m.currentPhase == MeleePhase::None) continue;
                const auto& t = view.get<TransformComponent>(e);
                float footY = t.Position.y, height = 1.8f;
                if (const auto* col = reg.try_get<ColliderComponent>(e)) {
                    height = std::max(0.8f, col->halfExtents.y * 2.0f * t.Scale.y);
                    footY = t.Position.y + (col->localCenterOffset.y - col->halfExtents.y) * t.Scale.y;
                }
                const float yaw = glm::radians(t.AimYaw);
                const glm::vec3 fwd(-std::sin(yaw), 0.0f, -std::cos(yaw)), right(std::cos(yaw), 0.0f, -std::sin(yaw));
                const glm::vec3 base(t.Position.x, footY, t.Position.z);
                if (m.currentPhase == MeleePhase::Stunned) {   // stelline sopra la testa
                    for (int i = 0; i < 3; ++i) {
                        const float a = m.stunTimer * 5.0f + i * 2.094f;
                        VR::Billboard(base + glm::vec3(std::cos(a) * 0.3f, height + 0.25f, std::sin(a) * 0.3f), 0.16f, glm::vec4(1.0f, 0.9f, 0.3f, 0.9f), VR::Glow);
                    }
                    continue;
                }
                if (m.currentAttack == MeleeAttackType::Parry) {
                    if (m.currentPhase == MeleePhase::Recovery) continue;
                    const glm::vec3 p = base + glm::vec3(0, height * 0.62f, 0) + fwd * 0.55f;
                    const float pulse = 0.85f + 0.15f * std::sin(m.stateTimer * 40.0f);
                    VR::Billboard(p, 0.9f * pulse, glm::vec4(0.75f, 0.9f, 1.0f, 0.55f), VR::Ring);
                    VR::Billboard(p, 0.7f, glm::vec4(0.6f, 0.8f, 1.0f, 0.25f), VR::Glow);
                    continue;
                }
                if (m.currentPhase != MeleePhase::Windup) continue;
                const auto* wbp = DamageResolver::ResolveMeleeWeapon(reg, e);
                float windup = 0.25f;
                if (wbp && wbp->hasMoveset) {
                    auto it = wbp->moveset.nodes.find(m.currentAttackNode);
                    if (it != wbp->moveset.nodes.end()) windup = DamageResolver::EffectiveTiming(it->second, m.currentAttack, m.currentAttackNode).windup;
                }
                const float k = std::clamp(m.stateTimer / std::max(0.01f, windup), 0.0f, 1.0f);
                const int variant = m.currentAttack == MeleeAttackType::Light ? 0 : (m.currentAttack == MeleeAttackType::Heavy ? 2 : 1);
                const glm::vec4 c = SlashColor(variant);
                const float hFrac = variant == 0 ? 0.3f : (variant == 2 ? 0.95f : 0.65f);
                const glm::vec3 p = base + glm::vec3(0, height * hFrac, 0) + fwd * 0.35f + right * (variant == 2 ? 0.0f : 0.3f);
                const float big = variant == 2 ? 1.8f : 1.0f;
                VR::Billboard(p, (0.15f + 0.35f * k) * big, Hdr(c, 0.35f + 0.6f * k), VR::Glow);
                VR::Billboard(p, glm::mix(1.2f * big, 0.25f, k), Hdr(c, 0.25f + 0.65f * k), VR::Ring);   // si stringe fino al colpo
                if (variant == 2 && k > 0.6f) VR::Billboard(p, 0.5f, glm::vec4(1.0f, 1.0f, 1.0f, (k - 0.6f) * 2.0f), VR::Glow);
            }
        }

    public:
        void Update(Scene& scene, float deltaTime) {
            Consume();
            // Colpi in volo: impatto all'arrivo, poi via alla fine della vita
            auto view = scene.GetRegistry().view<TrailRequestComponent>();
            std::vector<entt::entity> done;
            for (auto entity : view) {
                auto& trail = view.get<TrailRequestComponent>(entity);
                trail.currentAge += deltaTime;
                if (trail.fx >= 0 && trail.hitSomething && !trail.impactDone && trail.currentAge >= trail.travelTime) {
                    trail.impactDone = true;
                    const glm::vec3 dirIn = trail.end - trail.start;
                    const glm::vec3 front = trail.end - (glm::length(dirIn) > 1e-4f ? glm::normalize(dirIn) * 0.15f : glm::vec3(0.0f));
                    if (trail.fx == static_cast<int>(WeaponFx::Explosive)) SpawnExplosion(front + glm::vec3(0.0f, 0.3f, 0.0f), trail.impactScale);
                    else SpawnImpact(static_cast<WeaponFx>(trail.fx), front, dirIn, trail.hitUnit, trail.color);
                }
                if (trail.currentAge >= trail.totalLife) done.push_back(entity);
            }
            for (auto e : done) scene.DestroyEntity(e);

            for (auto& p : m_Particles) {
                p.age += deltaTime;
                p.vel.y -= p.gravity * deltaTime;
                p.vel *= std::max(0.0f, 1.0f - p.drag * deltaTime);
                p.pos += p.vel * deltaTime;
                p.rot += p.rotVel * deltaTime;
            }
            m_Particles.erase(std::remove_if(m_Particles.begin(), m_Particles.end(), [](const Particle& p) { return p.age >= p.life; }), m_Particles.end());

            for (auto& s : m_Slashes) {
                s.age += deltaTime;
                if (s.chain && s.age < s.duration + 0.05f) {   // spada a catena: scintille lungo il taglio
                    const float head = std::clamp(s.age / s.duration, 0.0f, 1.0f);
                    const glm::vec3 a = Bezier(s, head), b = Bezier(s, std::max(0.0f, head - 0.05f));
                    Sparks(a, a - b + glm::vec3(0, 0.05f, 0), 2, 0.8f, 2.0f, 5.0f, glm::vec4(1.0f, 0.8f, 0.4f, 1.0f), 0.25f);
                }
            }
            m_Slashes.erase(std::remove_if(m_Slashes.begin(), m_Slashes.end(), [](const Slash& s) { return s.age >= s.duration + s.fade; }), m_Slashes.end());

            for (auto& t : m_Texts) { t.age += deltaTime; t.pos.y += deltaTime * 0.7f; }
            m_Texts.erase(std::remove_if(m_Texts.begin(), m_Texts.end(), [](const FloatText& t) { return t.age >= t.life; }), m_Texts.end());
        }

        void Clear() { m_Particles.clear(); m_Slashes.clear(); m_Texts.clear(); VfxEventQueue().clear(); }
        const std::vector<FloatText>& Texts() const { return m_Texts; }

        void Render(Scene& scene, const glm::mat4& view, const glm::mat4& viewProj) {
            if (!VR::Ready()) VR::Init();
            VR::Begin(view, viewProj);

            auto trails = scene.GetRegistry().view<TrailRequestComponent>();
            for (auto entity : trails) {
                const auto& trail = trails.get<TrailRequestComponent>(entity);
                const float progress = std::clamp(trail.currentAge / std::max(0.001f, trail.travelTime), 0.0f, 1.0f);
                const glm::vec3 currentPos = glm::mix(trail.start, trail.end, progress);
                if (trail.fx >= 0) DrawProjectile(trail, currentPos);
                else DrawLegacy(trail, currentPos);
            }

            // Fendenti: nastro lungo l'arco, rivelato durante il colpo e poi sfumato. Un alone largo e tenue sotto,
            // il taglio luminoso sopra (piu' spesso verso la punta che avanza).
            for (const auto& s : m_Slashes) {
                const float head = std::clamp(s.age / s.duration, 0.0f, 1.0f);
                const float tail = std::clamp((s.age - s.duration * 0.35f) / (s.duration * 0.65f + s.fade), 0.0f, 1.0f);
                if (head - tail < 0.01f) continue;
                const float fadeA = s.age <= s.duration ? 1.0f : 1.0f - std::clamp((s.age - s.duration) / s.fade, 0.0f, 1.0f);
                constexpr int N = 18;
                std::vector<glm::vec3> pts; std::vector<float> w, wWide; std::vector<glm::vec4> cols, colsWide;
                for (int i = 0; i <= N; ++i) {
                    const float u = static_cast<float>(i) / N;
                    const float tt = glm::mix(tail, head, u);
                    pts.push_back(Bezier(s, tt));
                    // piu' spesso verso la punta che avanza, ma la punta stessa si chiude (niente taglio netto)
                    const float tip = 1.0f - std::pow(std::clamp((u - 0.82f) / 0.18f, 0.0f, 1.0f), 1.5f) * 0.85f;
                    w.push_back(s.width * (0.2f + 0.8f * u) * tip);
                    wWide.push_back(s.width * 2.2f * (0.25f + 0.75f * u) * tip);
                    cols.push_back(Hdr(s.color, fadeA * u * (0.35f + 0.65f * tip)));
                    colsWide.push_back(Hdr(s.color, 0.22f * fadeA * u * tip));
                }
                VR::RibbonStrip(pts, wWide, colsWide, VR::Ribbon, true);
                VR::RibbonStrip(pts, w, cols, VR::Ribbon, true);
            }

            DrawMeleeTells(scene);

            for (const auto& p : m_Particles) {
                const float k = std::clamp(p.age / std::max(0.001f, p.life), 0.0f, 1.0f);
                const glm::vec4 c = glm::mix(p.col0, p.col1, k);
                const float size = glm::mix(p.size0, p.size1, k);
                if (p.streak > 0.0f) {
                    VR::Streak(p.pos - p.vel * p.streak, p.pos, size, glm::vec4(glm::vec3(c), 0.0f), c, VR::Spark, p.additive);
                    continue;
                }
                if (p.flat) VR::Flat(p.pos, size, c, static_cast<VR::Shape>(p.shape), p.additive, p.rot, p.seed);
                else VR::Billboard(p.pos, size, c, static_cast<VR::Shape>(p.shape), p.additive, p.rot, p.seed);
            }
            VR::End();
        }
    };

}
