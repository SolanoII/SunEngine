#pragma once
// Animazioni procedurali sul modello animato: rotazioni di alcune ossa (e un abbassamento delle anche) applicate
// SOPRA la clip in corso. Servono per le azioni che non hanno una clip: accovacciarsi, arrampicarsi, colpi in
// mischia, parata, lancio della granata, uso degli oggetti (cibo, mine, scatola di munizioni), chiave inglese e
// costruzione delle strutture.
//
// Le rotazioni sono nello spazio della MESH in posa (asse Y in alto, Z in avanti, X verso la sinistra del
// personaggio) attorno al giunto dell'osso: valgono uguali per ogni rig mixamo, senza dipendere dagli assi locali
// delle ossa. I figli seguono (ruotare la spalla muove tutto il braccio).
//
// Convenzioni (gradi):
//   Rx(+a): un osso che punta in alto (colonna) si piega in avanti; uno che punta in basso (coscia, braccio
//           lungo il fianco) va indietro. Rx(-a) alza in avanti cosce e braccia.
//   Ry(+a): ruota verso sinistra (la spalla destra viene avanti).
//   Rz(+a): abbassa il braccio destro teso di lato / alza il sinistro.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace SunEngine {

    struct ProceduralPose {
        std::unordered_map<std::string, glm::quat> rotations;   // nome osso senza prefisso -> rotazione
        glm::vec3 hipsOffset{ 0.0f };                            // spostamento delle anche (unita' del modello)

        bool Empty() const { return rotations.empty() && glm::dot(hipsOffset, hipsOffset) < 1e-8f; }
        void Clear() { rotations.clear(); hipsOffset = glm::vec3(0.0f); }
        // Somma una rotazione a quella gia' presente sull'osso (strati: accovacciato + lancio, ecc.)
        void Rotate(const std::string& bone, const glm::quat& q) {
            auto it = rotations.find(bone);
            if (it == rotations.end()) rotations[bone] = q;
            else it->second = glm::normalize(q * it->second);
        }
    };

    namespace PoseLib {

        inline glm::quat Rx(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(1.0f, 0.0f, 0.0f)); }
        inline glm::quat Ry(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(0.0f, 1.0f, 0.0f)); }
        inline glm::quat Rz(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(0.0f, 0.0f, 1.0f)); }
        inline float Smooth(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
        // Curva 0 -> 1 -> 0 su [0,1] con salita in 'rise' e discesa dopo 'hold'
        inline float Bell(float t, float rise, float hold) {
            if (t <= 0.0f || t >= 1.0f) return 0.0f;
            if (t < rise) return Smooth(t / rise);
            if (t < hold) return 1.0f;
            return 1.0f - Smooth((t - hold) / std::max(0.001f, 1.0f - hold));
        }

        // Rotazione minima che porta la direzione a sulla direzione b
        inline glm::quat RotBetween(const glm::vec3& a, const glm::vec3& b) {
            const float la = glm::length(a), lb = glm::length(b);
            if (la < 1e-6f || lb < 1e-6f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            return glm::rotation(a / la, b / lb);
        }

        // IK a due ossa del braccio ("Right"/"Left"): porta la mano sul punto target (spazio della mesh) ruotando
        // spalla e gomito, tenendo il gomito sul piano in cui gia' si trova (niente gomiti rovesciati).
        // S, E, H = posizioni di braccio, avambraccio e mano nella posa attuale (clip + rotazioni gia' nella posa).
        // Il target e' il punto da afferrare: il polso si ferma un palmo prima.
        inline void ArmIK(ProceduralPose& p, const std::string& side, const glm::vec3& S, const glm::vec3& E, const glm::vec3& H,
                          glm::vec3 target, float weight = 1.0f) {
            const float a = glm::length(E - S), b = glm::length(H - E);
            if (a < 1e-4f || b < 1e-4f || weight <= 0.0f) return;
            const glm::vec3 f1 = H - E;
            target -= glm::normalize(f1) * (0.3f * b);          // il polso resta un palmo prima dell'impugnatura
            glm::vec3 st = target - S;
            const float dist = glm::length(st);
            if (dist < 1e-4f) return;
            const glm::vec3 dir = st / dist;
            const float d = std::clamp(dist, std::abs(a - b) + 1e-3f, a + b - 1e-3f);
            glm::vec3 pole = (E - S) - glm::dot(E - S, dir) * dir;   // piano del gomito attuale
            if (glm::length(pole) < 1e-4f) pole = glm::vec3(0.0f, -1.0f, 0.0f) - glm::dot(glm::vec3(0.0f, -1.0f, 0.0f), dir) * dir;
            pole = glm::normalize(pole);
            const float x = (a * a - b * b + d * d) / (2.0f * d);
            const float h = std::sqrt(std::max(0.0f, a * a - x * x));
            const glm::vec3 E2 = S + dir * x + pole * h;
            const glm::vec3 T2 = S + dir * d;
            glm::quat dS = RotBetween(E - S, E2 - S);
            // L'avambraccio ha gia' la sua rotazione nella posa (R_fa): la nuova va calcolata sulla direzione che
            // avra' dopo la rotazione della spalla
            const auto itFa = p.rotations.find(side + "ForeArm");
            const glm::quat Rfa = itFa != p.rotations.end() ? itFa->second : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            const glm::vec3 v = Rfa * (dS * (glm::inverse(Rfa) * f1));
            glm::quat dE = RotBetween(v, T2 - E2);
            if (weight < 1.0f) {
                dS = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), dS, weight);
                dE = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), dE, weight);
            }
            p.Rotate(side + "Arm", dS);
            p.Rotate(side + "ForeArm", dE);
        }

        // Accovacciato: cosce avanti, ginocchia piegate, piedi a terra, busto un po' in avanti. legLen = coscia +
        // stinco (unita' del modello): l'abbassamento delle anche e' quello che tiene i piedi a terra.
        inline void Crouch(ProceduralPose& p, float amount, float thighLen, float shinLen) {
            if (amount <= 0.0f) return;
            const float thigh = 70.0f * amount, knee = 115.0f * amount;
            p.Rotate("LeftUpLeg", Rx(-thigh));  p.Rotate("RightUpLeg", Rx(-thigh));
            p.Rotate("LeftLeg", Rx(knee));      p.Rotate("RightLeg", Rx(knee));
            p.Rotate("LeftFoot", Rx(-(knee - thigh))); p.Rotate("RightFoot", Rx(-(knee - thigh)));
            p.Rotate("Spine", Rx(10.0f * amount)); p.Rotate("Spine1", Rx(8.0f * amount));
            const float a = glm::radians(thigh), b = glm::radians(knee - thigh);
            const float drop = (thighLen + shinLen) - (thighLen * std::cos(a) + shinLen * std::cos(b));
            p.hipsOffset.y -= drop;
            p.hipsOffset.z -= thighLen * std::sin(a) * 0.35f;   // il bacino arretra un po' (equilibrio)
        }

        // Arrampicata lenta: braccia in alto che si alternano a tirare, gambe che spingono a turno, busto contro il muro
        inline void Climb(ProceduralPose& p, float time, float amount) {
            if (amount <= 0.0f) return;
            const float c = std::sin(time * 4.0f);
            const float l = 0.5f + 0.5f * c, r = 0.5f - 0.5f * c;
            p.Rotate("LeftArm", Rx(-(150.0f - 40.0f * l) * amount));
            p.Rotate("RightArm", Rx(-(150.0f - 40.0f * r) * amount));
            p.Rotate("LeftForeArm", Rx(-30.0f * l * amount));
            p.Rotate("RightForeArm", Rx(-30.0f * r * amount));
            p.Rotate("LeftUpLeg", Rx(-70.0f * r * amount));  p.Rotate("LeftLeg", Rx(90.0f * r * amount));
            p.Rotate("RightUpLeg", Rx(-70.0f * l * amount)); p.Rotate("RightLeg", Rx(90.0f * l * amount));
            p.Rotate("Spine", Rx(-8.0f * amount));
            p.Rotate("Head", Rx(-20.0f * amount));   // guarda in alto
        }

        // Colpo in mischia. type: 0 veloce (affondo), 1 normale (fendente orizzontale), 2 pesante (calata dall'alto,
        // a due mani se twoHanded). phase: 0 carica, 1 colpo, 2 recupero; t = avanzamento della fase (0..1).
        inline void Melee(ProceduralPose& p, int type, int phase, float t, bool twoHanded) {
            t = std::clamp(t, 0.0f, 1.0f);
            // Posizione del braccio come curva: carica -> colpo -> ritorno
            float k;   // 0 riposo, 1 caricato, -1 colpo concluso
            if (phase == 0) k = Smooth(t);
            else if (phase == 1) k = 1.0f - 2.0f * Smooth(t);
            else k = -1.0f + Smooth(t);
            const float wind = std::max(0.0f, k), hit = std::max(0.0f, -k);
            if (type == 0) {
                // Affondo: braccio indietro poi spinto avanti, busto che segue
                p.Rotate("RightArm", Rx(35.0f * wind - 25.0f * hit));
                p.Rotate("RightForeArm", Rx(-60.0f * wind));
                p.Rotate("Spine1", Ry(-20.0f * wind + 15.0f * hit));
                p.Rotate("Spine", Rx(12.0f * hit));
            }
            else if (type == 1) {
                // Fendente: braccio alto e aperto a destra, poi spazzata verso sinistra
                p.Rotate("RightArm", Rx(-70.0f * wind - 20.0f * hit));
                p.Rotate("RightArm", Ry(-45.0f * wind + 70.0f * hit));
                p.Rotate("RightForeArm", Rx(-40.0f * wind));
                p.Rotate("Spine1", Ry(-30.0f * wind + 35.0f * hit));
                p.Rotate("Spine", Rx(10.0f * hit));
            }
            else {
                // Calata: braccia sopra la testa, poi giu' davanti con il busto che si piega
                p.Rotate("RightArm", Rx(-150.0f * wind + 30.0f * hit));
                p.Rotate("RightForeArm", Rx(-50.0f * wind));
                if (twoHanded) {
                    p.Rotate("LeftArm", Rx(-150.0f * wind + 30.0f * hit));
                    p.Rotate("LeftForeArm", Rx(-50.0f * wind));
                }
                p.Rotate("Spine", Rx(-15.0f * wind + 30.0f * hit));
                p.Rotate("Spine1", Rx(-10.0f * wind + 15.0f * hit));
            }
        }

        // Parata: braccia alzate davanti al viso
        inline void Parry(ProceduralPose& p, float amount) {
            if (amount <= 0.0f) return;
            p.Rotate("RightArm", Rx(-70.0f * amount)); p.Rotate("RightArm", Ry(25.0f * amount));
            p.Rotate("LeftArm", Rx(-70.0f * amount));  p.Rotate("LeftArm", Ry(-25.0f * amount));
            p.Rotate("RightForeArm", Rx(-60.0f * amount)); p.Rotate("LeftForeArm", Rx(-60.0f * amount));
            p.Rotate("Spine", Rx(-6.0f * amount));
        }

        // Stordito: busto all'indietro, braccia larghe
        inline void Stunned(ProceduralPose& p, float amount) {
            if (amount <= 0.0f) return;
            p.Rotate("Spine", Rx(-20.0f * amount)); p.Rotate("Head", Rx(-15.0f * amount));
            p.Rotate("RightArm", Rz(-30.0f * amount)); p.Rotate("LeftArm", Rz(30.0f * amount));
        }

        // Lancio della granata (t 0..1 su ~0,75 s): braccio dietro la spalla, lancio in avanti, accompagnamento
        inline void Throw(ProceduralPose& p, float t) {
            const float wind = t < 0.45f ? Smooth(t / 0.45f) : 1.0f - Smooth((t - 0.45f) / 0.15f);
            const float rel = t < 0.45f ? 0.0f : Bell((t - 0.45f) / 0.55f, 0.25f, 0.45f);
            p.Rotate("RightArm", Rx(-130.0f * wind + 20.0f * rel));
            p.Rotate("RightArm", Ry(-35.0f * wind));
            p.Rotate("RightForeArm", Rx(-70.0f * wind));
            p.Rotate("LeftArm", Rx(-50.0f * wind));                       // braccio sinistro avanti per bilanciare
            p.Rotate("Spine1", Ry(-30.0f * wind + 30.0f * rel));
            p.Rotate("Spine", Rx(-10.0f * wind + 20.0f * rel));
        }

        // Usare un oggetto posandolo a terra (cibo, mina): ci si china e si allunga la mano verso il basso
        inline void UseItem(ProceduralPose& p, float t, float thighLen, float shinLen) {
            const float b = Bell(t, 0.4f, 0.6f);
            Crouch(p, 0.55f * b, thighLen, shinLen);
            p.Rotate("Spine", Rx(30.0f * b)); p.Rotate("Spine1", Rx(15.0f * b));
            p.Rotate("RightArm", Rx(35.0f * b));
            p.Rotate("RightForeArm", Rx(-20.0f * b));
        }

        // Porgere la scatola di munizioni (in loop finche' dura il rifornimento)
        inline void Give(ProceduralPose& p, float time, float amount) {
            if (amount <= 0.0f) return;
            const float sway = std::sin(time * 3.0f) * 6.0f;
            p.Rotate("RightArm", Rx((-25.0f + sway) * amount)); p.Rotate("LeftArm", Rx((-25.0f - sway) * amount));
            p.Rotate("RightForeArm", Rx(-35.0f * amount));      p.Rotate("LeftForeArm", Rx(-35.0f * amount));
            p.Rotate("Spine", Rx(12.0f * amount));
        }

        // Chiave inglese: colpi ritmati col braccio destro, chinato sull'oggetto
        inline void Repair(ProceduralPose& p, float time, float amount) {
            if (amount <= 0.0f) return;
            const float hammer = std::abs(std::sin(time * 7.0f));
            p.Rotate("RightArm", Rx((-70.0f + 55.0f * hammer) * amount));
            p.Rotate("RightForeArm", Rx(-50.0f * (1.0f - hammer) * amount));
            p.Rotate("LeftArm", Rx(-20.0f * amount));
            p.Rotate("Spine", Rx(22.0f * amount)); p.Rotate("Spine1", Rx(8.0f * amount));
        }

        // Dietro un'arma pesante: mezzo accovacciato, mani avanti sulle impugnature
        inline void Gunner(ProceduralPose& p, float amount, float thighLen, float shinLen) {
            if (amount <= 0.0f) return;
            Crouch(p, 0.45f * amount, thighLen, shinLen);
            p.Rotate("RightArm", Rx(-15.0f * amount)); p.Rotate("LeftArm", Rx(-15.0f * amount));
            p.Rotate("Spine", Rx(12.0f * amount));
        }

        // Medicazione (medikit): avambraccio sinistro sollevato davanti al petto, la destra lo fascia, testa china
        inline void SelfHeal(ProceduralPose& p, float time, float amount) {
            if (amount <= 0.0f) return;
            const float wrap = std::sin(time * 5.0f) * 12.0f;
            p.Rotate("LeftArm", Rx(-45.0f * amount)); p.Rotate("LeftArm", Ry(-30.0f * amount));
            p.Rotate("LeftForeArm", Rx(-60.0f * amount));
            p.Rotate("RightArm", Rx((-35.0f + wrap) * amount)); p.Rotate("RightArm", Ry(30.0f * amount));
            p.Rotate("RightForeArm", Rx(-45.0f * amount));
            p.Rotate("Head", Rx(25.0f * amount)); p.Rotate("Spine", Rx(8.0f * amount));
        }

        // Vox caster: cornetta all'orecchio con la sinistra, testa inclinata verso la mano
        inline void VoxCall(ProceduralPose& p, float amount) {
            if (amount <= 0.0f) return;
            p.Rotate("LeftArm", Rx(-30.0f * amount)); p.Rotate("LeftArm", Rz(55.0f * amount));
            p.Rotate("LeftForeArm", Rx(-120.0f * amount));
            p.Rotate("Head", Rz(12.0f * amount));
        }

        // Costruzione (barricate, muri): mezzo accovacciato, le due braccia che battono a turno
        inline void Build(ProceduralPose& p, float time, float amount, float thighLen, float shinLen) {
            if (amount <= 0.0f) return;
            Crouch(p, 0.7f * amount, thighLen, shinLen);
            const float c = std::sin(time * 6.0f);
            const float r = 0.5f + 0.5f * c, l = 0.5f - 0.5f * c;
            p.Rotate("RightArm", Rx((-60.0f + 45.0f * r) * amount)); p.Rotate("LeftArm", Rx((-60.0f + 45.0f * l) * amount));
            p.Rotate("RightForeArm", Rx(-40.0f * (1.0f - r) * amount)); p.Rotate("LeftForeArm", Rx(-40.0f * (1.0f - l) * amount));
            p.Rotate("Spine", Rx(30.0f * amount));
        }
    }
}
