#include "DeferredRenderer.h"
#include "../../Core/Logger/Log.h"
#include "../../Renderer/Camera.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace SunEngine {

    // ============================================================
    // SHADERS (inline per ora; in Fase 2 li spostiamo in file .glsl)
    // ============================================================

    static const char* k_GBufferVertSrc = R"(
        #version 410 core
        layout (location = 0) in vec3 a_Position;
        layout (location = 1) in vec3 a_Normal;

        uniform mat4 u_Model;
        uniform mat4 u_ViewProjection;

        out vec3 v_WorldPos;
        out vec3 v_Normal;

        void main() {
            vec4 worldPos = u_Model * vec4(a_Position, 1.0);
            v_WorldPos = worldPos.xyz;
            mat3 normalMatrix = transpose(inverse(mat3(u_Model)));
            v_Normal = normalize(normalMatrix * a_Normal);
            gl_Position = u_ViewProjection * worldPos;
        }
    )";

    // Cubi: tinta unita oppure materiali procedurali delle mappe (SurfaceMaterials.h), proiettati nel mondo.
    // Tavolozza grimdark: fango, cemento annerito, ruggine, juta sporca, legno bruciato, alberi morti.
    static const char* k_GBufferFragSrc = R"(
        #version 410 core
        in vec3 v_WorldPos;
        in vec3 v_Normal;

        layout (location = 0) out vec4 out_Albedo;
        layout (location = 1) out vec4 out_Normal;
        layout (location = 2) out vec4 out_Material;

        uniform vec4  u_Color;
        uniform float u_Roughness;
        uniform float u_Metallic;
        uniform float u_Emissive;
        uniform int   u_Material;   // 0 tinta unita, 1 fango, 2 cemento, 3 lamiera, 4 sacchi, 5 legno, 6 chioma, 7 terrapieno, 8 detriti
        uniform float u_Relief;     // rilievo dei materiali (opzioni grafiche)

        float H31(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
        float H21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
        float N3(vec3 x) {
            vec3 i = floor(x), f = fract(x); f = f * f * (3.0 - 2.0 * f);
            return mix(mix(mix(H31(i), H31(i + vec3(1, 0, 0)), f.x), mix(H31(i + vec3(0, 1, 0)), H31(i + vec3(1, 1, 0)), f.x), f.y),
                       mix(mix(H31(i + vec3(0, 0, 1)), H31(i + vec3(1, 0, 1)), f.x), mix(H31(i + vec3(0, 1, 1)), H31(i + vec3(1, 1, 1)), f.x), f.y), f.z);
        }
        float Fbm(vec3 p) { float v = 0.0, a = 0.5; for (int i = 0; i < 4; ++i) { v += a * N3(p); p = p * 2.07 + 1.3; a *= 0.5; } return v; }
        float Ridge(vec3 p, float w) { return 1.0 - smoothstep(0.0, w, abs(N3(p) - 0.5)); }   // linee sottili (crepe, venature)
        float Lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

        // Rilievo da un'altezza in metri (derivate a schermo della posizione nel mondo)
        vec3 BumpW(vec3 N, float h) {
            vec3 dpdx = dFdx(v_WorldPos), dpdy = dFdy(v_WorldPos);
            vec3 r1 = cross(dpdy, N), r2 = cross(N, dpdx);
            float det = dot(dpdx, r1);
            vec3 grad = sign(det) * (dFdx(h) * r1 + dFdy(h) * r2);
            return normalize(abs(det) * N - grad);
        }

        void main() {
            vec3 N = normalize(v_Normal);
            vec3 col = u_Color.rgb;
            float rough = u_Roughness, metal = u_Metallic;
            if (u_Material > 0) {
                vec3 p = v_WorldPos;
                bool top = N.y > 0.6;
                bool sideX = abs(N.x) > abs(N.z);
                vec2 uv = top ? p.xz : (sideX ? p.zy : p.xy);          // coordinate sulla faccia (metri)
                vec3 tint = mix(vec3(Lum(col)), col, 0.45);             // il colore della mappa conta poco e spento
                float h = 0.0;                                          // rilievo (metri)
                float big = Fbm(p * 0.35), mid = N3(p * 3.1), fine = N3(p * 17.0);
                rough = 0.85;

                if (u_Material == 1) {
                    // ---- FANGO: asciutto e crepato / bagnato e scuro, ghiaia, crateri bruciati, pozzanghere
                    vec3 dry = vec3(0.23, 0.19, 0.15), wet = vec3(0.10, 0.085, 0.07);
                    float wetness = smoothstep(0.42, 0.62, Fbm(vec3(p.xz * 0.07, 1.7)));
                    col = mix(dry, wet, wetness);
                    col *= 0.85 + 0.25 * mid;
                    float gravel = smoothstep(0.7, 0.85, N3(p * 23.0)) * smoothstep(0.4, 0.7, N3(p * 2.3)) * (1.0 - wetness);
                    col = mix(col, vec3(0.25, 0.23, 0.21), gravel * 0.45);
                    float cracks = Ridge(p * 1.6, 0.03) * (1.0 - wetness) * smoothstep(0.45, 0.7, big);
                    col *= 1.0 - cracks * 0.35;
                    h = big * 0.06 + mid * 0.015 + fine * 0.004 + gravel * 0.01 - cracks * 0.01;
                    // crateri: uno ogni tanto in celle di 16 m
                    vec2 cell = floor(p.xz / 16.0);
                    for (int dx = -1; dx <= 1; ++dx) for (int dz = -1; dz <= 1; ++dz) {
                        vec2 c = cell + vec2(dx, dz);
                        if (H21(c) < 0.45) continue;
                        vec2 center = (c + vec2(H21(c + 7.1), H21(c + 3.7))) * 16.0;
                        float rad = 1.4 + 2.2 * H21(c + 1.9);
                        float d = length(p.xz - center) / rad;
                        if (d > 1.6) continue;
                        h += 0.10 * exp(-pow((d - 1.0) / 0.18, 2.0)) - 0.22 * (1.0 - smoothstep(0.0, 1.0, d));
                        col = mix(col, vec3(0.035, 0.03, 0.028), (1.0 - smoothstep(0.15, 0.75, d)) * 0.85);   // terra bruciata
                        col = mix(col, vec3(0.26, 0.25, 0.23), exp(-pow((d - 0.95) / 0.12, 2.0)) * 0.35);   // cenere sull'orlo
                        wetness = max(wetness, 1.0 - smoothstep(0.0, 0.45, d));
                    }
                    rough = mix(0.95, 0.55, wetness);
                    // pozzanghere (piatte, specchiano il cielo)
                    float puddle = smoothstep(0.60, 0.64, Fbm(vec3(p.xz * 0.11, 9.3)) + wetness * 0.08) * (top ? 1.0 : 0.0);
                    col = mix(col, vec3(0.045, 0.042, 0.04), puddle);
                    rough = mix(rough, 0.05, puddle);
                    h = mix(h, -0.02 + 0.002 * N3(p * 25.0), puddle);
                    col = mix(col, col * tint / max(Lum(tint), 0.05), 0.2);
                }
                else if (u_Material == 2 || u_Material == 8) {
                    // ---- CEMENTO (rockcrete): pannelli, crepe, colature, colpi di proiettile, fuliggine
                    col = vec3(0.34, 0.33, 0.31) * (0.8 + 0.35 * big) * mix(vec3(1.0), tint / max(Lum(tint), 0.05), 0.25);
                    vec2 pan = uv / vec2(2.0, 1.5);
                    vec2 g = abs(fract(pan) - 0.5);
                    float seam = u_Material == 8 ? 0.0 : (1.0 - smoothstep(0.0, 0.012, min(0.5 - g.x, 0.5 - g.y) * 1.5));
                    float streaks = top ? 0.0 : smoothstep(0.45, 0.8, N3(vec3(uv.x * 7.0, uv.y * 0.45, 3.0)));
                    float cracks = Ridge(p * 1.3, 0.02) * smoothstep(0.45, 0.7, Fbm(p * 0.6 + 5.0));
                    float stains = smoothstep(0.5, 0.8, Fbm(p * 0.9 + 11.0));
                    col *= 1.0 - 0.35 * streaks - 0.45 * stains * 0.6;
                    col *= 1.0 - cracks * 0.6 - seam * 0.45;
                    // colpi: buchetti sparsi
                    vec2 hc = floor(uv / 0.32);
                    float hit = 0.0;
                    if (H21(hc + 17.0) > 0.94) hit = 1.0 - smoothstep(0.03, 0.08, length(fract(uv / 0.32) - 0.5) * 0.32);
                    col = mix(col, vec3(0.08, 0.075, 0.07), hit * 0.8);
                    h = mid * 0.006 + fine * 0.002 - seam * 0.012 - cracks * 0.006 - hit * 0.015 + big * 0.01;
                    if (u_Material == 8) { h += big * 0.05; col *= 0.85; }
                    rough = 0.9 - stains * 0.1;
                }
                else if (u_Material == 3) {
                    // ---- LAMIERE: vernice militare scrostata, ruggine, rivetti, colature di ruggine
                    vec2 pl = uv / vec2(1.25, 2.5);
                    vec2 g = abs(fract(pl) - 0.5);
                    float edge = min(0.5 - g.x, 0.5 - g.y) * 1.25;          // metri dal bordo della piastra (circa)
                    float seam = 1.0 - smoothstep(0.0, 0.015, edge);
                    float rivetLine = 1.0 - smoothstep(0.035, 0.05, abs(edge - 0.06));
                    vec2 rv = fract(uv / 0.16) - 0.5;
                    float rivet = rivetLine * (1.0 - smoothstep(0.18, 0.3, length(rv)));
                    vec3 paint = vec3(0.16, 0.18, 0.16) * mix(vec3(1.0), tint / max(Lum(tint), 0.05), 0.3);
                    vec3 rust = mix(vec3(0.24, 0.12, 0.06), vec3(0.13, 0.07, 0.04), N3(p * 6.0));
                    float chip = smoothstep(0.52, 0.6, Fbm(p * 1.4 + 2.0) + seam * 0.25 + rivet * 0.3);
                    float streak = top ? 0.0 : smoothstep(0.55, 0.85, N3(vec3(uv.x * 9.0, uv.y * 0.35, 1.0))) * smoothstep(0.3, 0.7, big);
                    col = mix(paint * (0.85 + 0.3 * mid), rust, max(chip, streak * 0.7));
                    float bare = smoothstep(0.62, 0.66, Fbm(p * 1.4 + 2.0)) * (1.0 - streak) * smoothstep(0.5, 0.8, fine);
                    col = mix(col, vec3(0.38, 0.37, 0.36), bare * 0.6);
                    metal = mix(0.25, 0.85, bare);
                    rough = mix(0.7, 0.95, chip) - bare * 0.35;
                    col *= 1.0 - seam * 0.5;
                    h = -seam * 0.01 + rivet * 0.008 + chip * 0.002 + mid * 0.002;
                }
                else if (u_Material == 4) {
                    // ---- SACCHI DI SABBIA: file sfalsate, cuciture, juta, fango in basso
                    vec2 bagSize = vec2(0.55, 0.2);
                    vec2 q = uv / bagSize;
                    float row = floor(q.y);
                    q.x += mod(row, 2.0) * 0.5;
                    vec2 f = fract(q);
                    float pillow = (1.0 - pow(abs(2.0 * f.x - 1.0), 3.0)) * (1.0 - pow(abs(2.0 * f.y - 1.0), 2.2));
                    vec2 id = vec2(floor(q.x), row);
                    vec3 burlap = mix(vec3(0.31, 0.27, 0.20), vec3(0.24, 0.20, 0.15), H21(id)) * mix(vec3(1.0), tint / max(Lum(tint), 0.05), 0.2);
                    float weave = (sin(uv.x * 900.0) * sin(uv.y * 900.0)) * 0.5 + 0.5;
                    col = burlap * (0.7 + 0.35 * pillow) * (0.92 + 0.08 * weave);
                    col *= 0.85 + 0.25 * mid;
                    h = pillow * 0.05 + weave * 0.0006 + mid * 0.004;
                    rough = 0.95;
                }
                else if (u_Material == 5) {
                    // ---- LEGNO: venature lungo l'asse, bruciature
                    vec3 gp = abs(N.y) > 0.6 ? p : vec3(p.x * 6.0, p.y * 0.4, p.z * 6.0);
                    float grain = sin((gp.x + gp.z) * 18.0 + Fbm(gp * 2.0) * 8.0) * 0.5 + 0.5;
                    col = mix(vec3(0.19, 0.13, 0.085), vec3(0.11, 0.075, 0.05), grain) * (0.85 + 0.3 * mid);
                    float burnt = smoothstep(0.55, 0.75, Fbm(p * 0.8 + 4.0));
                    col = mix(col, vec3(0.035, 0.03, 0.028), burnt);
                    h = grain * 0.004 + Ridge(gp * 3.0, 0.05) * 0.003;
                    rough = 0.9;
                }
                else if (u_Material == 6) {
                    // ---- CHIOMA MORTA: ciuffi secchi oliva e marroni
                    float clump = Fbm(p * 2.2);
                    col = mix(vec3(0.15, 0.15, 0.09), vec3(0.16, 0.11, 0.065), smoothstep(0.35, 0.65, N3(p * 1.3)));
                    col *= 0.55 + 0.75 * clump;
                    h = clump * 0.12 + fine * 0.02;
                    rough = 0.95;
                }
                else if (u_Material == 7) {
                    // ---- TERRAPIENO: terra battuta sopra, assi di contenimento e pali sui lati
                    vec3 dirt = mix(vec3(0.21, 0.17, 0.13), vec3(0.11, 0.09, 0.07), smoothstep(0.4, 0.7, big)) * (0.85 + 0.25 * mid);
                    if (top) { col = dirt; h = big * 0.05 + mid * 0.01; }
                    else {
                        float plankY = fract(uv.y / 0.26);
                        float gap = 1.0 - smoothstep(0.02, 0.07, min(plankY, 1.0 - plankY));
                        float post = 1.0 - smoothstep(0.06, 0.09, abs(fract(uv.x / 1.6) - 0.5) * 1.6);
                        float pid = H21(vec2(floor(uv.y / 0.26), floor(uv.x / 1.6)));
                        vec3 wood = mix(vec3(0.17, 0.12, 0.08), vec3(0.10, 0.075, 0.05), pid) * (0.8 + 0.3 * N3(vec3(uv.x * 0.8, uv.y * 12.0, 2.0)));
                        col = mix(wood, dirt * 0.6, gap);
                        col = mix(col, vec3(0.08, 0.06, 0.045), post * 0.8);
                        h = (1.0 - gap) * 0.015 + post * 0.02 + mid * 0.003;
                    }
                    rough = 0.92;
                }

                // sporco comune: fango schizzato in basso sulle pareti, polvere sulle superfici in alto
                if (u_Material != 1 && u_Material != 6) {
                    float base = (1.0 - smoothstep(0.05, 0.9, p.y)) * (top ? 0.0 : 1.0);
                    float splash = base * smoothstep(0.35, 0.6, N3(p * 4.0) + base * 0.4);
                    col = mix(col, vec3(0.11, 0.09, 0.07), splash * 0.8);
                    rough = mix(rough, 0.6, splash * 0.5);
                    float dust = top ? smoothstep(0.3, 0.7, big) * 0.35 : 0.0;
                    col = mix(col, vec3(0.27, 0.24, 0.20), dust);
                }
                if (u_Relief > 0.001) N = BumpW(N, h * u_Relief);
            }
            out_Albedo   = vec4(col, u_Color.a);
            out_Normal   = vec4(N, 1.0);
            out_Material = vec4(clamp(rough, 0.04, 1.0), metal, u_Emissive, 0.0);
        }
    )";

    static const char* k_LightingVertSrc = R"(
        #version 410 core
        layout (location = 0) in vec2 a_Position;
        layout (location = 1) in vec2 a_TexCoord;
        out vec2 v_TexCoord;
        void main() {
            v_TexCoord = a_TexCoord;
            gl_Position = vec4(a_Position, 0.0, 1.0);
        }
    )";

    // Occlusione ambientale a schermo: campioni in un emisfero orientato sulla normale, nel mondo; un campione e'
    // occluso se la superficie vista in quel punto dello schermo e' piu' vicina alla camera del campione.
    static const char* k_SSAOFragSrc = R"(
