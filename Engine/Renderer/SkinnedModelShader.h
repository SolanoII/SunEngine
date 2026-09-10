#pragma once

#include <glad/glad.h>
#include "../Core/Logger/Log.h"
#include <string>

namespace SunEngine {

    static const int MAX_BONES = 100;

    class SkinnedModelShader {
    public:
        unsigned int ID = 0;

        void Init() {
            const char* vertexShaderSource = R"(
                #version 410 core
                layout (location = 0) in vec3 aPos;
                layout (location = 1) in vec3 aNormal;
                layout (location = 2) in vec2 aTexCoords;
                layout (location = 3) in ivec4 aBoneIDs;
                layout (location = 4) in vec4 aWeights;

                out vec3 FragPos;
                out vec3 Normal;

                uniform mat4 u_Model;
                uniform mat4 u_ViewProjection;
                uniform mat4 u_BoneMatrices[100];

                void main() {
                    vec4 skinnedPos = vec4(0.0);
                    vec3 skinnedNormal = vec3(0.0);
                    float totalWeight = 0.0;

                    for (int i = 0; i < 4; i++) {
                        if (aBoneIDs[i] == -1) continue;
                        if (aBoneIDs[i] >= 100) continue;

                        mat4 boneTransform = u_BoneMatrices[aBoneIDs[i]];
                        skinnedPos += boneTransform * vec4(aPos, 1.0) * aWeights[i];
                        skinnedNormal += mat3(boneTransform) * aNormal * aWeights[i];
                        totalWeight += aWeights[i];
                    }

                    // Fallback: vertice senza influenze ossee valide (raro, ma evita di collassarlo nell'origine)
                    if (totalWeight < 0.001) {
                        skinnedPos = vec4(aPos, 1.0);
                        skinnedNormal = aNormal;
                    }

                    FragPos = vec3(u_Model * skinnedPos);
                    Normal = mat3(transpose(inverse(u_Model))) * skinnedNormal;
                    gl_Position = u_ViewProjection * u_Model * skinnedPos;
                }
            )";

            const char* fragmentShaderSource = R"(
                #version 410 core
                out vec4 FragColor;

                in vec3 FragPos;
                in vec3 Normal;

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

            int success;
            char infoLog[512];
            glGetProgramiv(ID, GL_LINK_STATUS, &success);
            if (!success) {
                glGetProgramInfoLog(ID, 512, NULL, infoLog);
                SUN_CORE_ERROR("Errore linking SkinnedModelShader: {0}", infoLog);
            }

            glDeleteShader(vertex);
            glDeleteShader(fragment);
        }

        void Bind() const { glUseProgram(ID); }
        void Unbind() const { glUseProgram(0); }

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
                SUN_CORE_ERROR("Errore compilazione SkinnedModelShader: {0}", infoLog);
            }
            return shader;
        }
    };
}