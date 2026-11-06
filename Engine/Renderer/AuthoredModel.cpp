// Modelli d'autore: materiali e texture PBR dei personaggi fatti da un artista (vedi AuthoredModel.h).
#include "AuthoredModel.h"
#include "../Core/Logger/Log.h"

#include <assimp/scene.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <assimp/material.h>
#include <assimp/GltfMaterial.h>
#include <stb_image.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace SunEngine {

    namespace {

        std::string Lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }
        // solo lettere e cifre, minuscole: "Armor Plates.001" -> "armorplates001"
        std::string Key(const std::string& s) {
            std::string r;
            for (unsigned char c : s) if (std::isalnum(c)) r += static_cast<char>(std::tolower(c));
            return r;
        }
        bool IsImageFile(const fs::path& p) {
            const std::string e = Lower(p.extension().string());
            return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" || e == ".psd" || e == ".hdr";
        }
        bool IsModelFile(const fs::path& p) {
            const std::string e = Lower(p.extension().string());
            return e == ".fbx" || e == ".glb" || e == ".gltf";
        }

        // Immagine RGBA8 in memoria
        struct Image {
            int w = 0, h = 0;
            std::vector<unsigned char> px;
            bool Valid() const { return w > 0 && h > 0 && px.size() == static_cast<size_t>(w) * h * 4; }
        };

        bool FromStb(unsigned char* data, int w, int h, Image& out) {
            if (!data) return false;
            out.w = w; out.h = h;
            out.px.assign(data, data + static_cast<size_t>(w) * h * 4);
            stbi_image_free(data);
            return true;
        }
        bool LoadImageFile(const std::string& path, Image& out) {
            stbi_set_flip_vertically_on_load(false);   // le UV sono gia' girate dall'importazione (aiProcess_FlipUVs)
            int w = 0, h = 0, c = 0;
            unsigned char* data = stbi_load(path.c_str(), &w, &h, &c, 4);   // prima la chiamata: poi w e h sono validi
            if (FromStb(data, w, h, out)) return true;
            SUN_CORE_WARN("[Asset] texture illeggibile: {0} ({1})", path, stbi_failure_reason() ? stbi_failure_reason() : "?");
            return false;
        }
        // Texture incorporata nel file (FBX/GLB): compressa (png/jpg) o grezza (aiTexel BGRA)
        bool LoadEmbedded(const aiTexture* t, Image& out) {
            if (!t) return false;
            if (t->mHeight == 0) {
                stbi_set_flip_vertically_on_load(false);
                int w = 0, h = 0, c = 0;
                unsigned char* data = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(t->pcData), static_cast<int>(t->mWidth), &w, &h, &c, 4);
                return FromStb(data, w, h, out);
            }
            out.w = static_cast<int>(t->mWidth); out.h = static_cast<int>(t->mHeight);
            out.px.resize(static_cast<size_t>(out.w) * out.h * 4);
            for (size_t i = 0; i < static_cast<size_t>(out.w) * out.h; ++i) {
                const aiTexel& x = t->pcData[i];
                out.px[i * 4 + 0] = x.r; out.px[i * 4 + 1] = x.g; out.px[i * 4 + 2] = x.b; out.px[i * 4 + 3] = x.a;
            }
            return true;
        }
        // Campionamento al pixel piu' vicino (per unire mappe di risoluzioni diverse)
        unsigned char SampleChannel(const Image& im, int x, int y, int W, int H, int ch) {
            const int sx = std::min(im.w - 1, x * im.w / std::max(1, W));
            const int sy = std::min(im.h - 1, y * im.h / std::max(1, H));
            return im.px[(static_cast<size_t>(sy) * im.w + sx) * 4 + ch];
        }

        std::shared_ptr<GpuTexture> Upload(const Image& im, bool srgb) {
            if (!im.Valid()) return nullptr;
            auto t = std::make_shared<GpuTexture>();
            t->CreateFromData(im.w, im.h, 4, im.px.data(), TextureFilter::LinearMipmapLinear, srgb);
            return t;
        }

        // Da dove arriva un'immagine: file su disco o texture incorporata
        struct TexSource {
            std::string file;                 // percorso risolto
            const aiTexture* embedded = nullptr;
            bool Empty() const { return file.empty() && !embedded; }
            std::string Id() const { return embedded ? "*embedded*" + std::to_string(reinterpret_cast<uintptr_t>(embedded)) : file; }
            std::string Name() const { return embedded ? std::string("(incorporata) ") + embedded->mFilename.C_Str() : fs::path(file).filename().string(); }
        };

        bool LoadSource(const TexSource& s, Image& out) {
            if (s.embedded) return LoadEmbedded(s.embedded, out);
            if (!s.file.empty()) return LoadImageFile(s.file, out);
            return false;
        }

        // Percorso scritto nel file del modello -> file esistente. Gli FBX spesso hanno il percorso assoluto del PC
        // dell'artista: si cerca il nome del file accanto al modello e nella cartella Textures.
        std::string ResolveTextureFile(const std::string& raw, const fs::path& dir) {
            if (raw.empty()) return {};
            std::string norm = raw;
            std::replace(norm.begin(), norm.end(), '\\', '/');
            std::error_code ec;
            const fs::path p(norm);
            const fs::path name = p.filename();
            const fs::path tries[] = { dir / p, dir / name, dir / "Textures" / name, dir / "textures" / name, p };
            for (const auto& t : tries) if (!t.empty() && fs::is_regular_file(t, ec)) return t.string();
            // stesso nome con altra estensione (es. .tif nel file, .png esportata)
            const std::string stemKey = Key(name.stem().string());
            for (const fs::path& d : { dir, dir / "Textures", dir / "textures" }) {
                if (!fs::is_directory(d, ec)) continue;
                for (const auto& e : fs::directory_iterator(d, ec))
                    if (e.is_regular_file() && IsImageFile(e.path()) && Key(e.path().stem().string()) == stemKey) return e.path().string();
            }
            return {};
        }

        TexSource SourceFromMaterial(const aiScene* scene, const aiMaterial* mat, aiTextureType type, const fs::path& dir) {
            TexSource s;
            if (!mat || mat->GetTextureCount(type) == 0) return s;
            aiString path;
            if (mat->GetTexture(type, 0, &path) != AI_SUCCESS || path.length == 0) return s;
            if (const aiTexture* emb = scene->GetEmbeddedTexture(path.C_Str())) { s.embedded = emb; return s; }
            s.file = ResolveTextureFile(path.C_Str(), dir);
            if (s.file.empty()) SUN_CORE_WARN("[Asset] texture indicata nel modello ma non trovata: {0}", path.C_Str());
            return s;
        }

        // Ruolo di una texture dal nome del file (convenzioni di Substance Painter, Unreal, glTF, Quixel...)
        enum class Role { None, Albedo, Normal, ORM, MetalRough, Roughness, Gloss, Metallic, AO, Emissive, Opacity };

        // Il tipo di mappa e' di solito l'ultima parola del nome ("Arm_BaseColor" = colore del materiale "Arm", non
        // un ORM): si guardano le parole dall'ultima alla prima, poi le forme composte in tutto il nome.
        Role RoleFromFileName(const std::string& stem) {
            std::string s = Lower(stem);
            for (char& c : s) if (c == '-' || c == ' ' || c == '.') c = '_';
            std::vector<std::string> tok;
            { std::string cur; for (char c : s) { if (c == '_') { if (!cur.empty()) tok.push_back(cur); cur.clear(); } else cur += c; } if (!cur.empty()) tok.push_back(cur); }
            static const std::unordered_map<std::string, Role> words = {
                { "occlusionroughnessmetallic", Role::ORM }, { "occlusionroughnessmetalness", Role::ORM }, { "orm", Role::ORM }, { "arm", Role::ORM }, { "rma", Role::ORM },
                { "metallicroughness", Role::MetalRough }, { "metalroughness", Role::MetalRough }, { "mr", Role::MetalRough },
                { "normal", Role::Normal }, { "normalgl", Role::Normal }, { "normalopengl", Role::Normal }, { "nrm", Role::Normal }, { "nor", Role::Normal }, { "n", Role::Normal }, { "nml", Role::Normal },
                { "basecolor", Role::Albedo }, { "albedo", Role::Albedo }, { "diffuse", Role::Albedo }, { "color", Role::Albedo }, { "colour", Role::Albedo },
                { "col", Role::Albedo }, { "bc", Role::Albedo }, { "d", Role::Albedo }, { "diff", Role::Albedo },
                { "roughness", Role::Roughness }, { "rough", Role::Roughness }, { "r", Role::Roughness },
                { "glossiness", Role::Gloss }, { "gloss", Role::Gloss }, { "smoothness", Role::Gloss },
                { "metallic", Role::Metallic }, { "metalness", Role::Metallic }, { "metal", Role::Metallic }, { "m", Role::Metallic },
                { "ao", Role::AO }, { "ambientocclusion", Role::AO }, { "occlusion", Role::AO },
                { "emissive", Role::Emissive }, { "emission", Role::Emissive }, { "emit", Role::Emissive }, { "e", Role::Emissive }, { "glow", Role::Emissive },
                { "opacity", Role::Opacity }, { "alpha", Role::Opacity }, { "mask", Role::Opacity },
            };
            for (auto it = tok.rbegin(); it != tok.rend(); ++it) {
                auto w = words.find(*it);
                if (w != words.end()) return w->second;
            }
            auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
            if (has("occlusionroughnessmetal")) return Role::ORM;
            if (has("metallicroughness")) return Role::MetalRough;
            if (has("normal")) return Role::Normal;
            if (has("basecolor") || has("base_color") || has("albedo") || has("diffuse")) return Role::Albedo;
            if (has("roughness")) return Role::Roughness;
            if (has("gloss") || has("smoothness")) return Role::Gloss;
            if (has("metallic") || has("metalness")) return Role::Metallic;
            if (has("occlusion")) return Role::AO;
            if (has("emissive") || has("emission")) return Role::Emissive;
            if (has("opacity")) return Role::Opacity;
            return Role::None;
        }

        int GuessKind(const std::string& matName) {
            const std::string n = Lower(matName);
            auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
            if (has("armor") || has("armour") || has("plate") || has("helm") || has("metal") || has("gun") || has("weapon") || has("pauldron")) return SkinMat_Metal;
            if (has("eye") || has("cornea") || has("iris")) return SkinMat_Eye;
            if (has("hair") || has("lash") || has("brow") || has("beard") || has("fur")) return SkinMat_Hair;
            if (has("skin") || has("body") || has("face") || has("head") || has("flesh") || has("hand")) return SkinMat_Skin;
            if (has("cloth") || has("fabric") || has("cape") || has("robe") || has("tabard") || has("cloak") || has("leather") || has("strap") || has("pants") || has("shirt")) return SkinMat_Cloth;
            return SkinMat_Generic;
        }

        int KindFromName(const std::string& t) {
            const std::string n = Lower(t);
            if (n == "skin" || n == "pelle") return SkinMat_Skin;
            if (n == "cloth" || n == "stoffa" || n == "leather" || n == "cuoio") return SkinMat_Cloth;
            if (n == "metal" || n == "metallo" || n == "armor" || n == "armatura") return SkinMat_Metal;
            if (n == "eye" || n == "occhio") return SkinMat_Eye;
            if (n == "hair" || n == "capelli") return SkinMat_Hair;
            return SkinMat_Generic;
        }

        float DefaultRoughness(int kind) {
            switch (kind) { case SkinMat_Skin: return 0.5f; case SkinMat_Cloth: return 0.85f; case SkinMat_Metal: return 0.4f;
                            case SkinMat_Eye: return 0.1f; case SkinMat_Hair: return 0.6f; default: return 0.6f; }
        }

        // Tutte le fonti di un materiale, poi caricate e unite in Build()
        struct MatSources {
            TexSource albedo, normal, orm, metalRough, rough, gloss, metal, ao, emissive, opacity;
        };

        // Immagini nella cartella del modello (e in Textures/), per la ricerca per nome
        std::vector<fs::path> ListImages(const fs::path& dir) {
            std::vector<fs::path> r;
            std::error_code ec;
            for (const fs::path& d : { dir, dir / "Textures", dir / "textures" }) {
                if (!fs::is_directory(d, ec)) continue;
                for (const auto& e : fs::directory_iterator(d, ec)) if (e.is_regular_file() && IsImageFile(e.path())) r.push_back(e.path());
            }
            std::sort(r.begin(), r.end());
            r.erase(std::unique(r.begin(), r.end()), r.end());
            return r;
        }

        void FillByFileName(MatSources& src, const std::string& matName, const std::vector<fs::path>& images, bool singleMaterial) {
            const std::string mk = Key(matName);
            // passata 0: il file inizia col nome del materiale; 1: lo contiene; 2: qualsiasi (modello con un solo materiale)
            auto consider = [&](int pass) {
                for (const auto& p : images) {
                    const std::string stem = p.stem().string();
                    const std::string sk = Key(stem);
                    if (pass == 0 && (mk.empty() || sk.rfind(mk, 0) != 0)) continue;
                    if (pass == 1 && (mk.empty() || sk.find(mk) == std::string::npos)) continue;
                    TexSource* slot = nullptr;
                    switch (RoleFromFileName(stem)) {
                    case Role::Albedo: slot = &src.albedo; break;
                    case Role::Normal: slot = &src.normal; break;
                    case Role::ORM: slot = &src.orm; break;
                    case Role::MetalRough: slot = &src.metalRough; break;
                    case Role::Roughness: slot = &src.rough; break;
                    case Role::Gloss: slot = &src.gloss; break;
                    case Role::Metallic: slot = &src.metal; break;
                    case Role::AO: slot = &src.ao; break;
                    case Role::Emissive: slot = &src.emissive; break;
                    case Role::Opacity: slot = &src.opacity; break;
                    default: break;
                    }
                    if (slot && slot->Empty()) slot->file = p.string();
                }
            };
            consider(0);
            consider(1);
            if (singleMaterial) consider(2);   // un solo materiale: valgono anche le texture senza il suo nome
        }

        // Carica le immagini e crea le texture del materiale (con cache: stessa immagine = stessa texture)
        struct TexCache {
            std::unordered_map<std::string, std::shared_ptr<GpuTexture>> map;
        };

        void Build(SkinnedMaterial& m, const MatSources& s, TexCache& cache, bool hasPbrFactors, std::string& report) {
            // colore (+ opacita' separata nell'alfa)
            if (!s.albedo.Empty()) {
                const std::string id = s.albedo.Id() + "|" + s.opacity.Id();
                auto it = cache.map.find(id);
                if (it != cache.map.end()) m.albedo = it->second;
                else {
                    Image im;
                    if (LoadSource(s.albedo, im)) {
                        Image op;
                        if (!s.opacity.Empty() && LoadSource(s.opacity, op))
                            for (int y = 0; y < im.h; ++y) for (int x = 0; x < im.w; ++x)
                                im.px[(static_cast<size_t>(y) * im.w + x) * 4 + 3] = SampleChannel(op, x, y, im.w, im.h, 0);
                        m.albedo = cache.map[id] = Upload(im, true);
                    }
                }
                if (m.albedo) report += " colore=" + s.albedo.Name();
            }
            if (!s.normal.Empty()) {
                auto it = cache.map.find(s.normal.Id());
                if (it != cache.map.end()) m.normal = it->second;
                else { Image im; if (LoadSource(s.normal, im)) m.normal = cache.map[s.normal.Id()] = Upload(im, false); }
                if (m.normal) report += " normal=" + s.normal.Name();
            }
            if (!s.emissive.Empty()) {
                auto it = cache.map.find(s.emissive.Id());
                if (it != cache.map.end()) m.emissive = it->second;
                else { Image im; if (LoadSource(s.emissive, im)) m.emissive = cache.map[s.emissive.Id()] = Upload(im, true); }
                if (m.emissive) report += " emissiva=" + s.emissive.Name();
            }
            // ORM: occlusione (R), rugosita' (G), metallo (B), da una mappa unica o da mappe separate
            Image packed, mr, rough, gloss, metal, ao;
            const bool hasPacked = !s.orm.Empty() && LoadSource(s.orm, packed);
            const bool hasMR = !hasPacked && !s.metalRough.Empty() && LoadSource(s.metalRough, mr);
            const bool hasRough = !hasPacked && !hasMR && !s.rough.Empty() && LoadSource(s.rough, rough);
            const bool hasGloss = !hasPacked && !hasMR && !hasRough && !s.gloss.Empty() && LoadSource(s.gloss, gloss);
            const bool hasMetal = !hasPacked && !hasMR && !s.metal.Empty() && LoadSource(s.metal, metal);
            const bool hasAO = !s.ao.Empty() && !(hasPacked && s.ao.Id() == s.orm.Id()) && LoadSource(s.ao, ao);
            if (!(hasPacked || hasMR || hasRough || hasGloss || hasMetal || hasAO)) return;
            int W = 0, H = 0;
            for (const Image* im : { &packed, &mr, &rough, &gloss, &metal, &ao })
                if (im->Valid() && im->w * im->h > W * H) { W = im->w; H = im->h; }
            Image orm; orm.w = W; orm.h = H; orm.px.assign(static_cast<size_t>(W) * H * 4, 255);
            const bool roughFromTex = hasPacked || hasMR || hasRough || hasGloss;
            const bool metalFromTex = hasPacked || hasMR || hasMetal;
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                unsigned char* o = &orm.px[(static_cast<size_t>(y) * W + x) * 4];
                if (hasPacked) { o[0] = SampleChannel(packed, x, y, W, H, 0); o[1] = SampleChannel(packed, x, y, W, H, 1); o[2] = SampleChannel(packed, x, y, W, H, 2); }
                if (hasMR) { o[1] = SampleChannel(mr, x, y, W, H, 1); o[2] = SampleChannel(mr, x, y, W, H, 2); }
                if (hasRough) o[1] = SampleChannel(rough, x, y, W, H, 0);
                if (hasGloss) o[1] = static_cast<unsigned char>(255 - SampleChannel(gloss, x, y, W, H, 0));
                if (hasMetal) o[2] = SampleChannel(metal, x, y, W, H, 0);
                if (hasAO) o[0] = SampleChannel(ao, x, y, W, H, 0);
            }
            m.orm = Upload(orm, false);
            // canale preso da texture: il fattore resta un moltiplicatore (1 se il file non ne indica uno)
            if (roughFromTex && !hasPbrFactors) m.roughness = 1.0f;
            if (metalFromTex && !hasPbrFactors) m.metallic = 1.0f;
            report += hasPacked ? " orm=" + s.orm.Name() : " orm=(unita da mappe separate)";
        }

        void ApplyJsonVec(const nlohmann::json& j, const char* k, glm::vec3& out) {
            if (j.contains(k) && j[k].is_array() && j[k].size() >= 3)
                for (int i = 0; i < 3; ++i) if (j[k][i].is_number()) out[i] = j[k][i].get<float>();
        }
    }

    // Estensione verticale del modello in posa di riposo, come viene disegnato: vertici passati per le ossa (un glTF
    // esportato da Blender ha spesso la scala e la rotazione sul nodo dell'armatura, non nei vertici)
    void SkinnedBindBounds(const SkinnedModel& model, float& lo, float& hi) {
        std::unordered_map<std::string, glm::mat4> globals;
        struct Walk {
            std::unordered_map<std::string, glm::mat4>& g;
            void operator()(const AssimpNodeData& n, const glm::mat4& parent) {
                const glm::mat4 m = parent * n.transformation;
                g[n.name] = m;
                for (const auto& c : n.children) (*this)(c, m);
            }
        };
        Walk{ globals }(model.rootNode, glm::mat4(1.0f));
        std::vector<glm::mat4> B(static_cast<size_t>(std::max(model.boneCounter, 1)), glm::mat4(1.0f));
        for (const auto& [name, info] : model.boneInfoMap) {
            auto it = globals.find(name);
            if (it != globals.end() && info.id >= 0 && info.id < static_cast<int>(B.size()))
                B[info.id] = model.m_GlobalInverseTransform * it->second * info.offsetMatrix;
        }
        lo = 1e30f; hi = -1e30f;
        for (const auto& m : model.meshes) for (const auto& v : m.vertices) {
            glm::vec4 p(0.0f); float tw = 0.0f;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                if (v.BoneIDs[k] < 0 || v.BoneIDs[k] >= static_cast<int>(B.size()) || v.Weights[k] <= 0.0f) continue;
                p += B[v.BoneIDs[k]] * glm::vec4(v.Position, 1.0f) * v.Weights[k]; tw += v.Weights[k];
            }
            const float y = tw > 0.001f ? p.y / tw : v.Position.y;
            lo = std::min(lo, y); hi = std::max(hi, y);
        }
        if (hi < lo) lo = hi = 0.0f;
    }

    float SkinnedModelHeight(const SkinnedModel& model) {
        float lo, hi;
        SkinnedBindBounds(model, lo, hi);
        return hi - lo;
    }

    void ReadSkinnedMaterials(const aiScene* scene, const std::string& modelPath, SkinnedModel& model) {
        const fs::path dir = fs::path(modelPath).parent_path();
        const std::string ext = Lower(fs::path(modelPath).extension().string());
        const bool gltf = ext == ".gltf" || ext == ".glb";
        const std::vector<fs::path> images = ListImages(dir);
        TexCache cache;
        model.materials.clear();
        // quanti materiali sono usati davvero (un FBX ha spesso un materiale "DefaultMaterial" inutilizzato)
        std::vector<char> used(scene->mNumMaterials, 0);
        for (unsigned int i = 0; i < scene->mNumMeshes; ++i) if (scene->mMeshes[i]->mMaterialIndex < scene->mNumMaterials) used[scene->mMeshes[i]->mMaterialIndex] = 1;
        const bool single = std::count(used.begin(), used.end(), 1) <= 1;

        for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
            const aiMaterial* am = scene->mMaterials[i];
            SkinnedMaterial m;
            m.name = am->GetName().C_Str();
            m.kind = GuessKind(m.name);
            m.roughness = DefaultRoughness(m.kind);
            m.metallic = m.kind == SkinMat_Metal ? 0.85f : 0.0f;
            if (m.kind == SkinMat_Hair) m.alphaCut = 0.5f;

            // fattori PBR (glTF e FBX "Stingray/PBR"): se ci sono valgono come moltiplicatori
            bool pbr = false;
            aiColor4D c;
            if (am->Get(AI_MATKEY_BASE_COLOR, c) == AI_SUCCESS) { m.baseColor = glm::vec4(c.r, c.g, c.b, c.a); pbr = true; }
            float f = 0.0f;
            if (am->Get(AI_MATKEY_METALLIC_FACTOR, f) == AI_SUCCESS) { m.metallic = f; pbr = true; }
            if (am->Get(AI_MATKEY_ROUGHNESS_FACTOR, f) == AI_SUCCESS) { m.roughness = f; pbr = true; }
            aiColor3D e;
            if (am->Get(AI_MATKEY_COLOR_EMISSIVE, e) == AI_SUCCESS) m.emissiveColor = glm::vec3(e.r, e.g, e.b);
            aiString alphaMode;
            if (am->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS && std::string(alphaMode.C_Str()) == "MASK") {
                float cut = 0.5f; am->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cut); m.alphaCut = cut;
            }

            MatSources src;
            src.albedo = SourceFromMaterial(scene, am, aiTextureType_BASE_COLOR, dir);
            if (src.albedo.Empty()) src.albedo = SourceFromMaterial(scene, am, aiTextureType_DIFFUSE, dir);
            src.normal = SourceFromMaterial(scene, am, aiTextureType_NORMAL_CAMERA, dir);
            if (src.normal.Empty()) src.normal = SourceFromMaterial(scene, am, aiTextureType_NORMALS, dir);
            src.emissive = SourceFromMaterial(scene, am, aiTextureType_EMISSION_COLOR, dir);
            if (src.emissive.Empty()) src.emissive = SourceFromMaterial(scene, am, aiTextureType_EMISSIVE, dir);
            src.opacity = SourceFromMaterial(scene, am, aiTextureType_OPACITY, dir);
            // glTF: metallicRoughness (G = rugosita', B = metallo) arriva come METALNESS e/o DIFFUSE_ROUGHNESS (stesso file)
            TexSource met = SourceFromMaterial(scene, am, aiTextureType_METALNESS, dir);
            TexSource rou = SourceFromMaterial(scene, am, aiTextureType_DIFFUSE_ROUGHNESS, dir);
            TexSource unk = SourceFromMaterial(scene, am, aiTextureType_UNKNOWN, dir);
            src.ao = SourceFromMaterial(scene, am, aiTextureType_AMBIENT_OCCLUSION, dir);
            if (src.ao.Empty()) src.ao = SourceFromMaterial(scene, am, aiTextureType_LIGHTMAP, dir);
            if (!met.Empty() && !rou.Empty() && met.Id() == rou.Id()) src.metalRough = met;
            else if (!unk.Empty()) src.metalRough = unk;
            else if (gltf && !met.Empty()) src.metalRough = met;   // glTF: e' sempre la mappa unica metallo/rugosita'
            else { src.metal = met; src.rough = rou; }
            if (!src.metalRough.Empty() && !src.ao.Empty() && src.ao.Id() == src.metalRough.Id()) { src.orm = src.metalRough; src.metalRough = {}; }
            // poi per nome del file, solo per cio' che manca
            if (used[i]) FillByFileName(src, m.name, images, single);

            std::string report;
            if (used[i]) Build(m, src, cache, pbr, report);
            if (!pbr && m.albedo) m.baseColor = glm::vec4(1.0f);   // FBX: il colore "diffuse" e' spesso un grigio di default
            if (used[i]) SUN_CORE_INFO("[Asset] materiale '{0}' ({1}):{2}", m.name, m.kind, report.empty() ? " nessuna texture (solo colore)" : report);
            model.materials.push_back(std::move(m));
        }
    }

    bool FindAuthoredCharacter(const std::string& folder, AuthoredCharacterInfo& out) {
        std::error_code ec;
        const fs::path dir(folder);
        if (!fs::is_directory(dir, ec)) return false;
        out = {};
        out.folder = folder;
        if (!out.folder.empty() && out.folder.back() != '/' && out.folder.back() != '\\') out.folder += "/";
        // model.json -> "model" indica il file; altrimenti model.* oppure il primo FBX/GLB/GLTF della cartella
        const fs::path cfg = dir / "model.json";
        if (fs::is_regular_file(cfg, ec)) {
            try {
                std::ifstream in(cfg);
                const nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);   // commenti ammessi
                if (j.contains("model") && j["model"].is_string()) {
                    const fs::path p = dir / j["model"].get<std::string>();
                    if (fs::is_regular_file(p, ec)) out.modelPath = p.string();
                    else SUN_CORE_WARN("[Asset] model.json indica '{0}' ma il file non c'e'", p.string());
                }
            }
            catch (const std::exception& e) { SUN_CORE_WARN("[Asset] {0}: JSON non valido ({1})", cfg.string(), e.what()); }
        }
        if (out.modelPath.empty()) {
            std::vector<fs::path> models;
            for (const auto& e : fs::directory_iterator(dir, ec)) if (e.is_regular_file() && IsModelFile(e.path())) models.push_back(e.path());
            std::sort(models.begin(), models.end());
            for (const auto& p : models) if (Lower(p.stem().string()) == "model") { out.modelPath = p.string(); break; }
            if (out.modelPath.empty() && !models.empty()) out.modelPath = models.front().string();
        }
        if (out.modelPath.empty()) return false;
        if (fs::is_directory(dir / "Animations", ec)) out.animsDir = (dir / "Animations").string() + "/";
        return true;
    }

    bool LoadAuthoredCharacter(const AuthoredCharacterInfo& info, SkinnedModel& out, float targetHeightCm) {
        SUN_CORE_INFO("[Asset] modello d'autore: {0}", info.modelPath);
        if (!out.LoadModel(info.modelPath, true)) return false;
        out.authored = true;
        // La trasformazione della radice resta applicata: negli export da Blender/Maya contiene il passaggio da Z-su a
        // Y-su e da centimetri a metri (i modelli Mixamo del gioco hanno la radice neutra, per loro non cambia nulla).
        out.m_GlobalInverseTransform = glm::mat4(1.0f);
        if (out.boneInfoMap.empty()) SUN_CORE_WARN("[Asset] {0}: nessun osso nel modello (serve uno scheletro con pesi per animarlo)", info.modelPath);
        if (out.boneInfoMap.find(CanonicalBonePrefix() + "Hips") == out.boneInfoMap.end() || out.boneInfoMap.find(CanonicalBonePrefix() + "RightHand") == out.boneInfoMap.end())
            SUN_CORE_WARN("[Asset] {0}: scheletro non riconosciuto (mancano Hips/RightHand Mixamo): le animazioni del gioco non si applicheranno", info.modelPath);

        // Verso del modello: i personaggi del gioco (Mixamo) guardano verso +Z. Un modello esportato girato al contrario
        // camminerebbe all'indietro: si gira la radice di 180 gradi (verso dedotto dalla punta dei piedi).
        {
            std::unordered_map<std::string, glm::mat4> g;
            struct Walk {
                std::unordered_map<std::string, glm::mat4>& g;
                void operator()(const AssimpNodeData& n, const glm::mat4& p) { const glm::mat4 m = p * n.transformation; g[n.name] = m; for (const auto& c : n.children) (*this)(c, m); }
            };
            Walk{ g }(out.rootNode, glm::mat4(1.0f));
            glm::vec3 fwd(0.0f);
            for (const char* side : { "Left", "Right" }) {
                auto f = g.find(CanonicalBonePrefix() + side + "Foot"), t = g.find(CanonicalBonePrefix() + side + "ToeBase");
                if (f != g.end() && t != g.end()) fwd += glm::vec3(t->second[3]) - glm::vec3(f->second[3]);
            }
            fwd.y = 0.0f;
            if (glm::length(fwd) > 1e-6f && fwd.z < 0.0f && std::abs(fwd.z) > std::abs(fwd.x)) {
                out.rootNode.transformation = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)) * out.rootNode.transformation;
                SUN_CORE_INFO("[Asset] {0}: modello girato di 180 gradi (guardava all'indietro)", fs::path(info.modelPath).filename().string());
            }
        }

        // Unita' del file: si sceglie fra cm, m, mm e pollici quella che da' un'altezza piu' vicina al corpo sostituito
        const float h = SkinnedModelHeight(out);
        float scale = 1.0f;
        if (h > 1e-4f) {
            if (targetHeightCm > 1.0f) {
                float best = 1e30f;
                for (float s : { 1.0f, 100.0f, 0.1f, 2.54f }) {
                    const float d = std::abs(std::log(h * s / targetHeightCm));
                    if (d < best) { best = d; scale = s; }
                }
            }
            else scale = h < 10.0f ? 100.0f : (h > 1000.0f ? 0.1f : 1.0f);
        }

        // model.json: scala, altezza e materiali indicati a mano
        nlohmann::json cfg;
        {
            std::error_code ec;
            const fs::path p = fs::path(info.folder) / "model.json";
            if (fs::is_regular_file(p, ec)) {
                try { std::ifstream in(p); cfg = nlohmann::json::parse(in, nullptr, true, true); }
                catch (...) { cfg = nlohmann::json(); }
            }
        }
        if (cfg.is_object()) {
            if (cfg.contains("height_m") && cfg["height_m"].is_number() && h > 1e-4f) scale = cfg["height_m"].get<float>() * 100.0f / h;
            if (cfg.contains("scale") && cfg["scale"].is_number()) scale *= cfg["scale"].get<float>();
            if (cfg.contains("rotate_y") && cfg["rotate_y"].is_number())   // gradi in piu' attorno all'asse verticale
                out.rootNode.transformation = glm::rotate(glm::mat4(1.0f), glm::radians(cfg["rotate_y"].get<float>()), glm::vec3(0.0f, 1.0f, 0.0f)) * out.rootNode.transformation;
            if (cfg.contains("materials") && cfg["materials"].is_object()) {
                TexCache cache;
                const fs::path dir(info.folder);
                for (auto it = cfg["materials"].begin(); it != cfg["materials"].end(); ++it) {
                    if (!it.value().is_object()) continue;
                    const nlohmann::json& mj = it.value();
                    bool found = false;
                    for (auto& m : out.materials) {
                        if (Key(m.name) != Key(it.key()) && it.key() != "*") continue;
                        found = true;
                        if (mj.contains("type") && mj["type"].is_string()) m.kind = KindFromName(mj["type"].get<std::string>());
                        if (mj.contains("tint") && mj["tint"].is_boolean()) m.tint = mj["tint"].get<bool>();
                        if (mj.contains("grime") && mj["grime"].is_number()) m.grime = std::clamp(mj["grime"].get<float>(), 0.0f, 2.0f);
                        if (mj.contains("alpha_cut") && mj["alpha_cut"].is_number()) m.alphaCut = mj["alpha_cut"].get<float>();
                        if (mj.contains("roughness") && mj["roughness"].is_number()) m.roughness = mj["roughness"].get<float>();
                        if (mj.contains("metallic") && mj["metallic"].is_number()) m.metallic = mj["metallic"].get<float>();
                        if (mj.contains("normal_strength") && mj["normal_strength"].is_number()) m.normalStrength = mj["normal_strength"].get<float>();
                        if (mj.contains("normal_directx") && mj["normal_directx"].is_boolean()) m.normalDirectX = mj["normal_directx"].get<bool>();
                        if (mj.contains("emissive_strength") && mj["emissive_strength"].is_number()) m.emissiveStrength = mj["emissive_strength"].get<float>();
                        if (mj.contains("hide") && mj["hide"].is_boolean()) m.hidden = mj["hide"].get<bool>();
                        glm::vec3 col(m.baseColor);
                        ApplyJsonVec(mj, "color", col); m.baseColor = glm::vec4(col, m.baseColor.a);
                        ApplyJsonVec(mj, "emissive_color", m.emissiveColor);
                        // texture indicate a mano (percorsi relativi alla cartella del modello)
                        MatSources src;
                        auto file = [&](const char* k, TexSource& slot) {
                            if (!mj.contains(k) || !mj[k].is_string()) return;
                            const fs::path p = dir / mj[k].get<std::string>();
                            std::error_code ec;
                            if (fs::is_regular_file(p, ec)) slot.file = p.string();
                            else SUN_CORE_WARN("[Asset] model.json, materiale '{0}': '{1}' non trovato", it.key(), p.string());
                        };
                        file("albedo", src.albedo); file("normal", src.normal); file("orm", src.orm); file("roughness_map", src.rough);
                        file("metallic_map", src.metal); file("ao", src.ao); file("emissive", src.emissive); file("opacity", src.opacity);
                        file("metal_rough", src.metalRough); file("gloss", src.gloss);
                        // le mappe nuove sostituiscono quelle trovate in automatico, le altre restano
                        SkinnedMaterial tmp = m;
                        tmp.albedo = tmp.normal = tmp.orm = tmp.emissive = nullptr;
                        std::string report;
                        Build(tmp, src, cache, true, report);
                        if (tmp.albedo) m.albedo = tmp.albedo;
                        if (tmp.normal) m.normal = tmp.normal;
                        if (tmp.orm) { m.orm = tmp.orm; if (!mj.contains("roughness")) m.roughness = 1.0f; if (!mj.contains("metallic")) m.metallic = 1.0f; }
                        if (tmp.emissive) m.emissive = tmp.emissive;
                        if (!report.empty()) SUN_CORE_INFO("[Asset] model.json, materiale '{0}':{1}", m.name, report);
                    }
                    if (!found) SUN_CORE_WARN("[Asset] model.json: nessun materiale si chiama '{0}' nel modello", it.key());
                }
            }
        }
        out.unitScale = scale;
        {
            float lo, hi;
            SkinnedBindBounds(out, lo, hi);
            if (h > 1e-4f && std::abs(lo) > 0.05f * h)
                SUN_CORE_WARN("[Asset] {0}: i piedi sono a {1:.2f} m dall'origine del modello (va messa fra i piedi, a terra)",
                    fs::path(info.modelPath).filename().string(), lo * scale / 100.0f);
        }
        SUN_CORE_INFO("[Asset] {0}: {1} sotto-mesh, {2} materiali, {3} ossa, altezza {4:.1f} unita' -> scala {5} ({6:.2f} m)",
            fs::path(info.modelPath).filename().string(), out.meshes.size(), out.materials.size(), out.boneInfoMap.size(), h, scale, h * scale / 100.0f);
        return true;
    }
}