#version 410 core
in vec2 v_TexCoord;
out float out_AO;
uniform sampler2D u_Depth;
uniform sampler2D u_Normal;
uniform mat4 u_InvViewProjection;
uniform mat4 u_ViewProjection;
uniform vec3 u_CameraPos;
uniform float u_Radius;
vec3 Recon(vec2 uv, float d) { vec4 w = u_InvViewProjection * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0); return w.xyz / w.w; }
float Hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
void main() {
    float depth = texture(u_Depth, v_TexCoord).r;
    if (depth >= 0.9999) { out_AO = 1.0; return; }
    vec3 P = Recon(v_TexCoord, depth);
    vec3 N = normalize(texture(u_Normal, v_TexCoord).xyz);
    float camDist = length(P - u_CameraPos);
    float radius = u_Radius * clamp(camDist / 6.0, 0.6, 2.5);   // un po' piu' largo lontano (meno rumore)
    // base tangente ruotata a caso per pixel (il rumore lo toglie la sfocatura)
    float a = Hash(gl_FragCoord.xy) * 6.2831853;
    vec3 rnd = vec3(cos(a), sin(a), 0.0);
    vec3 T = normalize(abs(N.y) < 0.95 ? cross(N, vec3(0.0, 1.0, 0.0)) : cross(N, vec3(1.0, 0.0, 0.0)));
    vec3 B = cross(N, T);
    T = T * rnd.x + B * rnd.y; B = cross(N, T);
    const int COUNT = 12;
    float occ = 0.0;
    for (int i = 0; i < COUNT; ++i) {
        float fi = float(i) + 0.5;
        float r = sqrt(fi / float(COUNT));
        float th = fi * 2.399963;                                // spirale aurea
        vec3 k = vec3(cos(th) * r, sin(th) * r, sqrt(max(0.0, 1.0 - r * r)));
        float scale = mix(0.15, 1.0, (fi / float(COUNT)) * (fi / float(COUNT)));
        vec3 S = P + (T * k.x + B * k.y + N * k.z) * radius * scale + N * 0.02;
        vec4 c = u_ViewProjection * vec4(S, 1.0);
        vec3 ndc = c.xyz / c.w;
        vec2 uv = ndc.xy * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) continue;
        float sd = texture(u_Depth, uv).r;
        if (sd >= 0.9999) continue;
        vec3 Q = Recon(uv, sd);
        float dq = length(Q - u_CameraPos), ds = length(S - u_CameraPos);
        float range = smoothstep(0.0, 1.0, radius / max(abs(camDist - dq), 1e-3));
        occ += (dq < ds - 0.03 ? 1.0 : 0.0) * range;
    }
    out_AO = pow(clamp(1.0 - occ / float(COUNT), 0.0, 1.0), 1.6);
}
)";

    // Sfocatura dell'occlusione che rispetta i bordi (non sbava fra oggetti a profondita' diverse)
    static const char* k_SSAOBlurFragSrc = R"(
#version 410 core
in vec2 v_TexCoord;
out float out_AO;
uniform sampler2D u_AO;
uniform sampler2D u_Depth;
void main() {
    vec2 texel = 1.0 / vec2(textureSize(u_AO, 0));
    float d0 = texture(u_Depth, v_TexCoord).r;
    float sum = 0.0, wsum = 0.0;
    for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y) {
            vec2 uv = v_TexCoord + vec2(x, y) * texel;
            float d = texture(u_Depth, uv).r;
            float w = 1.0 / (1.0 + abs(d - d0) * 4000.0);
            sum += texture(u_AO, uv).r * w; wsum += w;
        }
    out_AO = sum / max(wsum, 1e-4);
}
)";

    // Antialiasing FXAA (versione compatta della 3.11 di Timothy Lottes): ammorbidisce i bordi seghettati
    // cercando la direzione del bordo dalla luminosita' dei vicini.
    static const char* k_FXAAFragSrc = R"(
#version 410 core
in vec2 v_TexCoord;
out vec4 out_Color;
uniform sampler2D u_Image;
float Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
    vec2 px = 1.0 / vec2(textureSize(u_Image, 0));
    vec3 rgbNW = texture(u_Image, v_TexCoord + vec2(-1.0, -1.0) * px).rgb;
    vec3 rgbNE = texture(u_Image, v_TexCoord + vec2( 1.0, -1.0) * px).rgb;
    vec3 rgbSW = texture(u_Image, v_TexCoord + vec2(-1.0,  1.0) * px).rgb;
    vec3 rgbSE = texture(u_Image, v_TexCoord + vec2( 1.0,  1.0) * px).rgb;
    vec3 rgbM  = texture(u_Image, v_TexCoord).rgb;
    float lNW = Luma(rgbNW), lNE = Luma(rgbNE), lSW = Luma(rgbSW), lSE = Luma(rgbSE), lM = Luma(rgbM);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) { out_Color = vec4(rgbM, 1.0); return; }   // niente bordo
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, vec2(-8.0), vec2(8.0)) * px;
    vec3 rgbA = 0.5 * (texture(u_Image, v_TexCoord + dir * (1.0 / 3.0 - 0.5)).rgb + texture(u_Image, v_TexCoord + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(u_Image, v_TexCoord + dir * -0.5).rgb + texture(u_Image, v_TexCoord + dir * 0.5).rgb);
    float lB = Luma(rgbB);
    out_Color = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);
}
)";

    static const char* k_LightingFragSrc = R"(
#version 410 core
in vec2 v_TexCoord;
out vec4 out_Color;

uniform sampler2D u_GBufferAlbedo;
uniform sampler2D u_GBufferNormal;
uniform sampler2D u_GBufferMaterial;
uniform sampler2D u_GBufferDepth;

uniform vec3  u_CameraPos;
uniform mat4  u_InvViewProjection;
uniform float u_AmbientIntensity;
uniform vec3  u_AmbientSkyColor;
uniform vec3  u_AmbientGroundColor;
// Atmosfera (vedi struct Atmosphere)
uniform int   u_AtmoOn;
uniform vec3  u_Zenith;
uniform vec3  u_Horizon;
uniform vec3  u_Ground;
uniform float u_CloudCover;
uniform vec3  u_SunDirL;        // verso il sole
uniform vec3  u_SunColor;
uniform float u_Time;
uniform float u_FogDensity;
uniform float u_FogFalloff;
uniform float u_FogBase;
uniform vec3  u_FogColor;
uniform float u_Dust;
uniform sampler2D u_SSAO;
uniform int   u_UseSSAO;

#define PI 3.14159265359

#define MAX_LIGHTS 64
struct Light {
    vec4 positionRange;
    vec4 directionCone;
    vec4 colorIntensity;
    ivec4 flags;
};
layout (std140) uniform LightBlock {
    Light u_Lights[MAX_LIGHTS];
};
uniform int u_LightCount;

// ============================================================
// SHADOW BLOCK — binding point 1
// ============================================================
layout (std140) uniform ShadowBlock {
    mat4 u_LightSpaceMatrices[4];
};

uniform sampler2D u_ShadowMap0;
uniform sampler2D u_ShadowMap1;
uniform sampler2D u_ShadowMap2;
uniform sampler2D u_ShadowMap3;
uniform float u_ShadowBias;
uniform float u_ShadowNormalBias;
uniform int   u_CascadeCount;
uniform float u_CascadeSplits[4];
uniform float u_CascadeFade;   // 0.0 = nessun blend, 0.15 = blend ultimo 15%
uniform int   u_UseShadows;    // 0 = ombre del sole spente (opzioni grafiche)
uniform int   u_ShadowPCF;     // raggio del filtro: 0 netto, 1 = 3x3, 2 = 5x5
uniform float u_SSSScale;      // intensita' della diffusione nella pelle
uniform float u_SSAOStrength;  // intensita' dell'occlusione ambientale
vec3 ReconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = u_InvViewProjection * ndc;
    return wp.xyz / wp.w;
}

