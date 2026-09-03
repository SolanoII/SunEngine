#pragma once

#include <spdlog/spdlog.h>
#include <memory>

namespace SunEngine {

    class Log {
    public:
        static void Init();

        static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
        static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }

    private:
        static std::shared_ptr<spdlog::logger> s_CoreLogger;
        static std::shared_ptr<spdlog::logger> s_ClientLogger;
    };

}

#define SUN_CORE_TRACE(...) ::SunEngine::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define SUN_CORE_INFO(...)  ::SunEngine::Log::GetCoreLogger()->info(__VA_ARGS__)
#define SUN_CORE_WARN(...)  ::SunEngine::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define SUN_CORE_ERROR(...) ::SunEngine::Log::GetCoreLogger()->error(__VA_ARGS__)