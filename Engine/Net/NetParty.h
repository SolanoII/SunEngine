#pragma once
// Squadra (party), lobby e collegamento peer to peer.
//
// Non c'e' un server: i giocatori si collegano fra loro direttamente (UDP affidabile, libreria ENet).
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
// Limite senza server: via internet chi ospita il primo contatto deve essere raggiungibile (porta UDP aperta sul
// router, oppure una rete virtuale tipo Tailscale/ZeroTier/Radmin). In rete locale funziona sempre. Il server
// (amici, stato online, attraversamento automatico dei router) si appoggera' a questa stessa classe.
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
        bool linked = false;       // collegamento diretto attivo con me
        int joinOrder = 0;
        float rttMs = 0.0f;        // latenza fra me e lui
        float lossPct = 0.0f;      // pacchetti persi fra me e lui (%)
        float score = 0.0f;        // qualita' della sua connessione verso tutti (piu' basso = meglio)
    };

    struct LobbyInfo {
        PeerId leaderId = 0;
        std::string leaderName;
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

        // --- lobby visibili in rete locale ---
        const std::vector<LobbyInfo>& Lobbies() const;

        // --- sessione di gioco ---
        bool StartSession();                                  // solo il capo: sceglie l'host migliore e avvia
        void EndSession();                                    // solo il capo
        bool InSession() const;
        PeerId HostId() const;
        bool IsHost() const;
        std::uint32_t Epoch() const;                          // cresce a ogni cambio di host
        const std::vector<PeerId>& Succession() const;        // chi subentra, in ordine
        // L'host pubblica lo stato della partita (ogni 1-2 s): chi subentra riparte da qui
        void PublishSnapshot(const std::vector<std::uint8_t>& bytes);
        const std::vector<std::uint8_t>& Snapshot() const;
        float SnapshotAge() const;                            // secondi dall'ultima istantanea ricevuta
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
