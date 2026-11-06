#pragma once
// Nomi delle ossa: il gioco ragiona con lo scheletro Mixamo e il prefisso "mixamorig1:" (armi in mano, pose
// procedurali, animazioni). I modelli fatti da un artista possono arrivare con altri nomi per le stesse ossa:
//   "mixamorig:Hips", "mixamorig5:Hips", "Armature|mixamorig:Hips", "Hips"   (Mixamo esportato da Blender o Maya)
//   "pelvis", "spine_01", "upperarm_l", ...                                    (manichino di Unreal)
// CanonicalBoneName li porta tutti al nome usato dal gioco. I nomi che non riconosce restano invariati
// (ossa in piu' dell'artista: mantelli, armi, catene...).
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace SunEngine {

    inline const std::string& CanonicalBonePrefix() {
        static const std::string p = "mixamorig1:";
        return p;
    }

    // Ossa dello scheletro Mixamo (senza prefisso)
    inline bool IsMixamoBoneSuffix(const std::string& s) {
        static const std::unordered_set<std::string> k = [] {
            std::unordered_set<std::string> b = { "Hips", "Spine", "Spine1", "Spine2", "Neck", "Head", "HeadTop_End",
                                                  "LeftEye", "RightEye" };
            for (const char* side : { "Left", "Right" }) {
                const std::string S = side;
                for (const char* n : { "Shoulder", "Arm", "ForeArm", "Hand", "UpLeg", "Leg", "Foot", "ToeBase", "Toe_End" }) b.insert(S + n);
                for (const char* f : { "Thumb", "Index", "Middle", "Ring", "Pinky" })
                    for (int i = 1; i <= 4; ++i) b.insert(S + "Hand" + f + std::to_string(i));
            }
            return b;
        }();
        return k.count(s) != 0;
    }

    // Manichino di Unreal Engine -> Mixamo (stesse ossa, altri nomi)
    inline const std::unordered_map<std::string, std::string>& UnrealToMixamo() {
        static const std::unordered_map<std::string, std::string> m = [] {
            std::unordered_map<std::string, std::string> r = {
                { "pelvis", "Hips" }, { "spine_01", "Spine" }, { "spine_02", "Spine1" }, { "spine_03", "Spine2" },
                { "neck_01", "Neck" }, { "head", "Head" } };
            const char* sides[2][2] = { { "_l", "Left" }, { "_r", "Right" } };
            for (auto& sd : sides) {
                const std::string u = sd[0], m2 = sd[1];
                r["clavicle" + u] = m2 + "Shoulder"; r["upperarm" + u] = m2 + "Arm"; r["lowerarm" + u] = m2 + "ForeArm";
                r["hand" + u] = m2 + "Hand"; r["thigh" + u] = m2 + "UpLeg"; r["calf" + u] = m2 + "Leg";
                r["foot" + u] = m2 + "Foot"; r["ball" + u] = m2 + "ToeBase";
                const char* fingers[5][2] = { { "thumb", "Thumb" }, { "index", "Index" }, { "middle", "Middle" }, { "ring", "Ring" }, { "pinky", "Pinky" } };
                for (auto& f : fingers)
                    for (int i = 1; i <= 3; ++i) r[std::string(f[0]) + "_0" + std::to_string(i) + u] = m2 + "Hand" + f[1] + std::to_string(i);
            }
            return r;
        }();
        return m;
    }

    inline std::string CanonicalBoneName(const std::string& name) {
        // togli tutto fino all'ultimo separatore ("Armature|mixamorig:Hips" -> "Hips")
        size_t cut = name.find_last_of(":|");
        const std::string suffix = cut == std::string::npos ? name : name.substr(cut + 1);
        if (IsMixamoBoneSuffix(suffix)) {
            // solo se il prefisso (se c'e') e' di Mixamo o di un'armatura: un "Cape:Hips" resta com'e'
            const std::string prefix = cut == std::string::npos ? std::string() : name.substr(0, cut);
            std::string lp = prefix;
            std::transform(lp.begin(), lp.end(), lp.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (prefix.empty() || lp.find("mixamo") != std::string::npos || lp.find("armature") != std::string::npos)
                return CanonicalBonePrefix() + suffix;
            return name;
        }
        std::string low = suffix;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto& ue = UnrealToMixamo();
        auto it = ue.find(low);
        if (it != ue.end()) return CanonicalBonePrefix() + it->second;
        return name;
    }
}