// ============================================================
// PBR — Cook-Torrance GGX
// ============================================================
float DistributionGGX(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

float GeometrySchlickGGX(float NdotV, float k) {
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ============================================================
// CIELO E NEBBIA
// ============================================================
float AHash(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float ANoise(vec3 x) {
    vec3 i = floor(x), f = fract(x); f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(AHash(i), AHash(i + vec3(1, 0, 0)), f.x), mix(AHash(i + vec3(0, 1, 0)), AHash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(AHash(i + vec3(0, 0, 1)), AHash(i + vec3(1, 0, 1)), f.x), mix(AHash(i + vec3(0, 1, 1)), AHash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float AFbm(vec3 p) { float v = 0.0, a = 0.5; for (int i = 0; i < 4; ++i) { v += a * ANoise(p); p *= 2.03; a *= 0.5; } return v; }

// Cielo senza nuvole: gradiente orizzonte (smog) -> zenit, terra sotto, alone del sole
vec3 SkyBase(vec3 d) {
    float h = d.y;
    vec3 col = mix(u_Horizon, u_Zenith, pow(clamp(h, 0.0, 1.0), 0.45));
    col = mix(col, u_Ground, smoothstep(0.0, -0.15, h));
    float sd = max(dot(d, u_SunDirL), 0.0);
    col += u_SunColor * (pow(sd, 5.0) * 0.12 + pow(sd, 48.0) * 0.30);
    return col;
}
vec3 SkyColor(vec3 d, bool withSun) {
    vec3 col = SkyBase(d);
    float h = d.y;
    if (h > 0.0 && u_CloudCover > 0.0) {
        // strato di nuvole basse e sporche, mosse dal vento
        vec2 uv = d.xz / (h + 0.15) * 1.4;
        float c = AFbm(vec3(uv + vec2(u_Time * 0.006, u_Time * 0.002), u_Time * 0.003));
        float cov = smoothstep(1.0 - u_CloudCover, 1.0 - u_CloudCover + 0.32, c);
        float sd = max(dot(d, u_SunDirL), 0.0);
        vec3 cloud = mix(u_Zenith * 1.1, u_Horizon * 0.6, 0.45) * (0.55 + 0.7 * pow(sd, 3.0)) * (0.75 + 0.5 * c) + u_SunColor * pow(sd, 12.0) * 0.25 * (1.0 - cov);
        col = mix(col, cloud, cov * smoothstep(0.0, 0.18, h) * 0.9);
    }
    if (withSun) {
        float sd = max(dot(d, u_SunDirL), 0.0);
        col += u_SunColor * smoothstep(0.99955, 0.99985, sd) * 5.0;   // disco del sole, velato
    }
    return col;
}

vec3 ComputeAmbient(vec3 N, vec3 V, vec3 albedo, float roughness, float metallic) {
    vec3 irradiance = mix(u_AmbientGroundColor, u_AmbientSkyColor, N.y * 0.5 + 0.5);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 kS = FresnelSchlick(max(dot(N, V), 0.0), F0);
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    vec3 diffuseAmb = kD * albedo * irradiance;

    vec3 R = reflect(-V, N);
    vec3 envColor = mix(u_AmbientGroundColor, u_AmbientSkyColor, R.y * 0.5 + 0.5);
    if (u_AtmoOn == 1) envColor = mix(SkyBase(R) * 1.5, envColor, roughness);   // riflessi del cielo vero, sfocati dalla ruvidita'
    float fresnelBoost = pow(1.0 - max(dot(N, V), 0.0), 5.0);
    vec3 specAmb = mix(envColor, envColor * 2.0, fresnelBoost) * F0;

    return (diffuseAmb + specAmb) * u_AmbientIntensity;
}

// ============================================================
// SHADOW SAMPLING
// ============================================================
// Seleziona la texture di shadow corretta via branch esplicito.
// Non si possono copiare sampler in variabili locali in GLSL: va fatto
// direttamente dentro la texture() con l'if-chain.
float SampleShadowTex(int cascade, vec2 uv) {
    if (cascade == 0) return texture(u_ShadowMap0, uv).r;
    if (cascade == 1) return texture(u_ShadowMap1, uv).r;
    if (cascade == 2) return texture(u_ShadowMap2, uv).r;
    return texture(u_ShadowMap3, uv).r;
}

float SampleShadowCascade(int cascade, vec3 worldPos, vec3 N, float NdotL) {
    float normalOffsetScale = u_ShadowNormalBias * (1.0 - NdotL);
    vec3 offsetPos = worldPos + N * normalOffsetScale;

    vec4 lightSpace = u_LightSpaceMatrices[cascade] * vec4(offsetPos, 1.0);
    vec3 projCoords = lightSpace.xyz / lightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0) return 1.0;
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) return 1.0;

    float currentDepth = projCoords.z;

    // Tutte le 4 cascade hanno la stessa risoluzione (2048x2048), quindi
    // il texelSize di una vale per tutte. Uso la texture #0 come riferimento.
    vec2 texelSize = 1.0 / vec2(textureSize(u_ShadowMap0, 0));

    float bias = max(u_ShadowBias * (1.0 - NdotL), u_ShadowBias * 0.1);

    float shadow = 0.0;
    int r = clamp(u_ShadowPCF, 0, 2);
    for (int x = -r; x <= r; ++x) {
        for (int y = -r; y <= r; ++y) {
            float pcfDepth = SampleShadowTex(cascade, projCoords.xy + vec2(x, y) * texelSize);
            shadow += (currentDepth - bias > pcfDepth) ? 0.0 : 1.0;
        }
    }
    float side = float(2 * r + 1);
    return shadow / (side * side);
}
float GetShadow(vec3 worldPos, vec3 N, float NdotL, float viewDistance) {
    // Trova la cascade primaria
    int cascade = u_CascadeCount - 1;
    for (int i = 0; i < u_CascadeCount; ++i) {
        if (viewDistance <= u_CascadeSplits[i]) {
            cascade = i;
            break;
        }
    }

    float shadowPrimary = SampleShadowCascade(cascade, worldPos, N, NdotL);

    // Blend con la cascade successiva se siamo nell'ultima frazione (u_CascadeFade)
    // dell'intervallo corrente. Elimina lo "scalino" visibile ai bordi.
    if (cascade < u_CascadeCount - 1) {
        float cascadeEnd = u_CascadeSplits[cascade];
        float fadeStart = cascadeEnd * (1.0 - u_CascadeFade);

        if (viewDistance > fadeStart) {
            float t = (viewDistance - fadeStart) / max(cascadeEnd - fadeStart, 0.001);
            t = clamp(t, 0.0, 1.0);

            float shadowNext = SampleShadowCascade(cascade + 1, worldPos, N, NdotL);
            return mix(shadowPrimary, shadowNext, t);
        }
    }

    return shadowPrimary;
}

// ============================================================
// MAIN
// ============================================================
void main() {
    vec4  albedoSample   = texture(u_GBufferAlbedo,   v_TexCoord);
    vec4  normalSample   = texture(u_GBufferNormal,   v_TexCoord);
    vec4  materialSample = texture(u_GBufferMaterial, v_TexCoord);
    float depth          = texture(u_GBufferDepth,    v_TexCoord).r;

    if (depth >= 0.9999) {
        if (u_AtmoOn == 1) {
            vec3 dir = normalize(ReconstructWorldPos(v_TexCoord, 0.9999) - u_CameraPos);
            out_Color = vec4(SkyColor(dir, true), 1.0);
        }
        else out_Color = vec4(0.01, 0.01, 0.015, 1.0);
        return;
    }

    vec3  albedo    = albedoSample.rgb;
    float ao        = albedoSample.a;
    vec3  N         = normalize(normalSample.rgb);
    float roughness = clamp(materialSample.r, 0.04, 1.0);
    float metallic  = clamp(materialSample.g, 0.0, 1.0);
    float emissive  = materialSample.b;
    float sss       = materialSample.a * u_SSSScale;   // pelle (0 = superficie normale)

    vec3 worldPos = ReconstructWorldPos(v_TexCoord, depth);
    vec3 V = normalize(u_CameraPos - worldPos);
    float NdotV = max(dot(N, V), 1e-4);
    float viewDist = length(worldPos - u_CameraPos);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    if (sss > 0.0) F0 = vec3(0.028);   // pelle: riflesso speculare dell'acqua/sebo, non della plastica
    // Colore della luce diffusa sotto la pelle: rossastra negli umani (sangue), calda e verde negli orki
    vec3 sssTint = clamp(albedo * vec3(2.4, 1.25, 1.05) + vec3(0.12, 0.0, 0.0), 0.0, 1.0);

    vec3 Lo = vec3(0.0);

    for (int i = 0; i < u_LightCount; ++i) {
        int type = u_Lights[i].flags.x;

        vec3  L;
        float attenuation = 1.0;
        float spotFactor  = 1.0;

        if (type == 0) {
            L = normalize(-u_Lights[i].directionCone.xyz);
        } else {
            vec3 toLight = u_Lights[i].positionRange.xyz - worldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 1e-4);

            float range = max(u_Lights[i].positionRange.w, 0.001);
            float ratio = dist / range;
            attenuation = clamp(1.0 - ratio * ratio, 0.0, 1.0);
            attenuation *= attenuation;

            if (type == 2) {
                float cosA = dot(-L, normalize(u_Lights[i].directionCone.xyz));
                float outer = u_Lights[i].directionCone.w;
                spotFactor = clamp((cosA - outer) / max(1.0 - outer, 0.001), 0.0, 1.0);
            }
        }

        float NdotLraw = dot(N, L);
        float NdotL = max(NdotLraw, 0.0);
        // Pelle: la luce "gira" oltre il terminatore (diffusione avvolgente) e il passaggio luce/ombra e' morbido e rosso
        float wrap = 0.45 * sss;
        float NdotLw = clamp((NdotLraw + wrap) / (1.0 + wrap), 0.0, 1.0);
        if (NdotLw <= 0.0) continue;

        vec3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        float HdotV = max(dot(H, V), 0.0);

        // Cook-Torrance
        float NDF = DistributionGGX(NdotH, roughness * roughness);
        float G   = GeometrySmith(NdotV, NdotL, roughness);
        vec3  F   = FresnelSchlick(HdotV, F0);

        vec3 numerator    = NDF * G * F;
        float denominator = 4.0 * NdotV * NdotL + 1e-6;
        vec3 specular = NdotL > 0.0 ? numerator / denominator : vec3(0.0);

        vec3 kS = F;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

        // Shadow SOLO per la direzionale (type == 0)
        float shadowFactor = 1.0;
        if (type == 0 && u_UseShadows == 1) {
            shadowFactor = GetShadow(worldPos, N, max(NdotL, 0.05), viewDist);
        }

        vec3 radiance = u_Lights[i].colorIntensity.rgb * u_Lights[i].colorIntensity.w;
        vec3 lightAmt = radiance * attenuation * spotFactor;
        if (sss > 0.0) {
            // diffusa: la parte "in piu'" oltre il terminatore prende il colore del sangue sotto la pelle
            vec3 diff = vec3(NdotL) + (NdotLw - NdotL) * sssTint;
            // ombra colorata: nella penombra il rosso passa piu' del verde e del blu
            vec3 sh = mix(vec3(shadowFactor), pow(vec3(shadowFactor), vec3(0.55, 1.15, 1.4)), sss);
            // luce che attraversa le parti sottili (orecchie, dita, bordi) quando il sole e' dietro
            float trans = pow(clamp(dot(V, -L), 0.0, 1.0), 6.0) * (1.0 - NdotV) * 0.35 * sss;
            Lo += (kD * albedo / PI * (diff + trans * sssTint) * sh + specular * NdotL * shadowFactor) * lightAmt;
        }
        else {
            Lo += (kD * albedo / PI + specular) * lightAmt * NdotL * shadowFactor;
        }
    }

    float ssao = u_UseSSAO == 1 ? pow(texture(u_SSAO, v_TexCoord).r, u_SSAOStrength) : 1.0;
    vec3 ambient = ComputeAmbient(N, V, albedo, roughness, metallic) * ao * ssao;
    Lo *= mix(1.0, ssao, 0.25);   // un filo anche sulla luce diretta: gli oggetti "toccano" il terreno
    if (sss > 0.0) {
        // peluria e sebo: un leggero alone sul bordo controluce, e un'ambiente un po' piu' caldo
        float rim = pow(1.0 - NdotV, 4.0) * 0.18 * sss;
        ambient += (mix(u_AmbientSkyColor, vec3(1.0), 0.5) * rim + ambient * sssTint * 0.25 * sss) * u_AmbientIntensity;
    }

    vec3 color = ambient + Lo + albedo * emissive;

    if (u_AtmoOn == 1) {
        // Nebbia in altezza (piu' fitta in basso) con la luce del sole che la attraversa, e polvere che si muove nel vento
        vec3 rd = (worldPos - u_CameraPos) / max(viewDist, 1e-4);
        float k = max(u_FogFalloff, 1e-4);
        float dy = worldPos.y - u_CameraPos.y;
        float heightTerm = exp(-k * (u_CameraPos.y - u_FogBase)) * (abs(k * dy) > 1e-3 ? (1.0 - exp(-k * dy)) / (k * dy) : 1.0);
        float dust = 0.0;
        for (int i = 1; i <= 3; ++i) {
            vec3 p = u_CameraPos + rd * min(viewDist, 60.0) * (float(i) / 4.0);
            dust += ANoise(p * 0.3 + vec3(u_Time * 0.45, -u_Time * 0.05, u_Time * 0.16));
        }
        dust /= 3.0;
        float density = u_FogDensity * max(0.0, 1.0 + u_Dust * (dust - 0.5) * 2.4);
        float fog = 1.0 - exp(-density * viewDist * max(heightTerm, 0.0));
        float sd = max(dot(rd, u_SunDirL), 0.0);
        vec3 fogCol = mix(u_FogColor, SkyBase(rd), 0.5) + u_SunColor * (pow(sd, 8.0) * 0.4 + 0.04);
        color = mix(color, fogCol, clamp(fog, 0.0, 0.94));
    }

    out_Color = vec4(color, 1.0);
}
)";

    static const char* k_TonemapFragSrc = R"(
        #version 410 core
in vec2 v_TexCoord;
out vec4 out_Color;

uniform sampler2D u_HDRTexture;
uniform sampler2D u_ExposureTex;   // 1x1 R32F dall'AutoExposure
uniform float u_ExposureFallback;  // usato se auto-exposure disabilitata
uniform float u_Brightness;        // opzioni grafiche: moltiplica l'esposizione
uniform float u_Gamma;
uniform int   u_Grade;             // 1 = correzione colore grimdark
uniform float u_Saturation;
uniform float u_Contrast;
uniform float u_Vignette;
uniform float u_Grain;
uniform vec3  u_ShadowTint;
uniform vec3  u_HighlightTint;
uniform float u_Time;

vec3 ACESFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    float exposure = texture(u_ExposureTex, vec2(0.5)).r;
    if (exposure < 0.001) exposure = u_ExposureFallback;

    vec3 hdr = texture(u_HDRTexture, v_TexCoord).rgb * exposure * u_Brightness;
    vec3 ldr = ACESFilm(hdr);
    if (u_Grade == 1) {
        float l = dot(ldr, vec3(0.2126, 0.7152, 0.0722));
        // ombre fredde, luci calde
        ldr *= mix(u_ShadowTint, u_HighlightTint, smoothstep(0.05, 0.7, l));
        // meno saturazione, un po' piu' di contrasto (curva a S attorno al grigio medio)
        l = dot(ldr, vec3(0.2126, 0.7152, 0.0722));
        ldr = mix(vec3(l), ldr, u_Saturation);
        ldr = clamp((ldr - 0.18) * u_Contrast + 0.18, 0.0, 1.0);
        // vignetta
        vec2 c = v_TexCoord - 0.5;
        ldr *= 1.0 - u_Vignette * smoothstep(0.25, 0.85, dot(c, c) * 2.2);
    }
    ldr = pow(ldr, vec3(1.0 / max(u_Gamma, 1.0)));
    if (u_Grade == 1) {
        // grana della pellicola (cambia ogni fotogramma)
        float g = fract(sin(dot(v_TexCoord * 1000.0 + fract(u_Time) * 37.0, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
        ldr += g * u_Grain;
    }
    out_Color = vec4(ldr, 1.0);
}
    )";

    // Mimetismo: la superficie mostra la scena dietro, spostata lungo la normale (rifrazione), con un'increspatura
    // che scorre e un bordo di Fresnel azzurrino. Test di profondita' a mano col depth del G-Buffer.
    static const char* k_CloakFragSrc = R"(
#version 410 core
in vec3 v_WorldPos;
in vec3 v_Normal;
out vec4 out_Color;
uniform sampler2D u_Scene;
uniform sampler2D u_Depth;
uniform vec2  u_Viewport;
uniform mat4  u_View;
uniform vec3  u_CameraPos;
uniform float u_Amount;
uniform float u_Time;
void main() {
    vec2 uv = gl_FragCoord.xy / u_Viewport;
    if (gl_FragCoord.z > texture(u_Depth, uv).r + 0.00002) discard;   // dietro a qualcosa di solido
    vec3 n = normalize(v_Normal);
    vec3 vn = normalize(mat3(u_View) * n);
    float ripple = 0.5 * sin(v_WorldPos.y * 18.0 + u_Time * 4.0) + 0.5 * sin((v_WorldPos.x + v_WorldPos.z) * 14.0 - u_Time * 3.0);
    vec2 off = vn.xy * (0.022 + 0.008 * ripple) * u_Amount;
    vec3 refr = texture(u_Scene, clamp(uv + off, vec2(0.001), vec2(0.999))).rgb;
    vec3 V = normalize(u_CameraPos - v_WorldPos);
    float fres = pow(1.0 - max(dot(n, V), 0.0), 3.0);
    vec3 col = refr * mix(vec3(1.0), vec3(0.9, 0.97, 1.05), u_Amount) + vec3(0.55, 0.72, 0.9) * fres * 0.3 * u_Amount;
    out_Color = vec4(col, 1.0);
}
)";

    // ============================================================
    // Impl
    // ============================================================
    static const char* k_GBufferSkinnedVertSrc = R"(
#version 410 core
layout (location = 0) in vec3 a_Position;
layout (location = 1) in vec3 a_Normal;
layout (location = 2) in vec2 a_TexCoord;
layout (location = 3) in ivec4 a_BoneIDs;
layout (location = 4) in vec4 a_Weights;
layout (location = 5) in vec4 a_Tangent;   // modelli d'autore: tangente per le normal map

uniform mat4 u_Model;
uniform mat4 u_ViewProjection;
uniform mat4 u_BoneMatrices[128];

out vec4 v_Tangent;
out vec3 v_WorldPos;
out vec3 v_Normal;
out vec3 v_BindPos;      // posizione e normale in posa T: la texture dei vestiti resta "incollata" al corpo
out vec3 v_BindNormal;
out vec2 v_UV;           // vestiti: x = distanza dal bordo (frazione dell'altezza del corpo)
out vec2 v_Data;         // canali negli slot liberi delle ossa (id < 0): parti scolpite

void main() {
    v_Data = vec2(a_BoneIDs.z < 0 ? a_Weights.z : 0.0, a_BoneIDs.w < 0 ? a_Weights.w : 0.0);
    v_BindPos = a_Position;
    v_BindNormal = a_Normal;
    v_UV = a_TexCoord;
    vec4 skinnedPos = vec4(0.0);
    vec3 skinnedNormal = vec3(0.0);
    vec3 skinnedTangent = vec3(0.0);
    float totalWeight = 0.0;

    for (int i = 0; i < 4; ++i) {
        int bID = a_BoneIDs[i];
        if (bID < 0 || bID >= 128) continue;
        float w = a_Weights[i];
        if (w <= 0.0001) continue;

        mat4 boneMat = u_BoneMatrices[bID];
        skinnedPos += (boneMat * vec4(a_Position, 1.0)) * w;
        skinnedNormal += (mat3(boneMat) * a_Normal) * w;
        skinnedTangent += (mat3(boneMat) * a_Tangent.xyz) * w;
        totalWeight += w;
    }

    // Fallback: vertice senza pesi validi
    if (totalWeight < 0.001) {
        skinnedPos = vec4(a_Position, 1.0);
        skinnedNormal = a_Normal;
        skinnedTangent = a_Tangent.xyz;
    }

    vec4 worldPos = u_Model * skinnedPos;
    v_WorldPos = worldPos.xyz;

    mat3 normalMatrix = transpose(inverse(mat3(u_Model)));
    v_Normal = normalize(normalMatrix * skinnedNormal);
    v_Tangent = vec4(mat3(u_Model) * skinnedTangent, a_Tangent.w);

    gl_Position = u_ViewProjection * worldPos;
}
)";

    static const char* k_GBufferSkinnedFragSrc = R"(
#version 410 core
in vec3 v_WorldPos;
in vec3 v_Normal;
in vec3 v_BindPos;
in vec3 v_BindNormal;
in vec2 v_UV;
in vec2 v_Data;
in vec4 v_Tangent;

layout (location = 0) out vec4 out_Albedo;
layout (location = 1) out vec4 out_Normal;
layout (location = 2) out vec4 out_Material;

uniform int   u_Baked;        // 1: v_UV = curvatura/occlusione (corpi preparati e volti)
uniform vec4  u_SkinStyle;    // pelle: x chiazze, y verruche, z definizione muscolare, w vene
uniform vec4  u_Color2;       // seconda tinta (macchie)
uniform vec4  u_Color;
uniform float u_Roughness;
uniform float u_Metallic;
uniform float u_Emissive;
uniform int   u_UseTexture;   // 1 = strato di vestito con texture
uniform sampler2D u_Albedo;
uniform float u_TexScale;     // ripetizioni per unita' del modello
uniform float u_AlphaCut;     // alpha sotto soglia = buco (stracci)
// Modelli d'autore (AuthoredModel.h): texture PBR sulle coordinate UV del modello, al posto dei dettagli procedurali
uniform int   u_Authored;
uniform sampler2D u_TexAlbedo;     // unita' 1, sRGB
uniform sampler2D u_TexNormal;     // unita' 2
uniform sampler2D u_TexORM;        // unita' 3: R occlusione, G rugosita', B metallo
uniform sampler2D u_TexEmissive;   // unita' 4
uniform int   u_HasAlbedo;
uniform int   u_HasNormal;
uniform int   u_HasORM;
uniform int   u_HasEmissive;
uniform vec4  u_BaseColor;
uniform vec3  u_EmissiveColor;
uniform float u_EmissiveStrength;
uniform float u_NormalStrength;
uniform float u_NormalY;           // -1 = normal map DirectX
uniform int   u_MatKind;           // 1 pelle, 2 stoffa, 3 metallo, 4 occhio, 5 capelli
uniform int   u_Tint;              // 1 = moltiplica per il colore dell'unita'
uniform float u_MatGrime;          // sporco del motore sopra le texture
uniform int   u_Detail;       // 0 niente, 1 pelle, 2 stoffa, 3 armatura/metallo, 5 occhio, 6 dente
uniform float u_InvHeight;    // 1 / altezza del corpo in posa T (scala dei dettagli)
uniform float u_Grime;        // opzioni grafiche: sporco su vestiti e armature
uniform float u_Desat;        // opzioni grafiche: tinte spente grimdark (0 = colori originali)

// Proiezione triplanare in posa T: niente UV, la texture segue il corpo in ogni animazione
vec4 SampleTriplanar() {
    vec3 n = abs(normalize(v_BindNormal));
    n = pow(n, vec3(4.0));
    n /= max(n.x + n.y + n.z, 1e-4);
    vec3 p = v_BindPos * u_TexScale;
    return texture(u_Albedo, p.zy) * n.x + texture(u_Albedo, p.xz) * n.y + texture(u_Albedo, p.xy) * n.z;
}

float Hash3(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float Noise3(vec3 x) {
    vec3 i = floor(x), f = fract(x); f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(Hash3(i), Hash3(i + vec3(1, 0, 0)), f.x), mix(Hash3(i + vec3(0, 1, 0)), Hash3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(Hash3(i + vec3(0, 0, 1)), Hash3(i + vec3(1, 0, 1)), f.x), mix(Hash3(i + vec3(0, 1, 1)), Hash3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

// Rilievo da un'altezza (in frazioni dell'altezza del corpo): normale perturbata con le derivate a schermo
vec3 Bump(vec3 N, float h, float strength) {
    vec3 dpdx = dFdx(v_WorldPos), dpdy = dFdy(v_WorldPos);
    float bindScale = length(dFdx(v_BindPos)) + length(dFdy(v_BindPos));
    float worldScale = length(dpdx) + length(dpdy);
    float k = (bindScale > 1e-6) ? worldScale / bindScale / max(u_InvHeight, 1e-6) : 1.0;   // altezza -> metri
    float hw = h * k * strength;
    vec3 r1 = cross(dpdy, N), r2 = cross(N, dpdx);
    float det = dot(dpdx, r1);
    vec3 grad = sign(det) * (dFdx(hw) * r1 + dFdy(hw) * r2);
    return normalize(abs(det) * N - grad);
}

void main() {
    vec4 color = u_Color;
    float rough = u_Roughness, metal = u_Metallic;
    vec3 N = normalize(v_Normal);
    if (u_UseTexture == 1) {
        vec4 t = SampleTriplanar();
        if (t.a < u_AlphaCut) discard;
        color.rgb *= t.rgb;
    }
    float aoTex = 1.0;
    if (u_Authored == 1) {
        vec4 base = u_BaseColor;
        if (u_HasAlbedo == 1) base *= texture(u_TexAlbedo, v_UV);
        if (u_AlphaCut > 0.0 && base.a < u_AlphaCut) discard;
        color = vec4(base.rgb * (u_Tint == 1 ? u_Color.rgb : vec3(1.0)), 1.0);
        if (u_HasORM == 1) {
            vec3 o = texture(u_TexORM, v_UV).rgb;
            aoTex = o.r; rough *= o.g; metal *= o.b;
        }
        if (u_HasNormal == 1) {
            vec3 tn = texture(u_TexNormal, v_UV).xyz * 2.0 - 1.0;
            tn.y *= u_NormalY;
            tn.xy *= u_NormalStrength;
            vec3 T = v_Tangent.xyz - N * dot(N, v_Tangent.xyz);
            if (dot(T, T) > 1e-10) {
                T = normalize(T);
                vec3 B = cross(N, T) * (v_Tangent.w < 0.0 ? -1.0 : 1.0);
                N = normalize(T * tn.x + B * tn.y + N * max(tn.z, 0.001));
            }
        }
        rough = clamp(rough, 0.04, 1.0);
        metal = clamp(metal, 0.0, 1.0);
    }
    vec3 pn = v_BindPos * u_InvHeight;   // posizione in posa T, normalizzata sull'altezza del corpo
    float emissive = u_Emissive;
    float subsurface = 0.0;   // pelle: la luce entra e si diffonde (canale w del materiale, vedi il passaggio di illuminazione)
    if (u_Authored == 1) {
        subsurface = u_MatKind == 1 ? 1.0 : 0.0;
        // parti che si illuminano: il colore va verso quello emesso, l'intensita' fa il bagliore
        vec3 E = u_EmissiveColor;
        if (u_HasEmissive == 1) E = texture(u_TexEmissive, v_UV).rgb * (dot(u_EmissiveColor, vec3(1.0)) > 0.003 ? u_EmissiveColor : vec3(1.0));
        float el = dot(E, vec3(0.299, 0.587, 0.114)) * u_EmissiveStrength;
        if (el > 0.002) {
            color.rgb = mix(color.rgb, E / max(max(E.r, max(E.g, E.b)), 1e-4), clamp(el * 2.0, 0.0, 1.0));
            emissive = max(emissive, el * 4.0);
        }
    }
    if (u_Detail == 1) {
        subsurface = 1.0;
        // grimdark: anche la pelle e' spenta (il verde degli orki tende all'oliva) e i piedi nudi sono infangati
        {
            float lum = dot(color.rgb, vec3(0.299, 0.587, 0.114));
            color.rgb = mix(color.rgb, mix(vec3(lum), color.rgb, 0.78) * 0.94, u_Desat);
            float feet = 1.0 - smoothstep(0.02, 0.11, pn.y + (Noise3(pn * 9.0) - 0.5) * 0.04);
            float mudS = feet * smoothstep(0.35, 0.6, Noise3(pn * 40.0 + 3.0) * 0.6 + feet * 0.5);
            color.rgb = mix(color.rgb, vec3(0.12, 0.095, 0.07), clamp(mudS * 0.8 * u_Grime, 0.0, 1.0));
        }
        // Pelle: macchie e venature leggere, pori (rilievo fine), un po' di rossore variabile
        float big = Noise3(pn * 16.0), mid = Noise3(pn * 55.0), fine = Noise3(pn * 380.0);
        color.rgb *= 0.9 + 0.14 * big + 0.05 * mid;
        color.rgb = mix(color.rgb, color.rgb * vec3(1.1, 0.93, 0.9), smoothstep(0.5, 0.85, big) * 0.45);
        // stile della razza: chiazze (pelle d'orko), verruche e bitorzoli
        float mott = Noise3(pn * 7.0 + 3.0) * 0.6 + Noise3(pn * 23.0) * 0.4;
        color.rgb *= mix(1.0, 0.7 + 0.55 * mott, u_SkinStyle.x);
        float warts = smoothstep(0.66, 0.82, Noise3(pn * 150.0)) * u_SkinStyle.y;
        color.rgb *= 1.0 - warts * 0.08;
        float h = fine * 0.00012 + mid * 0.0004 + warts * 0.0011;
        rough = clamp(rough - 0.1 + 0.15 * mid, 0.25, 1.0);
        if (u_Baked == 1) {
            // Muscoli: solchi piu' scuri e rossastri, ventri piu' chiari e lucidi (curvatura e occlusione nel vertice)
            float cav = clamp((v_UV.x - 0.5) * 2.0, -1.0, 1.0);
            float ao = clamp(v_UV.y, 0.0, 1.0);
            float def = u_SkinStyle.z;
            color.rgb *= mix(1.0, ao, 0.85);
            color.rgb *= 1.0 + cav * 0.22 * def;
            color.rgb = mix(color.rgb, color.rgb * vec3(0.95, 0.72, 0.68), clamp(-cav, 0.0, 1.0) * 0.45 * def);
            rough = clamp(rough - cav * 0.12 * def, 0.2, 1.0);
            // vene in rilievo sui muscoli in tensione (braccia in posa T lungo X, il resto in verticale)
            vec3 vf = abs(pn.x) > 0.17 ? vec3(22.0, 85.0, 85.0) : vec3(85.0, 22.0, 85.0);
            float vn = abs(Noise3(pn * vf) - 0.5);
            float vein = (1.0 - smoothstep(0.0, 0.035, vn)) * u_SkinStyle.w * smoothstep(0.5, 0.68, v_UV.x);
            h += vein * 0.0009;
            color.rgb *= 1.0 - vein * 0.1;
            // Parti scolpite: rossore (oltre 1.2 interno della bocca) e macchie (da 2 in su, tinta u_Color2)
            float red = v_Data.y;
            if (red > 1.2) { color.rgb = vec3(0.32, 0.07, 0.06) * (0.35 + 0.65 * ao); rough = 0.35; }
            else color.rgb = mix(color.rgb, color.rgb * vec3(1.06, 0.74, 0.72), clamp(red, 0.0, 1.0) * 0.5);
            float sd = v_Data.x;
            if (sd >= 1.9) {
                float paint = clamp(sd - 2.0, 0.0, 1.0) * (0.85 + 0.15 * Noise3(pn * 300.0));
                color.rgb = mix(color.rgb, u_Color2.rgb * (0.7 + 0.3 * ao), paint);
                rough = mix(rough, 0.55, paint);
            }
            else if (sd > 0.01) {
                float dots = smoothstep(0.35, 0.75, Noise3(pn * 2600.0));
                color.rgb *= 1.0 - clamp(sd, 0.0, 1.0) * (0.25 + 0.35 * dots);
            }
        }
        N = Bump(N, h, 1.0);
    }
    else if (u_Detail == 5) {
        // Occhio: sclera, iride (u_Color) con fibre radiali e bordo scuro, pupilla; bagnato
        vec2 uv = v_Data;
        float r = length(uv);
        float ao = clamp(v_UV.y, 0.0, 1.0);
        vec3 sclera = vec3(0.86, 0.83, 0.78);
        sclera = mix(sclera, vec3(0.8, 0.45, 0.4), smoothstep(1.2, 2.2, r) * 0.35);
        if (r < 1.0) {
            float ang = atan(uv.y, uv.x);
            float fib = Noise3(vec3(cos(ang) * 6.0, sin(ang) * 6.0, r * 5.0));
            vec3 iris = u_Color.rgb * (0.45 + 0.55 * fib) * mix(1.0, 0.35, smoothstep(0.78, 1.0, r));
            iris = mix(iris, u_Color.rgb * 1.25, (1.0 - smoothstep(0.3, 0.55, r)) * 0.35);
            color.rgb = r < 0.36 ? vec3(0.02) : iris;
        }
        else {
            color.rgb = sclera;
            emissive = 0.0;
        }
        color.rgb *= mix(0.25, 1.0, ao);
        rough = 0.08; metal = 0.0;
    }
    else if (u_Detail == 6) {
        // Dente / zanna: avorio sporco alla radice
        float n = Noise3(pn * 120.0);
        float ao = clamp(v_UV.y, 0.0, 1.0);
        color.rgb *= (0.85 + 0.2 * n) * mix(0.55, 1.0, ao);
        color.rgb = mix(color.rgb, color.rgb * vec3(0.75, 0.62, 0.42), (1.0 - ao) * 0.6);
        rough = 0.35 + 0.2 * n;
    }
    else if (u_Detail == 2 || u_Detail == 3) {
        float edge = v_UV.x;                       // distanza dal bordo del vestito
        float n1 = Noise3(pn * 9.0), n2 = Noise3(pn * 40.0), n3 = Noise3(pn * 220.0);
        if (u_Detail == 2) {
            // Stoffa: pieghe attorno a braccia e gambe (in posa T le braccia stanno lungo X), trama fine, orli
            bool arm = abs(pn.x) > 0.17;
            float c = (arm ? pn.x : pn.y) * 46.0 + n1 * 5.0;
            float folds = (sin(c) * 0.5 + 0.5) * smoothstep(0.25, 0.8, Noise3(pn * 6.0 + 3.1));
            float weave = Noise3(pn * vec3(900.0, 900.0, 900.0));
            N = Bump(N, folds * 0.0045 + weave * 0.00035 + n2 * 0.001, 1.0);
            color.rgb *= 0.86 + 0.18 * n1 - 0.08 * folds;                     // pieghe in ombra, stoffa non uniforme
            float hem = smoothstep(0.0, 0.006, edge);
            color.rgb *= mix(0.68, 1.0, hem);                                 // orlo ripiegato piu' scuro
            float stitch = (1.0 - smoothstep(0.0, 0.0012, abs(edge - 0.0085))) * step(0.5, fract((pn.x + pn.y + pn.z) * 260.0));
            color.rgb *= 1.0 - 0.35 * stitch;                                 // cucitura
            rough = clamp(rough + 0.05, 0.0, 1.0);
        }
        else {
            // Armatura: ammaccature, sporco negli incavi, vernice consumata sugli spigoli (metallo nudo che luccica)
            float wear = (1.0 - smoothstep(0.0, 0.012, edge)) * smoothstep(0.35, 0.75, n2 + 0.3);
            float scratches = smoothstep(0.9, 0.98, Noise3(pn * vec3(500.0, 30.0, 500.0))) * smoothstep(0.4, 0.7, n1);
            N = Bump(N, n1 * 0.002 + n2 * 0.0006 - (1.0 - smoothstep(0.0, 0.004, edge)) * 0.0015, 1.0);
            color.rgb *= 0.82 + 0.25 * n1;                                    // patina non uniforme
            color.rgb *= mix(0.75, 1.0, smoothstep(0.2, 0.6, n1));            // sporco
            vec3 bare = vec3(0.62, 0.6, 0.58);
            color.rgb = mix(color.rgb, bare, max(wear * 0.7, scratches * 0.35));
            metal = mix(metal, 0.9, max(wear, scratches * 0.5));
            rough = clamp(mix(rough, 0.3, max(wear, scratches * 0.5)) + 0.1 * (n2 - 0.5), 0.15, 1.0);
        }
    }
    if (u_Detail == 2 || u_Detail == 3 || (u_Authored == 1 && u_MatGrime > 0.0)) {
        // Grimdark: niente colori accesi. Tinte spente, fango che sale dagli stivali, polvere sulle spalle,
        // macchie (sangue rappreso sulla stoffa, bruciature sull'armatura), ruggine nei graffi, colature d'olio.
        float lum = dot(color.rgb, vec3(0.299, 0.587, 0.114));
        color.rgb = mix(color.rgb, mix(vec3(lum), color.rgb, 0.62) * 0.9, u_Desat);
        float gr = u_Grime * (u_Authored == 1 ? u_MatGrime : 1.0);
        bool cloth = u_Detail == 2 || (u_Authored == 1 && u_MatKind != 3);   // modelli d'autore: tutto cio' che non e' metallo come stoffa
        float g1 = Noise3(pn * 7.0 + 13.0), g2 = Noise3(pn * 31.0 + 5.0), g3 = Noise3(pn * 140.0);
        // fango dal basso: pieno sugli stivali, a schizzi fino al ginocchio
        float mudH = 1.0 - smoothstep(0.03, 0.30, pn.y + (g1 - 0.5) * 0.08);
        float mud = clamp(mudH * smoothstep(0.35, 0.6, g2 * 0.7 + g3 * 0.3 + mudH * 0.45) * gr, 0.0, 1.0);
        vec3 mudCol = mix(vec3(0.13, 0.10, 0.075), vec3(0.07, 0.055, 0.045), g1);
        color.rgb = mix(color.rgb, mudCol, mud * 0.85);
        rough = mix(rough, 0.7, mud * 0.6);
        metal = mix(metal, 0.0, mud);
        // polvere sulle superfici rivolte in alto (spalle, elmetto, braccia)
        float up = smoothstep(0.35, 0.9, normalize(v_BindNormal).y);
        color.rgb = mix(color.rgb, vec3(0.30, 0.27, 0.22), clamp(up * (0.18 + 0.2 * g1) * (1.0 - mud) * gr, 0.0, 1.0));
        if (cloth) {
            // stoffa: sporco unto e qualche macchia scura di sangue rappreso
            color.rgb *= mix(1.0, 0.9 + 0.12 * g1, min(gr, 1.0));
            float blood = smoothstep(0.80, 0.86, Noise3(pn * 11.0 + 41.0)) * smoothstep(0.3, 0.7, g2) * gr;
            color.rgb = mix(color.rgb, color.rgb * vec3(0.38, 0.17, 0.14), blood * 0.8);
        }
        else {
            // armatura: ruggine nei graffi e sugli spigoli consumati, bruciature, colature d'olio verticali
            float rust = clamp(smoothstep(0.55, 0.8, g2 + (1.0 - smoothstep(0.0, 0.01, v_UV.x)) * 0.3) * smoothstep(0.4, 0.7, g3) * gr, 0.0, 1.0);
            color.rgb = mix(color.rgb, mix(vec3(0.24, 0.12, 0.06), vec3(0.12, 0.065, 0.04), g1), rust * 0.55);
            metal = mix(metal, 0.1, rust);
            rough = mix(rough, 0.92, rust);
            float scorch = clamp(smoothstep(0.74, 0.82, Noise3(pn * 6.0 + 23.0)) * gr, 0.0, 1.0);
            color.rgb = mix(color.rgb, color.rgb * 0.25, scorch * 0.7);
            float oil = clamp(smoothstep(0.62, 0.8, Noise3(pn * vec3(70.0, 3.5, 70.0) + 3.0)) * (1.0 - up) * gr, 0.0, 1.0);
            color.rgb *= 1.0 - oil * 0.35;
            rough = mix(rough, 0.25, oil * 0.5);
        }
    }
    out_Albedo   = vec4(color.rgb, color.a * aoTex);   // alfa del G-Buffer = occlusione ambientale
    out_Normal   = vec4(N, 1.0);
    out_Material = vec4(rough, metal, emissive, subsurface);
}
)";
    bool DeferredRenderer::Init(int width, int height) {
        m_Width = width;
        m_Height = height;

        m_GBuffer.Create(width, height);

        // HDR target con un solo attachment RGBA16F
        m_HDRTarget.Create(width, height, { TextureFormat::RGBA16F },
            /*hasDepth*/ false, TextureFilter::Linear);
        // Occlusione ambientale (grezza e sfocata) e immagine finale prima dell'antialiasing
        m_SSAOTarget.Create(width, height, { TextureFormat::R8 }, false, TextureFilter::Linear);
        m_SSAOBlurTarget.Create(width, height, { TextureFormat::R8 }, false, TextureFilter::Linear);
        m_LDRTarget.Create(width, height, { TextureFormat::RGBA8 }, false, TextureFilter::Linear);

        // Shaders
        m_GBufferShader = std::make_unique<Shader>();
        if (!m_GBufferShader->LoadFromSource(k_GBufferVertSrc, k_GBufferFragSrc, "GBuffer")) {
            SUN_CORE_ERROR("DeferredRenderer: gbuffer shader fallito");
            return false;
        }
        m_GBufferSkinnedShader = std::make_unique<Shader>();
        if (!m_GBufferSkinnedShader->LoadFromSource(
            k_GBufferSkinnedVertSrc, k_GBufferSkinnedFragSrc, "GBufferSkinned")) {
            SUN_CORE_ERROR("DeferredRenderer: gbuffer skinned shader fallito");
            return false;
        }
        // Mimetismo (non bloccante: senza, le unita' mimetizzate restano solo invisibili)
        m_CloakShader = std::make_unique<Shader>();
        if (!m_CloakShader->LoadFromSource(k_GBufferVertSrc, k_CloakFragSrc, "Cloak")) SUN_CORE_WARN("DeferredRenderer: shader mimetismo fallito");
        m_CloakSkinnedShader = std::make_unique<Shader>();
        if (!m_CloakSkinnedShader->LoadFromSource(k_GBufferSkinnedVertSrc, k_CloakFragSrc, "CloakSkinned")) SUN_CORE_WARN("DeferredRenderer: shader mimetismo skinnato fallito");
        m_CloakCopy.Create(width, height, { TextureFormat::RGBA16F }, /*hasDepth*/ false, TextureFilter::Linear);
        m_LightingShader = std::make_unique<Shader>();
        if (!m_LightingShader->LoadFromSource(k_LightingVertSrc, k_LightingFragSrc, "Lighting")) {
            SUN_CORE_ERROR("DeferredRenderer: lighting shader fallito");
            return false;
        }

        m_SSAOShader = std::make_unique<Shader>();
        if (!m_SSAOShader->LoadFromSource(k_LightingVertSrc, k_SSAOFragSrc, "SSAO")) { SUN_CORE_WARN("DeferredRenderer: SSAO non disponibile"); m_SSAOShader.reset(); }
        m_SSAOBlurShader = std::make_unique<Shader>();
        if (!m_SSAOBlurShader->LoadFromSource(k_LightingVertSrc, k_SSAOBlurFragSrc, "SSAOBlur")) { SUN_CORE_WARN("DeferredRenderer: sfocatura SSAO non disponibile"); m_SSAOBlurShader.reset(); }
        m_FXAAShader = std::make_unique<Shader>();
        if (!m_FXAAShader->LoadFromSource(k_LightingVertSrc, k_FXAAFragSrc, "FXAA")) { SUN_CORE_WARN("DeferredRenderer: FXAA non disponibile"); m_FXAAShader.reset(); }

        m_TonemapShader = std::make_unique<Shader>();
        if (!m_TonemapShader->LoadFromSource(k_LightingVertSrc, k_TonemapFragSrc, "Tonemap")) {
            SUN_CORE_ERROR("DeferredRenderer: tonemap shader fallito");
            return false;
        }
        if (!m_Bloom.Init(width, height)) {
            SUN_CORE_WARN("DeferredRenderer: Bloom init fallito, proseguo senza");
            m_BloomEnabled = false;
        }
        if (!m_AutoExposure.Init(width, height)) {
            SUN_CORE_WARN("DeferredRenderer: AutoExposure init fallito, proseguo senza");
            m_AutoExposureEnabled = false;
        }
        // Shadow shader
        m_ShadowDepthShader = std::make_unique<Shader>();
        {
            std::string shadowVert = R"(
        #version 410 core
        layout (location = 0) in vec3 a_Position;
        uniform mat4 u_Model;
        uniform mat4 u_LightSpaceMatrix;
        void main() { gl_Position = u_LightSpaceMatrix * u_Model * vec4(a_Position, 1.0); }
    )";
            std::string shadowFrag = R"(
        #version 410 core
        void main() { }
    )";
            if (!m_ShadowDepthShader->LoadFromSource(shadowVert, shadowFrag, "ShadowDepth")) {
                SUN_CORE_ERROR("DeferredRenderer: shadow depth shader fallito");
                return false;
            }
        }

        // Ombre dei modelli animati: stesso shader di profondita' con lo skinning delle ossa
        m_ShadowSkinnedShader = std::make_unique<Shader>();
        {
            std::string vert = R"(
        #version 410 core
        layout (location = 0) in vec3 a_Position;
        layout (location = 3) in ivec4 a_BoneIDs;
        layout (location = 4) in vec4 a_Weights;
        uniform mat4 u_Model;
        uniform mat4 u_LightSpaceMatrix;
        uniform mat4 u_BoneMatrices[128];
        void main() {
            vec4 p = vec4(0.0);
            float tw = 0.0;
            for (int i = 0; i < 4; ++i) {
                int b = a_BoneIDs[i];
                if (b < 0 || b >= 128) continue;
                float w = a_Weights[i];
                if (w <= 0.0001) continue;
                p += (u_BoneMatrices[b] * vec4(a_Position, 1.0)) * w;
                tw += w;
            }
            if (tw < 0.001) p = vec4(a_Position, 1.0); else p /= tw;
            gl_Position = u_LightSpaceMatrix * u_Model * vec4(p.xyz, 1.0);
        }
    )";
            std::string frag = R"(
        #version 410 core
        void main() { }
    )";
            if (!m_ShadowSkinnedShader->LoadFromSource(vert, frag, "ShadowSkinned")) {
                SUN_CORE_WARN("DeferredRenderer: shader delle ombre skinnate fallito, i personaggi non proietteranno ombre");
                m_ShadowSkinnedShader.reset();
        m_SSAOShader.reset();
        m_SSAOBlurShader.reset();
        m_FXAAShader.reset();
            }
        }

        if (!m_ShadowMap.Init()) {
            SUN_CORE_ERROR("DeferredRenderer: ShadowMap init fallita");
            return false;
        }
        // UBO per le luci (binding 0)
        glGenBuffers(1, &m_LightUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, m_LightUBO);
        glBufferData(GL_UNIFORM_BUFFER,
            sizeof(Light) * MAX_LIGHTS,
            nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_LightUBO);
        m_LightingShader->BindUniformBlock("LightBlock", 0);
        m_LightingShader->BindUniformBlock("ShadowBlock", 1);   // FIX: matrici delle cascate (ShadowMap::UploadMatrices usa il punto 1)

        CreateTestGeometry();

        m_Initialized = true;
        SUN_CORE_INFO("DeferredRenderer inizializzato: {0}x{1}", width, height);
        return true;
    }

    void DeferredRenderer::Shutdown() {
        if (m_LightUBO != 0) {
            glDeleteBuffers(1, &m_LightUBO);
            m_LightUBO = 0;
        }
        if (m_TestCubeVAO != 0) {
            glDeleteVertexArrays(1, &m_TestCubeVAO);
            glDeleteBuffers(1, &m_TestCubeVBO);
            glDeleteBuffers(1, &m_TestCubeEBO);
            m_TestCubeVAO = 0;
        }
        m_GBufferShader.reset();
        m_LightingShader.reset();
        m_TonemapShader.reset();
        if (m_BloomEnabled) m_Bloom.Shutdown();
        if (m_AutoExposureEnabled) m_AutoExposure.Shutdown();
        m_Initialized = false;
        m_ShadowMap.Shutdown();
        m_ShadowDepthShader.reset();
        m_ShadowSkinnedShader.reset();
        m_GBufferSkinnedShader.reset();
    }

    void DeferredRenderer::Resize(int width, int height) {
        if (width == m_Width && height == m_Height) return;
        m_Width = width;
        m_Height = height;
        m_GBuffer.Resize(width, height);
        m_HDRTarget.Resize(width, height);
        m_SSAOTarget.Resize(width, height);
        m_SSAOBlurTarget.Resize(width, height);
        m_LDRTarget.Resize(width, height);
        m_CloakCopy.Resize(width, height);
        if (m_BloomEnabled) m_Bloom.Resize(width, height);
        if (m_AutoExposureEnabled) m_AutoExposure.Resize(width, height);
        // FIX #8: nessun FBO lasciato bindato (Resize puo' arrivare durante il frame ImGui)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void DeferredRenderer::CreateTestGeometry() {
        // Cube 24 vertici (pos + normal), 36 indici — non posso usare quello
        // del vecchio Renderer perché non ha normali.
        float verts[] = {
            // -Z (back)
            -0.5f,-0.5f,-0.5f,  0, 0,-1,
             0.5f,-0.5f,-0.5f,  0, 0,-1,
             0.5f, 0.5f,-0.5f,  0, 0,-1,
            -0.5f, 0.5f,-0.5f,  0, 0,-1,
            // +Z (front)
            -0.5f,-0.5f, 0.5f,  0, 0, 1,
             0.5f,-0.5f, 0.5f,  0, 0, 1,
             0.5f, 0.5f, 0.5f,  0, 0, 1,
            -0.5f, 0.5f, 0.5f,  0, 0, 1,
            // -X (left)
            -0.5f,-0.5f,-0.5f, -1, 0, 0,
            -0.5f,-0.5f, 0.5f, -1, 0, 0,
            -0.5f, 0.5f, 0.5f, -1, 0, 0,
            -0.5f, 0.5f,-0.5f, -1, 0, 0,
            // +X (right)
             0.5f,-0.5f,-0.5f,  1, 0, 0,
             0.5f,-0.5f, 0.5f,  1, 0, 0,
             0.5f, 0.5f, 0.5f,  1, 0, 0,
             0.5f, 0.5f,-0.5f,  1, 0, 0,
             // -Y (bottom)
             -0.5f,-0.5f,-0.5f,  0,-1, 0,
              0.5f,-0.5f,-0.5f,  0,-1, 0,
              0.5f,-0.5f, 0.5f,  0,-1, 0,
             -0.5f,-0.5f, 0.5f,  0,-1, 0,
             // +Y (top)
             -0.5f, 0.5f,-0.5f,  0, 1, 0,
              0.5f, 0.5f,-0.5f,  0, 1, 0,
              0.5f, 0.5f, 0.5f,  0, 1, 0,
             -0.5f, 0.5f, 0.5f,  0, 1, 0
        };

        unsigned int idx[] = {
     0, 3, 2, 2, 1, 0,       // back (-Z)    [FIXED]
     4, 5, 6, 6, 7, 4,       // front (+Z)
     8, 9, 10, 10, 11, 8,    // left (-X)
     14, 13, 12, 12, 15, 14, // right (+X)   [FIXED]
     16, 17, 18, 18, 19, 16, // bottom (-Y)
     22, 21, 20, 20, 23, 22  // top (+Y)     [FIXED]
        };

        glGenVertexArrays(1, &m_TestCubeVAO);
        glGenBuffers(1, &m_TestCubeVBO);
        glGenBuffers(1, &m_TestCubeEBO);

        glBindVertexArray(m_TestCubeVAO);

        glBindBuffer(GL_ARRAY_BUFFER, m_TestCubeVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_TestCubeEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
            (void*)(3 * sizeof(float)));

        glBindVertexArray(0);

        m_TestCubeIndexCount = 36;
    }

    Shader* DeferredRenderer::BeginGeometryPass(const Camera& camera) {
        m_ViewProjection = camera.GetViewProjection();
        m_View = camera.GetView();
        m_CameraPos = camera.GetPosition();
        m_CloakCubes.clear();
        m_CloakSkinned.clear();
        m_CloakCapture = false;

        // --- SHADOW PASS ---
        // Calcola le light-space matrices, poi il chiamante disegna
        // la scena 4 volte con RenderShadowPassCube. Per ora la facciamo
        // in automatico qui usando i cubi di test.
        m_ShadowMap.UpdateCascades(camera, m_SunDirection, /*shadowDistance*/ 60.0f);

        // Bind dello shadow UBO (binding point 1)
        m_ShadowMap.UploadMatrices();

        // (Il chiamante, se ha una scena reale, dovrebbe iterare qui.)
        // Nel tuo GameLayer il loop shadow va fatto PRIMA di BeginGeometryPass.
        // Vedi patch a GameLayer::OnRender.

        // --- G-BUFFER ---
        m_GBuffer.BindForWriting();
        m_GBuffer.Clear();

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        return m_GBufferShader.get();
    }

    void DeferredRenderer::EndGeometryPassAndPresent(const Camera& camera) {
        // Estraiamo la posizione reale della camera (fix del bug precedente:
        // l'inversa di VP * origine NDC restituisce un punto sulla linea di
        // vista, non la posizione della camera).
        m_CameraPos = camera.GetPosition();

        // Ricaviamo la camera pos dalla view matrix: -R^T * t
        // (per ora basta che il chiamante la passi in un modo semplice)
        // → Strategia alternativa: estrarre dalla view matrix è più pulito,
        //   ma dato che Camera qui è una black box, uso l'inversa di VP e
        //   prendo la posizione "di fronte". Non perfetto ma funziona per test.

        // Upload luci
        UploadLights();

        m_Time += m_LastFrameDelta;

        // 0. Occlusione ambientale (dal G-Buffer)
        RunSSAOPass();

        // 1. Lighting pass
        RunLightingPass();

        // 1b. Unita' mimetizzate: rifrangono la scena appena illuminata
        RunCloakPass();

        // 2. Tonemap pass (con correzione colore), poi antialiasing verso lo schermo
        RunTonemapPass();
        RunFXAAPass();
    }

    void DeferredRenderer::UploadLights() {
        int count = (int)m_Lights.size();
        if (count > MAX_LIGHTS) count = MAX_LIGHTS;

        glBindBuffer(GL_UNIFORM_BUFFER, m_LightUBO);
        if (count > 0) {
            glBufferSubData(GL_UNIFORM_BUFFER, 0, count * sizeof(Light), m_Lights.data());
        }
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        m_LightingShader->Bind();
        m_LightingShader->SetInt("u_LightCount", count);
    }

    void DeferredRenderer::RunLightingPass() {
        m_HDRTarget.Bind();
        m_HDRTarget.Clear(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);

        m_LightingShader->Bind();

        // Bind G-Buffer textures
        m_GBuffer.BindForReading();
        m_LightingShader->SetInt("u_GBufferAlbedo", 0);
        m_LightingShader->SetInt("u_GBufferNormal", 1);
        m_LightingShader->SetInt("u_GBufferMaterial", 2);
        m_LightingShader->SetInt("u_GBufferDepth", 3);

        m_ShadowMap.BindDepthTextures(4);
        m_LightingShader->SetInt("u_ShadowMap0", 4);
        m_LightingShader->SetInt("u_ShadowMap1", 5);
        m_LightingShader->SetInt("u_ShadowMap2", 6);
        m_LightingShader->SetInt("u_ShadowMap3", 7);
        m_LightingShader->SetFloat("u_ShadowBias", m_ShadowMap.GetShadowBias());
        m_LightingShader->SetFloat("u_ShadowNormalBias", m_ShadowMap.GetNormalBias());
        m_LightingShader->SetInt("u_CascadeCount", ShadowMap::CASCADE_COUNT);   // FIX: e' un int nello shader (con SetFloat restava 0: niente ombre)

        // Le splits le passiamo come array
        float splits[ShadowMap::CASCADE_COUNT];
        for (int i = 0; i < ShadowMap::CASCADE_COUNT; ++i)
            splits[i] = m_ShadowMap.GetCascadeSplits()[i];
        glUniform1fv(glGetUniformLocation(m_LightingShader->GetID(), "u_CascadeSplits"),
            ShadowMap::CASCADE_COUNT, splits);
        m_LightingShader->SetFloat("u_CascadeFade", m_CascadeFade);
        m_LightingShader->SetInt("u_UseShadows", m_Quality.sunShadows ? 1 : 0);
        m_LightingShader->SetInt("u_ShadowPCF", glm::clamp(m_Quality.shadowFilter, 0, 2));
        m_LightingShader->SetFloat("u_SSSScale", glm::clamp(m_Quality.skinSSS, 0.0f, 1.5f));
        m_LightingShader->SetFloat("u_SSAOStrength", glm::clamp(m_Quality.ssaoStrength, 0.0f, 3.0f));
        // Camera uniforms
        m_LightingShader->SetVec3("u_CameraPos", m_CameraPos);

        glm::mat4 invVP = glm::inverse(m_ViewProjection);
        m_LightingShader->SetMat4("u_InvViewProjection", invVP);

        // Fullscreen quad
        glBindVertexArray(Framebuffer::GetFullscreenQuadVAO());
        m_LightingShader->SetFloat("u_AmbientIntensity", m_AmbientIntensity);
        {
            const Atmosphere& a = m_Atmosphere;
            // con l'atmosfera, la luce ambiente viene dal cielo (sopra) e dal terreno (sotto)
            const glm::vec3 skyAmb = a.enabled ? glm::mix(a.zenith, a.horizon, 0.45f) * 1.25f : m_AmbientSkyColor;
            const glm::vec3 groundAmb = a.enabled ? glm::mix(a.ground, a.horizon, 0.3f) : m_AmbientGroundColor;
            m_LightingShader->SetVec3("u_AmbientSkyColor", skyAmb);
            m_LightingShader->SetVec3("u_AmbientGroundColor", groundAmb);
            m_LightingShader->SetInt("u_AtmoOn", a.enabled ? 1 : 0);
            m_LightingShader->SetVec3("u_Zenith", a.zenith);
            m_LightingShader->SetVec3("u_Horizon", a.horizon);
            m_LightingShader->SetVec3("u_Ground", a.ground);
            m_LightingShader->SetFloat("u_CloudCover", a.cloudCover);
            m_LightingShader->SetVec3("u_SunDirL", -m_SunDirection);
            const glm::vec3 sunCol = m_HasDirectional ? glm::vec3(m_DirectionalLight.colorIntensity) * m_DirectionalLight.colorIntensity.w : glm::vec3(1.0f);
            m_LightingShader->SetVec3("u_SunColor", sunCol);
            m_LightingShader->SetFloat("u_Time", m_Time);
            m_LightingShader->SetFloat("u_FogDensity", a.fogDensity);
            m_LightingShader->SetFloat("u_FogFalloff", a.fogHeightFalloff);
            m_LightingShader->SetFloat("u_FogBase", a.fogBaseHeight);
            m_LightingShader->SetVec3("u_FogColor", a.fogColor);
            m_LightingShader->SetFloat("u_Dust", a.dust);
            const bool ssao = m_SSAOEnabled && m_SSAOShader && m_SSAOBlurShader;
            m_LightingShader->SetInt("u_UseSSAO", ssao ? 1 : 0);
            if (ssao) {
                glActiveTexture(GL_TEXTURE8);
                glBindTexture(GL_TEXTURE_2D, m_SSAOBlurTarget.GetColorTexture(0)->GetID());
                m_LightingShader->SetInt("u_SSAO", 8);
            }
        }
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        m_HDRTarget.Unbind();
    }
    void DeferredRenderer::RunSSAOPass() {
        if (!m_SSAOEnabled || !m_SSAOShader || !m_SSAOBlurShader) return;
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glViewport(0, 0, m_Width, m_Height);
        glBindVertexArray(Framebuffer::GetFullscreenQuadVAO());
        // occlusione grezza
        m_SSAOTarget.Bind();
        m_SSAOShader->Bind();
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_GBuffer.GetDepth()->GetID());
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_GBuffer.GetNormal()->GetID());
        m_SSAOShader->SetInt("u_Depth", 0);
        m_SSAOShader->SetInt("u_Normal", 1);
        m_SSAOShader->SetMat4("u_InvViewProjection", glm::inverse(m_ViewProjection));
        m_SSAOShader->SetMat4("u_ViewProjection", m_ViewProjection);
        m_SSAOShader->SetVec3("u_CameraPos", m_CameraPos);
        m_SSAOShader->SetFloat("u_Radius", glm::clamp(m_Quality.ssaoRadius, 0.1f, 2.0f));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        // sfocatura che rispetta i bordi
        m_SSAOBlurTarget.Bind();
        m_SSAOBlurShader->Bind();
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_SSAOTarget.GetColorTexture(0)->GetID());
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_GBuffer.GetDepth()->GetID());
        m_SSAOBlurShader->SetInt("u_AO", 0);
        m_SSAOBlurShader->SetInt("u_Depth", 1);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void DeferredRenderer::RunFXAAPass() {
        if (!m_FXAAEnabled || !m_FXAAShader) return;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_Width, m_Height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        m_FXAAShader->Bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_LDRTarget.GetColorTexture(0)->GetID());
        m_FXAAShader->SetInt("u_Image", 0);
        glBindVertexArray(Framebuffer::GetFullscreenQuadVAO());
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }

    void DeferredRenderer::RunTonemapPass() {
        GLuint hdrSource = m_HDRTarget.GetColorTexture(0)->GetID();
        if (m_BloomEnabled && m_Quality.bloom) {   // m_BloomEnabled = bloom inizializzato; m_Quality.bloom = scelta del giocatore
            m_Bloom.SetIntensity(m_Quality.bloomIntensity);
            m_Bloom.Render(hdrSource);
            hdrSource = m_Bloom.GetCompositeTexture()->GetID();
        }

        // Auto-exposure: analizza il composite HDR e aggiorna l'expousure 1x1
        if (m_AutoExposureEnabled) {
            float dt = m_LastFrameDelta;  // vedi sotto
            m_AutoExposure.Update(hdrSource, dt);
        }

        // con l'antialiasing il tonemap scrive nell'immagine intermedia, poi FXAA la porta sullo schermo
        const bool fxaa = m_FXAAEnabled && m_FXAAShader;
        glBindFramebuffer(GL_FRAMEBUFFER, fxaa ? m_LDRTarget.GetID() : 0);
        glViewport(0, 0, m_Width, m_Height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_TonemapShader->Bind();
        {
            const Atmosphere& a = m_Atmosphere;
            m_TonemapShader->SetInt("u_Grade", a.enabled ? 1 : 0);
            m_TonemapShader->SetFloat("u_Saturation", a.saturation);
            m_TonemapShader->SetFloat("u_Contrast", a.contrast);
            m_TonemapShader->SetFloat("u_Vignette", a.vignette);
            m_TonemapShader->SetFloat("u_Grain", a.grain);
            m_TonemapShader->SetVec3("u_ShadowTint", a.shadowTint);
            m_TonemapShader->SetVec3("u_HighlightTint", a.highlightTint);
            m_TonemapShader->SetFloat("u_Time", m_Time);
        }
        m_TonemapShader->SetInt("u_HDRTexture", 0);
        m_TonemapShader->SetInt("u_ExposureTex", 1);
        m_TonemapShader->SetFloat("u_ExposureFallback", 1.0f);
        m_TonemapShader->SetFloat("u_Brightness", glm::clamp(m_Quality.brightness, 0.2f, 3.0f));
        m_TonemapShader->SetFloat("u_Gamma", glm::clamp(m_Quality.gamma, 1.6f, 2.8f));

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdrSource);
        if (m_AutoExposureEnabled) {
            m_AutoExposure.BindExposureTexture(1);
        }
        else {
            // bind dummy 1x1 R32F con valore 1
            static GLuint s_DummyExposureTex = 0;
            glActiveTexture(GL_TEXTURE1);   // prima di crearla: sull'unita' 0 c'e' gia' l'immagine HDR
            if (s_DummyExposureTex == 0) {
                glGenTextures(1, &s_DummyExposureTex);
                glBindTexture(GL_TEXTURE_2D, s_DummyExposureTex);
                float one = 1.0f;
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 1, 1, 0, GL_RED, GL_FLOAT, &one);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            }
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, s_DummyExposureTex);
        }

        glBindVertexArray(Framebuffer::GetFullscreenQuadVAO());
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }
    void DeferredRenderer::DrawCube(const glm::mat4& transform,
        const glm::vec4& color,
        float roughness, float metallic, float emissive, int material) {
        if (m_CloakCapture) { m_CloakCubes.push_back({ transform, m_CloakAmount }); return; }   // mimetizzato
        m_GBufferShader->Bind();
        m_GBufferShader->SetMat4("u_Model", transform);
        m_GBufferShader->SetMat4("u_ViewProjection", m_ViewProjection);
        m_GBufferShader->SetVec4("u_Color", color);
        m_GBufferShader->SetFloat("u_Roughness", roughness);
        m_GBufferShader->SetFloat("u_Metallic", metallic);
        m_GBufferShader->SetFloat("u_Emissive", emissive);
        m_GBufferShader->SetInt("u_Material", m_Quality.mapMaterials ? material : 0);
        m_GBufferShader->SetFloat("u_Relief", m_Quality.materialRelief);

        glBindVertexArray(m_TestCubeVAO);
        glDrawElements(GL_TRIANGLES, m_TestCubeIndexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    void DeferredRenderer::DrawTestGround(float size) {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f));
        t = glm::scale(t, glm::vec3(size, 0.05f, size));
        DrawCube(t, glm::vec4(0.25f, 0.25f, 0.28f, 1.0f), 0.9f, 0.0f);
    }

    void DeferredRenderer::ClearLights() {
        m_Lights.clear();
        m_DirectionalLight = {};
        m_HasDirectional = false;
    }

    void DeferredRenderer::AddLight(const Light& light) {
        if ((int)m_Lights.size() >= MAX_LIGHTS) return;
        m_Lights.push_back(light);
    }

    void DeferredRenderer::SetDirectionalLight(const glm::vec3& direction,
        const glm::vec3& color,
        float intensity) {
        m_DirectionalLight = Light::Directional(direction, color, intensity);
        m_HasDirectional = true;

        if (m_Lights.empty() ||
            m_Lights[0].flags.x != LIGHT_DIRECTIONAL) {
            m_Lights.insert(m_Lights.begin(), m_DirectionalLight);
        }
        else {
            m_Lights[0] = m_DirectionalLight;
        }
    }
    void DeferredRenderer::RenderShadowPassCube(const glm::mat4& transform, int cascadeIndex) {
        m_ShadowDepthShader->Bind();
        m_ShadowDepthShader->SetMat4("u_Model", transform);
        m_ShadowDepthShader->SetMat4("u_LightSpaceMatrix",
            m_ShadowMap.GetLightSpaceMatrix(cascadeIndex));

        glBindVertexArray(m_TestCubeVAO);
        glDrawElements(GL_TRIANGLES, m_TestCubeIndexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }
    bool DeferredRenderer::BeginShadowSkinned(int cascadeIndex, const glm::mat4& model, const std::vector<glm::mat4>& boneMatrices) {
        if (!m_ShadowSkinnedShader || !m_ShadowSkinnedShader->IsValid()) return false;
        m_ShadowSkinnedShader->Bind();
        m_ShadowSkinnedShader->SetMat4("u_Model", model);
        m_ShadowSkinnedShader->SetMat4("u_LightSpaceMatrix", m_ShadowMap.GetLightSpaceMatrix(cascadeIndex));
        const int count = std::min(static_cast<int>(boneMatrices.size()), MAX_BONES);
        if (count > 0)
            glUniformMatrix4fv(glGetUniformLocation(m_ShadowSkinnedShader->GetID(), "u_BoneMatrices"), count, GL_FALSE, glm::value_ptr(boneMatrices[0]));
        return true;
    }
    // twoSided: vestiti e armature sono gusci aperti, fanno ombra con tutte e due le facce. Il corpo (chiuso) resta
    // con le facce frontali scartate come i cubi: niente acne d'ombra sulla pelle.
    void DeferredRenderer::DrawShadowSkinned(const SkinnedMesh& mesh, bool twoSided) {
        if (twoSided) glDisable(GL_CULL_FACE);
        mesh.Draw();
        if (twoSided) glEnable(GL_CULL_FACE);
    }
    void DeferredRenderer::EndShadowSkinned() {}

    void DeferredRenderer::DrawSkinnedMesh(
        const SkinnedMesh& mesh,
        const glm::mat4& model,
        const std::vector<glm::mat4>& boneMatrices,
        const glm::vec4& color,
        float roughness, float metallic, float emissive,
        const GpuTexture* texture, float texScale, float alphaCut, int detail, float invHeight, const SkinnedDrawExtra* extra)
    {
        if (!m_GBufferSkinnedShader || !m_GBufferSkinnedShader->IsValid()) return;
        if (m_CloakCapture) { m_CloakSkinned.push_back({ &mesh, model, boneMatrices, m_CloakAmount }); return; }   // mimetizzato

        m_GBufferSkinnedShader->Bind();
        // Strato di vestito: texture triplanare (unita' 0) e soglia per i buchi
        m_GBufferSkinnedShader->SetInt("u_UseTexture", texture ? 1 : 0);
        if (texture) {
            texture->Bind(0);
            m_GBufferSkinnedShader->SetInt("u_Albedo", 0);
            m_GBufferSkinnedShader->SetFloat("u_TexScale", texScale);
            m_GBufferSkinnedShader->SetFloat("u_AlphaCut", alphaCut);
        }
        const SkinnedMaterial* mat = extra ? extra->material : nullptr;
        if (mat && mat->hidden) return;
        m_GBufferSkinnedShader->SetInt("u_Detail", (invHeight > 0.0f && !mat) ? detail : 0);
        m_GBufferSkinnedShader->SetInt("u_Authored", mat ? 1 : 0);
        if (mat) {
            // modello d'autore: texture del materiale sulle unita' 1-4, fattori al posto di rugosita'/metallo del chiamante
            auto bindTex = [&](const std::shared_ptr<GpuTexture>& t, int unit, const char* sampler, const char* flag) {
                m_GBufferSkinnedShader->SetInt(sampler, unit);
                m_GBufferSkinnedShader->SetInt(flag, t ? 1 : 0);
                if (t) t->Bind(unit);
            };
            bindTex(mat->albedo, 1, "u_TexAlbedo", "u_HasAlbedo");
            bindTex(mat->normal, 2, "u_TexNormal", "u_HasNormal");
            bindTex(mat->orm, 3, "u_TexORM", "u_HasORM");
            bindTex(mat->emissive, 4, "u_TexEmissive", "u_HasEmissive");
            glActiveTexture(GL_TEXTURE0);
            m_GBufferSkinnedShader->SetVec4("u_BaseColor", mat->baseColor);
            m_GBufferSkinnedShader->SetVec3("u_EmissiveColor", mat->emissiveColor);
            m_GBufferSkinnedShader->SetFloat("u_EmissiveStrength", mat->emissiveStrength);
            m_GBufferSkinnedShader->SetFloat("u_NormalStrength", mat->normalStrength);
            m_GBufferSkinnedShader->SetFloat("u_NormalY", mat->normalDirectX ? -1.0f : 1.0f);
            m_GBufferSkinnedShader->SetInt("u_MatKind", mat->kind);
            m_GBufferSkinnedShader->SetInt("u_Tint", mat->tint ? 1 : 0);
            m_GBufferSkinnedShader->SetFloat("u_MatGrime", mat->grime);
            m_GBufferSkinnedShader->SetFloat("u_AlphaCut", mat->alphaCut);
            roughness = mat->roughness;
            metallic = mat->metallic;
        }
        {
            static const SkinnedDrawExtra s_None;
            const SkinnedDrawExtra& ex = extra ? *extra : s_None;
            m_GBufferSkinnedShader->SetInt("u_Baked", ex.baked);
            m_GBufferSkinnedShader->SetVec4("u_SkinStyle", ex.skinStyle * glm::clamp(m_Quality.skinDetail, 0.0f, 2.0f));
            m_GBufferSkinnedShader->SetVec4("u_Color2", ex.color2);
        }
        m_GBufferSkinnedShader->SetFloat("u_Grime", glm::clamp(m_Quality.gearGrime, 0.0f, 2.0f));
        m_GBufferSkinnedShader->SetFloat("u_Desat", glm::clamp(m_Quality.desaturation, 0.0f, 1.0f));
        m_GBufferSkinnedShader->SetFloat("u_InvHeight", invHeight > 0.0f ? invHeight : 1.0f);
        m_GBufferSkinnedShader->SetMat4("u_Model", model);
        m_GBufferSkinnedShader->SetMat4("u_ViewProjection", m_ViewProjection);
        m_GBufferSkinnedShader->SetVec4("u_Color", color);
        m_GBufferSkinnedShader->SetFloat("u_Roughness", roughness);
        m_GBufferSkinnedShader->SetFloat("u_Metallic", metallic);
        m_GBufferSkinnedShader->SetFloat("u_Emissive", emissive);

        int count = std::min((int)boneMatrices.size(), MAX_BONES);
        if (count > 0) {
            glUniformMatrix4fv(
                glGetUniformLocation(m_GBufferSkinnedShader->GetID(), "u_BoneMatrices"),
                count, GL_FALSE, glm::value_ptr(boneMatrices[0]));
        }
        else {
            // Bind identity matrices se boneMatrices è vuoto (fallback)
            static std::vector<glm::mat4> s_Identity(MAX_BONES, glm::mat4(1.0f));
            glUniformMatrix4fv(
                glGetUniformLocation(m_GBufferSkinnedShader->GetID(), "u_BoneMatrices"),
                MAX_BONES, GL_FALSE, glm::value_ptr(s_Identity[0]));
        }

        mesh.Draw();
    }
    // Disegna le unita' mimetizzate sopra l'immagine HDR illuminata: la scena viene prima copiata (non si puo'
    // leggere e scrivere la stessa texture), poi ogni superficie mostra la copia spostata lungo la sua normale.
    void DeferredRenderer::RunCloakPass() {
        if (m_CloakCubes.empty() && m_CloakSkinned.empty()) return;
        m_CloakTime += m_LastFrameDelta;
        if (!m_CloakShader || !m_CloakShader->IsValid() || !m_CloakSkinnedShader || !m_CloakSkinnedShader->IsValid()) {
            m_CloakCubes.clear(); m_CloakSkinned.clear();
            return;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_HDRTarget.GetID());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_CloakCopy.GetID());
        glBlitFramebuffer(0, 0, m_Width, m_Height, 0, 0, m_Width, m_Height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        m_HDRTarget.Bind();
        glViewport(0, 0, m_Width, m_Height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        const GLboolean blendWasOn = glIsEnabled(GL_BLEND);   // il renderer forward conta sul blending acceso
        glDisable(GL_BLEND);

        auto setup = [&](Shader& sh) {
            sh.Bind();
            m_CloakCopy.BindColorTexture(0, 0);
            if (GpuTexture* depth = m_GBuffer.GetDepth()) depth->Bind(1);
            sh.SetInt("u_Scene", 0);
            sh.SetInt("u_Depth", 1);
            sh.SetVec2("u_Viewport", glm::vec2(static_cast<float>(m_Width), static_cast<float>(m_Height)));
            sh.SetMat4("u_View", m_View);
            sh.SetMat4("u_ViewProjection", m_ViewProjection);
            sh.SetVec3("u_CameraPos", m_CameraPos);
            sh.SetFloat("u_Time", m_CloakTime);
        };

        setup(*m_CloakShader);
        glBindVertexArray(m_TestCubeVAO);
        for (const auto& c : m_CloakCubes) {
            m_CloakShader->SetMat4("u_Model", c.model);
            m_CloakShader->SetFloat("u_Amount", c.amount);
            glDrawElements(GL_TRIANGLES, m_TestCubeIndexCount, GL_UNSIGNED_INT, 0);
        }
        glBindVertexArray(0);

        setup(*m_CloakSkinnedShader);
        const GLint bonesLoc = glGetUniformLocation(m_CloakSkinnedShader->GetID(), "u_BoneMatrices");
        static std::vector<glm::mat4> s_Identity(MAX_BONES, glm::mat4(1.0f));
        for (const auto& s : m_CloakSkinned) {
            if (!s.mesh) continue;
            m_CloakSkinnedShader->SetMat4("u_Model", s.model);
            m_CloakSkinnedShader->SetFloat("u_Amount", s.amount);
            const int count = std::min(static_cast<int>(s.bones.size()), MAX_BONES);
            if (count > 0) glUniformMatrix4fv(bonesLoc, count, GL_FALSE, glm::value_ptr(s.bones[0]));
            else glUniformMatrix4fv(bonesLoc, MAX_BONES, GL_FALSE, glm::value_ptr(s_Identity[0]));
            s.mesh->Draw();
        }
        m_CloakCubes.clear();
        m_CloakSkinned.clear();
        if (blendWasOn) glEnable(GL_BLEND);
    }
}