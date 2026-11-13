#pragma once
// Leggi dell'impugnatura: dove sta l'arma rispetto al corpo e dove punta la canna.
//
// Prima l'arma veniva messa "fra le due mani" della clip in corso: finiva davanti alla faccia, sempre puntata
// sulla mira anche correndo, rigida. Ora la posizione nasce dalle SPALLE e dallo stato del soldato; le mani la
// raggiungono dopo con l'IK (GameLayer::ApplyHandIK). Nessuna parte OpenGL: solo geometria.
//
//   1. Una sola direzione: quella della canna. Mirando (o subito dopo un colpo) e' la linea di mira.
//   2. Fucili (due mani): il calcio sta nell'incavo della spalla forte, la canna parte da li'.
//        - mira:          canna sulla linea di mira
//        - pronto basso:  canna giu' di 30 gradi e un po' verso il lato debole (chi cammina o sta fermo)
//        - corsa:         arma di traverso al petto, canna in alto verso il lato debole
//   3. Armi a una mano: mirando il braccio si stende verso il bersaglio all'altezza degli occhi; a riposo
//      l'arma scende lungo il fianco con la canna verso terra.
//   4. Armi bianche: la lama prosegue l'avambraccio e sale in guardia; segue il braccio in ogni colpo.
//   5. Inerzia: l'arma arriva un attimo dopo il corpo nelle girate, molleggia col passo, respira, rincula.
//   6. Il colpo parte dalla bocca della canna (MuzzleComponent, riempito da chi posa l'arma).
//
// Spazio del modello dell'arma (BlockModel.h): canna verso -Z, +Y alto, +X destra di chi la impugna.
#include "BlockModel.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace SunEngine {

    enum class WeaponHoldKind { Rifle = 0, Pistol, Melee };

    // Corpo di chi impugna, nel mondo
    struct WeaponHoldBody {
        glm::vec3 shoulder{ 0.0f };        // spalla del lato che impugna
        glm::vec3 otherShoulder{ 0.0f };
        glm::vec3 elbow{ 0.0f }, hand{ 0.0f };   // braccio che impugna, nella posa del fotogramma
        glm::vec3 fwd{ 0.0f, 0.0f, -1.0f };      // dove guarda il corpo (orizzontale)
        float armLen = 0.6f;               // braccio + avambraccio (metri)
    };

    // Punti dell'arma nello spazio del suo modello
    struct WeaponHoldGrips {
        glm::vec3 grip{ 0.0f };            // impugnatura (mano forte)
        glm::vec3 butt{ 0.0f };            // fondo del calcio
        bool hasStock = false;             // c'e' un calcio da appoggiare alla spalla
    };

    struct WeaponHoldInput {
        WeaponHoldKind kind = WeaponHoldKind::Rifle;
        glm::vec3 aimDir{ 0.0f, 0.0f, -1.0f };
        bool aiming = false;               // mira (o ha appena sparato)
        bool sprinting = false;
        bool shot = false;                 // colpo partito in questo fotogramma
        bool leftHand = false;             // arma nella mano sinistra
        float speed = 0.0f;                // m/s a terra
        float dt = 0.0f;
    };

    // Memoria fra un fotogramma e l'altro (una per arma impugnata)
    struct WeaponHoldState {
        bool init = false;
        float aim = 0.0f, sprint = 0.0f;
        float phase = 0.0f, time = 0.0f;
        float recoil = 0.0f;
        float lagYaw = 0.0f, lagPitch = 0.0f;   // gradi
        float lastYaw = 0.0f, lastPitch = 0.0f;
        float sinceShot = 99.0f;
    };

    struct WeaponHoldResult {
        glm::mat4 basis{ 1.0f };           // rotazione dell'arma nel mondo
        glm::vec3 position{ 0.0f };        // origine del modello nel mondo
    };

    // Impugnatura e calcio dal modello a blocchi: il calcio e' il pezzo che arriva piu' indietro (+Z)
    inline WeaponHoldGrips WeaponGripsOf(const BlockModel& m) {
        WeaponHoldGrips g;
        g.grip = m.hasGripRight ? m.gripRight : glm::vec3(0.0f);
        float rear = -1e9f;
        for (const auto& p : m.parts) {
            const float r = p.offset.z + std::fabs(p.size.z) * 0.5f;
            if (r > rear) { rear = r; g.butt = glm::vec3(0.0f, p.offset.y, r); }
        }
        g.hasStock = m.hasGripLeft && rear - g.grip.z > 0.14f;
        return g;
    }

    namespace WeaponHoldDetail {
        inline glm::vec3 SafeNorm(const glm::vec3& v, const glm::vec3& fallback) {
            const float l = glm::length(v);
            return l > 1e-5f ? v / l : fallback;
        }
        // Base dell'arma: canna lungo dir, alto verso upRef
        inline glm::mat4 Basis(const glm::vec3& dir, const glm::vec3& upRef) {
            const glm::vec3 z = -dir;
            glm::vec3 x = glm::cross(upRef, z);
            if (glm::length(x) < 1e-3f) x = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), z);
            x = glm::normalize(x);
            const glm::vec3 y = glm::cross(z, x);
            glm::mat4 b(1.0f);
            b[0] = glm::vec4(x, 0.0f); b[1] = glm::vec4(y, 0.0f); b[2] = glm::vec4(z, 0.0f);
            return b;
        }
        // Alza (+) o abbassa (-) una direzione di 'deg' gradi
        inline glm::vec3 Pitch(const glm::vec3& dir, float deg) {
            const glm::vec3 up(0.0f, 1.0f, 0.0f);
            glm::vec3 perp = up - dir * glm::dot(up, dir);
            if (glm::length(perp) < 1e-4f) return dir;
            perp = glm::normalize(perp);
            const float a = glm::radians(deg);
            return glm::normalize(dir * std::cos(a) + perp * std::sin(a));
        }
        inline glm::vec3 Yaw(const glm::vec3& dir, float deg) {
            const float a = glm::radians(deg), c = std::cos(a), s = std::sin(a);
            return glm::vec3(dir.x * c + dir.z * s, dir.y, -dir.x * s + dir.z * c);
        }
        inline float WrapDeg(float d) { while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f; return d; }
    }

    inline WeaponHoldResult SolveWeaponHold(WeaponHoldState& s, const WeaponHoldBody& b, const WeaponHoldGrips& g, const WeaponHoldInput& in) {
        using namespace WeaponHoldDetail;
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const float dt = std::clamp(in.dt, 0.0f, 0.1f);
        const glm::vec3 aimDir = SafeNorm(in.aimDir, glm::vec3(0.0f, 0.0f, -1.0f));
        const glm::vec3 fwd = SafeNorm(glm::vec3(b.fwd.x, 0.0f, b.fwd.z), glm::vec3(0.0f, 0.0f, -1.0f));
        const glm::vec3 right = glm::normalize(glm::cross(fwd, up));
        const glm::vec3 weak = in.leftHand ? right : -right;            // verso il lato debole
        const glm::vec3 across = b.otherShoulder - b.shoulder;
        const float width = glm::length(across);
        const glm::vec3 inward = width > 1e-4f ? across / width : weak;
        const float k = std::clamp(width / 0.36f, 0.6f, 2.6f);          // taglia del corpo (1 = uomo)
        WeaponHoldResult out;

        // --- armi bianche: la lama prosegue l'avambraccio e sale in guardia ---
        if (in.kind == WeaponHoldKind::Melee) {
            const glm::vec3 fore = SafeNorm(b.hand - b.elbow, fwd);
            const glm::vec3 dir = glm::normalize(fore * 0.55f + up * 0.80f + fwd * 0.25f);
            const glm::vec3 palm = b.hand + (b.hand - b.elbow) * 0.3f;
            out.basis = Basis(dir, SafeNorm(-fore + up * 0.2f, up));
            out.position = palm - glm::vec3(out.basis * glm::vec4(g.grip, 0.0f));
            return out;
        }

        // --- stato: mira, corsa, colpo ---
        s.sinceShot = in.shot ? 0.0f : s.sinceShot + dt;
        const bool aimNow = in.aiming || s.sinceShot < 0.9f;            // chi ha appena sparato tiene l'arma in mira
        const float yawNow = glm::degrees(std::atan2(-aimDir.x, -aimDir.z));
        const float pitchNow = glm::degrees(std::asin(std::clamp(aimDir.y, -1.0f, 1.0f)));
        if (!s.init) { s.init = true; s.aim = aimNow ? 1.0f : 0.0f; s.lastYaw = yawNow; s.lastPitch = pitchNow; }
        auto approach = [&](float& v, float target, float rate) { v += (target - v) * std::min(1.0f, rate * dt); };
        approach(s.aim, aimNow ? 1.0f : 0.0f, aimNow ? 14.0f : 5.0f);
        approach(s.sprint, (in.sprinting && !aimNow) ? 1.0f : 0.0f, 6.0f);
        s.time += dt;
        if (in.shot) s.recoil = 1.0f;
        s.recoil *= std::exp(-dt * 12.0f);
        const float a = s.aim * s.aim * (3.0f - 2.0f * s.aim);
        const float sp = s.sprint * s.sprint * (3.0f - 2.0f * s.sprint) * (1.0f - a);

        glm::vec3 dirA = aimDir, dirL, dirS, gA, gL, gS;
        float cant = 0.0f;                                              // gradi: l'arma si corica verso il lato debole
        if (in.kind == WeaponHoldKind::Rifle) {
            const glm::vec3 aimH = SafeNorm(glm::vec3(aimDir.x, 0.0f, aimDir.z), fwd);
            dirL = Pitch(glm::normalize(fwd * std::cos(glm::radians(12.0f)) + weak * std::sin(glm::radians(12.0f))), -30.0f);
            dirS = glm::normalize(weak * 0.76f + up * 0.46f + fwd * 0.30f);
            const glm::mat4 bA = Basis(dirA, up), bL = Basis(dirL, up);
            if (g.hasStock) {
                const glm::vec3 toGrip = g.grip - g.butt;
                gA = b.shoulder + inward * (0.035f * k) - up * (0.045f * k) + aimH * (0.02f * k) + glm::vec3(bA * glm::vec4(toGrip, 0.0f));
                gL = b.shoulder + inward * (0.03f * k) - up * (0.085f * k) + fwd * (0.02f * k) + glm::vec3(bL * glm::vec4(toGrip, 0.0f));
            }
            else {   // senza calcio: l'impugnatura sta davanti alla spalla
                gA = b.shoulder + inward * (0.07f * k) - up * (0.10f * k) + dirA * (0.26f * k);
                gL = b.shoulder + inward * (0.06f * k) - up * (0.22f * k) + fwd * (0.20f * k);
            }
            const glm::vec3 chest = (b.shoulder + b.otherShoulder) * 0.5f;
            gS = chest - weak * (0.12f * k) - up * (0.24f * k) + fwd * (0.20f * k);
            cant = glm::mix(8.0f * (1.0f - a), 16.0f, sp);
        }
        else {
            dirL = Pitch(fwd, -48.0f);
            dirS = Pitch(fwd, -60.0f);
            gA = b.shoulder + inward * (0.11f * k) + up * (0.05f * k) + dirA * (b.armLen * 0.80f);
            gL = b.shoulder - up * (b.armLen * 0.60f) + fwd * (b.armLen * 0.34f) - inward * (0.02f * k);
            gS = b.shoulder - up * (b.armLen * 0.50f) + fwd * (b.armLen * 0.22f) - inward * (0.03f * k);
        }
        glm::vec3 dir = SafeNorm(glm::mix(glm::mix(dirL, dirA, a), dirS, sp), dirA);
        glm::vec3 grip = glm::mix(glm::mix(gL, gA, a), gS, sp);

        // --- inerzia: ritardo nelle girate, passo, respiro, rinculo ---
        const float yawVel = dt > 1e-5f ? WrapDeg(yawNow - s.lastYaw) / dt : 0.0f;
        const float pitchVel = dt > 1e-5f ? (pitchNow - s.lastPitch) / dt : 0.0f;
        s.lastYaw = yawNow; s.lastPitch = pitchNow;
        approach(s.lagYaw, std::clamp(-yawVel * 0.035f, -9.0f, 9.0f), 9.0f);
        approach(s.lagPitch, std::clamp(-pitchVel * 0.03f, -6.0f, 6.0f), 9.0f);
        const float loose = 1.0f - 0.65f * a;                           // mirando l'arma e' piu' ferma
        dir = Pitch(Yaw(dir, s.lagYaw * loose), s.lagPitch * loose);
        s.phase += in.speed * dt * 4.2f;                                // un ciclo ogni due passi (~1,5 m)
        if (s.phase > 6283.0f) s.phase -= 6283.0f;
        const float step = std::clamp(in.speed / 3.5f, 0.0f, 1.0f) * (1.0f - 0.7f * a);
        grip += up * (std::sin(2.0f * s.phase) * 0.010f * k * step) + right * (std::sin(s.phase) * 0.007f * k * step);
        dir = Pitch(dir, std::sin(2.0f * s.phase + 0.6f) * 0.9f * step);
        grip += up * (std::sin(s.time * 1.7f) * 0.0025f * k * (1.0f - 0.5f * step));
        dir = Pitch(dir, std::sin(s.time * 1.7f + 0.8f) * 0.35f);
        grip -= dir * (0.028f * k * s.recoil);
        dir = Pitch(dir, 2.4f * s.recoil);

        const float c = glm::radians(cant);
        out.basis = Basis(dir, glm::normalize(up * std::cos(c) + weak * std::sin(c)));
        out.position = grip - glm::vec3(out.basis * glm::vec4(g.grip, 0.0f));
        return out;
    }

}
