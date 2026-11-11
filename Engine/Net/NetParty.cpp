// ENet per primo: su Windows porta con se' winsock2.h, che deve arrivare prima di windows.h
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>
#include "NetParty.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <unordered_map>

namespace SunEngine::Net {

    namespace {
        const std::uint32_t kProto = 0x53554E32u;        // "SUN2": versione del protocollo
        const std::uint16_t kDiscoveryPort = 27649;
        const std::uint32_t kDiscoveryMagic = 0x53554E4Cu;   // "SUNL": annuncio di una lobby visibile
        const std::uint32_t kQueryMagic = 0x53554E51u;       // "SUNQ": chi ha la squadra con questo codice a 6 cifre?
        const std::uint32_t kReplyMagic = 0x53554E52u;       // "SUNR": ce l'ho io, ecco dove collegarti
        const size_t kMaxSnapshot = 512 * 1024;
        const double kSilentSuspect = 2.0;     // secondi di silenzio dopo i quali un compagno e' sospetto
        const double kSuspectResolve = 1.0;    // poi si decide: caduto lui, o isolato io
        const double kFresh = 1.0;             // "l'ho sentito da poco"

        enum Msg : std::uint8_t {
            MsgHello = 1, MsgDenied, MsgPending, MsgRedirect, MsgState, MsgMemberUpdate, MsgLeave, MsgPing,
            MsgSessionStart, MsgSessionEnd, MsgHostAnnounce, MsgSnapshot, MsgGameToHost, MsgGameToAll
        };

        // --- scrittura/lettura dei messaggi: la lettura controlla sempre i limiti (i dati arrivano dalla rete) ---
        struct Writer {
            std::vector<std::uint8_t> b;
            void U8(std::uint8_t v) { b.push_back(v); }
            void U16(std::uint16_t v) { U8(static_cast<std::uint8_t>(v)); U8(static_cast<std::uint8_t>(v >> 8)); }
            void U32(std::uint32_t v) { U16(static_cast<std::uint16_t>(v)); U16(static_cast<std::uint16_t>(v >> 16)); }
            void U64(std::uint64_t v) { U32(static_cast<std::uint32_t>(v)); U32(static_cast<std::uint32_t>(v >> 32)); }
            void Str(const std::string& s) { const size_t n = (std::min)(s.size(), static_cast<size_t>(64)); U8(static_cast<std::uint8_t>(n)); b.insert(b.end(), s.begin(), s.begin() + n); }
            void Bytes(const std::uint8_t* d, size_t n) { U32(static_cast<std::uint32_t>(n)); b.insert(b.end(), d, d + n); }
        };
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

        double NowSeconds() {
            using clock = std::chrono::steady_clock;
            static const clock::time_point t0 = clock::now();
            return std::chrono::duration<double>(clock::now() - t0).count();
        }

