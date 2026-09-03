#pragma once
#include <entt/entt.hpp>

namespace SunEngine {
    class Scene {
    public:
        Scene() = default;
        ~Scene() = default;

        entt::entity CreateEntity() { return m_Registry.create(); }
        void DestroyEntity(entt::entity entity) { m_Registry.destroy(entity); }

        entt::registry& GetRegistry() { return m_Registry; }
        const entt::registry& GetRegistry() const { return m_Registry; }

    private:
        entt::registry m_Registry;
    };
}