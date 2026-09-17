#pragma once

#include <random>
#include <cstdint>
#include <chrono>

namespace SunEngine {

    // RNG thread-local: ogni thread ha la sua istanza, nessuna race condition,
    // nessun lock. Seed derivato da high_resolution_clock + thread id: due run
    // producono sequenze diverse, ma lo stesso thread mantiene riproducibilità
    // se il seed e' fissato esplicitamente.
    class Random {
    public:
        static std::mt19937& Engine() {
            thread_local std::mt19937 s_Engine = MakeSeedEngine();
            return s_Engine;
        }

        // Uniforme in [0.0, 1.0)
        static float Float01() {
            thread_local std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            return dist(Engine());
        }

        // Uniforme in [min, max]
        static float Range(float min, float max) {
            if (max <= min) return min;
            std::uniform_real_distribution<float> dist(min, max);
            return dist(Engine());
        }

        // Uniforme in [min, max] inclusi
        static int RangeInt(int min, int max) {
            if (max < min) std::swap(min, max);
            std::uniform_int_distribution<int> dist(min, max);
            return dist(Engine());
        }

        // Probabilita' p in [0.0, 1.0]. Ritorna true con probabilita' p.
        static bool Chance(float p) {
            if (p <= 0.0f) return false;
            if (p >= 1.0f) return true;
            return Float01() < p;
        }

        // Seed esplicito (utile per test deterministici)
        static void SetSeed(uint32_t seed) {
            Engine().seed(seed);
        }

    private:
        inline static int s_SeedAnchor_ = 0;   // <-- inline static, definito qui

        static std::mt19937 MakeSeedEngine() {
            // Seed non deterministico ma stabile per sessione:
            // high_resolution_clock + reinterpret del puntatore a thread_local
            auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            uint64_t seed = static_cast<uint64_t>(now);
            // Mix con l'indirizzo dello storage per disambiguare i thread
            seed ^= reinterpret_cast<uint64_t>(&s_SeedAnchor_);
            return std::mt19937(static_cast<uint32_t>(seed));
        }

    };

}