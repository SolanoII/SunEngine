#pragma once
// Disegno dei tipi di danno e delle difese (DamageTypes.h) nelle schermate: magazzino, Codex.
#include "Engine/AssetManager/DamageTypes.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace SunEngine::DamageUi {

    inline ImVec4 Col(int t, float a = 1.0f) { const glm::vec3 c = DamageInfo(t).color; return ImVec4(c.r, c.g, c.b, a); }

    // Barra divisa nei colori dei tipi, con la legenda sotto ("Perforazione 60%  Esplosivo 40%")
    inline void SplitBar(const DamageSplit& d, float width, const char* note = nullptr) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = 12.0f;
        float x = p.x;
        std::vector<int> order;
        for (int i = 0; i < kDamageTypeCount; ++i) if (d.Has(i)) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return d.share[a] > d.share[b]; });
        for (int i : order) {
            const float w = width * d.share[i];
            dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + h), ImGui::GetColorU32(Col(i)));
            x += w;
        }
        dl->AddRect(p, ImVec2(p.x + width, p.y + h), ImGui::GetColorU32(ImVec4(0, 0, 0, 0.6f)));
        ImGui::Dummy(ImVec2(width, h + 2.0f));
        bool first = true;
        for (int i : order) {
            if (!first) ImGui::SameLine(0.0f, 14.0f);
            first = false;
            ImGui::TextColored(Col(i), "%s %d%%", DamageInfo(i).name, static_cast<int>(std::lround(d.share[i] * 100.0f)));
        }
        if (note) { ImGui::SameLine(0.0f, 14.0f); ImGui::TextDisabled("%s", note); }
    }

    // Difese, una riga per tipo con barretta (solo quelle diverse da zero)
    inline void ResistRows(const DamageResist& r, float width, const DamageResist* base = nullptr) {
        bool any = false;
        for (int i = 0; i < kDamageTypeCount; ++i) {
            const float v = r[i], b = base ? (*base)[i] : v;
            if (std::abs(v) < 0.005f && std::abs(b) < 0.005f) continue;
            any = true;
            ImGui::TextColored(Col(i), "%s", DamageInfo(i).name);
            ImGui::SameLine(150.0f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float h = ImGui::GetTextLineHeight() * 0.6f, y = p.y + ImGui::GetTextLineHeight() * 0.2f;
            dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + width, y + h), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.07f)));
            dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + width * std::clamp(v / 0.5f, 0.0f, 1.0f), y + h), ImGui::GetColorU32(Col(i, 0.85f)));
            ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            if (base && std::abs(b - v) > 0.004f)
                ImGui::TextColored(v > b ? ImVec4(0.45f, 0.8f, 0.55f, 1.0f) : ImVec4(0.9f, 0.4f, 0.3f, 1.0f), "%d%%  (base %d%%)",
                    static_cast<int>(std::lround(v * 100.0f)), static_cast<int>(std::lround(b * 100.0f)));
            else ImGui::Text("%d%%", static_cast<int>(std::lround(v * 100.0f)));
        }
        if (!any) ImGui::TextDisabled("Nessuna difesa dai tipi di danno.");
    }
}
