#include "Log.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <vector>

namespace SunEngine {

    std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
    std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

    void Log::Init() {
        // Console + file "sunengine_log.txt" accanto all'eseguibile (riscritto a ogni avvio): chi prova un modello o
        // segnala un problema puo' leggerlo o inviarlo anche senza la console
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        try { sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("sunengine_log.txt", true)); }
        catch (...) {}   // cartella in sola lettura: solo console

        s_CoreLogger = std::make_shared<spdlog::logger>("SUN_ENGINE", sinks.begin(), sinks.end());
        s_CoreLogger->set_level(spdlog::level::trace);
        s_CoreLogger->flush_on(spdlog::level::info);
        spdlog::register_logger(s_CoreLogger);

        s_ClientLogger = std::make_shared<spdlog::logger>("APP", sinks.begin(), sinks.end());
        s_ClientLogger->set_level(spdlog::level::trace);
        s_ClientLogger->flush_on(spdlog::level::info);
        spdlog::register_logger(s_ClientLogger);
        spdlog::set_pattern("%^[%T] %n: %v%$");   // dopo la registrazione: vale per entrambi i logger
    }

}