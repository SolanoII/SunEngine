// Server d'incontro di Project SunCollector (vedi Engine/Net/ServerProtocol.h).
// Uso:  SunCollectorServer [porta]      (porta UDP, predefinita 27640: e' l'unica da aprire, e solo qui)
// Non salva niente su disco: se si riavvia, le lobby si ripubblicano da sole in pochi secondi.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>
#include "ServerProtocol.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace SunEngine::Net::Wire;

namespace {
    double Now() { using c = std::chrono::steady_clock; static const c::time_point t0 = c::now(); return std::chrono::duration<double>(c::now() - t0).count(); }
    std::string Ip(std::uint32_t hostOrder) { char b[32]; std::snprintf(b, sizeof(b), "%u.%u.%u.%u", hostOrder >> 24, (hostOrder >> 16) & 255u, (hostOrder >> 8) & 255u, hostOrder & 255u); return b; }
    void Log(const char* fmt, const std::string& a = {}, const std::string& b = {}) {
        std::time_t t = std::time(nullptr); char ts[32]; std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
        std::printf("[%s] ", ts); std::printf(fmt, a.c_str(), b.c_str()); std::printf("\n"); std::fflush(stdout);
    }

    struct Client {
        ENetPeer* peer = nullptr;
        std::uint64_t id = 0;
        std::string name;
        std::uint32_t pubIp = 0, lip = 0; std::uint16_t pubPort = 0, lport = 0;
        double relayWindow = 0.0; size_t relayBytes = 0;   // limite del ponte: byte nell'ultimo secondo
    };
    struct Lobby {
        std::uint32_t code = 0; std::uint64_t owner = 0; std::uint16_t key = 0;
        std::string name, leaderName;
        std::uint8_t privacy = 0, members = 1, maxMembers = 5, inSession = 0;
        double updated = 0.0;
    };

    std::unordered_map<std::uint64_t, Client*> g_ById;
    std::unordered_map<std::uint32_t, Lobby> g_Lobbies;          // per codice
    std::mt19937 g_Rng{ static_cast<unsigned>(std::random_device{}()) };
    const size_t kRelayBytesPerSecond = 192 * 1024;             // per giocatore: abbastanza per una partita, non per abusarne

