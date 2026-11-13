#pragma once
// Modo di muoversi di ogni razza, sopra le clip (che sono le stesse per tutti): postura, passo, molleggio,
// dondolio, braccia, testa, respiro e tic dell'idle, piu' le ossa in piu' (orecchie, mascella, pancia, spallacci)
// con un po' di moto secondario a molla.
//
//   Umano (guardsman)  soldato addestrato: dritto, passo regolare, si guarda attorno con calma
//   Ork                gorilla spavaldo: gobbo, spalle avanti, braccia larghe, rolla di lato e torce il busto a
//                      ogni passo; in carica abbassa la testa e urla; ringhia nell'idle
//   Gretchin           furtivo e nervoso: accucciato, passi corti e rapidi, scatti della testa, si volta a
//                      guardarsi le spalle, orecchie che sbattono, chiacchiera
//   Astartes           semidio in armatura: eretto, petto in fuori, passo pesante e preciso con un affondo a ogni
//                      appoggio, immobile quando mira, scansiona lentamente
//   Aspirante / scout  agile e attento: leggermente chino in avanti, passo rapido e leggero
//   Ogryn              montagna lenta: braccia che non toccano i fianchi, gambe larghe, ondeggia pesante da un
//                      lato all'altro, bocca socchiusa, pancia che ballonzola, testa che si inclina
//
// Rotazioni nello spazio della mesh in posa (vedi ProceduralPose.h): Rx(+) piega avanti la colonna, Rx(-) alza
// in avanti cosce e braccia; Ry(+) ruota a sinistra; Rz(+) porta il braccio/la gamba sinistra in fuori.
#include "ProceduralPose.h"
#include <cmath>
#include <algorithm>

namespace SunEngine {

    enum class RaceMotionKind { Human = 0, Ork, Gretchin, Astartes, Neophyte, Ogryn };

    struct RaceMotionProfile {
        float hunch = 0.0f;          // piega in avanti della schiena (gradi, divisi sulle vertebre)
        float headUp = 0.0f;         // compenso di collo e testa (guarda avanti nonostante la gobba)
        float shoulderFwd = 0.0f;    // spalle in avanti/chiuse (+) o indietro, petto in fuori (-)
        float armOut = 0.0f;         // braccia aperte di lato (gradi)
        float stance = 0.0f;         // gambe divaricate (gradi)
        float knee = 0.0f;           // ginocchia piegate (gradi)
        float sway = 0.0f;           // rollio laterale del busto a ogni passo (gradi)
        float swagger = 0.0f;        // torsione del busto a ogni passo (gradi)
        float bob = 0.0f;            // molleggio delle anche (frazione della lunghezza della gamba)
        float heavyStep = 0.0f;      // 0 molleggio dolce, 1 affondo secco all'appoggio
        float strideLen = 1.4f;      // lunghezza del passo (in lunghezze di gamba): cadenza dei cicli
        float rateMult = 1.0f;       // velocita' delle clip
        float breath = 1.0f;         // ampiezza del respiro nell'idle (gradi)
        float breathRate = 0.25f;    // respiri al secondo
        float lookAmp = 20.0f;       // quanto gira la testa guardandosi attorno (gradi)
        float lookEvery = 3.0f;      // ogni quanto cambia sguardo (secondi)
        float lookSpeed = 3.0f;      // velocita' con cui la testa raggiunge lo sguardo
        float headTilt = 0.0f;       // testa inclinata di lato (gradi): l'ogryn tonto
        float aimSteady = 0.8f;      // quanto lo stile si spegne mirando (0 = niente, 1 = tutto)
        float chargeHunch = 0.0f;    // gobba in piu' correndo/caricando
        // ossa in piu'
        bool ears = false, jaw = false, belly = false, pauldrons = false;
        float jawIdle = 0.0f;        // bocca socchiusa a riposo (gradi)
        float jawChatter = 0.0f;     // chiacchiericcio (gradi)
    };

