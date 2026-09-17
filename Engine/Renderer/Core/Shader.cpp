#include "Shader.h"
#include "../../Core/Logger/Log.h"
#include <fstream>
#include <sstream>
#include <glm/gtc/type_ptr.hpp>

namespace SunEngine {

    Shader::~Shader() {
        if (m_ProgramID != 0) {
            glDeleteProgram(m_ProgramID);
        }
    }

    Shader::Shader(Shader&& other) noexcept
        : m_ProgramID(other.m_ProgramID),
        m_UniformCache(std::move(other.m_UniformCache)) {
        other.m_ProgramID = 0;
    }

    Shader& Shader::operator=(Shader&& other) noexcept {
        if (this != &other) {
            if (m_ProgramID != 0) glDeleteProgram(m_ProgramID);
            m_ProgramID = other.m_ProgramID;
            m_UniformCache = std::move(other.m_UniformCache);
            other.m_ProgramID = 0;
        }
        return *this;
    }

    std::string Shader::ReadFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            SUN_CORE_ERROR("Shader::ReadFile - impossibile aprire: {0}", path);
            return "";
        }
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    GLuint Shader::CompileShader(GLenum type, const std::string& source,
        const std::string& debugName) {
        GLuint shader = glCreateShader(type);
        const char* src = source.c_str();
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);

        GLint success = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

        if (!success) {
            GLint logLength = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
            std::vector<char> log(logLength);
            glGetShaderInfoLog(shader, logLength, nullptr, log.data());

            const char* typeStr = (type == GL_VERTEX_SHADER) ? "VERTEX" :
                (type == GL_FRAGMENT_SHADER) ? "FRAGMENT" :
                (type == GL_COMPUTE_SHADER) ? "COMPUTE" :
                (type == GL_GEOMETRY_SHADER) ? "GEOMETRY" : "UNKNOWN";

            SUN_CORE_ERROR("Shader compile error [{0}] ({1}):\n{2}",
                debugName, typeStr, log.data());

            glDeleteShader(shader);
            return 0;
        }

        return shader;
    }

    bool Shader::LoadFromSource(const std::string& vertSrc, const std::string& fragSrc,
        const std::string& debugName) {
        if (m_ProgramID != 0) {
            glDeleteProgram(m_ProgramID);
            m_ProgramID = 0;
            m_UniformCache.clear();
        }

        GLuint vert = CompileShader(GL_VERTEX_SHADER, vertSrc, debugName);
        if (vert == 0) return false;

        GLuint frag = CompileShader(GL_FRAGMENT_SHADER, fragSrc, debugName);
        if (frag == 0) {
            glDeleteShader(vert);
            return false;
        }

        GLuint program = glCreateProgram();
        glAttachShader(program, vert);
        glAttachShader(program, frag);
        glLinkProgram(program);

        glDeleteShader(vert);
        glDeleteShader(frag);

        GLint success = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success) {
            GLint logLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
            std::vector<char> log(logLength);
            glGetProgramInfoLog(program, logLength, nullptr, log.data());

            SUN_CORE_ERROR("Shader link error [{0}]:\n{1}", debugName, log.data());
            glDeleteProgram(program);
            return false;
        }

        m_ProgramID = program;
        SUN_CORE_INFO("Shader caricato: {0}", debugName);
        return true;
    }

    bool Shader::LoadFromFiles(const std::string& vertPath, const std::string& fragPath) {
        std::string vertSrc = ReadFile(vertPath);
        std::string fragSrc = ReadFile(fragPath);

        if (vertSrc.empty() || fragSrc.empty()) return false;

        std::string debugName = vertPath + " + " + fragPath;
        return LoadFromSource(vertSrc, fragSrc, debugName);
    }

    void Shader::Bind() const {
        glUseProgram(m_ProgramID);
    }

    void Shader::Unbind() {
        glUseProgram(0);
    }

    GLint Shader::GetUniformLocation(const std::string& name) {
        auto it = m_UniformCache.find(name);
        if (it != m_UniformCache.end()) return it->second;

        GLint loc = glGetUniformLocation(m_ProgramID, name.c_str());
        m_UniformCache[name] = loc;

        if (loc == -1) {
            SUN_CORE_WARN("Shader uniform non trovato: '{0}' (program {1})",
                name, m_ProgramID);
        }
        return loc;
    }

    void Shader::SetInt(const std::string& name, int value) {
        glUniform1i(GetUniformLocation(name), value);
    }

    void Shader::SetFloat(const std::string& name, float value) {
        glUniform1f(GetUniformLocation(name), value);
    }

    void Shader::SetVec2(const std::string& name, const glm::vec2& v) {
        glUniform2fv(GetUniformLocation(name), 1, glm::value_ptr(v));
    }

    void Shader::SetVec3(const std::string& name, const glm::vec3& v) {
        glUniform3fv(GetUniformLocation(name), 1, glm::value_ptr(v));
    }

    void Shader::SetVec4(const std::string& name, const glm::vec4& v) {
        glUniform4fv(GetUniformLocation(name), 1, glm::value_ptr(v));
    }

    void Shader::SetMat3(const std::string& name, const glm::mat3& m) {
        glUniformMatrix3fv(GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(m));
    }

    void Shader::SetMat4(const std::string& name, const glm::mat4& m) {
        glUniformMatrix4fv(GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(m));
    }

    void Shader::BindUniformBlock(const std::string& blockName, GLuint bindingPoint) {
        GLuint blockIndex = glGetUniformBlockIndex(m_ProgramID, blockName.c_str());
        if (blockIndex == GL_INVALID_INDEX) {
            SUN_CORE_WARN("Uniform block non trovato: '{0}'", blockName);
            return;
        }
        glUniformBlockBinding(m_ProgramID, blockIndex, bindingPoint);
    }

    void Shader::SetTexture(const std::string& name, GLuint textureId, int unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, textureId);
        SetInt(name, unit);
    }
    bool Shader::LoadComputeFromSource(const std::string& src, const std::string& debugName) {
        if (m_ProgramID != 0) {
            glDeleteProgram(m_ProgramID);
            m_ProgramID = 0;
            m_UniformCache.clear();
        }

        GLuint comp = CompileShader(GL_COMPUTE_SHADER, src, debugName);
        if (comp == 0) return false;

        GLuint program = glCreateProgram();
        glAttachShader(program, comp);
        glLinkProgram(program);
        glDeleteShader(comp);

        GLint success = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success) {
            GLint logLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
            std::vector<char> log(logLength);
            glGetProgramInfoLog(program, logLength, nullptr, log.data());
            SUN_CORE_ERROR("Compute shader link error [{0}]:\n{1}", debugName, log.data());
            glDeleteProgram(program);
            return false;
        }

        m_ProgramID = program;
        SUN_CORE_INFO("Compute shader caricato: {0}", debugName);
        return true;
    }
} // namespace SunEngine