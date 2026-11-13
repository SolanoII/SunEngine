#include "UiTheme.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace SunEngine::Ui {

    namespace {
        ImFont* s_Body = nullptr; ImFont* s_Small = nullptr; ImFont* s_Strong = nullptr; ImFont* s_Heading = nullptr; ImFont* s_Title = nullptr;
        const float kPi = 3.14159265f;
        ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
        ImVec4 Dim(const ImVec4& c, float k) { return ImVec4(c.x * k, c.y * k, c.z * k, c.w); }
        // testo "in grassetto" per i caratteri che non ce l'hanno: due passate sfalsate di mezzo punto
        void BoldText(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* text, const char* end = nullptr) {
            dl->AddText(f, size, p, col, text, end);
            dl->AddText(f, size, ImVec2(p.x + 0.6f, p.y), col, text, end);
        }
        void Diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
            dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col);
        }
    }

    ImU32 U32(const ImVec4& c, float alpha) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }
    ImFont* FontBody() { return s_Body ? s_Body : ImGui::GetFont(); }
    ImFont* FontSmall() { return s_Small ? s_Small : FontBody(); }
    ImFont* FontStrong() { return s_Strong ? s_Strong : FontBody(); }
    ImFont* FontHeading() { return s_Heading ? s_Heading : FontBody(); }
    ImFont* FontTitle() { return s_Title ? s_Title : FontHeading(); }

    const char* Roman(int n) {
        static const char* k[] = { "0", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII" };
        return (n >= 0 && n <= 12) ? k[n] : "?";
    }

    void Apply(const std::string& fontDirIn) {
        ImGuiIO& io = ImGui::GetIO();
        // --- caratteri (se mancano i file resta quello di ImGui: l'interfaccia funziona lo stesso) ---
        std::string dir = fontDirIn.empty() ? std::string("GameData/Fonts/") : fontDirIn;
        if (dir.back() != '/' && dir.back() != '\\') dir += "/";
        auto load = [&](const char* file, float size) -> ImFont* {
            const std::string path = dir + file;
            if (!std::filesystem::exists(path)) return nullptr;
            ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 2;
            return io.Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg);
        };
        s_Body = load("Rajdhani-SemiBold.ttf", 16.0f);       // il primo caricato e' quello di tutta l'interfaccia
        if (!s_Body) io.Fonts->AddFontDefault();
        s_Small = load("Rajdhani-SemiBold.ttf", 14.0f);
        s_Strong = load("Rajdhani-Bold.ttf", 22.0f);
        s_Heading = load("Cinzel.ttf", 18.0f);
        s_Title = load("CinzelDecorative-Bold.ttf", 28.0f);

        // --- misure: spigoli vivi, bordi sottili, un po' piu' d'aria ---
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = st.GrabRounding = st.TabRounding = st.ScrollbarRounding = 0.0f;
        st.WindowBorderSize = 1.0f; st.ChildBorderSize = 1.0f; st.FrameBorderSize = 1.0f; st.PopupBorderSize = 1.0f; st.TabBorderSize = 0.0f;
        st.WindowPadding = ImVec2(14.0f, 12.0f);
        st.FramePadding = ImVec2(10.0f, 5.0f);
        st.ItemSpacing = ImVec2(9.0f, 7.0f);
        st.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
        st.ScrollbarSize = 12.0f; st.GrabMinSize = 10.0f;
        st.WindowTitleAlign = ImVec2(0.5f, 0.5f);
        st.SeparatorTextBorderSize = 1.0f;
        st.DisabledAlpha = 0.45f;

        // --- colori ---
        ImVec4* c = st.Colors;
        c[ImGuiCol_Text] = Col::Bone;
        c[ImGuiCol_TextDisabled] = Col::Ash;
        c[ImGuiCol_WindowBg] = ImVec4(0.050f, 0.050f, 0.057f, 0.97f);
        c[ImGuiCol_ChildBg] = ImVec4(0.030f, 0.030f, 0.035f, 0.55f);
        c[ImGuiCol_PopupBg] = ImVec4(0.045f, 0.045f, 0.050f, 0.98f);
        c[ImGuiCol_Border] = ImVec4(0.36f, 0.29f, 0.17f, 0.80f);
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = ImVec4(0.095f, 0.092f, 0.090f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.170f, 0.145f, 0.105f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.250f, 0.195f, 0.110f, 1.0f);
        c[ImGuiCol_TitleBg] = ImVec4(0.060f, 0.057f, 0.055f, 1.0f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.125f, 0.098f, 0.065f, 1.0f);
        c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.050f, 0.050f, 0.050f, 0.8f);
        c[ImGuiCol_MenuBarBg] = ImVec4(0.070f, 0.068f, 0.066f, 1.0f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.020f, 0.020f, 0.024f, 0.7f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.270f, 0.230f, 0.160f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.450f, 0.360f, 0.200f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive] = Col::Brass;
        c[ImGuiCol_CheckMark] = Col::Gold;
        c[ImGuiCol_SliderGrab] = Col::Brass;
        c[ImGuiCol_SliderGrabActive] = Col::Gold;
        c[ImGuiCol_Button] = ImVec4(0.135f, 0.125f, 0.115f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.330f, 0.245f, 0.115f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.500f, 0.130f, 0.090f, 1.0f);
        c[ImGuiCol_Header] = ImVec4(0.230f, 0.175f, 0.095f, 0.85f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.340f, 0.255f, 0.120f, 0.95f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.450f, 0.330f, 0.150f, 1.0f);
        c[ImGuiCol_Separator] = ImVec4(0.36f, 0.29f, 0.17f, 0.70f);
        c[ImGuiCol_SeparatorHovered] = Col::Brass;
        c[ImGuiCol_SeparatorActive] = Col::Gold;
        c[ImGuiCol_ResizeGrip] = ImVec4(0.36f, 0.29f, 0.17f, 0.4f);
        c[ImGuiCol_ResizeGripHovered] = Col::Brass;
        c[ImGuiCol_ResizeGripActive] = Col::Gold;
        c[ImGuiCol_Tab] = ImVec4(0.085f, 0.082f, 0.080f, 1.0f);
        c[ImGuiCol_TabHovered] = ImVec4(0.340f, 0.255f, 0.120f, 1.0f);
        c[ImGuiCol_TabActive] = ImVec4(0.270f, 0.200f, 0.100f, 1.0f);
        c[ImGuiCol_TabUnfocused] = c[ImGuiCol_Tab];
        c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.200f, 0.155f, 0.085f, 1.0f);
        c[ImGuiCol_PlotLines] = Col::Phosphor;
        c[ImGuiCol_PlotLinesHovered] = Col::Gold;
        c[ImGuiCol_PlotHistogram] = Col::Brass;
        c[ImGuiCol_PlotHistogramHovered] = Col::Gold;
        c[ImGuiCol_TableHeaderBg] = ImVec4(0.115f, 0.095f, 0.070f, 1.0f);
        c[ImGuiCol_TableBorderStrong] = ImVec4(0.36f, 0.29f, 0.17f, 0.8f);
        c[ImGuiCol_TableBorderLight] = ImVec4(0.22f, 0.20f, 0.17f, 0.6f);
        c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 0.9f, 0.7f, 0.025f);
        c[ImGuiCol_TextSelectedBg] = ImVec4(0.70f, 0.54f, 0.26f, 0.40f);
        c[ImGuiCol_DragDropTarget] = Col::Gold;
        c[ImGuiCol_NavHighlight] = Col::Gold;
        c[ImGuiCol_NavWindowingHighlight] = Col::Gold;
        c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.5f);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.72f);
    }

    // Angolari d'ottone e filo interno su ogni finestra visibile (anche quelle che non sanno niente del tema)
    void DecorateWindows() {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        for (ImGuiWindow* w : g.Windows) {
            if (!w->WasActive || w->Hidden || w->SkipItems) continue;
            if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_Tooltip)) continue;
            if (w->Size.x < 120.0f || w->Size.y < 50.0f) continue;
            ImDrawList* dl = w->DrawList;
            const ImVec2 a = w->Pos, b(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
            dl->PushClipRect(a, b, false);
            const ImU32 brass = U32(Col::Brass, 0.95f);
            CornerBrackets(dl, ImVec2(a.x + 1.0f, a.y + 1.0f), ImVec2(b.x - 1.0f, b.y - 1.0f), 16.0f, brass, 2.0f);
            // testata: filo d'ottone sotto il titolo, col rombo al centro
            if (!(w->Flags & ImGuiWindowFlags_NoTitleBar) && !w->Collapsed) {
                const float y = a.y + w->CalcFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;   // altezza della testata
                dl->AddLine(ImVec2(a.x + 1.0f, y), ImVec2(b.x - 1.0f, y), U32(Col::Brass, 0.7f), 1.0f);
                Diamond(dl, ImVec2((a.x + b.x) * 0.5f, y), 4.0f, U32(Col::Gold));
            }
            dl->PopClipRect();
        }
    }

    void ChamferRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut, ImU32 fill, ImU32 border, float thickness) {
        cut = (std::min)(cut, (std::min)(b.x - a.x, b.y - a.y) * 0.45f);
        const ImVec2 pts[6] = { a, ImVec2(b.x - cut, a.y), ImVec2(b.x, a.y + cut), b, ImVec2(a.x + cut, b.y), ImVec2(a.x, b.y - cut) };
        if (fill & IM_COL32_A_MASK) dl->AddConvexPolyFilled(pts, 6, fill);
        if ((border & IM_COL32_A_MASK) && thickness > 0.0f) dl->AddPolyline(pts, 6, border, ImDrawFlags_Closed, thickness);
    }

    void CornerBrackets(ImDrawList* dl, ImVec2 a, ImVec2 b, float len, ImU32 col, float t) {
        len = (std::min)(len, (std::min)(b.x - a.x, b.y - a.y) * 0.4f);
        const ImVec2 c[4] = { a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y) };
        const float sx[4] = { 1, -1, -1, 1 }, sy[4] = { 1, 1, -1, -1 };
        for (int i = 0; i < 4; ++i) {
            dl->AddLine(c[i], ImVec2(c[i].x + sx[i] * len, c[i].y), col, t);
            dl->AddLine(c[i], ImVec2(c[i].x, c[i].y + sy[i] * len), col, t);
        }
    }

    void EclipseSigil(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float alpha) {
        const ImU32 k = (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha) << IM_COL32_A_SHIFT);
        // raggi: otto lunghi e otto corti
        for (int i = 0; i < 16; ++i) {
            const float ang = i * (2.0f * kPi / 16.0f) - kPi * 0.5f;
            const float r0 = r * 0.72f, r1 = r * ((i % 2) ? 0.86f : 1.0f);
            dl->AddLine(ImVec2(c.x + std::cos(ang) * r0, c.y + std::sin(ang) * r0), ImVec2(c.x + std::cos(ang) * r1, c.y + std::sin(ang) * r1), k, (i % 2) ? 1.0f : 1.6f);
        }
        dl->AddCircle(c, r * 0.60f, k, 32, (std::max)(1.2f, r * 0.11f));          // corona
        dl->AddCircleFilled(c, r * 0.50f, IM_COL32(6, 6, 8, static_cast<int>(255 * alpha)), 32);   // disco nero
        // falce di luce che sfugge all'eclissi
        dl->PathArcTo(c, r * 0.50f, -kPi * 0.85f, -kPi * 0.35f, 12);
        dl->PathStroke(k, 0, (std::max)(1.0f, r * 0.07f));
    }

    void Backdrop(ImDrawList* dl, ImVec2 a, ImVec2 b) {
        dl->AddRectFilledMultiColor(a, b, IM_COL32(20, 18, 17, 255), IM_COL32(20, 18, 17, 255), IM_COL32(7, 7, 9, 255), IM_COL32(7, 7, 9, 255));
        dl->PushClipRect(a, b, true);
        // griglia da strumento
        const ImU32 grid = IM_COL32(255, 235, 200, 9);
        for (float x = a.x + 48.0f; x < b.x; x += 48.0f) dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), grid);
        for (float y = a.y + 48.0f; y < b.y; y += 48.0f) dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), grid);
        // navata: archi a sesto acuto che salgono dal basso, appena visibili
        const float w = 220.0f, base = b.y + 40.0f, spring = b.y - (b.y - a.y) * 0.42f;
        const ImU32 arch = IM_COL32(200, 160, 90, 24), pier = IM_COL32(200, 160, 90, 17);
        const int n = static_cast<int>((b.x - a.x) / w) + 2;
        const float x0 = (a.x + b.x) * 0.5f - n * w * 0.5f;
        for (int i = 0; i < n; ++i) {
            const float l = x0 + i * w, r = l + w;
            for (int k = 0; k < 2; ++k) {   // due costoloni concentrici
                const float in = k * 14.0f;
                dl->PathArcTo(ImVec2(r - in, spring), w - 2.0f * in, kPi, kPi + kPi / 3.0f, 20);   // arco che parte dal pilastro sinistro
                dl->PathStroke(arch, 0, 1.5f);
                dl->PathArcTo(ImVec2(l + in, spring), w - 2.0f * in, -kPi / 3.0f, 0.0f, 20);       // e dal destro: si incontrano in punta
                dl->PathStroke(arch, 0, 1.5f);
            }
            dl->AddLine(ImVec2(l, spring), ImVec2(l, base), pier, 2.0f);
            dl->AddLine(ImVec2(l + 14.0f, spring), ImVec2(l + 14.0f, base), pier, 1.0f);
            dl->AddLine(ImVec2(r - 14.0f, spring), ImVec2(r - 14.0f, base), pier, 1.0f);
        }
        // ombra ai bordi
        const ImU32 dark = IM_COL32(0, 0, 0, 150), clear = IM_COL32(0, 0, 0, 0);
        const float v = (b.x - a.x) * 0.16f;
        dl->AddRectFilledMultiColor(a, ImVec2(a.x + v, b.y), dark, clear, clear, dark);
        dl->AddRectFilledMultiColor(ImVec2(b.x - v, a.y), b, clear, dark, dark, clear);
        dl->PopClipRect();
    }

    void TextIn(ImFont* font, const ImVec4& col, const char* text) {
        ImGui::PushFont(font);
        ImGui::TextColored(col, "%s", text);
        ImGui::PopFont();
    }

    void Divider(float width, const ImVec4& col) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (width <= 0.0f) width = ImGui::GetContentRegionAvail().x;
        const float y = p.y + 5.0f, mid = p.x + width * 0.5f;
        dl->AddRectFilledMultiColor(ImVec2(p.x, y), ImVec2(mid - 10.0f, y + 1.0f), U32(col, 0.0f), U32(col, 0.8f), U32(col, 0.8f), U32(col, 0.0f));
        dl->AddRectFilledMultiColor(ImVec2(mid + 10.0f, y), ImVec2(p.x + width, y + 1.0f), U32(col, 0.8f), U32(col, 0.0f), U32(col, 0.0f), U32(col, 0.8f));
        Diamond(dl, ImVec2(mid, y + 0.5f), 4.0f, U32(col));
        ImGui::Dummy(ImVec2(width, 11.0f));
    }

    void Heading(const char* text, const ImVec4& col) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* f = FontHeading();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 sz = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, text);
        const float avail = ImGui::GetContentRegionAvail().x;
        // rombo, testo, filo che sfuma verso destra
        Diamond(dl, ImVec2(p.x + 5.0f, p.y + sz.y * 0.5f + 1.0f), 4.0f, U32(col));
        BoldText(dl, f, f->FontSize, ImVec2(p.x + 16.0f, p.y), U32(col), text);
        const float x0 = p.x + 16.0f + sz.x + 12.0f, y = p.y + sz.y * 0.5f + 1.0f;
        if (x0 < p.x + avail) dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(p.x + avail, y + 1.0f), U32(col, 0.6f), U32(col, 0.0f), U32(col, 0.0f), U32(col, 0.6f));
        ImGui::Dummy(ImVec2(avail, sz.y + 4.0f));
    }

    void Title(const char* text, const ImVec4& col) {
        ImGui::PushFont(FontTitle());
        ImGui::TextColored(col, "%s", text);
        ImGui::PopFont();
    }

    bool Button(const char* label, ImVec2 size, Btn kind, bool enabled) {
        const ImGuiStyle& st = ImGui::GetStyle();
        const char* end = ImGui::FindRenderedTextEnd(label);
        ImFont* f = FontBody();
        const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, label, end);
        if (size.x < 0.0f) size.x = ImGui::GetContentRegionAvail().x;
        if (size.x == 0.0f) size.x = ts.x + st.FramePadding.x * 2.0f + 14.0f;
        if (size.y <= 0.0f) size.y = ts.y + st.FramePadding.y * 2.0f + 4.0f;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
        ImGui::BeginDisabled(!enabled);
        const bool clicked = ImGui::InvisibleButton(label, size);
        const bool hov = enabled && ImGui::IsItemHovered(), act = enabled && ImGui::IsItemActive();
        ImGui::EndDisabled();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec4 fill = Col::IronLight, edge = Col::Steel, text = Col::Bone;
        if (kind == Btn::Primary) { fill = ImVec4(0.30f, 0.22f, 0.09f, 1.0f); edge = Col::Gold; text = ImVec4(1.0f, 0.93f, 0.75f, 1.0f); }
        else if (kind == Btn::Danger) { fill = ImVec4(0.26f, 0.06f, 0.05f, 1.0f); edge = Col::Ember; }
        else if (kind == Btn::Ghost) { fill = ImVec4(0, 0, 0, 0); edge = ImVec4(0.36f, 0.29f, 0.17f, 0.8f); }
        if (hov) { fill = kind == Btn::Ghost ? ImVec4(0.70f, 0.54f, 0.26f, 0.15f) : Mix(fill, Col::Brass, 0.35f); edge = kind == Btn::Danger ? Col::Ember : Col::Gold; }
        if (act) fill = kind == Btn::Danger ? Col::Blood : Mix(fill, Col::Gold, 0.35f);
        const float alpha = enabled ? 1.0f : 0.40f;
        ChamferRect(dl, a, b, 8.0f, U32(fill, alpha), U32(edge, alpha), 1.0f);
        if (kind == Btn::Primary && enabled) dl->AddRectFilled(ImVec2(a.x + 1.0f, a.y + 1.0f), ImVec2(a.x + 4.0f, b.y - 9.0f), U32(Col::Gold));   // tacca piena a sinistra
        dl->AddText(f, f->FontSize, ImVec2(a.x + (size.x - ts.x) * 0.5f, a.y + (size.y - ts.y) * 0.5f), U32(text, alpha), label, end);
        return clicked && enabled;
    }

    bool PlainButton(const char* label, const ImVec2& sizeArg) {
        const ImGuiStyle& st = ImGui::GetStyle();
        const char* end = ImGui::FindRenderedTextEnd(label);
        const ImVec2 ts = ImGui::CalcTextSize(label, end);
        ImVec2 size = sizeArg;
        if (size.x < 0.0f) size.x = (std::max)(4.0f, ImGui::GetContentRegionAvail().x + size.x + 1.0f);
        if (size.x == 0.0f) size.x = ts.x + st.FramePadding.x * 2.0f;
        if (size.y < 0.0f) size.y = (std::max)(4.0f, ImGui::GetContentRegionAvail().y + size.y + 1.0f);
        if (size.y == 0.0f) size.y = ts.y + st.FramePadding.y * 2.0f;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
        const bool clicked = ImGui::InvisibleButton(label, size);
        const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 fill = ImGui::GetColorU32(act ? ImGuiCol_ButtonActive : (hov ? ImGuiCol_ButtonHovered : ImGuiCol_Button));
        const ImU32 edge = hov ? ImGui::GetColorU32(Col::Gold) : ImGui::GetColorU32(ImGuiCol_Border);
        ChamferRect(dl, a, b, (std::min)(7.0f, size.y * 0.3f), fill, edge, 1.0f);
        dl->AddText(ImVec2(a.x + (size.x - ts.x) * 0.5f, a.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label, end);
        return clicked;
    }

    bool NavTab(const char* label, float width, float height, bool active, const ImVec4& accent) {
        const char* end = ImGui::FindRenderedTextEnd(label);
        ImFont* f = FontHeading();
        const float fs = f->FontSize * 0.92f;
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, label, end);
        if (width <= 0.0f) width = ts.x + 30.0f;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + height);
        const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (active) {
            dl->AddRectFilledMultiColor(a, b, U32(accent, 0.0f), U32(accent, 0.0f), U32(accent, 0.20f), U32(accent, 0.20f));
            dl->AddRectFilled(ImVec2(a.x, b.y - 3.0f), b, U32(accent));
            Diamond(dl, ImVec2((a.x + b.x) * 0.5f, b.y - 1.5f), 5.0f, U32(accent));
        }
        else if (hov) {
            dl->AddRectFilledMultiColor(a, b, U32(Col::Brass, 0.0f), U32(Col::Brass, 0.0f), U32(Col::Brass, 0.12f), U32(Col::Brass, 0.12f));
            dl->AddRectFilled(ImVec2(a.x, b.y - 1.0f), b, U32(Col::Brass, 0.7f));
        }
        const ImVec4 tc = active ? ImVec4(1.0f, 0.95f, 0.82f, 1.0f) : (hov ? Col::Bone : Col::Ash);
        const ImVec2 tp(a.x + (width - ts.x) * 0.5f, a.y + (height - ts.y) * 0.5f - 1.0f);
        if (active) BoldText(dl, f, fs, tp, U32(tc), label, end); else   // senza la parte "##id" dell'etichetta
        dl->AddText(f, fs, tp, U32(tc), label, end);
        return clicked;
    }

    void StatTile(const char* label, const char* value, float width, const ImVec4& valueCol) {
        ImFont* fs = FontSmall(); ImFont* fv = FontStrong();
        const float h = fs->FontSize + fv->FontSize + 12.0f;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + h);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, b, IM_COL32(255, 240, 210, 8));
        dl->AddRectFilled(a, ImVec2(a.x + 2.0f, b.y), U32(Col::Brass, 0.8f));
        dl->AddText(fs, fs->FontSize, ImVec2(a.x + 10.0f, a.y + 4.0f), U32(Col::Ash), label);
        dl->AddText(fv, fv->FontSize, ImVec2(a.x + 10.0f, a.y + 5.0f + fs->FontSize), U32(valueCol), value);
        ImGui::Dummy(ImVec2(width, h));
    }

    void Chip(const char* text, const ImVec4& col) {
        ImFont* f = FontSmall();
        const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, text);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + ts.x + 16.0f, a.y + ts.y + 6.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ChamferRect(dl, a, b, 5.0f, U32(col, 0.14f), U32(col, 0.85f), 1.0f);
        dl->AddText(f, f->FontSize, ImVec2(a.x + 8.0f, a.y + 3.0f), U32(col), text);
        ImGui::Dummy(ImVec2(b.x - a.x, b.y - a.y));
    }

    void Meter(float fraction, ImVec2 size, const ImVec4& col) {
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 160));
        dl->AddRectFilled(a, ImVec2(a.x + size.x * std::clamp(fraction, 0.0f, 1.0f), b.y), U32(col));
        dl->AddRect(a, b, U32(Col::Steel, 0.8f));
        ImGui::Dummy(size);
    }

    void Currency(const char* label, long long amount, const ImVec4& col) {
        ImFont* fs = FontSmall(); ImFont* fv = FontStrong();
        char buf[32]; std::snprintf(buf, sizeof(buf), "%lld", amount);
        const ImVec2 ls = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.0f, label), vs = fv->CalcTextSizeA(fv->FontSize, FLT_MAX, 0.0f, buf);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = fv->FontSize;
        Diamond(dl, ImVec2(a.x + 7.0f, a.y + h * 0.5f), 6.0f, U32(col));
        Diamond(dl, ImVec2(a.x + 7.0f, a.y + h * 0.5f), 2.5f, IM_COL32(10, 10, 12, 255));
        dl->AddText(fs, fs->FontSize, ImVec2(a.x + 20.0f, a.y + (h - ls.y) * 0.5f + 1.0f), U32(Col::Ash), label);
        dl->AddText(fv, fv->FontSize, ImVec2(a.x + 26.0f + ls.x, a.y), U32(col), buf);
        ImGui::Dummy(ImVec2(26.0f + ls.x + vs.x, h));
    }

    void DrawBranch(ImDrawList* dl, ImVec2 a, ImVec2 b, bool lit, const ImVec4& accent) {
        const float midX = std::floor((a.x + b.x) * 0.5f) + 0.5f;
        const ImU32 col = lit ? U32(Mix(accent, Col::Gold, 0.35f), 0.95f) : U32(Col::Steel, 0.55f);
        const float t = lit ? 2.0f : 1.0f;
        if (lit) {   // alone sotto il ramo acceso
            const ImU32 glow = U32(accent, 0.16f);
            dl->AddLine(a, ImVec2(midX, a.y), glow, 6.0f); dl->AddLine(ImVec2(midX, a.y), ImVec2(midX, b.y), glow, 6.0f); dl->AddLine(ImVec2(midX, b.y), b, glow, 6.0f);
        }
        dl->AddLine(a, ImVec2(midX, a.y), col, t);
        dl->AddLine(ImVec2(midX, a.y), ImVec2(midX, b.y), col, t);
        dl->AddLine(ImVec2(midX, b.y), b, col, t);
        Diamond(dl, ImVec2(midX, a.y), lit ? 3.5f : 2.5f, col);
        if (std::abs(a.y - b.y) > 1.0f) Diamond(dl, ImVec2(midX, b.y), lit ? 3.5f : 2.5f, col);
        // punta d'ingresso nel nodo
        dl->AddTriangleFilled(ImVec2(b.x, b.y), ImVec2(b.x - 7.0f, b.y - 4.0f), ImVec2(b.x - 7.0f, b.y + 4.0f), col);
    }

    void DrawMasteryBranch(ImDrawList* dl, ImVec2 a, ImVec2 b, bool lit) {
        const ImU32 col = lit ? U32(Col::Gold) : U32(Mix(Col::Brass, Col::Steel, 0.5f), 0.8f);
        // tratteggio: e' un requisito, non una discendenza
        const float dir = b.y > a.y ? 1.0f : -1.0f;
        for (float y = a.y; (b.y - y) * dir > 0.0f; y += dir * 9.0f) dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, y + dir * (std::min)(5.0f, std::abs(b.y - y))), col, 1.5f);
        for (float x = a.x; x < b.x - 7.0f; x += 9.0f) dl->AddLine(ImVec2(x, b.y), ImVec2((std::min)(x + 5.0f, b.x - 7.0f), b.y), col, 1.5f);
        dl->AddTriangleFilled(ImVec2(b.x, b.y), ImVec2(b.x - 7.0f, b.y - 4.0f), ImVec2(b.x - 7.0f, b.y + 4.0f), col);
        const ImVec2 mid(a.x, (a.y + b.y) * 0.5f);
        dl->AddCircleFilled(mid, 10.0f, IM_COL32(10, 10, 12, 255));
        EclipseSigil(dl, mid, 9.0f, col);
    }

    void DrawNodeCard(ImDrawList* dl, ImVec2 p, ImVec2 size, const NodeCard& k) {
        const ImVec2 q(p.x + size.x, p.y + size.y);
        const float cut = 12.0f, spine = 34.0f;
        const bool done = k.state == NodeState::Done, avail = k.state == NodeState::Available;
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.2f);

        // alone: disponibile = oro che respira, selezionato = pieno
        if (avail || k.selected) {
            const ImVec4 gc = avail ? Col::Gold : Col::Brass;
            const float ga = k.selected ? 0.30f : 0.10f + 0.14f * pulse;
            for (int i = 3; i >= 1; --i)
                ChamferRect(dl, ImVec2(p.x - i * 2.0f, p.y - i * 2.0f), ImVec2(q.x + i * 2.0f, q.y + i * 2.0f), cut + i * 1.5f, 0, U32(gc, ga / i), 2.0f);
        }
        // corpo
        const ImVec4 body = done ? ImVec4(0.105f, 0.092f, 0.072f, 1.0f) : (avail ? ImVec4(0.095f, 0.088f, 0.078f, 1.0f) : ImVec4(0.060f, 0.060f, 0.066f, 1.0f));
        ChamferRect(dl, p, q, cut, U32(k.hovered ? Mix(body, Col::Brass, 0.10f) : body), 0);
        // dorso col colore dell'unita' e il grado in numeri romani
        dl->PushClipRect(p, q, true);
        {
            const ImVec4 uc = done || avail ? k.unitColor : Mix(Dim(k.unitColor, 0.45f), Col::Steel, 0.5f);
            const ImVec2 sp[5] = { p, ImVec2(p.x + spine, p.y), ImVec2(p.x + spine, q.y), ImVec2(p.x + cut, q.y), ImVec2(p.x, q.y - cut) };
            dl->AddConvexPolyFilled(sp, 5, U32(Dim(uc, 0.42f)));
            dl->AddRectFilled(ImVec2(p.x + spine - 2.0f, p.y), ImVec2(p.x + spine, q.y), U32(uc));
            ImFont* f = FontHeading();
            const char* rn = Roman(k.tier);
            const float fs = f->FontSize * (std::strlen(rn) > 2 ? 0.82f : 1.0f);
            const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, rn);
            BoldText(dl, f, fs, ImVec2(p.x + (spine - 2.0f - ts.x) * 0.5f, p.y + 9.0f), U32(done || avail ? Col::Bone : Col::Ash), rn);
            // tacche sotto il numero
            for (int i = 0; i < 3; ++i) dl->AddRectFilled(ImVec2(p.x + 9.0f, q.y - 26.0f + i * 5.0f), ImVec2(p.x + spine - 11.0f, q.y - 24.0f + i * 5.0f), U32(uc, 0.55f));
        }
        // testi
        const float tx = p.x + spine + 12.0f;
        ImFont* fh = FontHeading(); ImFont* fb = FontSmall(); ImFont* fn = FontBody();
        const ImVec4 nameCol = done ? ImVec4(1.0f, 0.94f, 0.80f, 1.0f) : (avail ? Col::Bone : Col::Ash);
        const float maxName = size.x - spine - 24.0f - (k.mastered ? 24.0f : 0.0f);
        float nfs = fh->FontSize * 0.90f;
        const float nw = fh->CalcTextSizeA(nfs, FLT_MAX, 0.0f, k.name).x;
        if (nw > maxName) nfs *= (std::max)(0.72f, maxName / nw);   // nomi lunghi: un po' piu' piccoli
        BoldText(dl, fh, nfs, ImVec2(tx, p.y + 9.0f), U32(nameCol), k.name);
        dl->AddText(fb, fb->FontSize, ImVec2(tx, p.y + 34.0f), U32(done || avail ? Col::Ash : Dim(Col::Ash, 0.75f)), k.info);
        const ImVec4 stCol = k.missing ? Col::Rust : (done ? Col::Verdigris : (avail ? Col::Gold : Dim(Col::Ash, 0.9f)));
        // quadratino di stato + riga
        const float sy = q.y - 28.0f;
        if (done) { dl->AddLine(ImVec2(tx, sy + 9.0f), ImVec2(tx + 4.0f, sy + 13.0f), U32(stCol), 2.0f); dl->AddLine(ImVec2(tx + 4.0f, sy + 13.0f), ImVec2(tx + 11.0f, sy + 4.0f), U32(stCol), 2.0f); }
        else if (avail) Diamond(dl, ImVec2(tx + 5.0f, sy + 9.0f), 4.0f + pulse, U32(stCol));
        else {   // lucchetto
            dl->AddRectFilled(ImVec2(tx + 1.0f, sy + 8.0f), ImVec2(tx + 10.0f, sy + 15.0f), U32(stCol));
            dl->PathArcTo(ImVec2(tx + 5.5f, sy + 8.0f), 3.2f, kPi, 2.0f * kPi, 8); dl->PathStroke(U32(stCol), 0, 1.5f);
        }
        dl->AddText(fn, fn->FontSize * 0.94f, ImVec2(tx + 17.0f, sy), U32(stCol), k.status);
        // avanzamento verso il costo della ricerca
        if (k.progress >= 0.0f && !done) {
            const ImVec2 ba(p.x + spine, q.y - 4.0f), bb(q.x, q.y);
            dl->AddRectFilled(ba, bb, IM_COL32(0, 0, 0, 170));
            dl->AddRectFilled(ba, ImVec2(ba.x + (bb.x - ba.x) * std::clamp(k.progress, 0.0f, 1.0f), bb.y), U32(avail ? Col::Gold : Col::Brass, avail ? 1.0f : 0.7f));
        }
        dl->PopClipRect();
        // maestria: il sole in eclissi in alto a destra
        if (k.mastered) EclipseSigil(dl, ImVec2(q.x - 26.0f, p.y + 19.0f), 11.0f, U32(Col::Gold));
        // bordo
        const ImVec4 edge = done ? Col::Brass : (avail ? Mix(Col::Brass, Col::Gold, 0.4f + 0.6f * pulse) : Col::Steel);
        ChamferRect(dl, p, q, cut, 0, U32(k.selected ? Col::Gold : edge, done || avail || k.selected ? 1.0f : 0.7f), k.selected ? 2.0f : (k.hovered ? 1.8f : 1.0f));
        if (k.selected) CornerBrackets(dl, ImVec2(p.x - 6.0f, p.y - 6.0f), ImVec2(q.x + 6.0f, q.y + 6.0f), 12.0f, U32(Col::Gold), 2.0f);
    }
}
