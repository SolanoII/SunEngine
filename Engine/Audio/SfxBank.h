#pragma once
// Banca di effetti sonori: ogni "suono" ha una o piu' varianti (file diversi, scelti a caso per non ripetersi)
// e ogni variante ha alcune voci, cosi' lo stesso effetto puo' suonare piu' volte insieme (raffiche, pallettoni).
// Volume per distanza e bilanciamento destra/sinistra calcolati qui, dalla posizione dell'ascoltatore di miniaudio.
// I suoni lontani possono partire in ritardo (velocita' del suono, proiettile in volo).
#include <miniaudio.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace SunEngine {

    class SfxBank {
    public:
        ~SfxBank() { Shutdown(); }

        void Init(ma_engine* engine, ma_sound_group* group, const std::string& dir) {
            m_Engine = engine; m_Group = group; m_Dir = dir;
            if (!m_Dir.empty() && m_Dir.back() != '/' && m_Dir.back() != '\\') m_Dir += "/";
        }

        // Carica "nome_1.wav", "nome_2.wav"... (o "nome.wav"). Ritorna l'indice del suono, -1 se non c'e' nessun file.
        int Load(const std::string& name, int voices = 3) {
            if (!m_Engine) return -1;
            Sound snd;
            auto add = [&](const std::string& file) {
                if (!std::filesystem::exists(file)) return false;
                Variant var;
                for (int v = 0; v < voices; ++v) {
                    auto voice = std::make_unique<Voice>();   // indirizzo stabile: miniaudio conserva il puntatore
                    ma_sound_config cfg = ma_sound_config_init();
                    cfg.pFilePath = file.c_str();
                    cfg.flags = MA_SOUND_FLAG_DECODE;         // file brevi: decodificati in RAM (una volta, poi condivisi)
                    cfg.pInitialAttachment = m_Group;
                    if (ma_sound_init_ex(m_Engine, &cfg, &voice->sound) != MA_SUCCESS) break;
                    voice->loaded = true;
                    ma_sound_set_spatialization_enabled(&voice->sound, MA_FALSE);   // volume e bilanciamento li decide la banca
                    var.voices.push_back(std::move(voice));
                }
                if (var.voices.empty()) return false;
                snd.variants.push_back(std::move(var));
                return true;
            };
            if (!add(m_Dir + name + ".wav"))
                for (int i = 1; i <= 8; ++i) if (!add(m_Dir + name + "_" + std::to_string(i) + ".wav")) break;
            if (snd.variants.empty()) return -1;
            m_Sounds.push_back(std::move(snd));
            return static_cast<int>(m_Sounds.size()) - 1;
        }

        bool Has(int id) const { return id >= 0 && id < static_cast<int>(m_Sounds.size()); }
        int LoadedCount() const { return static_cast<int>(m_Sounds.size()); }

        // Suono nel mondo. range = distanza a cui non si sente piu'; il volume scende con una curva morbida.
        void PlayAt(int id, const glm::vec3& pos, float volume, float range, float pitch = 1.0f, float delay = 0.0f) {
            if (!Has(id) || !m_Engine) return;
            const ma_vec3f lp = ma_engine_listener_get_position(m_Engine, 0);
            const float d = glm::distance(pos, glm::vec3(lp.x, lp.y, lp.z));
            if (d >= range) return;
            const float k = 1.0f - d / range;
            Start(id, volume * k * k, pitch, true, pos, delay);
        }
        // Suono "in testa" (la propria arma, conferme di colpo): niente posizione
        void Play2D(int id, float volume, float pitch = 1.0f, float delay = 0.0f) {
            if (!Has(id) || !m_Engine) return;
            Start(id, volume, pitch, false, glm::vec3(0.0f), delay);
        }

        // Fa partire i suoni in attesa. Da chiamare una volta per fotogramma col tempo reale.
        void Update(float dt) {
            m_Load *= std::exp(-dt / 0.15f);
            for (size_t i = 0; i < m_Pending.size();) {
                m_Pending[i].delay -= dt;
                if (m_Pending[i].delay <= 0.0f) {
                    const Pending p = m_Pending[i];
                    m_Pending[i] = m_Pending.back(); m_Pending.pop_back();
                    Fire(p);
                }
                else ++i;
            }
        }

        // Prima di ma_sound_group_uninit / ma_engine_uninit
        void Shutdown() {
            for (auto& s : m_Sounds) for (auto& v : s.variants) for (auto& voice : v.voices)
                if (voice && voice->loaded) { ma_sound_uninit(&voice->sound); voice->loaded = false; }
            m_Sounds.clear(); m_Pending.clear();
            m_Engine = nullptr; m_Group = nullptr;
        }

    private:
        struct Voice { ma_sound sound{}; bool loaded = false; };
        struct Variant { std::vector<std::unique_ptr<Voice>> voices; size_t next = 0; };
        struct Sound { std::vector<Variant> variants; int last = -1; };
        struct Pending { int id; float volume, pitch; bool spatial; glm::vec3 pos; float delay; };

        void Start(int id, float volume, float pitch, bool spatial, const glm::vec3& pos, float delay) {
            if (volume <= 0.02f) return;   // troppo piano per sentirlo: non occupa una voce
            const Pending p{ id, (std::min)(volume, 2.0f), pitch, spatial, pos, delay };
            if (delay > 0.005f) { if (m_Pending.size() < 256) m_Pending.push_back(p); }
            else Fire(p);
        }
        void Fire(const Pending& p) {
            Sound& s = m_Sounds[p.id];
            int vi = 0;
            if (s.variants.size() > 1) {   // mai la stessa variante due volte di fila
                std::uniform_int_distribution<int> pick(0, static_cast<int>(s.variants.size()) - 1);
                do { vi = pick(m_Rng); } while (vi == s.last);
            }
            s.last = vi;
            Variant& var = s.variants[vi];
            // una voce libera, altrimenti la piu' vecchia (a rotazione)
            Voice* voice = nullptr;
            for (auto& v : var.voices) if (!ma_sound_is_playing(&v->sound)) { voice = v.get(); break; }
            if (!voice) { voice = var.voices[var.next % var.voices.size()].get(); var.next++; ma_sound_stop(&voice->sound); }
            // Mischia affollata: tanti suoni insieme sforerebbero il massimo e graccherebbero. Piu' ce ne sono in
            // corso, piu' i nuovi entrano piano (quelli "in testa" del giocatore meno degli altri).
            const float duck = 1.0f / (1.0f + (p.spatial ? 0.45f : 0.2f) * (std::max)(0.0f, m_Load - 1.0f) / (std::max)(1.0f, p.volume));   // i suoni forti passano sopra
            m_Load += p.volume * (p.spatial ? 0.7f : 1.0f);
            // Destra/sinistra calcolati qui (non con lo spazializzatore di miniaudio, che in alcune versioni ignora il
            // volume del suono): bilanciamento dalla direzione rispetto a dove guarda l'ascoltatore, un po' piu' piano dietro.
            float pan = 0.0f, rear = 1.0f;
            if (p.spatial) {
                const ma_vec3f lp = ma_engine_listener_get_position(m_Engine, 0), ld = ma_engine_listener_get_direction(m_Engine, 0);
                glm::vec3 fwd(ld.x, 0.0f, ld.z), to = p.pos - glm::vec3(lp.x, lp.y, lp.z);
                const float dist = glm::length(to);
                if (glm::dot(fwd, fwd) > 1e-6f && dist > 0.3f) {
                    fwd = glm::normalize(fwd); to /= dist;
                    const glm::vec3 right(-fwd.z, 0.0f, fwd.x);
                    const float near = (std::min)(1.0f, dist / 2.0f);   // addosso all'ascoltatore: al centro
                    pan = std::clamp(glm::dot(to, right), -1.0f, 1.0f) * 0.85f * near;
                    rear = 1.0f - 0.22f * (std::max)(0.0f, -glm::dot(to, fwd)) * near;
                }
            }
            ma_sound_set_pan(&voice->sound, pan);
            ma_sound_set_volume(&voice->sound, p.volume * duck * kMasterGain * rear);
            ma_sound_set_pitch(&voice->sound, std::clamp(p.pitch, 0.4f, 2.5f));
            ma_sound_seek_to_pcm_frame(&voice->sound, 0);
            ma_sound_start(&voice->sound);
        }

        static constexpr float kMasterGain = 0.55f;   // margine: piu' suoni insieme non devono saturare
        float m_Load = 0.0f;                          // "affollamento" recente (somma dei volumi, si scarica in 0,15 s)
        ma_engine* m_Engine = nullptr;
        ma_sound_group* m_Group = nullptr;
        std::string m_Dir;
        std::vector<Sound> m_Sounds;
        std::vector<Pending> m_Pending;
        std::mt19937 m_Rng{ 1337u };
    };
}