        std::string IpToString(std::uint32_t hostOrder) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (hostOrder >> 24) & 255u, (hostOrder >> 16) & 255u, (hostOrder >> 8) & 255u, hostOrder & 255u);
            return buf;
        }
        // ENet tiene gli indirizzi in ordine di rete; qui si ragiona sempre in ordine "umano" (a.b.c.d = a nel byte alto)
        std::uint32_t FromEnet(std::uint32_t netOrder) { return ENET_NET_TO_HOST_32(netOrder); }
        std::uint32_t ToEnet(std::uint32_t hostOrder) { return ENET_HOST_TO_NET_32(hostOrder); }
        bool ParseIp(const std::string& s, std::uint32_t& out) {
            unsigned a, b, c, d; char tail;
            if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
            out = (a << 24) | (b << 16) | (c << 8) | d;
            return true;
        }
        bool IsLoopback(std::uint32_t ip) { return ip == 0 || (ip >> 24) == 127u; }

        // Codice d'invito: indirizzo, porta e chiave della squadra in 15 lettere/cifre senza quelle che si confondono
        const char* kAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        std::string EncodeCode(std::uint32_t ip, std::uint16_t port, std::uint16_t key) {
            std::uint8_t raw[9] = { static_cast<std::uint8_t>(ip >> 24), static_cast<std::uint8_t>(ip >> 16), static_cast<std::uint8_t>(ip >> 8), static_cast<std::uint8_t>(ip),
                                    static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port), static_cast<std::uint8_t>(key >> 8), static_cast<std::uint8_t>(key), 0 };
            std::uint8_t sum = 0x5A; for (int i = 0; i < 8; ++i) sum = static_cast<std::uint8_t>(sum * 31u + raw[i]);
            raw[8] = sum;
            std::string out;
            unsigned acc = 0; int bits = 0;
            for (int i = 0; i < 9; ++i) {
                acc = (acc << 8) | raw[i]; bits += 8;
                while (bits >= 5) { out += kAlphabet[(acc >> (bits - 5)) & 31u]; bits -= 5; }
            }
            if (bits > 0) out += kAlphabet[(acc << (5 - bits)) & 31u];
            std::string grouped;
            for (size_t i = 0; i < out.size(); ++i) { if (i && i % 5 == 0) grouped += '-'; grouped += out[i]; }
            return grouped;
        }
        bool DecodeCode(const std::string& text, std::uint32_t& ip, std::uint16_t& port, std::uint16_t& key) {
            std::vector<std::uint8_t> raw;
            unsigned acc = 0; int bits = 0, count = 0;
            for (char ch : text) {
                if (ch == '-' || ch == ' ') continue;
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                const char* pos = std::strchr(kAlphabet, ch);
                if (!pos) return false;
                acc = (acc << 5) | static_cast<unsigned>(pos - kAlphabet); bits += 5; ++count;
                if (bits >= 8) { raw.push_back(static_cast<std::uint8_t>((acc >> (bits - 8)) & 255u)); bits -= 8; }
            }
            if (count != 15 || raw.size() < 9) return false;
            std::uint8_t sum = 0x5A; for (int i = 0; i < 8; ++i) sum = static_cast<std::uint8_t>(sum * 31u + raw[i]);
            if (sum != raw[8]) return false;
            ip = (static_cast<std::uint32_t>(raw[0]) << 24) | (static_cast<std::uint32_t>(raw[1]) << 16) | (static_cast<std::uint32_t>(raw[2]) << 8) | raw[3];
            port = static_cast<std::uint16_t>((raw[4] << 8) | raw[5]);
            key = static_cast<std::uint16_t>((raw[6] << 8) | raw[7]);
            return true;
        }

        int g_EnetUsers = 0;
    }

    PeerId RandomPeerId() {
        std::random_device rd;
        std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^ static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        PeerId id = 0;
        while (id == 0) id = gen();
        return id;
    }

    struct Party::Impl {
        struct Link {
            ENetPeer* peer = nullptr;
            PeerId id = 0;             // 0 finche' non arriva il suo Hello
            bool outgoing = false;     // l'ho aperto io
            bool established = false;  // compagno di squadra confermato
            bool joinAttempt = false;  // e' il mio tentativo di entrare in una squadra
            double created = 0.0;
            std::string helloName;
        };
        // Dove si trova un compagno: come lo vede il capo (indirizzo pubblico, dietro il router) e come si vede lui
        // nella sua rete (indirizzo locale). Si usa il locale fra computer dietro lo stesso router.
        struct Addr { std::uint32_t ip = 0; std::uint16_t port = 0; std::uint32_t lip = 0; std::uint16_t lport = 0; };
        struct Report { float rtt = 999.0f, loss = 0.0f; bool fresh = false; double time = 0.0; };

        Party* owner = nullptr;
        ENetHost* host = nullptr;
        ENetSocket disc = ENET_SOCKET_NULL;
        std::uint16_t port = 0;
        PeerId selfId = 0;
        std::string name, unit, advertisedIp;
        bool ready = false;

        PartyState state = PartyState::Offline;
        std::string status;
        std::vector<std::string> events;

        std::vector<Member> members;
        std::unordered_map<PeerId, Addr> addrs;
        PeerId leaderId = 0;
        Privacy privacy = Privacy::Private;
        int maxMembers = 5;
        std::uint16_t key = 0;
        std::uint32_t code6 = 0;           // codice a 6 cifre della squadra (da dettare o incollare)
        std::uint32_t lookupCode = 0;      // sto cercando in rete la squadra con questo codice
        double lookupUntil = 0.0, lastLookup = 0.0;
        std::uint32_t version = 0;
        int nextOrder = 0;

        std::vector<Link*> links;
        std::unordered_map<PeerId, double> lastAttempt;
        std::unordered_map<PeerId, int> attempts;            // tentativi di collegamento diretto (si alternano gli indirizzi)
        double lastPunch = 0.0;
        std::uint32_t localIp = 0;                           // il mio indirizzo nella rete locale
        std::unordered_map<PeerId, std::unordered_map<PeerId, Report>> matrix;   // chi -> verso chi
        std::unordered_map<PeerId, double> suspectSince;
        std::vector<JoinRequest> requests;
        double joinStarted = 0.0;
        std::uint16_t joinKey = 0; bool joinHasKey = false;

        bool session = false;
        PeerId hostId = 0;
        std::uint32_t epoch = 0;
        std::vector<PeerId> succession;
        std::vector<std::uint8_t> snapshot;
        double snapshotTime = -1.0;
        double lastAnnounce = 0.0;

        std::vector<LobbyInfo> lobbies;
        std::unordered_map<PeerId, Addr> lobbyAddrs;
        double lastBroadcast = 0.0, lastReport = 0.0, lastMesh = 0.0, now = 0.0;

        // ---------- utilita' ----------
        Member* Find(PeerId id) { for (auto& mb : members) if (mb.id == id) return &mb; return nullptr; }
        Link* LinkOf(PeerId id) { for (auto* l : links) if (l->id == id && l->established) return l; return nullptr; }
        bool IsLeader() const { return state == PartyState::InParty && leaderId == selfId; }
        void Event(const std::string& s) { events.push_back(s); status = s; if (events.size() > 32) events.erase(events.begin()); }
        std::string NameOf(PeerId id) { if (auto* mb = Find(id)) return mb->name; return "?"; }

        void Send(Link* l, const Writer& w, bool reliable = true, int channel = 0) {
            if (!l || !l->peer || l->peer->state != ENET_PEER_STATE_CONNECTED) return;
            ENetPacket* pkt = enet_packet_create(w.b.data(), w.b.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
            if (pkt && enet_peer_send(l->peer, static_cast<enet_uint8>(channel), pkt) < 0) enet_packet_destroy(pkt);
        }
        void SendMembers(const Writer& w, bool reliable = true, int channel = 0) {
            for (auto* l : links) if (l->established) Send(l, w, reliable, channel);
        }

        Link* Connect(std::uint32_t ip, std::uint16_t prt, bool joinAttempt) {
            if (!host) return nullptr;
            ENetAddress a; a.host = ToEnet(ip); a.port = prt;
            ENetPeer* p = enet_host_connect(host, &a, 2, 0);
            if (!p) return nullptr;
            auto* l = new Link(); l->peer = p; l->outgoing = true; l->joinAttempt = joinAttempt; l->created = now;
            p->data = l;
            links.push_back(l);
            return l;
        }
        void Tune(ENetPeer* p) {
            enet_peer_timeout(p, 0, 4000, 6000);     // caduta rilevata da ENet in 4-6 s (noi sospettiamo gia' dopo 2)
            enet_peer_ping_interval(p, 250);
        }
        void Drop(Link* l, bool hard) {
            if (!l) return;
            if (l->peer) { l->peer->data = nullptr; if (hard || l->peer->state != ENET_PEER_STATE_CONNECTED) enet_peer_reset(l->peer); else enet_peer_disconnect_later(l->peer, 0); }
            links.erase(std::remove(links.begin(), links.end(), l), links.end());
            delete l;
        }
        double SilentFor(Link* l) const {
            if (!l || !l->peer) return 1e9;
            if (l->peer->lastReceiveTime == 0) return (std::max)(0.0, now - l->created);   // ancora nessuna conferma da lui
            const enet_uint32 diff = enet_time_get() - l->peer->lastReceiveTime;   // millisecondi dall'ultima conferma ricevuta
            return diff > 0x7FFFFFFFu ? 0.0 : static_cast<double>(diff) / 1000.0;
        }

        void ResetParty(const std::string& why) {
            for (auto* l : std::vector<Link*>(links)) Drop(l, false);
            members.clear(); addrs.clear(); matrix.clear(); suspectSince.clear(); requests.clear(); lastAttempt.clear(); attempts.clear();
            leaderId = 0; version = 0; nextOrder = 0; key = 0; code6 = 0; lookupCode = 0;
            const bool had = session;
            session = false; hostId = 0; epoch = 0; succession.clear(); snapshot.clear(); snapshotTime = -1.0;
            state = host ? PartyState::Idle : PartyState::Offline;
            if (!why.empty()) Event(why);
            if (had && owner->onHostChanged) owner->onHostChanged(0, false, false);
        }

        void SendHello(Link* l, bool wantMesh) {
            Writer w; w.U8(MsgHello); w.U32(kProto); w.U64(selfId); w.Str(name); w.Str(unit); w.U16(port);
            w.U16(wantMesh ? key : joinKey); w.U8(wantMesh ? 1 : (joinHasKey ? 1 : 0)); w.U8(wantMesh ? 1 : 0);
            w.U32(localIp);
            Send(l, w);
        }

        // ---------- stato della squadra (lo decide il capo) ----------
        Writer BuildState() {
            Writer w; w.U8(MsgState); w.U32(version); w.U64(leaderId); w.U8(static_cast<std::uint8_t>(privacy)); w.U8(static_cast<std::uint8_t>(maxMembers)); w.U16(key);
            w.U32(code6);
            w.U8(session ? 1 : 0); w.U32(epoch); w.U64(hostId);
            w.U8(static_cast<std::uint8_t>((std::min)(succession.size(), static_cast<size_t>(16))));
            for (size_t i = 0; i < succession.size() && i < 16; ++i) w.U64(succession[i]);
            w.U8(static_cast<std::uint8_t>(members.size()));
            for (const auto& mb : members) {
                const Addr a = addrs[mb.id];
                w.U64(mb.id); w.Str(mb.name); w.Str(mb.unitId); w.U8(mb.ready ? 1 : 0); w.U32(static_cast<std::uint32_t>(mb.joinOrder)); w.U32(a.ip); w.U16(a.port); w.U32(a.lip); w.U16(a.lport);
            }
            return w;
        }
        void BroadcastState() {
            if (!IsLeader()) return;
            ++version;
            SendMembers(BuildState());
        }
        void RefreshFlags() {
            for (auto& mb : members) {
                mb.self = mb.id == selfId; mb.leader = mb.id == leaderId;
                Link* l = LinkOf(mb.id);
                mb.linked = mb.self || l != nullptr;
                if (l && l->peer) { mb.rttMs = static_cast<float>(l->peer->roundTripTime); mb.lossPct = 100.0f * static_cast<float>(l->peer->packetLoss) / static_cast<float>(ENET_PEER_PACKET_LOSS_SCALE); }
                else { mb.rttMs = 0.0f; mb.lossPct = 0.0f; }
                mb.score = mb.id == 0 ? 0.0f : Score(mb.id);
            }
            std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) { return a.joinOrder < b.joinOrder; });
        }

        // Latenza fra due compagni: quella che misuro io se c'entro, altrimenti quella che riferiscono loro
        bool PairQuality(PeerId a, PeerId b, float& rtt, float& loss) {
            float sum = 0.0f, ls = 0.0f; int n = 0;
            auto mine = [&](PeerId other) { if (Link* l = LinkOf(other)) { sum += static_cast<float>(l->peer->roundTripTime); ls += 100.0f * static_cast<float>(l->peer->packetLoss) / static_cast<float>(ENET_PEER_PACKET_LOSS_SCALE); ++n; } };
            if (a == selfId) mine(b); else if (b == selfId) mine(a);
            auto rep = [&](PeerId from, PeerId to) {
                auto it = matrix.find(from); if (it == matrix.end()) return;
                auto jt = it->second.find(to); if (jt == it->second.end() || now - jt->second.time > 3.0 || !jt->second.fresh) return;
                sum += jt->second.rtt; ls += jt->second.loss; ++n;
            };
            if (a != selfId && b != selfId) { rep(a, b); rep(b, a); }
            if (n == 0) return false;
            rtt = sum / n; loss = ls / n;
            return true;
        }
        // Qualita' della connessione di un compagno verso tutti gli altri: latenza media + pacchetti persi (pesano molto)
        float Score(PeerId c) {
            float total = 0.0f; int n = 0;
            for (const auto& mb : members) {
                if (mb.id == c) continue;
                float rtt = 0.0f, loss = 0.0f;
                if (PairQuality(c, mb.id, rtt, loss)) total += rtt + loss * 4.0f; else total += 999.0f;   // collegamento mancante: pessimo host
                ++n;
            }
            return n ? total / n : 0.0f;
        }
        std::vector<PeerId> ComputeSuccession(PeerId first) {
            std::vector<std::pair<float, PeerId>> v;
            for (const auto& mb : members) if (mb.id != first) v.push_back({ Score(mb.id), mb.id });
            std::sort(v.begin(), v.end());
            std::vector<PeerId> out;
            if (first) out.push_back(first);
            for (auto& e : v) out.push_back(e.second);
            return out;
        }

        // Indirizzi da provare per raggiungere un compagno, il piu' probabile per primo
        std::vector<Addr> Candidates(PeerId id) {
            std::vector<Addr> out;
            const Addr a = addrs[id];
            const Link* ll = LinkOf(leaderId);
            const std::uint32_t leaderSeen = (ll && ll->peer) ? FromEnet(ll->peer->address.host) : 0x7F000001u;
            // indirizzo pubblico (0 = il capo stesso: quello da cui lo vedo io)
            Addr pub; pub.ip = (a.ip == 0 || IsLoopback(a.ip)) ? leaderSeen : a.ip; pub.port = a.ip == 0 ? ((ll && ll->peer) ? ll->peer->address.port : a.port) : a.port;
            if (pub.port == 0) pub.port = a.lport;
            // io come mi vede il capo: se il compagno e' dietro il mio stesso router, meglio il suo indirizzo locale
            const Addr me = addrs[selfId];
            const std::uint32_t myPub = me.ip;
            const bool sameRouter = myPub != 0 && myPub == a.ip;
            Addr loc; loc.ip = a.lip; loc.port = a.lport;
            if (a.lip == localIp && a.lip != 0) loc.ip = 0x7F000001u;   // stesso computer
            const bool locOk = loc.ip != 0 && loc.port != 0;
            if (sameRouter && locOk) { out.push_back(loc); if (pub.ip && pub.port) out.push_back(pub); }
            else { if (pub.ip && pub.port) out.push_back(pub); if (locOk) out.push_back(loc); }
            return out;
        }

        void EnsureMesh() {
            if (state != PartyState::InParty) return;
            const bool punch = now - lastPunch > 0.5;
            if (punch) lastPunch = now;
            for (const auto& mb : members) {
                if (mb.id == selfId || LinkOf(mb.id)) continue;
                const std::vector<Addr> cand = Candidates(mb.id);
                if (cand.empty()) continue;
                if (selfId < mb.id) {
                    // fra due compagni chiama quello con l'id piu' alto; io intanto mando pacchetti vuoti verso di lui:
                    // aprono il mio router alla sua chiamata (ENet ignora i pacchetti da un byte)
                    if (punch && host) {
                        std::uint8_t one = 0; ENetBuffer buf; buf.data = &one; buf.dataLength = 1;
                        for (const Addr& c : cand) { ENetAddress to; to.host = ToEnet(c.ip); to.port = c.port; enet_socket_send(host->socket, &to, &buf, 1); }
                    }
                    continue;
                }
                bool pending = false;
                for (auto* l : links) if (l->outgoing && !l->established && !l->joinAttempt && l->id == mb.id) pending = true;
                if (pending) continue;
                double& last = lastAttempt[mb.id];
                if (last > 0.0 && now - last < 2.0) continue;
                last = now;
                // un tentativo per volta, alternando gli indirizzi
                const int k = attempts[mb.id]++;
                const Addr& c = cand[static_cast<size_t>(k) % cand.size()];
                if (Link* l = Connect(c.ip, c.port, false)) { l->id = mb.id; enet_peer_timeout(l->peer, 0, 1800, 1800); }   // tentativo breve
            }
        }

        // ---------- sessione ----------
        void ApplyHost(PeerId h, std::uint32_t e, const std::vector<PeerId>& list, bool migrated) {
            const bool changed = !session || h != hostId || e != epoch;
            session = true; hostId = h; epoch = e; succession = list;
            if (changed) {
                Event((migrated ? "Nuovo host: " : "Host della partita: ") + NameOf(h));
                if (owner->onHostChanged) owner->onHostChanged(h, h == selfId, migrated);
            }
        }
        void Announce() {
            succession = ComputeSuccession(selfId);
            Writer w; w.U8(MsgHostAnnounce); w.U32(epoch); w.U64(hostId);
            w.U8(static_cast<std::uint8_t>((std::min)(succession.size(), static_cast<size_t>(16))));
            for (size_t i = 0; i < succession.size() && i < 16; ++i) w.U64(succession[i]);
            SendMembers(w);
            lastAnnounce = now;
        }
        // L'host non c'e' piu': subentra il primo della lista ancora presente
        void Migrate() {
            if (!session) return;
            PeerId next = 0;
            for (PeerId id : succession) if (id != hostId && Find(id)) { next = id; break; }
            if (!next) { auto v = ComputeSuccession(0); for (PeerId id : v) if (id != hostId) { next = id; break; } }
            if (!next) next = selfId;
            if (next == selfId) {
                const std::uint32_t e = epoch + 1;
                ApplyHost(selfId, e, ComputeSuccession(selfId), true);
                Announce();
                if (IsLeader()) BroadcastState();
            }
            else hostId = next;   // provvisorio: lo conferma il suo annuncio (con l'epoca nuova)
        }

        // ---------- un compagno non c'e' piu' ----------
        void MemberGone(PeerId id, bool graceful) {
            Member* mb = Find(id);
            if (!mb) return;
            const std::string who = mb->name;
            const bool wasLeader = id == leaderId, wasHost = session && id == hostId;
            if (Link* l = LinkOf(id)) Drop(l, !graceful);
            members.erase(std::remove_if(members.begin(), members.end(), [&](const Member& x) { return x.id == id; }), members.end());
            addrs.erase(id); matrix.erase(id); suspectSince.erase(id);
            for (auto& row : matrix) row.second.erase(id);
            Event(who + (graceful ? " ha lasciato la squadra" : " ha perso la connessione"));
            if (wasLeader) {
                // nuovo capo: chi e' in squadra da piu' tempo
                PeerId best = 0; int order = 1 << 30;
                for (const auto& x : members) if (x.joinOrder < order) { order = x.joinOrder; best = x.id; }
                leaderId = best;
                if (leaderId == selfId) { version += 100; Event("Ora sei il capo della squadra"); }
                else Event("Nuovo capo: " + NameOf(leaderId));
            }
            if (wasHost) Migrate();
            RefreshFlags();
            if (IsLeader()) BroadcastState();
        }

        // ---------- messaggi in arrivo ----------
        void OnHello(Link* l, Reader& r) {
            const std::uint32_t proto = r.U32(); const PeerId id = r.U64(); const std::string nm = r.Str(), un = r.Str();
            const std::uint16_t lport = r.U16(), k = r.U16(); const bool hasKey = r.U8() != 0, wantMesh = r.U8() != 0;
            const std::uint32_t lip = r.U32();
            auto deny = [&](const char* why) { Writer w; w.U8(MsgDenied); w.Str(why); Send(l, w); enet_peer_disconnect_later(l->peer, 0); };
            if (!r.ok || proto != kProto || id == 0 || id == selfId) { deny("Versione del gioco diversa"); return; }
            l->id = id; l->helloName = nm;
            if (state != PartyState::InParty) { deny("Nessuna squadra a questo indirizzo"); return; }
            if (Find(id)) {   // un compagno che apre il collegamento diretto con me
                if (wantMesh && k != key) { deny("Chiave della squadra errata"); return; }
                for (auto* o : std::vector<Link*>(links)) if (o != l && o->id == id && o->established) Drop(o, true);   // collegamento vecchio rimasto appeso
                l->established = true; suspectSince.erase(id);
                RefreshFlags();
                return;
            }
            if (wantMesh) return;   // forse non mi e' ancora arrivato l'elenco che lo contiene: resta in attesa (vedi ApplyState)
            if (!IsLeader()) {      // si entra dal capo
                Writer w; w.U8(MsgRedirect);
                const Addr a = addrs[leaderId]; std::uint32_t ip = a.ip;
                if (Link* ll = LinkOf(leaderId)) ip = FromEnet(ll->peer->address.host);
                w.U32(ip); w.U16(LinkOf(leaderId) ? LinkOf(leaderId)->peer->address.port : a.port);
                Send(l, w); enet_peer_disconnect_later(l->peer, 0);
                return;
            }
            if (static_cast<int>(members.size()) >= maxMembers) { deny("Squadra al completo"); return; }
            const bool keyOk = hasKey && k == key;
            if (privacy == Privacy::Private && !keyOk) { deny("Squadra privata: serve il codice d'invito"); return; }
            if (privacy == Privacy::Closed && !keyOk) {
                bool known = false; for (auto& q : requests) if (q.id == id) known = true;
                if (!known) { requests.push_back({ id, nm }); Event(nm + " chiede di entrare"); }
                addrs[id] = { FromEnet(l->peer->address.host), l->peer->address.port, lip, lport };
                pendingUnit[id] = un;
                Writer w; w.U8(MsgPending); Send(l, w);
                return;
            }
            Admit(l, id, nm, un, lport, lip);
        }
        std::unordered_map<PeerId, std::string> pendingUnit;
        void Admit(Link* l, PeerId id, const std::string& nm, const std::string& un, std::uint16_t lport, std::uint32_t lip) {
            Member mb; mb.id = id; mb.name = nm.empty() ? "Giocatore" : nm; mb.unitId = un; mb.joinOrder = nextOrder++;
            members.push_back(mb);
            addrs[id] = { FromEnet(l->peer->address.host), l->peer->address.port, lip, lport };   // porta vista dal capo: e' quella aperta nel router
            l->established = true;
            Event(mb.name + " e' entrato in squadra");
            RefreshFlags();
            BroadcastState();
        }

        void ApplyState(Link* from, Reader& r) {
            const std::uint32_t ver = r.U32(); const PeerId lead = r.U64(); const std::uint8_t priv = r.U8(), mx = r.U8(); const std::uint16_t k = r.U16();
            const std::uint32_t c6 = r.U32();
            const bool sess = r.U8() != 0; const std::uint32_t ep = r.U32(); const PeerId hst = r.U64();
            std::vector<PeerId> succ; { const int n = r.U8(); if (n > 16) return; for (int i = 0; i < n; ++i) succ.push_back(r.U64()); }
            const int count = r.U8();
            if (!r.ok || count < 1 || count > 16 || priv > 2) return;
            if (from->joinAttempt && from->id == 0) from->id = lead;   // sto entrando: chi mi risponde con l'elenco e' il capo
            if (from->id != lead) return;
            std::vector<Member> list; std::unordered_map<PeerId, Addr> la;
            for (int i = 0; i < count; ++i) {
                Member mb; mb.id = r.U64(); mb.name = r.Str(); mb.unitId = r.Str(); mb.ready = r.U8() != 0; mb.joinOrder = static_cast<int>(r.U32());
                Addr a; a.ip = r.U32(); a.port = r.U16(); a.lip = r.U32(); a.lport = r.U16();
                if (!r.ok || mb.id == 0) return;
                list.push_back(mb); la[mb.id] = a;
            }
            const bool joining = state == PartyState::Joining && from->joinAttempt;
            if (!joining && (state != PartyState::InParty || ver <= version)) return;
            bool hasMe = false; for (auto& mb : list) if (mb.id == selfId) hasMe = true;
            if (!hasMe) { if (state == PartyState::InParty) ResetParty("Sei stato rimosso dalla squadra"); return; }
            if (joining) { from->joinAttempt = false; from->established = true; state = PartyState::InParty; Event("Sei entrato nella squadra di " + list.front().name); }
            // chi e' uscito (per l'avviso) e l'host, prima di sostituire l'elenco
            for (auto& old : members) { bool still = false; for (auto& mb : list) if (mb.id == old.id) still = true; if (!still && old.id != selfId) { if (Link* l = LinkOf(old.id)) Drop(l, false); } }
            members = list; addrs = la; leaderId = lead; privacy = static_cast<Privacy>(priv); maxMembers = mx; key = k; code6 = c6; version = ver;
            for (auto& mb : members) if (mb.id == selfId) { mb.name = name; }
            // collegamenti arrivati prima dell'elenco: ora sono compagni
            for (auto* l : links) if (!l->established && !l->outgoing && l->id && Find(l->id)) l->established = true;
            if (sess && (!session || ep > epoch)) ApplyHost(hst, ep, succ, session);
            else if (!sess && session) { session = false; hostId = 0; succession.clear(); Event("Partita terminata"); if (owner->onHostChanged) owner->onHostChanged(0, false, false); }
            RefreshFlags();
            EnsureMesh();
        }

        void OnMessage(Link* l, const std::uint8_t* data, size_t size, int channel) {
            if (size < 1) return;
            Reader r(data + 1, size - 1);
            const std::uint8_t type = data[0];
            if (type == MsgHello) { OnHello(l, r); return; }
            if (l->joinAttempt) {
                if (type == MsgDenied) { const std::string why = r.Str(); Drop(l, false); state = PartyState::Idle; Event(why.empty() ? "Ingresso rifiutato" : why); }
                else if (type == MsgPending) { status = "In attesa che il capo ti faccia entrare..."; joinStarted = now + 60.0; }
                else if (type == MsgRedirect) {
                    std::uint32_t ip = r.U32(); const std::uint16_t p = r.U16();
                    if (!r.ok) return;
                    if (IsLoopback(ip)) ip = FromEnet(l->peer->address.host);
                    Drop(l, false);
                    if (!Connect(ip, p, true)) { state = PartyState::Idle; Event("Impossibile raggiungere il capo della squadra"); }
                }
                else if (type == MsgState) ApplyState(l, r);
                return;
            }
            if (!l->established || state != PartyState::InParty) return;
            const PeerId from = l->id;
            switch (type) {
            case MsgState: ApplyState(l, r); break;
            case MsgMemberUpdate: {
                if (!IsLeader()) break;
                const std::string nm = r.Str(), un = r.Str(); const bool rd = r.U8() != 0;
                if (!r.ok) break;
                if (Member* mb = Find(from)) { if (!nm.empty()) mb->name = nm; mb->unitId = un; mb->ready = rd; BroadcastState(); }
                break;
            }
            case MsgLeave: {
                const PeerId who = r.U64(); const bool kicked = r.U8() != 0;
                if (!r.ok) break;
                if (kicked && who == selfId && from == leaderId) { ResetParty("Sei stato rimosso dalla squadra"); return; }
                if (who == from) MemberGone(who, true);
                break;
            }
            case MsgPing: {
                const int n = r.U8(); if (n > 16) break;
                auto& row = matrix[from];
                for (int i = 0; i < n; ++i) {
                    const PeerId to = r.U64(); Report rep; rep.rtt = static_cast<float>(r.U16()); rep.loss = static_cast<float>(r.U16()) / 100.0f; rep.fresh = r.U8() != 0; rep.time = now;
                    if (!r.ok) break;
                    row[to] = rep;
                }
                break;
            }
            case MsgSessionStart:
            case MsgHostAnnounce: {
                const std::uint32_t ep = r.U32(); const PeerId h = r.U64(); const int n = r.U8(); if (n > 16) break;
                std::vector<PeerId> list; for (int i = 0; i < n; ++i) list.push_back(r.U64());
                if (!r.ok || !Find(h)) break;
                if (type == MsgSessionStart && from != leaderId) break;
                if (type == MsgHostAnnounce && from != h) break;
                // vale l'epoca piu' alta; a pari epoca resta l'host che conosco (o, se sono due, quello con l'id piu' basso)
                if (!session || ep > epoch || (ep == epoch && (h == hostId || h < hostId))) ApplyHost(h, ep, list, session && ep > epoch);
                break;
            }
            case MsgSessionEnd:
                if (from == leaderId && session) { session = false; hostId = 0; succession.clear(); Event("Partita terminata"); if (owner->onHostChanged) owner->onHostChanged(0, false, false); }
                break;
            case MsgSnapshot: {
                const std::uint32_t ep = r.U32(); std::vector<std::uint8_t> bytes;
                if (!r.Bytes(bytes, kMaxSnapshot) || !session || from != hostId || ep != epoch) break;
                snapshot.swap(bytes); snapshotTime = now;
                break;
            }
            case MsgGameToHost:
                if (session && hostId == selfId && owner->onGameData) owner->onGameData(from, data + 1, size - 1);
                break;
            case MsgGameToAll:
                if (session && owner->onGameData) owner->onGameData(from, data + 1, size - 1);
                break;
            default: break;
            }
            (void)channel;
        }

        void SendSelfUpdate() {
            if (state != PartyState::InParty) return;
            if (Member* me = Find(selfId)) { me->name = name; me->unitId = unit; me->ready = ready; }
            if (IsLeader()) { BroadcastState(); return; }
            Writer w; w.U8(MsgMemberUpdate); w.Str(name); w.Str(unit); w.U8(ready ? 1 : 0);
            Send(LinkOf(leaderId), w);
        }

        // ---------- lobby in rete locale ----------
        void Discovery() {
            if (disc == ENET_SOCKET_NULL) return;
            if (IsLeader() && privacy != Privacy::Private && now - lastBroadcast > 1.0) {
                lastBroadcast = now;
                Writer w; w.U32(kDiscoveryMagic); w.U64(selfId); w.Str(name); w.U8(static_cast<std::uint8_t>(members.size())); w.U8(static_cast<std::uint8_t>(maxMembers));
                w.U8(static_cast<std::uint8_t>(privacy)); w.U16(port);
                ENetAddress to; to.host = ENET_HOST_BROADCAST; to.port = kDiscoveryPort;
                ENetBuffer buf; buf.data = w.b.data(); buf.dataLength = w.b.size();
                enet_socket_send(disc, &to, &buf, 1);
            }
            auto broadcast = [&](const Writer& w) {
                ENetAddress to; to.host = ENET_HOST_BROADCAST; to.port = kDiscoveryPort;
                ENetBuffer buf; buf.data = const_cast<std::uint8_t*>(w.b.data()); buf.dataLength = w.b.size();
                enet_socket_send(disc, &to, &buf, 1);
            };
            // Ingresso col codice a 6 cifre: chiedo in rete chi ha quella squadra, finche' qualcuno risponde o scade il tempo
            if (lookupCode && state == PartyState::Joining) {
                if (now > lookupUntil) {
                    lookupCode = 0; state = PartyState::Idle;
                    Event("Nessuna squadra con questo codice in rete. Via internet serve l'indirizzo o il codice esteso.");
                }
                else if (now - lastLookup > 0.4) {
                    lastLookup = now;
                    Writer w; w.U32(kQueryMagic); w.U32(lookupCode); w.U64(selfId);
                    broadcast(w);
                }
            }
            for (int guard = 0; guard < 32; ++guard) {
                std::uint8_t tmp[256]; ENetBuffer buf; buf.data = tmp; buf.dataLength = sizeof(tmp);
                ENetAddress from;
                const int got = enet_socket_receive(disc, &from, &buf, 1);
                if (got <= 0) break;
                Reader r(tmp, static_cast<size_t>(got));
                const std::uint32_t magic = r.U32();
                if (magic == kQueryMagic) {
                    // qualcuno cerca una squadra per codice: risponde il capo (anche se la squadra e' privata: il codice e' l'invito)
                    const std::uint32_t code = r.U32(); const PeerId who = r.U64();
                    if (!r.ok || !IsLeader() || code == 0 || code != code6 || who == selfId) continue;
                    Writer w; w.U32(kReplyMagic); w.U32(code6); w.U64(who); w.U64(selfId); w.U16(port); w.U16(key);
                    broadcast(w);   // in broadcast: cosi' arriva anche a piu' giochi aperti sullo stesso computer
                    continue;
                }
                if (magic == kReplyMagic) {
                    const std::uint32_t code = r.U32(); const PeerId who = r.U64(); const PeerId lead = r.U64(); const std::uint16_t p = r.U16(), k = r.U16();
                    if (!r.ok || who != selfId || !lookupCode || code != lookupCode || state != PartyState::Joining || lead == selfId) continue;
                    std::uint32_t ip = FromEnet(from.host);
                    lookupCode = 0;
                    joinKey = k; joinHasKey = true;
                    if (!Connect(ip, p, true)) { state = PartyState::Idle; Event("Impossibile aprire il collegamento"); continue; }
                    joinStarted = now;
                    status = "Squadra trovata, collegamento a " + IpToString(ip) + ":" + std::to_string(p) + "...";
                    continue;
                }
                if (magic != kDiscoveryMagic) continue;
                LobbyInfo info; info.leaderId = r.U64(); info.leaderName = r.Str(); info.members = r.U8(); info.maxMembers = r.U8();
                const std::uint8_t priv = r.U8(); const std::uint16_t p = r.U16();
                if (!r.ok || priv > 1 || info.leaderId == selfId || info.leaderId == 0) continue;
                info.privacy = static_cast<Privacy>(priv);
                const std::uint32_t ip = FromEnet(from.host);
                info.address = IpToString(ip) + ":" + std::to_string(p);
                lobbyAddrs[info.leaderId] = { ip, p };
                bool found = false;
                for (auto& lb : lobbies) if (lb.leaderId == info.leaderId) { lb = info; found = true; }
                if (!found) lobbies.push_back(info);
            }
        }
    };

    Party::Party() : m(std::make_unique<Impl>()) { m->owner = this; }
    Party::~Party() { Shutdown(); }

    bool Party::Init(PeerId selfId, const std::string& name, std::uint16_t basePort) {
        if (m->host) return true;
        if (g_EnetUsers == 0 && enet_initialize() != 0) { m->status = "Rete non disponibile"; return false; }
        ++g_EnetUsers;
        m->selfId = selfId ? selfId : RandomPeerId();
        m->name = name.empty() ? "Giocatore" : name;
        for (int i = 0; i < 16 && !m->host; ++i) {
            ENetAddress a; a.host = ENET_HOST_ANY; a.port = static_cast<enet_uint16>(basePort + i);
            m->host = enet_host_create(&a, 32, 2, 0, 0);
            if (m->host) m->port = a.port;
        }
        if (!m->host) { if (--g_EnetUsers == 0) enet_deinitialize(); m->status = "Nessuna porta UDP libera per il gioco in rete"; return false; }
        // socket per trovare le lobby in rete locale (se non si apre, resta tutto il resto)
        m->disc = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if (m->disc != ENET_SOCKET_NULL) {
            enet_socket_set_option(m->disc, ENET_SOCKOPT_REUSEADDR, 1);
            enet_socket_set_option(m->disc, ENET_SOCKOPT_NONBLOCK, 1);
            enet_socket_set_option(m->disc, ENET_SOCKOPT_BROADCAST, 1);
            ENetAddress a; a.host = ENET_HOST_ANY; a.port = kDiscoveryPort;
            if (enet_socket_bind(m->disc, &a) < 0) { enet_socket_destroy(m->disc); m->disc = ENET_SOCKET_NULL; }
        }
        { std::uint32_t ip = 0; if (ParseIp(LocalIp(), ip) && !IsLoopback(ip)) m->localIp = ip; }
        m->state = PartyState::Idle;
        m->now = NowSeconds();
        return true;
    }

    void Party::Shutdown() {
        onHostChanged = nullptr; onGameData = nullptr;   // in chiusura niente avvisi: chi li riceve puo' non esistere piu'
        if (!m->host) return;
        if (m->state == PartyState::InParty) Leave();
        for (auto* l : std::vector<Impl::Link*>(m->links)) m->Drop(l, true);
        if (m->disc != ENET_SOCKET_NULL) { enet_socket_destroy(m->disc); m->disc = ENET_SOCKET_NULL; }
        enet_host_destroy(m->host); m->host = nullptr;
        if (--g_EnetUsers == 0) enet_deinitialize();
        m->state = PartyState::Offline;
    }

    void Party::Update(float dt) {
        if (!m->host) return;
        m->now = NowSeconds();
        const double now = m->now;

        ENetEvent ev;
        while (enet_host_service(m->host, &ev, 0) > 0) {
            auto* l = ev.peer ? static_cast<Impl::Link*>(ev.peer->data) : nullptr;
            switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                m->Tune(ev.peer);
                if (!l) {   // qualcuno mi chiama: aspetto il suo Hello
                    l = new Impl::Link(); l->peer = ev.peer; l->created = now; ev.peer->data = l; m->links.push_back(l);
                }
                else m->SendHello(l, !l->joinAttempt);
                if (l->outgoing && !l->joinAttempt) { l->established = true; m->suspectSince.erase(l->id); m->attempts.erase(l->id); m->RefreshFlags(); }
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                if (l) m->OnMessage(l, ev.packet->data, ev.packet->dataLength, ev.channelID);
                enet_packet_destroy(ev.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                if (l) {
                    const PeerId id = l->id; const bool wasMember = l->established, wasJoin = l->joinAttempt;
                    ev.peer->data = nullptr; l->peer = nullptr;
                    m->links.erase(std::remove(m->links.begin(), m->links.end(), l), m->links.end());
                    delete l;
                    if (wasJoin && m->state == PartyState::Joining) { m->state = PartyState::Idle; m->Event("Nessuna risposta dalla squadra"); }
                    else if (wasMember && m->state == PartyState::InParty && m->Find(id) && !m->suspectSince.count(id)) m->suspectSince[id] = now - kSilentSuspect;
                }
                break;
            default: break;
            }
        }

        if (m->state == PartyState::Joining && now - m->joinStarted > 8.0) {
            for (auto* l : std::vector<Impl::Link*>(m->links)) if (l->joinAttempt) m->Drop(l, true);
            m->state = PartyState::Idle; m->Event("Nessuna risposta: controlla codice, indirizzo e porta");
        }
        // collegamenti in arrivo mai diventati compagni
        for (auto* l : std::vector<Impl::Link*>(m->links))
            if (!l->established && !l->joinAttempt && now - l->created > 8.0) {
                bool pendingReq = false; for (auto& q : m->requests) if (q.id == l->id) pendingReq = true;
                if (!pendingReq) m->Drop(l, true);
            }

        if (m->state == PartyState::InParty) {
            // --- chi tace da troppo e' sospetto; poi si decide se e' caduto lui o sono isolato io ---
            for (auto* l : m->links) if (l->established && m->Find(l->id) && !m->suspectSince.count(l->id) && m->SilentFor(l) > kSilentSuspect) m->suspectSince[l->id] = now;
            std::vector<PeerId> gone; PeerId isolatedFrom = 0;
            for (auto it = m->suspectSince.begin(); it != m->suspectSince.end();) {
                const PeerId id = it->first;
                Impl::Link* l = m->LinkOf(id);
                if (l && m->SilentFor(l) < kFresh) { it = m->suspectSince.erase(it); continue; }   // si e' rifatto vivo
                if (now - it->second < kSuspectResolve) { ++it; continue; }
                // qualcun altro lo sente ancora?
                bool heard = false;
                for (auto& row : m->matrix) {
                    if (row.first == id || !m->Find(row.first)) continue;
                    auto jt = row.second.find(id);
                    if (jt != row.second.end() && jt->second.fresh && now - jt->second.time < 1.2) heard = true;
                }
                if (heard && !m->IsLeader()) { if (id == m->leaderId) isolatedFrom = id; ++it; continue; }   // il problema e' il mio collegamento
                gone.push_back(id);
                it = m->suspectSince.erase(it);
            }
            if (isolatedFrom) { m->ResetParty("Collegamento con la squadra perso"); return; }
            for (PeerId id : gone) m->MemberGone(id, false);

            // --- rapporto delle latenze a tutti (serve a scegliere l'host) ---
            if (now - m->lastReport > 0.5) {
                m->lastReport = now;
                Writer w; w.U8(MsgPing);
                std::vector<Impl::Link*> est; for (auto* l : m->links) if (l->established) est.push_back(l);
                w.U8(static_cast<std::uint8_t>((std::min)(est.size(), static_cast<size_t>(16))));
                for (size_t i = 0; i < est.size() && i < 16; ++i) {
                    w.U64(est[i]->id); w.U16(static_cast<std::uint16_t>((std::min)(est[i]->peer->roundTripTime, 65000u)));
                    w.U16(static_cast<std::uint16_t>(10000.0f * static_cast<float>(est[i]->peer->packetLoss) / static_cast<float>(ENET_PEER_PACKET_LOSS_SCALE)));
                    w.U8(m->SilentFor(est[i]) < kFresh ? 1 : 0);
                }
                m->SendMembers(w, false, 1);
                m->RefreshFlags();
            }
            if (now - m->lastMesh > 1.0) { m->lastMesh = now; m->EnsureMesh(); }
            // --- l'host conferma se stesso e aggiorna la lista di successione ---
            if (m->session && m->hostId == m->selfId && now - m->lastAnnounce > 2.0) m->Announce();
            // host provvisorio che non si e' mai annunciato e non e' piu' in squadra
            if (m->session && !m->Find(m->hostId)) m->Migrate();
        }

        m->Discovery();
        for (auto& lb : m->lobbies) lb.age += dt;
        m->lobbies.erase(std::remove_if(m->lobbies.begin(), m->lobbies.end(), [](const LobbyInfo& lb) { return lb.age > 3.5f; }), m->lobbies.end());
        enet_host_flush(m->host);
    }

    PartyState Party::GetState() const { return m->state; }
    const std::string& Party::StatusText() const { return m->status; }
    std::vector<std::string> Party::PopEvents() { std::vector<std::string> e; e.swap(m->events); return e; }
    std::uint16_t Party::Port() const { return m->port; }
    PeerId Party::SelfId() const { return m->selfId; }

    void Party::SetName(const std::string& name) { if (name == m->name || name.empty()) return; m->name = name; m->SendSelfUpdate(); }
    void Party::SetUnit(const std::string& unitId) { if (unitId == m->unit) return; m->unit = unitId; m->SendSelfUpdate(); }
    void Party::SetReady(bool ready) { if (ready == m->ready) return; m->ready = ready; m->SendSelfUpdate(); }

    bool Party::Create(Privacy privacy, int maxMembers) {
        if (!m->host) return false;
        if (m->state != PartyState::Idle) Leave();
        std::random_device rd;
        m->key = static_cast<std::uint16_t>(rd() & 0xFFFFu); if (m->key == 0) m->key = 0x51A7;
        m->code6 = 100000u + static_cast<std::uint32_t>((static_cast<std::uint64_t>(rd()) * 900000ull) >> 32);   // 100000..999999
        m->privacy = privacy; m->maxMembers = std::clamp(maxMembers, 2, 16);
        m->leaderId = m->selfId; m->version = 1; m->nextOrder = 0; m->ready = false;
        Member me; me.id = m->selfId; me.name = m->name; me.unitId = m->unit; me.joinOrder = m->nextOrder++;
        m->members = { me };
        m->addrs[m->selfId] = { 0, m->port, m->localIp, m->port };
        m->state = PartyState::InParty;
        m->RefreshFlags();
        m->Event("Squadra creata");
        return true;
    }

    bool Party::Join(const std::string& text) {
        if (!m->host) return false;
        std::uint32_t ip = 0; std::uint16_t port = 27650, key = 0; bool hasKey = false;
        std::string t = text;
        t.erase(std::remove_if(t.begin(), t.end(), [](unsigned char c) { return std::isspace(c) != 0; }), t.end());
        // Codice a 6 cifre: la squadra si cerca in rete (locale o virtuale), poi ci si collega da soli
        if (t.size() == 6 && std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            if (m->disc == ENET_SOCKET_NULL) { m->Event("Ricerca in rete non disponibile su questo computer"); return false; }
            if (m->state != PartyState::Idle) Leave();
            m->now = NowSeconds();
            m->lookupCode = static_cast<std::uint32_t>(std::atoi(t.c_str()));
            m->lookupUntil = m->now + 4.0; m->lastLookup = 0.0; m->ready = false;
            m->state = PartyState::Joining; m->joinStarted = m->now + 4.0;
            m->status = "Cerco la squadra " + t + "...";
            return true;
        }
        if (DecodeCode(t, ip, port, key)) hasKey = true;
        else {
            const size_t colon = t.find(':');
            const std::string hostPart = colon == std::string::npos ? t : t.substr(0, colon);
            if (colon != std::string::npos) { const int p = std::atoi(t.c_str() + colon + 1); if (p <= 0 || p > 65535) { m->Event("Porta non valida"); return false; } port = static_cast<std::uint16_t>(p); }
            if (!ParseIp(hostPart, ip)) { m->Event("Codice d'invito o indirizzo non valido"); return false; }
        }
        if (m->state != PartyState::Idle) Leave();
        m->joinKey = key; m->joinHasKey = hasKey; m->ready = false;
        if (!m->Connect(ip, port, true)) { m->Event("Impossibile aprire il collegamento"); return false; }
        m->state = PartyState::Joining; m->joinStarted = m->now = NowSeconds();
        m->status = "Collegamento a " + IpToString(ip) + ":" + std::to_string(port) + "...";
        return true;
    }

    bool Party::Join(const LobbyInfo& lobby) {
        auto it = m->lobbyAddrs.find(lobby.leaderId);
        if (it == m->lobbyAddrs.end()) return Join(lobby.address);
        if (!m->host) return false;
        if (m->state != PartyState::Idle) Leave();
        m->joinKey = 0; m->joinHasKey = false; m->ready = false;
        if (!m->Connect(it->second.ip, it->second.port, true)) { m->Event("Impossibile aprire il collegamento"); return false; }
        m->state = PartyState::Joining; m->joinStarted = m->now = NowSeconds();
        m->status = "Collegamento alla squadra di " + lobby.leaderName + "...";
        return true;
    }

    void Party::Leave() {
        if (!m->host) return;
        if (m->state == PartyState::InParty) {
            Writer w; w.U8(MsgLeave); w.U64(m->selfId); w.U8(0);
            m->SendMembers(w);
            enet_host_flush(m->host);
        }
        m->ResetParty(m->state == PartyState::InParty ? "Hai lasciato la squadra" : "");
        m->state = PartyState::Idle;
    }

    const std::vector<Member>& Party::Members() const { return m->members; }
    bool Party::IsLeader() const { return m->IsLeader(); }
    PeerId Party::LeaderId() const { return m->leaderId; }
    Privacy Party::GetPrivacy() const { return m->privacy; }
    int Party::MaxMembers() const { return m->maxMembers; }

    std::string Party::LocalIp() const {
        // indirizzo della scheda che esce verso internet: si "collega" una socket UDP (non parte nessun pacchetto) e si legge da dove
        std::string out = "127.0.0.1";
        ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if (s == ENET_SOCKET_NULL) return out;
        ENetAddress to; to.host = ToEnet(0x08080808u); to.port = 53;
        ENetAddress me;
        if (enet_socket_connect(s, &to) == 0 && enet_socket_get_address(s, &me) == 0 && me.host != 0) out = IpToString(FromEnet(me.host));
        enet_socket_destroy(s);
        return out;
    }
    void Party::SetAdvertisedIp(const std::string& ip) { m->advertisedIp = ip; }

    std::string Party::Code6() const {
        if (m->state != PartyState::InParty || m->code6 == 0) return {};
        return std::to_string(m->code6);
    }

    std::string Party::InviteCode() const {
        if (m->state != PartyState::InParty) return {};
        std::uint32_t ip = 0; std::uint16_t port = m->port;
        if (m->IsLeader()) { if (!ParseIp(m->advertisedIp, ip)) ParseIp(LocalIp(), ip); }
        else if (Impl::Link* l = m->LinkOf(m->leaderId)) { ip = FromEnet(l->peer->address.host); port = l->peer->address.port; }
        else return {};
        return EncodeCode(ip, port, m->key);
    }

    void Party::SetPrivacy(Privacy privacy) { if (!m->IsLeader() || privacy == m->privacy) return; m->privacy = privacy; m->BroadcastState(); }

    void Party::Kick(PeerId id) {
        if (!m->IsLeader() || id == m->selfId || !m->Find(id)) return;
        Writer w; w.U8(MsgLeave); w.U64(id); w.U8(1);
        m->Send(m->LinkOf(id), w);
        const std::string who = m->NameOf(id);
        if (Impl::Link* l = m->LinkOf(id)) m->Drop(l, false);
        const bool wasHost = m->session && id == m->hostId;
        m->members.erase(std::remove_if(m->members.begin(), m->members.end(), [&](const Member& x) { return x.id == id; }), m->members.end());
        m->addrs.erase(id); m->matrix.erase(id); m->suspectSince.erase(id);
        m->Event(who + " e' stato rimosso");
        if (wasHost) m->Migrate();
        m->RefreshFlags();
        m->BroadcastState();
    }

    const std::vector<JoinRequest>& Party::Requests() const { return m->requests; }

    void Party::AnswerRequest(PeerId id, bool accept) {
        if (!m->IsLeader()) return;
        std::string nm;
        for (auto& q : m->requests) if (q.id == id) nm = q.name;
        m->requests.erase(std::remove_if(m->requests.begin(), m->requests.end(), [&](const JoinRequest& q) { return q.id == id; }), m->requests.end());
        Impl::Link* link = nullptr;
        for (auto* l : m->links) if (l->id == id && !l->established) link = l;
        if (!link) return;
        if (accept && static_cast<int>(m->members.size()) < m->maxMembers) m->Admit(link, id, nm, m->pendingUnit[id], m->addrs[id].lport, m->addrs[id].lip);
        else { Writer w; w.U8(MsgDenied); w.Str(accept ? "Squadra al completo" : "Il capo non ti ha fatto entrare"); m->Send(link, w); m->Drop(link, false); }
        m->pendingUnit.erase(id);
    }

    const std::vector<LobbyInfo>& Party::Lobbies() const { return m->lobbies; }

    bool Party::StartSession() {
        if (!m->IsLeader()) return false;
        m->RefreshFlags();
        const auto order = m->ComputeSuccession(0);   // il migliore davanti
        if (order.empty()) return false;
        const std::uint32_t e = m->epoch + 1;
        m->snapshot.clear(); m->snapshotTime = -1.0;
        m->ApplyHost(order.front(), e, order, false);
        Writer w; w.U8(MsgSessionStart); w.U32(e); w.U64(order.front());
        w.U8(static_cast<std::uint8_t>((std::min)(order.size(), static_cast<size_t>(16))));
        for (size_t i = 0; i < order.size() && i < 16; ++i) w.U64(order[i]);
        m->SendMembers(w);
        m->BroadcastState();
        return true;
    }

    void Party::EndSession() {
        if (!m->IsLeader() || !m->session) return;
        m->session = false; m->hostId = 0; m->succession.clear();
        Writer w; w.U8(MsgSessionEnd);
        m->SendMembers(w);
        m->BroadcastState();
        m->Event("Partita terminata");
        if (onHostChanged) onHostChanged(0, false, false);
    }

    bool Party::InSession() const { return m->session; }
    PeerId Party::HostId() const { return m->session ? m->hostId : 0; }
    bool Party::IsHost() const { return m->session && m->hostId == m->selfId; }
    std::uint32_t Party::Epoch() const { return m->epoch; }
    const std::vector<PeerId>& Party::Succession() const { return m->succession; }

    void Party::PublishSnapshot(const std::vector<std::uint8_t>& bytes) {
        if (!IsHost() || bytes.size() > kMaxSnapshot) return;
        m->snapshot = bytes; m->snapshotTime = m->now;
        Writer w; w.U8(MsgSnapshot); w.U32(m->epoch); w.Bytes(bytes.data(), bytes.size());
        m->SendMembers(w);
    }
    const std::vector<std::uint8_t>& Party::Snapshot() const { return m->snapshot; }
    float Party::SnapshotAge() const { return m->snapshotTime < 0.0 ? -1.0f : static_cast<float>(m->now - m->snapshotTime); }

    void Party::SendToHost(const std::uint8_t* data, std::size_t size, bool reliable) {
        if (!m->session || size > kMaxSnapshot) return;
        if (m->hostId == m->selfId) { if (onGameData) onGameData(m->selfId, data, size); return; }
        Writer w; w.U8(MsgGameToHost); w.b.insert(w.b.end(), data, data + size);
        m->Send(m->LinkOf(m->hostId), w, reliable, 1);
    }
    void Party::SendToAll(const std::uint8_t* data, std::size_t size, bool reliable) {
        if (!m->session || size > kMaxSnapshot) return;
        Writer w; w.U8(MsgGameToAll); w.b.insert(w.b.end(), data, data + size);
        m->SendMembers(w, reliable, 1);
    }
}