    void Send(ENetPeer* p, const Writer& w, bool reliable = true) {
        if (!p) return;
        ENetPacket* pkt = enet_packet_create(w.b.data(), w.b.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (pkt && enet_peer_send(p, reliable ? 0 : 1, pkt) < 0) enet_packet_destroy(pkt);
    }
    Lobby* LobbyOfOwner(std::uint64_t owner) { for (auto& kv : g_Lobbies) if (kv.second.owner == owner) return &kv.second; return nullptr; }
    std::uint32_t NewCode() {
        std::uniform_int_distribution<std::uint32_t> d(100000u, 999999u);
        for (int i = 0; i < 1000; ++i) { const std::uint32_t c = d(g_Rng); if (!g_Lobbies.count(c)) return c; }
        return 0;
    }
    void WriteWhere(Writer& w, const Client& c) { w.U32(c.pubIp); w.U16(c.pubPort); w.U32(c.lip); w.U16(c.lport); }

    // chi entra e il capo: ognuno sa dov'e' l'altro, e il capo apre il suo router verso chi arriva
    void Introduce(Client& joiner, Client& owner, const Lobby& lb, bool withKey) {
        Writer f; f.U8(S_Found); f.U64(owner.id); WriteWhere(f, owner); f.U16(withKey ? lb.key : 0); f.U8(withKey ? 1 : 0);
        Send(joiner.peer, f);
        Writer in; in.U8(S_Incoming); in.U64(joiner.id); in.Str(joiner.name); WriteWhere(in, joiner);
        Send(owner.peer, in);
    }

    void OnMessage(Client& c, const std::uint8_t* data, size_t size) {
        if (size < 1) return;
        Reader r(data + 1, size - 1);
        const std::uint8_t type = data[0];
        if (type != C_Hello && c.id == 0) return;   // prima ci si presenta
        switch (type) {
        case C_Hello: {
            const std::uint32_t proto = r.U32(); const std::uint64_t id = r.U64(); const std::string nm = r.Str(); const std::uint32_t lip = r.U32(); const std::uint16_t lport = r.U16();
            if (!r.ok || proto != kServerProto || id == 0) { enet_peer_disconnect_later(c.peer, 1); return; }
            if (auto it = g_ById.find(id); it != g_ById.end() && it->second != &c) { it->second->id = 0; enet_peer_disconnect_later(it->second->peer, 2); }   // stesso giocatore da un collegamento nuovo
            c.id = id; c.name = nm; c.lip = lip; c.lport = lport;
            g_ById[id] = &c;
            Writer w; w.U8(S_Welcome); w.U32(c.pubIp); w.U16(c.pubPort);
            Send(c.peer, w);
            Log("collegato %s da %s", nm, Ip(c.pubIp) + ":" + std::to_string(c.pubPort));
            break;
        }
        case C_Publish: {
            const std::uint32_t want = r.U32(); const std::uint16_t key = r.U16(); const std::string name = r.Str(), leader = r.Str();
            const std::uint8_t priv = r.U8(), members = r.U8(), mx = r.U8(), sess = r.U8();
            if (!r.ok || priv > 2) return;
            Lobby* mine = LobbyOfOwner(c.id);
            std::uint32_t code = mine ? mine->code : 0;
            if (!mine) {
                // codice gia' noto alla squadra: lo tiene (anche dopo un cambio di capo, se la chiave e' giusta)
                auto it = g_Lobbies.find(want);
                if (want >= 100000u && want <= 999999u && (it == g_Lobbies.end() || it->second.key == key)) code = want;
                else code = NewCode();
                if (!code) return;
                if (it != g_Lobbies.end() && code == want && it->second.owner != c.id) Log("lobby %s passa a %s", std::to_string(code), c.name);
            }
            Lobby& lb = g_Lobbies[code];
            const bool fresh = lb.owner == 0;
            lb.code = code; lb.owner = c.id; lb.key = key; lb.name = name; lb.leaderName = leader;
            lb.privacy = priv; lb.members = members; lb.maxMembers = mx; lb.inSession = sess; lb.updated = Now();
            if (fresh) Log("lobby %s aperta da %s", std::to_string(code), c.name);
            Writer w; w.U8(S_Code); w.U32(code);
            Send(c.peer, w);
            break;
        }
        case C_Unpublish:
            if (Lobby* lb = LobbyOfOwner(c.id)) { Log("lobby %s chiusa", std::to_string(lb->code)); g_Lobbies.erase(lb->code); }
            break;
        case C_List: {
            Writer w; w.U8(S_List);
            std::vector<const Lobby*> vis;
            for (auto& kv : g_Lobbies) if (kv.second.privacy != 2) vis.push_back(&kv.second);
            if (vis.size() > 120) vis.resize(120);
            w.U8(static_cast<std::uint8_t>(vis.size()));
            for (const Lobby* lb : vis) {
                const auto it = g_ById.find(lb->owner);
                w.U64(lb->owner); w.Str(lb->name); w.Str(lb->leaderName);
                w.U8(lb->privacy); w.U8(lb->members); w.U8(lb->maxMembers); w.U8(lb->inSession);
                w.U16(static_cast<std::uint16_t>(it != g_ById.end() && it->second->peer ? it->second->peer->roundTripTime : 0));
            }
            Send(c.peer, w);
            break;
        }
        case C_LookupCode: {
            const std::uint32_t code = r.U32();
            auto it = g_Lobbies.find(code);
            Client* owner = it != g_Lobbies.end() ? (g_ById.count(it->second.owner) ? g_ById[it->second.owner] : nullptr) : nullptr;
            if (!r.ok || !owner || owner == &c) { Writer w; w.U8(S_NotFound); Send(c.peer, w); break; }
            Introduce(c, *owner, it->second, true);
            break;
        }
        case C_LookupLeader: {
            const std::uint64_t lead = r.U64();
            Lobby* lb = LobbyOfOwner(lead);
            Client* owner = g_ById.count(lead) ? g_ById[lead] : nullptr;
            if (!r.ok || !lb || !owner || lb->privacy == 2 || owner == &c) { Writer w; w.U8(S_NotFound); Send(c.peer, w); break; }
            Introduce(c, *owner, *lb, false);
            break;
        }
        case C_Introduce: {
            const std::uint64_t to = r.U64();
            auto it = g_ById.find(to);
            if (!r.ok || it == g_ById.end()) break;
            Writer in; in.U8(S_Incoming); in.U64(c.id); in.Str(c.name); WriteWhere(in, c);
            Send(it->second->peer, in);
            break;
        }
        case C_Relay: {
            const std::uint64_t to = r.U64(); const bool reliable = r.U8() != 0;
            std::vector<std::uint8_t> bytes;
            if (!r.Bytes(bytes, 64 * 1024)) break;
            auto it = g_ById.find(to);
            if (it == g_ById.end()) break;
            const double now = Now();
            if (now - c.relayWindow > 1.0) { c.relayWindow = now; c.relayBytes = 0; }
            c.relayBytes += bytes.size();
            if (c.relayBytes > kRelayBytesPerSecond) break;   // troppo: si scarta (i messaggi importanti vengono rispediti)
            Writer w; w.U8(S_Relay); w.U64(c.id); w.Bytes(bytes.data(), bytes.size());
            Send(it->second->peer, w, reliable);
            break;
        }
        default: break;
        }
    }
}

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : kDefaultServerPort;
    if (enet_initialize() != 0) { std::printf("ENet non disponibile\n"); return 1; }
    ENetAddress a; a.host = ENET_HOST_ANY; a.port = static_cast<enet_uint16>(port);
    ENetHost* host = enet_host_create(&a, 2048, 2, 0, 0);
    if (!host) { std::printf("Impossibile aprire la porta UDP %d (gia' in uso?)\n", port); return 1; }
    Log("server d'incontro in ascolto sulla porta UDP %s%s", std::to_string(port), "");
    double lastSweep = 0.0;
    for (;;) {
        ENetEvent ev;
        while (enet_host_service(host, &ev, 10) > 0) {
            switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                auto* c = new Client(); c->peer = ev.peer; c->pubIp = ENET_NET_TO_HOST_32(ev.peer->address.host); c->pubPort = ev.peer->address.port;
                ev.peer->data = c;
                enet_peer_timeout(ev.peer, 0, 5000, 10000);   // chi sparisce libera la lobby in pochi secondi
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE:
                if (ev.peer->data) OnMessage(*static_cast<Client*>(ev.peer->data), ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT: {
                auto* c = static_cast<Client*>(ev.peer->data);
                if (c) {
                    if (c->id) {
                        if (Lobby* lb = LobbyOfOwner(c->id)) { Log("lobby %s chiusa (capo uscito)", std::to_string(lb->code)); g_Lobbies.erase(lb->code); }
                        auto it = g_ById.find(c->id);
                        if (it != g_ById.end() && it->second == c) g_ById.erase(it);
                        Log("uscito %s", c->name);
                    }
                    delete c;
                    ev.peer->data = nullptr;
                }
                break;
            }
            default: break;
            }
        }
        // lobby non piu' aggiornate dal capo (gioco bloccato, rete persa)
        const double now = Now();
        if (now - lastSweep > 2.0) {
            lastSweep = now;
            for (auto it = g_Lobbies.begin(); it != g_Lobbies.end();) {
                if (now - it->second.updated > 15.0) { Log("lobby %s scaduta", std::to_string(it->first)); it = g_Lobbies.erase(it); }
                else ++it;
            }
        }
    }
}
