#pragma once
// Protocollo fra il gioco e il server d'incontro (Tools/TrazynServer). Solo dichiarazioni: niente ENet, niente socket.
//
// Il server d'incontro NON fa girare la partita: e' un elenco telefonico con un centralino.
//   - tiene l'elenco delle squadre/lobby pubblicate (schermata SERVER) e i loro codici a 6 cifre;
//   - quando qualcuno vuole entrare, dice a entrambi dove si trova l'altro e li fa chiamare insieme: i due router
//     si aprono a vicenda ("hole punching") e il collegamento diventa diretto, senza aprire porte;
//   - se due router proprio non si aprono (succede con alcune reti mobili e aziendali), fa da ponte e passa i
//     messaggi da uno all'altro. La partita resta peer to peer: l'host e' sempre uno dei giocatori.
#include <cstdint>
#include <string>
#include <vector>

namespace SunEngine::Net::Wire {

    inline constexpr std::uint32_t kServerProto = 0x53524731u;   // "SRG1"
    inline constexpr std::uint16_t kDefaultServerPort = 27640;

    // dal gioco al server
    enum ClientMsg : std::uint8_t {
        C_Hello = 1,      // proto, id, nome, ip locale, porta locale
        C_Publish,        // la mia lobby (solo il capo): codice proposto, chiave, nome, capo, tipo, membri, max, in partita
        C_Unpublish,
        C_List,           // elenco delle lobby visibili
        C_LookupCode,     // codice a 6 cifre -> dov'e' il capo
        C_LookupLeader,   // id del capo (dall'elenco) -> dov'e'
        C_Introduce,      // presentami a questo compagno (per il collegamento diretto fra compagni)
        C_Relay,          // passa questi byte a questo giocatore (ponte)
    };
    // dal server al gioco
    enum ServerMsg : std::uint8_t {
        S_Welcome = 1,    // il tuo indirizzo pubblico visto da fuori
        S_Code,           // codice a 6 cifre assegnato alla tua lobby
        S_List,
        S_Found,          // dove trovare il capo (indirizzi pubblico e locale, chiave se cercato col codice)
        S_NotFound,
        S_Incoming,       // qualcuno sta per chiamarti: apri il router verso di lui
        S_Relay,          // byte arrivati via ponte da un giocatore
    };

    struct Writer {
        std::vector<std::uint8_t> b;
        void U8(std::uint8_t v) { b.push_back(v); }
        void U16(std::uint16_t v) { U8(static_cast<std::uint8_t>(v)); U8(static_cast<std::uint8_t>(v >> 8)); }
        void U32(std::uint32_t v) { U16(static_cast<std::uint16_t>(v)); U16(static_cast<std::uint16_t>(v >> 16)); }
        void U64(std::uint64_t v) { U32(static_cast<std::uint32_t>(v)); U32(static_cast<std::uint32_t>(v >> 32)); }
        void Str(const std::string& s) { const size_t n = s.size() < 64 ? s.size() : 64; U8(static_cast<std::uint8_t>(n)); b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n)); }
        void Bytes(const std::uint8_t* d, size_t n) { U32(static_cast<std::uint32_t>(n)); b.insert(b.end(), d, d + n); }
    };
    // la lettura controlla sempre i limiti: i dati arrivano dalla rete
    struct Reader {
        const std::uint8_t* p; size_t n; bool ok = true;
        Reader(const std::uint8_t* d, size_t len) : p(d), n(len) {}
        bool Need(size_t k) { if (!ok || n < k) { ok = false; return false; } return true; }
        std::uint8_t U8() { if (!Need(1)) return 0; const std::uint8_t v = *p; ++p; --n; return v; }
        std::uint16_t U16() { const std::uint16_t a = U8(); return static_cast<std::uint16_t>(a | (static_cast<std::uint16_t>(U8()) << 8)); }
        std::uint32_t U32() { const std::uint32_t a = U16(); return a | (static_cast<std::uint32_t>(U16()) << 16); }
        std::uint64_t U64() { const std::uint64_t a = U32(); return a | (static_cast<std::uint64_t>(U32()) << 32); }
        std::string Str() { const size_t k = U8(); if (k > 64 || !Need(k)) { ok = false; return {}; } std::string s(reinterpret_cast<const char*>(p), k); p += k; n -= k; return s; }
        bool Bytes(std::vector<std::uint8_t>& out, size_t maxLen) { const size_t k = U32(); if (k > maxLen || !Need(k)) { ok = false; return false; } out.assign(p, p + k); p += k; n -= k; return true; }
    };
}
