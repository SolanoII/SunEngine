#pragma once
// Squadra (party), lobby e collegamento peer to peer.
//
// La partita e' peer to peer: l'host e' sempre uno dei giocatori (UDP affidabile, libreria ENet). Per trovarsi via
// internet senza aprire porte c'e' il server d'incontro (Tools/TrazynServer, vedi ServerProtocol.h): elenco delle
// lobby, codici a 6 cifre, apertura dei router e, se proprio non si aprono, ponte. Senza server funziona la rete locale.
//   - SQUADRA: chi la crea ne e' il capo; gli altri entrano col codice a 6 cifre (o quello esteso), dall'elenco delle lobby in rete
//     locale, o su approvazione del capo. Tutti sono collegati con tutti (maglia), non solo col capo: cosi' ognuno
//     misura la latenza verso gli altri e, se il capo o l'host cadono, la squadra resta in piedi.
//   - LOBBY: aperta (visibile, entra chiunque), chiusa (visibile, serve l'ok del capo o il codice), privata
//     (invisibile, solo col codice).
//   - SESSIONE DI GIOCO: all'avvio fa da host chi ha la connessione migliore verso tutti gli altri (latenza media e
//     pacchetti persi). L'host manda ogni tanto un'istantanea dello stato della partita che tutti conservano; se
//     l'host esce o cade, subentra il successivo della lista (sempre il migliore rimasto) e riparte dall'ultima
//     istantanea. La lista di successione e' la stessa per tutti, quindi non serve votare.
//
// Senza server d'incontro, via internet chi ospita il primo contatto deve essere raggiungibile (porta UDP aperta, o una
// rete virtuale tipo Tailscale/ZeroTier/Radmin); in rete locale funziona sempre.
//   - HOST DURANTE LA PARTITA: se nel frattempo entra (o migliora) qualcuno con una connessione nettamente migliore
//     per tutti, l'host gli passa il ruolo da solo, senza interrompere.
//
// Questo file non espone ENet ne' le socket: si puo' includere ovunque.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace SunEngine::Net {

    using PeerId = std::uint64_t;

    enum class Privacy : std::uint8_t { Open = 0, Closed = 1, Private = 2 };
    enum class PartyState { Offline, Idle, Joining, InParty };

    struct Member {
        PeerId id = 0;
        std::string name;
        std::string unitId;        // unita' scelta nel garage (per vederlo accanto)
        bool ready = false;
        bool leader = false;
        bool self = false;
        bool linked = false;       // collegamento attivo con me
        bool relayed = false;      // ...ma passa dal ponte del server d'incontro (router che non si aprono)
        int joinOrder = 0;
        float rttMs = 0.0f;        // latenza fra me e lui
        float lossPct = 0.0f;      // pacchetti persi fra me e lui (%)
        float score = 0.0f;        // qualita' della sua connessione verso tutti (piu' basso = meglio)
    };

    struct LobbyInfo {
        PeerId leaderId = 0;
        std::string leaderName;
        std::string name;          // nome della lobby
        bool inSession = false;    // partita gia' iniziata (si puo' entrare lo stesso)
        bool online = false;       // dal server d'incontro (via internet); false = rete locale
        int pingMs = -1;           // latenza del capo verso il server d'incontro (indicativa)
        std::string address;       // "ip:porta"
        int members = 0, maxMembers = 0;
        Privacy privacy = Privacy::Open;
        float age = 0.0f;          // secondi dall'ultimo annuncio
    };

    struct JoinRequest { PeerId id = 0; std::string name; };

    class Party {
    public:
        Party();
        ~Party();
        Party(const Party&) = delete;
        Party& operator=(const Party&) = delete;

        // Apre la rete (porta UDP basePort, o le successive se occupata). Senza rete ritorna false e tutto il resto non fa nulla.
        bool Init(PeerId selfId, const std::string& name, std::uint16_t basePort = 27650);
        void Shutdown();
        void Update(float dt);     // ogni fotogramma

        PartyState GetState() const;
        const std::string& StatusText() const;      // ultima cosa successa / errore, per l'interfaccia
        std::vector<std::string> PopEvents();       // avvisi ("X e' entrato", "nuovo host: Y")
        std::uint16_t Port() const;
        PeerId SelfId() const;

        // --- io ---
        void SetName(const std::string& name);
        void SetUnit(const std::string& unitId);
        void SetReady(bool ready);

        // --- squadra ---
        bool Create(Privacy privacy = Privacy::Private, int maxMembers = 5);
        bool Join(const std::string& codeOrAddress);          // codice a 6 cifre, codice esteso, oppure "ip" / "ip:porta"
        bool Join(const LobbyInfo& lobby);                    // dall'elenco delle lobby
        void Leave();
        const std::vector<Member>& Members() const;
        bool IsLeader() const;
        PeerId LeaderId() const;
        Privacy GetPrivacy() const;
        int MaxMembers() const;
        std::string Code6() const;                            // codice a 6 cifre: chi lo scrive entra (rete locale o virtuale)
        std::string InviteCode() const;                       // codice esteso con dentro indirizzo e porta (per internet); vuoto se non si e' in squadra
        void SetAdvertisedIp(const std::string& ip);          // IP da mettere nel codice (es. quello pubblico); vuoto = quello locale
        std::string LocalIp() const;
        // solo il capo
        void SetPrivacy(Privacy privacy);
        void Kick(PeerId id);
        const std::vector<JoinRequest>& Requests() const;     // lobby chiusa: chi chiede di entrare
        void AnswerRequest(PeerId id, bool accept);

        // --- lobby visibili: rete locale + server d'incontro ---
        const std::vector<LobbyInfo>& Lobbies() const;
        void SetLobbyName(const std::string& name);            // solo il capo
        const std::string& LobbyName() const;

        // --- server d'incontro ---
        void SetServer(const std::string& hostPort);           // "indirizzo[:porta]"; vuoto = solo rete locale
        const std::string& ServerAddress() const;
        bool ServerOnline() const;
        float ServerRttMs() const;
        void RefreshServerList();                              // chiede l'elenco; arriva in Lobbies()

        // --- sessione di gioco ---
        // L'host c'e' sempre, dalla creazione della lobby (e' chi l'ha aperta, poi chi ha la connessione migliore).
        bool StartSession();                                  // solo il capo: avvia la partita (l'host diventa subito il migliore)
        void EndSession();                                    // solo il capo: fine della partita, si torna in lobby
        bool Playing() const;                                 // partita avviata
        bool InSession() const;                               // c'e' un host
        PeerId HostId() const;
        bool IsHost() const;
        std::uint32_t Epoch() const;                          // cresce a ogni cambio di host
        const std::vector<PeerId>& Succession() const;        // chi subentra, in ordine
        // L'host pubblica lo stato della partita (ogni 1-2 s): chi subentra riparte da qui
        void PublishSnapshot(const std::vector<std::uint8_t>& bytes);
        const std::vector<std::uint8_t>& Snapshot() const;
        float SnapshotAge() const;                            // secondi dall'ultima istantanea ricevuta
        // Chi e' davvero dentro la partita (lo sa il gioco: chi manda il suo stato). Se l'elenco non e' vuoto l'host
        // deve essere uno di loro: fuori partita passa subito il ruolo al migliore di loro, e la successione li mette
        // davanti a chi e' rimasto nel garage. Vuoto = come prima (contano solo le connessioni).
        void SetHostCandidates(const std::vector<PeerId>& inMatch);
        // Dati di gioco: verso l'host o verso tutti. reliable = arriva di sicuro e in ordine (piu' lento)
        void SendToHost(const std::uint8_t* data, std::size_t size, bool reliable);
        void SendToAll(const std::uint8_t* data, std::size_t size, bool reliable);
        std::function<void(PeerId from, const std::uint8_t* data, std::size_t size)> onGameData;
        std::function<void(PeerId newHost, bool iAmHost, bool migrated)> onHostChanged;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };

    // Identificativo casuale per un profilo nuovo
    PeerId RandomPeerId();
}
