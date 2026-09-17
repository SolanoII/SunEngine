#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <memory>

namespace SunEngine {

    class Shader {
    public:
        Shader() = default;
        ~Shader();

        // Non copiabile, spostabile
        Shader(const Shader&) = delete;
        Shader& operator=(const Shader&) = delete;
        Shader(Shader&& other) noexcept;
        Shader& operator=(Shader&& other) noexcept;

        // Compila vertex + fragment da file
        bool LoadFromFiles(const std::string& vertPath, const std::string& fragPath);

        // Compila da stringhe (utile per shader di debug)
        bool LoadFromSource(const std::string& vertSrc, const std::string& fragSrc,
            const std::string& debugName = "unnamed");

        void Bind() const;
        static void Unbind();

        // Uniform setters con cache di location
        void SetInt(const std::string& name, int value);
        void SetFloat(const std::string& name, float value);
        void SetVec2(const std::string& name, const glm::vec2& v);
        void SetVec3(const std::string& name, const glm::vec3& v);
        void SetVec4(const std::string& name, const glm::vec4& v);
        void SetMat3(const std::string& name, const glm::mat3& m);
        void SetMat4(const std::string& name, const glm::mat4& m);

        // Binding di un UBO a un binding point specifico
        void BindUniformBlock(const std::string& blockName, GLuint bindingPoint);

        // Rende un uniform texture sampler sul texture unit specificato
        void SetTexture(const std::string& name, GLuint textureId, int unit);

        GLuint GetID() const { return m_ProgramID; }
        bool IsValid() const { return m_ProgramID != 0; }

    private:
        GLuint m_ProgramID = 0;
        std::unordered_map<std::string, GLint> m_UniformCache;

        GLint GetUniformLocation(const std::string& name);

        static GLuint CompileShader(GLenum type, const std::string& source,
            const std::string& debugName);
        static std::string ReadFile(const std::string& path);
    };

    using ShaderPtr = std::shared_ptr<Shader>;

} // namespace SunEngine