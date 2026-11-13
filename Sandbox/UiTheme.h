#pragma once
// Aspetto dell'interfaccia: grimdark moderno, meta' gotico (archi a sesto acuto, ottone brunito, pergamena, sangue)
// e meta' futuristico (angoli smussati, griglie da strumento, fosforo verde-acqua). Tutto disegnato a codice:
// nessuna immagine, nessun simbolo preso da altri giochi. Il segno ricorrente e' il sole in eclissi.
//
//   Ui::Apply(cartellaFonts)   una volta, dopo ImGui::CreateContext: colori, misure e caratteri
//   Ui::DecorateWindows()      ogni fotogramma prima di ImGui::Render: angolari d'ottone su tutte le finestre
//   il resto sono pezzi da usare nelle schermate (pulsanti, intestazioni, schede dell'albero di ricerca...)
#include <imgui.h>
#include <string>

namespace SunEngine::Ui {

    // --- tavolozza ---
    namespace Col {
        inline constexpr ImVec4 Void{ 0.030f, 0.030f, 0.036f, 1.0f };       // nero del fondo
        inline constexpr ImVec4 Iron{ 0.070f, 0.070f, 0.078f, 1.0f };       // pannelli
        inline constexpr ImVec4 IronLight{ 0.125f, 0.120f, 0.118f, 1.0f };  // controlli
        inline constexpr ImVec4 Steel{ 0.300f, 0.285f, 0.265f, 1.0f };      // bordi spenti
        inline constexpr ImVec4 Bone{ 0.870f, 0.830f, 0.730f, 1.0f };       // testo
        inline constexpr ImVec4 Ash{ 0.540f, 0.515f, 0.470f, 1.0f };        // testo secondario
        inline constexpr ImVec4 Brass{ 0.700f, 0.540f, 0.260f, 1.0f };      // ottone brunito
        inline constexpr ImVec4 Gold{ 0.950f, 0.770f, 0.360f, 1.0f };       // oro (attivo, disponibile)
        inline constexpr ImVec4 Blood{ 0.560f, 0.090f, 0.075f, 1.0f };      // rosso scuro
        inline constexpr ImVec4 Ember{ 0.900f, 0.300f, 0.170f, 1.0f };      // rosso vivo (errori, pericolo)
        inline constexpr ImVec4 Rust{ 0.880f, 0.520f, 0.240f, 1.0f };       // ambra (avvisi)
        inline constexpr ImVec4 Phosphor{ 0.360f, 0.800f, 0.740f, 1.0f };   // verde-acqua degli strumenti
        inline constexpr ImVec4 Verdigris{ 0.500f, 0.740f, 0.440f, 1.0f };  // verde (fatto, a posto)
        inline constexpr ImVec4 Amethyst{ 0.640f, 0.470f, 0.840f, 1.0f };   // viola (Pietra Nera)
    }
    ImU32 U32(const ImVec4& c, float alpha = 1.0f);

    void Apply(const std::string& fontDir);
    void DecorateWindows();

    ImFont* FontBody();      // testo
    ImFont* FontSmall();     // note, etichette
    ImFont* FontRead();      // lettura dei racconti (corpo piu' grande)
    ImFont* FontStrong();    // numeri e voci in evidenza
    ImFont* FontHeading();   // intestazioni (capitali romane)
    ImFont* FontTitle();     // titoli grandi

    // --- disegno ---
    // Pannello con due angoli smussati (alto-destra e basso-sinistra)
    void ChamferRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut, ImU32 fill, ImU32 border, float thickness = 1.0f);
    // Angolari a L sui quattro angoli
    void CornerBrackets(ImDrawList* dl, ImVec2 a, ImVec2 b, float len, ImU32 col, float thickness = 2.0f);
    // Sole in eclissi: disco nero, corona e raggi
    void EclipseSigil(ImDrawList* dl, ImVec2 center, float radius, ImU32 col, float alpha = 1.0f);
    // Fondo delle schermate a tutto schermo: sfumatura, griglia, navata di archi a sesto acuto appena visibile
    void Backdrop(ImDrawList* dl, ImVec2 a, ImVec2 b);
    // Linea - rombo - linea
    void Divider(float width = 0.0f, const ImVec4& col = Col::Brass);
    void Heading(const char* text, const ImVec4& col = Col::Gold);   // intestazione di sezione con fregio
    void Title(const char* text, const ImVec4& col = Col::Bone);
    void TextIn(ImFont* font, const ImVec4& col, const char* text);

    enum class Btn { Normal, Primary, Danger, Ghost };
    bool Button(const char* label, ImVec2 size, Btn kind = Btn::Normal, bool enabled = true);
    // Come ImGui::Button (stessi colori dello stile, anche quelli messi con PushStyleColor), ma con gli angoli smussati
    bool PlainButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
    // Scheda di navigazione (barra in alto, fazioni): sottolineata e accesa se attiva
    bool NavTab(const char* label, float width, float height, bool active, const ImVec4& accent = Col::Gold);
    // Riquadro con etichetta piccola e valore grande
    void StatTile(const char* label, const char* value, float width, const ImVec4& valueCol = Col::Bone);
    // Targhetta di stato
    void Chip(const char* text, const ImVec4& col);
    // Barra sottile di avanzamento
    void Meter(float fraction, ImVec2 size, const ImVec4& col = Col::Brass);
    // Valuta: rombo colorato, etichetta e numero
    void Currency(const char* label, long long amount, const ImVec4& col);

    // --- albero di ricerca ---
    enum class NodeState { Locked, Available, Done };
    struct NodeCard {
        const char* name = "";
        const char* status = "";       // riga di stato
        const char* info = "";         // riga piccola (punti studio, oggetti)
        int tier = 1;
        ImVec4 unitColor{ 0.5f, 0.5f, 0.5f, 1.0f };
        NodeState state = NodeState::Locked;
        float progress = -1.0f;        // 0..1 verso il costo della ricerca, < 0 = niente barra
        bool selected = false, hovered = false, mastered = false, missing = false;
    };
    void DrawNodeCard(ImDrawList* dl, ImVec2 pos, ImVec2 size, const NodeCard& card);
    // Ramo a gomito da a (uscita a destra) a b (entrata a sinistra)
    void DrawBranch(ImDrawList* dl, ImVec2 a, ImVec2 b, bool lit, const ImVec4& accent);
    // Ramo della maestria: scende/sale da a e entra in b da sinistra, col sole a meta'
    void DrawMasteryBranch(ImDrawList* dl, ImVec2 a, ImVec2 b, bool lit);
    const char* Roman(int n);
}