    inline RaceMotionProfile GetRaceMotionProfile(RaceMotionKind k) {
        RaceMotionProfile p;
        switch (k) {
        case RaceMotionKind::Human:
            p.hunch = 2.0f; p.sway = 1.5f; p.swagger = 2.0f; p.bob = 0.012f; p.strideLen = 1.4f;
            p.breath = 1.2f; p.lookAmp = 22.0f; p.lookEvery = 3.5f; p.lookSpeed = 2.5f;
            break;
        case RaceMotionKind::Ork:
            p.hunch = 18.0f; p.headUp = 14.0f; p.shoulderFwd = 9.0f; p.armOut = 11.0f; p.stance = 5.0f; p.knee = 9.0f;
            p.sway = 6.0f; p.swagger = 9.0f; p.bob = 0.018f; p.heavyStep = 0.5f; p.strideLen = 1.25f; p.rateMult = 0.95f;
            p.breath = 2.4f; p.breathRate = 0.32f; p.lookAmp = 35.0f; p.lookEvery = 2.2f; p.lookSpeed = 4.0f;
            p.aimSteady = 0.5f; p.chargeHunch = 12.0f; p.jaw = true; p.jawIdle = 3.0f;
            break;
        case RaceMotionKind::Gretchin:
            p.hunch = 20.0f; p.headUp = 16.0f; p.shoulderFwd = 12.0f; p.armOut = 3.0f; p.stance = 3.0f; p.knee = 17.0f;
            p.sway = 3.5f; p.swagger = 4.0f; p.bob = 0.024f; p.strideLen = 0.95f; p.rateMult = 1.3f;
            p.breath = 1.6f; p.breathRate = 0.6f; p.lookAmp = 55.0f; p.lookEvery = 0.7f; p.lookSpeed = 14.0f;
            p.aimSteady = 0.4f; p.chargeHunch = 8.0f; p.ears = true; p.jaw = true; p.jawIdle = 2.0f; p.jawChatter = 5.0f;
            break;
        case RaceMotionKind::Astartes:
            p.hunch = -5.0f; p.headUp = -3.0f; p.shoulderFwd = -7.0f; p.armOut = 10.0f; p.stance = 7.0f; p.knee = 5.0f;
            p.sway = 0.8f; p.swagger = 1.5f; p.bob = 0.014f; p.heavyStep = 1.0f; p.strideLen = 1.5f; p.rateMult = 0.88f;
            p.breath = 0.6f; p.breathRate = 0.18f; p.lookAmp = 28.0f; p.lookEvery = 4.5f; p.lookSpeed = 1.4f;
            p.aimSteady = 1.0f; p.chargeHunch = 6.0f; p.pauldrons = true;
            break;
        case RaceMotionKind::Neophyte:
            p.hunch = 5.0f; p.headUp = 3.0f; p.knee = 4.0f; p.sway = 1.2f; p.swagger = 2.5f; p.bob = 0.011f;
            p.strideLen = 1.45f; p.rateMult = 1.06f; p.breath = 1.0f; p.lookAmp = 30.0f; p.lookEvery = 2.6f; p.lookSpeed = 4.0f;
            break;
        case RaceMotionKind::Ogryn:
            p.hunch = 11.0f; p.headUp = 8.0f; p.shoulderFwd = 5.0f; p.armOut = 17.0f; p.stance = 10.0f; p.knee = 9.0f;
            p.sway = 8.0f; p.swagger = 3.0f; p.bob = 0.02f; p.heavyStep = 0.8f; p.strideLen = 1.15f; p.rateMult = 0.8f;
            p.breath = 2.8f; p.breathRate = 0.16f; p.lookAmp = 18.0f; p.lookEvery = 5.0f; p.lookSpeed = 0.9f; p.headTilt = 7.0f;
            p.aimSteady = 0.6f; p.chargeHunch = 8.0f; p.jaw = true; p.belly = true; p.jawIdle = 6.0f;
            break;
        }
        return p;
    }

