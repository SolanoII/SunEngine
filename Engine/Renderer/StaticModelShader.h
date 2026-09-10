#pragma once

#include <glad/glad.h>
#include "../Core/Logger/Log.h"
#include <string>

namespace SunEngine {

    class StaticModelShader {
    public:
        unsigned int ID = 0;

        void Init() {
            const char* vertexShaderSource = R"(
                #version 410 core
                layout (location = 0) in vec3 aPos;
                layout (location = 1) in vec3 aNormal;
                layout (location = 2) in vec2 aTexCoords;

                out vec3 FragPos;
                out vec3 Normal;
                out vec2 TexCoords;

                uniform mat4 u_Model;
                uniform mat4 u_ViewProjection;

                void main() {
                    FragPos = vec3(u_Model * vec4(aPos, 1.0));
                    Normal = mat3(transpose(inverse(u_Model))) * aNormal;
                    TexCoords = aTexCoords;
                    gl_Position = u_ViewProjection * vec4(FragPos, 1.0);
                }
            )";

            const char* fragmentShaderSource = R"(
                #version 410 core
                out vec4 FragColor;

                in vec3 FragPos;
                in vec3 Normal;
                in vec2 TexCoords;

                uniform vec4 u_Color;

                void main() {
                    vec3 norm = normalize(Normal);
                    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
                    float diff = max(dot(norm, lightDir), 0.2);

                    vec3 baseColor = u_Color.rgb;
                    if (baseColor == vec3(0.0)) baseColor = vec3(0.7, 0.7, 0.75);

                    FragColor = vec4(baseColor * diff, u_Color.a);
                }
            )";

            unsigned int vertex = CompileShader(GL_VERTEX_SHADER, vertexShaderSource);
            unsigned int fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);

            ID = glCreateProgram();
            glAttachShader(ID, vertex);
            glAttachShader(ID, fragment);
            glLinkProgram(ID);

            glDeleteShader(vertex);
            glDeleteShader(fragment);
        }

        void Bind() const {
            glUseProgram(ID);
        }

        void Unbind() const {
            glUseProgram(0);
        }

    private:
        unsigned int CompileShader(unsigned int type, const char* source) {
            unsigned int shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, NULL);
            glCompileShader(shader);

            int success;
            char infoLog[512];
            glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
            if (!success) {
                glGetShaderInfoLog(shader, 512, NULL, infoLog);
                SUN_CORE_ERROR("Errore compilazione StaticModelShader: {0}", infoLog);
            }
            return shader;
        }
    };

}