    // Stato per istanza (fase del passo, sguardo, molle delle ossa in piu')
    struct RaceMotionState {
        bool init = false;
        float phase = 0.0f, time = 0.0f;
        float moveBlend = 0.0f, aimBlend = 0.0f, chargeBlend = 0.0f;
        float lookYaw = 0.0f, lookPitch = 0.0f, lookTargetYaw = 0.0f, lookTargetPitch = 0.0f, lookTimer = 0.0f;
        float lastBob = 0.0f, bobVel = 0.0f;
        float lastHeadYaw = 0.0f;
        float earAngle[2] = { 0.0f, 0.0f }, earVel[2] = { 0.0f, 0.0f };
        float bellyAngle = 0.0f, bellyVel = 0.0f;
        float pauldron = 0.0f, pauldronVel = 0.0f;
        float jawOpen = 0.0f, chatterTimer = 0.0f, chatterTarget = 0.0f;
        // inerzia del corpo: velocita' lisciata nel riferimento del corpo, piegamenti a molla
        float velFwd = 0.0f, velSide = 0.0f;
        float leanFwd = 0.0f, leanFwdVel = 0.0f, leanSide = 0.0f, leanSideVel = 0.0f;
        float turnLean = 0.0f, aimPitch = 0.0f;
        unsigned int seed = 1u;
        float Rand() { seed = seed * 1664525u + 1013904223u; return static_cast<float>((seed >> 8) & 0xffff) / 65535.0f; }
    };

    struct RaceMotionInput {
        float dt = 0.0f;
        float speed = 0.0f;          // m/s
        float legLen = 0.0f;         // lunghezza della gamba (unita' del modello) per il molleggio
        float modelUnitsPerMeter = 100.0f;
        bool aiming = false, sprinting = false, crouching = false;
        bool roar = false;           // urlo: carica, WAAAGH!, colpo pesante
        float yaw = 0.0f;            // orientamento del corpo (gradi): la testa reagisce alle girate (molle)
        float velFwd = 0.0f, velSide = 0.0f;   // velocita' nel riferimento del corpo (m/s; lato + = destra)
        float aimPitch = 0.0f;       // dove mira in altezza (gradi, + = in alto): il busto accompagna l'arma
        bool inertia = false;        // true = applica l'inerzia del corpo (chi chiama ha riempito velFwd / velSide)
    };

    namespace RaceMotionDetail {
        // Molla smorzata verso target
        inline void Spring(float& x, float& v, float target, float stiffness, float damping, float dt) {
            const float a = (target - x) * stiffness - v * damping;
            v += a * dt; x += v * dt;
        }
    }

    // Applica lo stile alla posa (sopra le clip e prima delle azioni). Ritorna il moltiplicatore della velocita'
    // delle clip.
    inline float ApplyRaceMotion(ProceduralPose& pose, const RaceMotionProfile& p, RaceMotionState& s, const RaceMotionInput& in) {
        using namespace PoseLib;
        using RaceMotionDetail::Spring;
        const float dt = std::clamp(in.dt, 0.0f, 0.1f);
        if (!s.init) { s.init = true; s.time = s.Rand() * 10.0f; s.phase = s.Rand() * 6.28f; s.lastHeadYaw = in.yaw; }
        s.time += dt;
        const bool moving = in.speed > 0.35f;
        auto approach = [&](float& v, float target, float rate) { v += (target - v) * std::min(1.0f, rate * dt); };
        approach(s.moveBlend, moving ? 1.0f : 0.0f, 6.0f);
        approach(s.aimBlend, in.aiming ? 1.0f : 0.0f, 8.0f);
        approach(s.chargeBlend, in.sprinting ? 1.0f : 0.0f, 4.0f);

        // fase del passo: avanza con la strada fatta (resta in sincrono con le clip agganciate alla velocita')
        const float legM = std::max(0.2f, in.legLen / std::max(1.0f, in.modelUnitsPerMeter));
        if (moving) s.phase += in.speed * dt / (p.strideLen * legM) * 3.14159f;
        const float st = std::sin(s.phase);
        const float steady = 1.0f - p.aimSteady * s.aimBlend;           // mirando lo stile si spegne
        const float mv = s.moveBlend;
        const float crouchK = in.crouching ? 0.4f : 1.0f;               // accovacciato: postura piu' contenuta

        // --- postura ---
        const float hunch = (p.hunch + p.chargeHunch * s.chargeBlend) * (0.55f + 0.45f * steady) * crouchK;
        pose.Rotate("Spine", Rx(hunch * 0.4f));
        pose.Rotate("Spine1", Rx(hunch * 0.3f));
        pose.Rotate("Spine2", Rx(hunch * 0.3f));
        const float headUp = (p.headUp + p.chargeHunch * 0.7f * s.chargeBlend) * (0.55f + 0.45f * steady) * crouchK;
        pose.Rotate("Neck", Rx(-headUp * 0.5f));
        pose.Rotate("Head", Rx(-headUp * 0.5f));
        if (std::fabs(p.shoulderFwd) > 0.01f) {
            pose.Rotate("LeftShoulder", Ry(-p.shoulderFwd * steady));
            pose.Rotate("RightShoulder", Ry(p.shoulderFwd * steady));
        }
        if (p.armOut > 0.01f) {   // braccia larghe (si attenua mirando: le mani vanno sull'arma)
            const float a = p.armOut * steady;
            pose.Rotate("LeftArm", Rz(a));
            pose.Rotate("RightArm", Rz(-a));
        }
        if (p.stance > 0.01f) {
            const float w = p.stance * crouchK;
            pose.Rotate("LeftUpLeg", Rz(w)); pose.Rotate("RightUpLeg", Rz(-w));
            pose.Rotate("LeftFoot", Rz(-w)); pose.Rotate("RightFoot", Rz(w));
        }
        if (p.knee > 0.01f && !in.crouching) {
            const float k = p.knee;
            pose.Rotate("LeftUpLeg", Rx(-k)); pose.Rotate("RightUpLeg", Rx(-k));
            pose.Rotate("LeftLeg", Rx(2.0f * k)); pose.Rotate("RightLeg", Rx(2.0f * k));
            pose.Rotate("LeftFoot", Rx(-k)); pose.Rotate("RightFoot", Rx(-k));
            const float drop = in.legLen * 0.5f * (1.0f - std::cos(glm::radians(k))) * 2.0f;
            pose.hipsOffset.y -= drop;
        }

        // --- passo: rollio, torsione, molleggio ---
        if (mv > 0.01f) {
            pose.Rotate("Spine", Rz(p.sway * st * mv * steady));
            pose.Rotate("Spine1", Ry(p.swagger * st * mv * steady));
            pose.Rotate("Head", Rz(-p.sway * 0.6f * st * mv));            // la testa resta piu' ferma del busto
            pose.Rotate("Neck", Ry(-p.swagger * 0.5f * st * mv));
            // molleggio: dolce (coseno) o affondo secco all'appoggio (|sin|^4)
            const float soft = 0.5f - 0.5f * std::cos(2.0f * s.phase);
            const float hard = std::pow(std::fabs(std::cos(s.phase)), 6.0f);
            const float dip = glm::mix(soft, hard, p.heavyStep);
            pose.hipsOffset.y -= p.bob * in.legLen * dip * mv * (in.sprinting ? 1.4f : 1.0f);
            pose.hipsOffset.x += p.sway * 0.0035f * in.legLen * st * mv;
        }
        const float bobNow = pose.hipsOffset.y;
        const float bobVel = dt > 0.0f ? (bobNow - s.lastBob) / dt : 0.0f;
        const float bobAcc = dt > 0.0f ? (bobVel - s.bobVel) / dt : 0.0f;
        s.lastBob = bobNow; s.bobVel = bobVel;

        // --- idle: respiro, sguardo, testa inclinata ---
        const float idle = (1.0f - mv) * (1.0f - s.aimBlend);
        const float breath = std::sin(s.time * p.breathRate * 6.28318f);
        pose.Rotate("Spine2", Rx(-p.breath * breath * (0.4f + 0.6f * idle)));
        pose.Rotate("LeftShoulder", Rz(p.breath * 0.4f * breath * idle));
        pose.Rotate("RightShoulder", Rz(-p.breath * 0.4f * breath * idle));
        s.lookTimer -= dt;
        if (s.lookTimer <= 0.0f) {
            s.lookTimer = p.lookEvery * (0.5f + s.Rand());
            const bool glance = p.lookEvery < 1.0f && s.Rand() < 0.15f;   // gretchin: occhiata alle spalle
            s.lookTargetYaw = (s.Rand() * 2.0f - 1.0f) * p.lookAmp * (glance ? 2.2f : 1.0f);
            s.lookTargetPitch = (s.Rand() * 2.0f - 1.0f) * p.lookAmp * 0.25f;
        }
        const float lookK = idle * (1.0f - s.aimBlend);
        approach(s.lookYaw, s.lookTargetYaw * lookK, p.lookSpeed);
        approach(s.lookPitch, s.lookTargetPitch * lookK, p.lookSpeed);
        pose.Rotate("Neck", Ry(s.lookYaw * 0.4f));
        pose.Rotate("Head", Ry(s.lookYaw * 0.6f) * Rx(s.lookPitch));
        if (p.headTilt > 0.01f) pose.Rotate("Head", Rz(p.headTilt * (0.6f + 0.4f * std::sin(s.time * 0.21f)) * (1.0f - s.aimBlend)));

        // girate del corpo: velocita' angolare per le molle
        float dyaw = in.yaw - s.lastHeadYaw;
        while (dyaw > 180.0f) dyaw -= 360.0f;
        while (dyaw < -180.0f) dyaw += 360.0f;
        s.lastHeadYaw = in.yaw;
        const float yawVel = dt > 0.0f ? dyaw / dt : 0.0f;

        // --- inerzia: un corpo ha peso. Si piega in avanti quando parte, si raddrizza indietro quando frena (con
        // un piccolo assestamento, e' una molla), si inclina di lato negli scarti e dentro le curve correndo, e il
        // busto accompagna l'arma quando mira in alto o in basso invece di restare dritto come un palo.
        if (in.inertia && dt > 1e-5f) {
            const float k = std::min(1.0f, dt * 12.0f);
            const float prevF = s.velFwd, prevS = s.velSide;
            s.velFwd += (in.velFwd - s.velFwd) * k;
            s.velSide += (in.velSide - s.velSide) * k;
            const float accF = std::clamp((s.velFwd - prevF) / dt, -14.0f, 14.0f);
            const float accS = std::clamp((s.velSide - prevS) / dt, -14.0f, 14.0f);
            const float run = std::clamp(s.velFwd / 6.0f, -0.3f, 1.0f);
            Spring(s.leanFwd, s.leanFwdVel, accF * 0.9f + run * 4.0f, 70.0f, 11.0f, dt);
            Spring(s.leanSide, s.leanSideVel, accS * 0.8f + s.velSide * 1.2f, 70.0f, 11.0f, dt);
            s.leanFwd = std::clamp(s.leanFwd, -8.0f, 11.0f);
            s.leanSide = std::clamp(s.leanSide, -8.0f, 8.0f);
            approach(s.turnLean, std::clamp(yawVel * std::max(0.0f, s.velFwd) * 0.006f, -7.0f, 7.0f), 6.0f);
            approach(s.aimPitch, in.aimPitch, 10.0f);
            const float hold = in.crouching ? 0.5f : 1.0f;
            pose.Rotate("Spine", Rx(s.leanFwd * 0.5f * hold) * Rz((s.leanSide * 0.5f - s.turnLean * 0.6f) * hold));
            pose.Rotate("Spine1", Rx(s.leanFwd * 0.3f * hold) * Rz((s.leanSide * 0.3f - s.turnLean * 0.4f) * hold));
            pose.Rotate("Head", Rx(-s.leanFwd * 0.5f * hold) * Rz(-(s.leanSide * 0.5f - s.turnLean * 0.6f) * hold));   // lo sguardo resta dritto
            pose.hipsOffset.y -= std::fabs(s.leanFwd) * 0.0012f * in.legLen;      // chi si piega si abbassa un filo
            // busto dietro la mira: meta' dell'alzo sulle vertebre, un po' sul collo
            const float ap = std::clamp(s.aimPitch, -60.0f, 60.0f) * (0.35f + 0.25f * s.aimBlend);
            pose.Rotate("Spine", Rx(-ap * 0.25f));
            pose.Rotate("Spine1", Rx(-ap * 0.35f));
            pose.Rotate("Spine2", Rx(-ap * 0.40f));
            pose.Rotate("Neck", Rx(-s.aimPitch * 0.15f));
            pose.Rotate("Head", Rx(-s.aimPitch * 0.20f));
        }
        const float headYawVel = yawVel + (s.lookTargetYaw - s.lookYaw) * p.lookSpeed * lookK;

        // --- ossa in piu' ---
        if (p.ears) {
            // orecchie: pendono un po', sbattono con i passi e con gli scatti della testa (molla molle)
            for (int i = 0; i < 2; ++i) {
                const float side = i == 0 ? 1.0f : -1.0f;
                const float target = -bobAcc * 0.0009f + headYawVel * 0.05f * side + (in.sprinting ? -10.0f : 0.0f);
                Spring(s.earAngle[i], s.earVel[i], target, 160.0f, 7.0f, dt);
                s.earAngle[i] = std::clamp(s.earAngle[i], -35.0f, 35.0f);
                const float droop = 6.0f + 3.0f * std::sin(s.time * 1.3f + i);
                // sinistra: Rz(-) abbassa la punta (l'orecchio sinistro sta su +X)
                pose.Rotate(i == 0 ? "LeftEar" : "RightEar", Rz(-side * (droop + s.earAngle[i])) * Rx(s.earAngle[i] * 0.4f));
            }
        }
        if (p.jaw) {
            s.chatterTimer -= dt;
            if (s.chatterTimer <= 0.0f) { s.chatterTimer = 0.06f + 0.12f * s.Rand(); s.chatterTarget = s.Rand(); }
            float open = p.jawIdle * (0.7f + 0.3f * breath);
            if (p.jawChatter > 0.0f) open += p.jawChatter * s.chatterTarget * idle;
            if (in.roar) open = 30.0f;                                     // urlo: bocca spalancata
            approach(s.jawOpen, open, in.roar ? 18.0f : 10.0f);
            pose.Rotate("Jaw", Rx(s.jawOpen));                             // la mascella scende ruotando sulla cerniera
        }
        if (p.belly) {
            Spring(s.bellyAngle, s.bellyVel, -bobAcc * 0.0006f + breath * 1.2f, 90.0f, 5.0f, dt);
            s.bellyAngle = std::clamp(s.bellyAngle, -8.0f, 8.0f);
            pose.Rotate("Belly", Rx(s.bellyAngle));
        }
        if (p.pauldrons) {
            // spallacci: un sobbalzo pesante a ogni appoggio, poi fermi
            Spring(s.pauldron, s.pauldronVel, -bobAcc * 0.00025f, 220.0f, 14.0f, dt);
            s.pauldron = std::clamp(s.pauldron, -4.0f, 4.0f);
            pose.Rotate("LeftPauldron", Rz(s.pauldron));
            pose.Rotate("RightPauldron", Rz(-s.pauldron));
        }
        return p.rateMult;
    }

}
