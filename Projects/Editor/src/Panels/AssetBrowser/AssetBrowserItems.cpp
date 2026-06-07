// FBZZ Engine
// AssetBrowserItems.cpp | fbzz::editor
// AssetBrowser のフォルダツリーとファイルアイコン描画
#include "AssetBrowserCommon.hpp"
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Scene/Systems/RenderPassContext.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>
#include <vector>

namespace fbzz::editor {
namespace {

//      未知の拡張子は拡張子文字列のハッシュから色を生成し、
//      追加のコード変更なしにどんなファイルでも識別色が付く。
struct ExtGroup {
    const char*  exts[6];   // 最大 6 拡張子。nullptr 終番。
    ImVec4       color;
    const char*  label;
};

static constexpr ExtGroup kExtGroups[] = {
    { { ".hlsl", ".hlsli", nullptr },                          { 0.15f, 0.65f, 0.25f, 1.0f }, "HLSL"    },
    { { ".hpp", ".cpp", ".h", ".c", ".cc", ".cxx" },            { 0.20f, 0.58f, 0.70f, 1.0f }, "CPP"     },
    { { ".png", ".jpg", ".jpeg", ".dds", ".bmp", ".tga" },     { 0.15f, 0.40f, 0.80f, 1.0f }, "TEX"     },
    { { ".fbx", ".obj", ".gltf", ".glb", nullptr },            { 0.80f, 0.45f, 0.10f, 1.0f }, "MESH"    },
    { { ".fbzzprefab", nullptr },                              { 0.25f, 0.65f, 0.75f, 1.0f }, "PREFAB"  },
    { { ".fbzzterrain", nullptr },                             { 0.35f, 0.70f, 0.30f, 1.0f }, "TERRAIN" },
    { { ".fbzz", nullptr },                                    { 0.60f, 0.15f, 0.70f, 1.0f }, "SCENE"   },
    { { ".fzasset", nullptr },                                 { 0.90f, 0.60f, 0.10f, 1.0f }, "ASSET"   },
    { { ".fzmesh", nullptr },                                  { 0.80f, 0.50f, 0.20f, 1.0f }, "MESH"    },
    { { ".fzmat", nullptr },                                   { 0.20f, 0.70f, 0.80f, 1.0f }, "MAT"     },
    { { ".fzskel", nullptr },                                  { 0.70f, 0.30f, 0.60f, 1.0f }, "SKEL"    },
    { { ".fzanim", nullptr },                                  { 0.20f, 0.75f, 0.35f, 1.0f }, "ANIM"    },
    { { ".toml", ".json", ".yaml", ".yml", nullptr },           { 0.65f, 0.65f, 0.10f, 1.0f }, "DATA"    },
    { { ".wav", ".mp3", ".ogg", ".flac", nullptr },             { 0.70f, 0.20f, 0.50f, 1.0f }, "SFX"     },
    { { ".ttf", ".otf", nullptr },                             { 0.60f, 0.30f, 0.85f, 1.0f }, "FONT"    },
    { { ".fnt", nullptr },                                     { 0.50f, 0.20f, 0.75f, 1.0f }, "FNT"     },
    { { ".txt", ".md", ".rst", nullptr },                      { 0.55f, 0.55f, 0.55f, 1.0f }, "TEXT"    },
    { { ".py", ".lua", ".cs", nullptr },                       { 0.20f, 0.70f, 0.55f, 1.0f }, "SCRIPT"  },
    { { ".lib", ".dll", ".a", nullptr },                       { 0.45f, 0.45f, 0.45f, 1.0f }, "LIB"     },
};

// 未知拡張子をハッシュで色付けする。
// WHAT: FNV-1a の下位ビットを色相に変換し、彩度・明度は固定で
//       読みやすい明るさに調整する。同じ拡張子なら常に同じ色になる。
static ImVec4 ColorFromExt(const std::string& ext)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : ext)
        h = (h ^ c) * 16777619u;
    const float hue = static_cast<float>(h & 0xFFFF) / 65536.0f; // 0..1
    // HSV → RGB (S=0.55, V=0.72)
    const float s = 0.55f, v = 0.72f;
    const float hi = std::fmodf(hue * 6.0f, 6.0f);
    const int   i  = static_cast<int>(hi);
    const float f  = hi - static_cast<float>(i);
    const float p  = v * (1.0f - s);
    const float q  = v * (1.0f - s * f);
    const float t  = v * (1.0f - s * (1.0f - f));
    switch (i % 6) {
    case 0: return { v, t, p, 1.0f };
    case 1: return { q, v, p, 1.0f };
    case 2: return { p, v, t, 1.0f };
    case 3: return { p, q, v, 1.0f };
    case 4: return { t, p, v, 1.0f };
    default:return { v, p, q, 1.0f };
    }
}

// 拡張子がグループに含まれるか確認する。
static const ExtGroup* FindGroup(const std::string& ext)
{
    for (const auto& g : kExtGroups) {
        for (int i = 0; i < 6 && g.exts[i]; ++i)
            if (ext == g.exts[i]) return &g;
    }
    return nullptr;
}

static bool IsTextureExt(const std::string& ext)
{
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".dds" || ext == ".bmp" || ext == ".tga";
}

static bool IsMeshExt(const std::string& ext)
{
    return ext == ".fbx" || ext == ".obj" || ext == ".gltf" ||
           ext == ".glb" || ext == ".fzmesh";
}

static std::filesystem::file_time_type ReadLastWriteTime(const std::string& path)
{
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(util::FileSystem::PathFromUtf8(path), ec);
    return ec ? std::filesystem::file_time_type{} : time;
}

static std::string ToTextureLoadPath(const std::string& path, const EditorContext& ctx)
{
    const std::string normalized = util::FileSystem::NormalizePathSeparators(path);
    const std::string projectRoot = util::FileSystem::NormalizePathSeparators(ctx.projectRoot);

    // WHY: .fzmat 内のテクスチャ参照は Assets/ 相対で保存されるため、
    //      ResourceManager が読める実ファイルパスに変換してからサムネイルを読み込む。
    if (normalized.starts_with("Assets/") && !projectRoot.empty()) {
        return projectRoot + "/" + normalized;
    }
    return normalized;
}

static ImTextureID ToImTextureID(void* ptr)
{
    // WHY: このプロジェクトの ImGui は ImTextureID を ImU64 として扱う。
    //      void* のビット列を整数 ID に移すだけなので、所有権や型変換の意味を持たせない。
    return static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(ptr));
}

static std::string SelectMaterialPreviewTexture(const asset::MaterialAsset& mat)
{
    static constexpr const char* PRIORITY_SLOTS[] = {
        "albedo", "base_color", "diffuse", "layer0_diffuse", "foamTex"
    };
    for (const char* slot : PRIORITY_SLOTS) {
        if (auto it = mat.textures.find(slot); it != mat.textures.end() && !it->second.empty()) {
            return it->second;
        }
    }
    for (const auto& [slot, path] : mat.textures) {
        if (!path.empty()) return path;
    }
    return {};
}

static ImVec4 SelectMaterialColor(const asset::MaterialAsset& mat)
{
    const auto findColor = [&mat]() -> const std::vector<float>* {
        if (auto it = mat.params.find("base_color"); it != mat.params.end() && it->second.size() >= 3) return &it->second;
        if (auto it = mat.params.find("baseColor"); it != mat.params.end() && it->second.size() >= 3) return &it->second;
        if (auto it = mat.params.find("albedo"); it != mat.params.end() && it->second.size() >= 3) return &it->second;
        return nullptr;
    };

    if (const std::vector<float>* values = findColor()) {
        const float alpha = values->size() >= 4 ? (*values)[3] : 1.0f;
        return { (*values)[0], (*values)[1], (*values)[2], alpha };
    }
    return { 0.20f, 0.70f, 0.80f, 1.0f };
}

enum class ThumbnailShaderFlavor {
    Surface,
    Skinned,
    Terrain,
    Water,
};

static std::string ToLowerAssetPath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return path;
}

static ThumbnailShaderFlavor DetectThumbnailShaderFlavor(std::string_view shaderPath)
{
    const std::string lower = ToLowerAssetPath(std::string(shaderPath));
    if (lower.find("/water/") != std::string::npos) return ThumbnailShaderFlavor::Water;
    if (lower.find("/terrain/") != std::string::npos) return ThumbnailShaderFlavor::Terrain;
    if (lower.find("/material/skinned/") != std::string::npos) return ThumbnailShaderFlavor::Skinned;
    return ThumbnailShaderFlavor::Surface;
}

// t0-t4 は標準 Material スロット。t5-t7 はカスタムシェーダー用の汎用名。
// WHY: サムネイルでも Scene 描画と同じ .fzmat のテクスチャ割り当てを再現する。
constexpr std::array<const char*, 8> kMaterialTextureSlotNames = {
    "albedo",
    "normal",
    "metallic",
    "emissive",
    "ao",
    "tex5",
    "tex6",
    "tex7",
};

static const std::vector<float>* FindMaterialParam(const asset::MaterialAsset& asset, std::string_view shaderVarName)
{
    auto it = asset.params.find(std::string(shaderVarName));
    if (it != asset.params.end()) return &it->second;

    // WHY: .fzmat は PBR 寄りの名前、HLSL は shader ごとの短い変数名を使う場合がある。
    //      サムネイルも本編描画と同じ別名吸収を行い、shaderPath を変えても色や係数を反映する。
    if (shaderVarName == "albedo")              it = asset.params.find("base_color");
    else if (shaderVarName == "metallic")       it = asset.params.find("metallic_factor");
    else if (shaderVarName == "roughness")      it = asset.params.find("roughness_factor");
    else if (shaderVarName == "normalStrength") it = asset.params.find("normal_strength");
    else if (shaderVarName == "emissiveColor")  it = asset.params.find("emissive_color");
    else if (shaderVarName == "emissiveScale")  it = asset.params.find("emissive_scale");

    return it != asset.params.end() ? &it->second : nullptr;
}

static float MaterialParamFloat(const asset::MaterialAsset* asset, std::string_view name, float fallback)
{
    if (!asset) return fallback;
    const auto* values = FindMaterialParam(*asset, name);
    return (values && !values->empty()) ? (*values)[0] : fallback;
}

static math::Vector3 MaterialParamFloat3(const asset::MaterialAsset* asset, std::string_view name, math::Vector3 fallback)
{
    if (!asset) return fallback;
    const auto* values = FindMaterialParam(*asset, name);
    if (!values || values->size() < 3) return fallback;
    return { (*values)[0], (*values)[1], (*values)[2] };
}

static void InitDefaultMaterialParams(const renderer::ShaderDescriptor& desc, std::vector<uint8_t>& paramData)
{
    const float one = 1.0f;
    for (const auto& v : desc.vars) {
        if (v.varType != renderer::ShaderVarType::Float) continue;
        for (uint32_t col = 0; col < v.columns; ++col) {
            const uint32_t byteOff = v.offset + col * sizeof(float);
            if (byteOff + sizeof(float) <= static_cast<uint32_t>(paramData.size()))
                std::memcpy(paramData.data() + byteOff, &one, sizeof(float));
        }
    }

    auto setFloat = [&](std::string_view name, float value) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns != 1) return;
        if (v->offset + sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, &value, sizeof(float));
    };
    auto setFloat2 = [&](std::string_view name, const float value[2]) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns < 2) return;
        if (v->offset + 2u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, value, 2u * sizeof(float));
    };
    auto setFloat3 = [&](std::string_view name, const float value[3]) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns < 3) return;
        if (v->offset + 3u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, value, 3u * sizeof(float));
    };

    const float uvTiling[2] = { 1.0f, 1.0f };
    const float uvOffset[2] = { 0.0f, 0.0f };
    const float white3[3] = { 1.0f, 1.0f, 1.0f };
    setFloat("metallic", 0.0f);
    setFloat("roughness", 0.65f);
    setFloat("emissiveScale", 0.0f);
    setFloat("alphaCutoff", 0.5f);
    setFloat2("uvTiling", uvTiling);
    setFloat2("uvOffset", uvOffset);
    setFloat3("emissiveColor", white3);
}

static void ApplyMaterialAssetParams(const asset::MaterialAsset& asset,
                                     const renderer::ShaderDescriptor& desc,
                                     std::vector<uint8_t>& paramData)
{
    for (const auto& v : desc.vars) {
        if (v.varType != renderer::ShaderVarType::Float) continue;
        if (v.offset + v.size > static_cast<uint32_t>(paramData.size())) continue;

        const auto* values = FindMaterialParam(asset, v.name);
        if (!values || values->empty()) continue;

        const size_t count = std::min<size_t>(v.columns, values->size());
        std::memcpy(paramData.data() + v.offset, values->data(), count * sizeof(float));
    }
}

static std::string FindMaterialTexturePath(const asset::MaterialAsset& asset,
                                           const renderer::ShaderTexBindDesc& bind)
{
    auto byShaderName = asset.textures.find(bind.name);
    if (byShaderName != asset.textures.end()) return byShaderName->second;

    const std::string lowerName = ToLowerAssetPath(bind.name);
    const auto findTexture = [&asset](const char* name) -> std::string {
        auto it = asset.textures.find(name);
        return it != asset.textures.end() ? it->second : std::string{};
    };
    if (lowerName == "g_normalmap1") return findTexture("normalMap1");
    if (lowerName == "g_normalmap2") return findTexture("normalMap2");
    if (lowerName == "g_foamtex")    return findTexture("foamTex");
    if (lowerName == "g_foammask")   return findTexture("foamMask");
    if (lowerName == "g_envtex")     return findTexture("envCubemap");
    if (lowerName == "g_flowmap")    return findTexture("flowMap");
    if (lowerName == "g_splatmap")   return findTexture("splatmap");
    if (lowerName.starts_with("g_diffuse") && bind.slot >= 1 && bind.slot <= 4) {
        const std::string slot = "layer" + std::to_string(bind.slot - 1) + "_diffuse";
        auto it = asset.textures.find(slot);
        return it != asset.textures.end() ? it->second : std::string{};
    }
    if (lowerName.starts_with("g_normal") && bind.slot >= 5 && bind.slot <= 8) {
        const std::string slot = "layer" + std::to_string(bind.slot - 5) + "_normal";
        auto it = asset.textures.find(slot);
        return it != asset.textures.end() ? it->second : std::string{};
    }
    if (lowerName.starts_with("g_aoroughness") && bind.slot >= 9 && bind.slot <= 12) {
        const std::string slot = "layer" + std::to_string(bind.slot - 9) + "_ao_roughness";
        auto it = asset.textures.find(slot);
        return it != asset.textures.end() ? it->second : std::string{};
    }

    if (bind.slot < kMaterialTextureSlotNames.size()) {
        auto byStandardName = asset.textures.find(kMaterialTextureSlotNames[bind.slot]);
        if (byStandardName != asset.textures.end()) return byStandardName->second;
    }
    return {};
}

} // namespace

bool AssetBrowserPanel::RebuildMaterialThumbnailGpuData(MaterialPreview& preview, EditorContext& ctx)
{
    if (!ctx.resources) return false;

    const bool useFallbackMaterial = preview.asset.shaderPath.empty();
    asset::MaterialAsset fallbackAsset;
    const asset::MaterialAsset* renderAsset = &preview.asset;
    if (useFallbackMaterial) {
        fallbackAsset.shaderPath = "Assets/Shaders/Material/Surface/Fallback.hlsl";
        fallbackAsset.params["albedo"] = { 1.0f, 0.0f, 1.0f, 1.0f };
        renderAsset = &fallbackAsset;
    }

    const std::string nextShaderPath = renderAsset->shaderPath.empty()
        ? "Assets/Shaders/Material/Surface/Fallback.hlsl"
        : renderAsset->shaderPath;
    if (nextShaderPath != preview.shaderPath) {
        preview.shaderPath = nextShaderPath;
        preview.shader = {};
        preview.materialCB = {};
        preview.textures.clear();
        preview.paramData.clear();
    }

    if (!preview.shader.IsValid())
        preview.shader = ctx.resources->LoadShader(preview.shaderPath);
    auto* shader = ctx.resources->Get(preview.shader);
    if (!shader) return false;

    const renderer::ShaderDescriptor& desc = shader->GetDescriptor();
    const ThumbnailShaderFlavor flavor = DetectThumbnailShaderFlavor(preview.shaderPath);
    if (!desc.IsValid() && flavor != ThumbnailShaderFlavor::Terrain) return false;

    preview.textures.assign(16, {});
    if (!desc.textures.empty()) {
        for (const auto& bind : desc.textures) {
            if (bind.slot >= preview.textures.size()) continue;
            const std::string texturePath = FindMaterialTexturePath(*renderAsset, bind);
            if (!texturePath.empty())
                preview.textures[bind.slot] = ctx.resources->LoadTexture(ToTextureLoadPath(texturePath, ctx));
        }
    }
    if (flavor == ThumbnailShaderFlavor::Terrain) {
        const auto loadSlot = [&](uint32_t slot, const char* name) {
            auto it = renderAsset->textures.find(name);
            if (it != renderAsset->textures.end() && !it->second.empty())
                preview.textures[slot] = ctx.resources->LoadTexture(ToTextureLoadPath(it->second, ctx));
        };
        loadSlot(0, "splatmap");
        for (uint32_t layer = 0; layer < 4; ++layer) {
            const std::string prefix = "layer" + std::to_string(layer);
            loadSlot(1 + layer, (prefix + "_diffuse").c_str());
            loadSlot(5 + layer, (prefix + "_normal").c_str());
            loadSlot(9 + layer, (prefix + "_ao_roughness").c_str());
        }
    }
    if (!desc.IsValid()) {
        preview.materialCB = {};
        preview.paramData.clear();
        return true;
    }

    preview.paramData.assign(desc.cbufferSize, 0u);
    InitDefaultMaterialParams(desc, preview.paramData);
    ApplyMaterialAssetParams(*renderAsset, desc, preview.paramData);

    if (desc.textureMaskOffset != UINT32_MAX &&
        desc.textureMaskOffset + sizeof(uint32_t) <= preview.paramData.size()) {
        uint32_t mask = 0;
        for (const auto& bind : desc.textures) {
            if (bind.slot >= preview.textures.size()) continue;
            const std::string texturePath = FindMaterialTexturePath(*renderAsset, bind);
            if (texturePath.empty()) continue;
            preview.textures[bind.slot] = ctx.resources->LoadTexture(ToTextureLoadPath(texturePath, ctx));
            if (preview.textures[bind.slot].IsValid() && bind.slot < 8)
                mask |= (1u << bind.slot);
        }
        std::memcpy(preview.paramData.data() + desc.textureMaskOffset, &mask, sizeof(uint32_t));
    } else {
        for (const auto& bind : desc.textures) {
            if (bind.slot >= preview.textures.size()) continue;
            const std::string texturePath = FindMaterialTexturePath(*renderAsset, bind);
            if (!texturePath.empty())
                preview.textures[bind.slot] = ctx.resources->LoadTexture(ToTextureLoadPath(texturePath, ctx));
        }
    }

    if (!preview.materialCB.IsValid())
        preview.materialCB = ctx.resources->CreateConstantBuffer(desc.cbufferSize);
    if (!preview.materialCB.IsValid()) return false;
    ctx.resources->Update(preview.materialCB, preview.paramData.data(), preview.paramData.size());
    return true;
}

namespace {

static void DrawThumbnailFrame(ImVec2 origin, float sz, bool hovered)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = hovered ? IM_COL32(42, 45, 52, 255) : IM_COL32(30, 32, 38, 255);
    dl->AddRectFilled(origin, { origin.x + sz, origin.y + sz }, bg, 4.0f);
    dl->AddRect(origin, { origin.x + sz, origin.y + sz }, IM_COL32(95, 100, 112, 230), 4.0f, 0, 1.0f);
}

static void DrawTextureThumbnail(void* rawID, uint32_t width, uint32_t height, ImVec2 origin, float sz, bool hovered)
{
    DrawThumbnailFrame(origin, sz, hovered);
    const float frameH = sz;
    const float w = static_cast<float>(std::max<uint32_t>(1, width));
    const float h = static_cast<float>(std::max<uint32_t>(1, height));
    const float scale = std::min((sz - 8.0f) / w, (frameH - 8.0f) / h);
    const ImVec2 imageSize = { std::max(1.0f, w * scale), std::max(1.0f, h * scale) };
    const ImVec2 imageMin = {
        origin.x + (sz - imageSize.x) * 0.5f,
        origin.y + (frameH - imageSize.y) * 0.5f
    };
    ImGui::GetWindowDrawList()->AddImage(
        ToImTextureID(rawID),
        imageMin,
        { imageMin.x + imageSize.x, imageMin.y + imageSize.y });
}

struct ThumbnailMaterialCB {
    math::Vector4 albedo = math::Vector4::WHITE;
    uint32_t textureMask = 0;
    float _pad[3] = {};
};

struct ThumbnailTerrainCB {
    math::Matrix4 worldMatrix;
    math::Matrix4 wvpMatrix;
    math::Vector4 layerTiling[4];
    math::Vector4 layerNormalStrength;
    math::Vector4 layerMaterial[4];
    math::Vector4 layerTextureFlags;
    math::Vector4 layerAutoHeight[4];
    math::Vector4 layerAutoSlope[4];
};

struct ThumbnailWaterCB {
    math::Matrix4 worldMatrix;
    math::Matrix4 wvpMatrix;
    math::Vector4 shallowColorDepth;
    math::Vector4 deepColorDepth;
    math::Vector4 surfaceParams;
    math::Vector4 normalMap1Params;
    math::Vector4 normalMap2Params;
    math::Vector4 foamParams;
    math::Vector4 refractionFlowParams;
    math::Vector4 waveDir[4];
    math::Vector4 waveParams[4];
};

struct ThumbnailSkinningCB {
    math::Matrix4 boneMatrices[128];
};

struct ThumbnailWaterVertex {
    math::Vector3 position;
    math::Vector2 uv;
};

static std::shared_ptr<renderer::Mesh> CreateSkinnedPreviewSphere(renderer::ResourceManager& resources, int segments)
{
    auto surface = renderer::PrimitiveMesh::Sphere(resources, segments);
    if (!surface) return {};

    std::vector<renderer::SkinnedVertex> verts;
    verts.reserve(surface->cpuVertices.size());
    for (const auto& v : surface->cpuVertices) {
        renderer::SkinnedVertex sv{};
        sv.position = v.position;
        sv.normal = v.normal;
        sv.tangent = v.tangent;
        sv.uv = v.uv;
        sv.boneIndices[0] = 0;
        sv.boneWeights[0] = 1.0f;
        verts.push_back(sv);
    }

    auto mesh = std::make_shared<renderer::Mesh>();
    mesh->vertexBuffer = resources.CreateVertexBuffer(verts.data(), verts.size() * sizeof(renderer::SkinnedVertex), sizeof(renderer::SkinnedVertex));
    mesh->indexBuffer = resources.CreateIndexBuffer(surface->cpuIndices.data(), static_cast<uint32_t>(surface->cpuIndices.size()));
    mesh->vertexCount = static_cast<uint32_t>(verts.size());
    mesh->indexCount = static_cast<uint32_t>(surface->cpuIndices.size());
    mesh->isSkinned = true;
    mesh->cpuSkinnedVertices = std::move(verts);
    mesh->cpuIndices = surface->cpuIndices;
    mesh->ComputeBounds();
    return mesh;
}

static std::shared_ptr<renderer::Mesh> CreateWaterPreviewSphere(renderer::ResourceManager& resources, int segments)
{
    auto surface = renderer::PrimitiveMesh::Sphere(resources, segments);
    if (!surface) return {};

    std::vector<ThumbnailWaterVertex> verts;
    verts.reserve(surface->cpuVertices.size());
    for (const auto& v : surface->cpuVertices)
        verts.push_back({ v.position, v.uv });

    auto mesh = std::make_shared<renderer::Mesh>();
    mesh->vertexBuffer = resources.CreateVertexBuffer(verts.data(), verts.size() * sizeof(ThumbnailWaterVertex), sizeof(ThumbnailWaterVertex));
    mesh->indexBuffer = resources.CreateIndexBuffer(surface->cpuIndices.data(), static_cast<uint32_t>(surface->cpuIndices.size()));
    mesh->vertexCount = static_cast<uint32_t>(verts.size());
    mesh->indexCount = static_cast<uint32_t>(surface->cpuIndices.size());
    mesh->cpuVertices = surface->cpuVertices;
    mesh->cpuIndices = surface->cpuIndices;
    mesh->ComputeBounds();
    return mesh;
}

static math::Vector3 MeshBoundsCenter(const renderer::Mesh& mesh)
{
    if (mesh.boundsRadius > 0.0f) return mesh.boundsCenter;

    math::Vector3 minP{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    math::Vector3 maxP{
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()
    };
    auto visit = [&](const math::Vector3& p) {
        minP.x = std::min(minP.x, p.x);
        minP.y = std::min(minP.y, p.y);
        minP.z = std::min(minP.z, p.z);
        maxP.x = std::max(maxP.x, p.x);
        maxP.y = std::max(maxP.y, p.y);
        maxP.z = std::max(maxP.z, p.z);
    };
    if (mesh.isSkinned) {
        for (const auto& v : mesh.cpuSkinnedVertices) visit(v.position);
    } else {
        for (const auto& v : mesh.cpuVertices) visit(v.position);
    }
    if (minP.x > maxP.x) return {};
    return (minP + maxP) * 0.5f;
}

static float MeshBoundsRadius(const renderer::Mesh& mesh, const math::Vector3& center)
{
    if (mesh.boundsRadius > 0.0f) return mesh.boundsRadius;

    float radiusSq = 0.0f;
    auto visit = [&](const math::Vector3& p) {
        radiusSq = std::max(radiusSq, (p - center).LengthSq());
    };
    if (mesh.isSkinned) {
        for (const auto& v : mesh.cpuSkinnedVertices) visit(v.position);
    } else {
        for (const auto& v : mesh.cpuVertices) visit(v.position);
    }
    return std::sqrt(std::max(radiusSq, 0.0001f));
}

static bool EnsureThumbnailDefaultTextures(renderer::ResourceManager& resources,
                                           renderer::ResourceHandle<renderer::TextureTag>& white,
                                           renderer::ResourceHandle<renderer::TextureTag>& black,
                                           renderer::ResourceHandle<renderer::TextureTag>& flatNormal)
{
    if (!white.IsValid()) {
        const uint8_t rgba[4] = { 255, 255, 255, 255 };
        white = resources.CreateTexture(rgba, 1, 1);
    }
    if (!black.IsValid()) {
        const uint8_t rgba[4] = { 0, 0, 0, 255 };
        black = resources.CreateTexture(rgba, 1, 1);
    }
    if (!flatNormal.IsValid()) {
        const uint8_t rgba[4] = { 128, 128, 255, 255 };
        flatNormal = resources.CreateTexture(rgba, 1, 1);
    }
    return white.IsValid() && black.IsValid() && flatNormal.IsValid();
}

static renderer::ResourceHandle<renderer::TextureTag> DefaultThumbnailTextureForSlot(
    ThumbnailShaderFlavor flavor,
    uint32_t slot,
    renderer::ResourceHandle<renderer::TextureTag> white,
    renderer::ResourceHandle<renderer::TextureTag> black,
    renderer::ResourceHandle<renderer::TextureTag> flatNormal)
{
    if (flavor == ThumbnailShaderFlavor::Water) {
        if (slot == 0 || slot == 1 || slot == 7) return flatNormal;
        if (slot == 4 || slot == 6 || slot == 8) return black;
        return white;
    }
    if (flavor == ThumbnailShaderFlavor::Terrain) {
        if (slot >= 5 && slot <= 8) return flatNormal;
        return white;
    }
    return {};
}

static ThumbnailTerrainCB BuildThumbnailTerrainCB(
    const asset::MaterialAsset* asset,
    const math::Matrix4& viewProjection)
{
    ThumbnailTerrainCB cb{};
    cb.worldMatrix = math::Matrix4::Identity();
    cb.wvpMatrix = viewProjection;
    for (int i = 0; i < 4; ++i) {
        const std::string prefix = "layer" + std::to_string(i) + "_";
        cb.layerTiling[i] = {
            MaterialParamFloat(asset, prefix + "tilingX", 2.0f),
            MaterialParamFloat(asset, prefix + "tilingZ", 2.0f),
            0.0f,
            0.0f
        };
        const float normalStrength = MaterialParamFloat(asset, prefix + "normalStrength", 1.0f);
        if (i == 0) cb.layerNormalStrength.x = normalStrength;
        else if (i == 1) cb.layerNormalStrength.y = normalStrength;
        else if (i == 2) cb.layerNormalStrength.z = normalStrength;
        else cb.layerNormalStrength.w = normalStrength;
        cb.layerMaterial[i] = {
            MaterialParamFloat(asset, prefix + "roughness", 0.8f),
            MaterialParamFloat(asset, prefix + "ambientOcclusion", 1.0f),
            0.0f,
            0.0f
        };
        cb.layerAutoHeight[i] = {
            MaterialParamFloat(asset, prefix + "autoMinHeight", -10000.0f),
            MaterialParamFloat(asset, prefix + "autoMaxHeight", 10000.0f),
            MaterialParamFloat(asset, prefix + "autoHeightFade", 1.0f),
            MaterialParamFloat(asset, prefix + "autoBlendEnabled", 0.0f)
        };
        cb.layerAutoSlope[i] = {
            MaterialParamFloat(asset, prefix + "autoMinSlope", 0.0f),
            MaterialParamFloat(asset, prefix + "autoMaxSlope", 1.0f),
            MaterialParamFloat(asset, prefix + "autoSlopeFade", 0.1f),
            MaterialParamFloat(asset, prefix + "autoBlendStrength", 1.0f)
        };
    }
    return cb;
}

static ThumbnailWaterCB BuildThumbnailWaterCB(
    const asset::MaterialAsset* asset,
    const math::Matrix4& viewProjection)
{
    ThumbnailWaterCB cb{};
    cb.worldMatrix = math::Matrix4::Identity();
    cb.wvpMatrix = viewProjection;
    const math::Vector3 shallow = MaterialParamFloat3(asset, "shallowColor", { 0.20f, 0.60f, 0.70f });
    const math::Vector3 deep = MaterialParamFloat3(asset, "deepColor", { 0.00f, 0.10f, 0.30f });
    cb.shallowColorDepth = { shallow.x, shallow.y, shallow.z, MaterialParamFloat(asset, "shallowDepth", 0.5f) };
    cb.deepColorDepth = { deep.x, deep.y, deep.z, MaterialParamFloat(asset, "deepDepth", 5.0f) };
    cb.surfaceParams = {
        MaterialParamFloat(asset, "opacity", 0.85f),
        MaterialParamFloat(asset, "reflectivity", 0.35f),
        MaterialParamFloat(asset, "fresnelBias", 0.02f),
        MaterialParamFloat(asset, "fresnelPower", 5.0f)
    };
    cb.normalMap1Params = { 0.015f, 0.010f, MaterialParamFloat(asset, "normalMap1Tiling", 3.0f), MaterialParamFloat(asset, "normalStrength", 0.75f) };
    cb.normalMap2Params = { -0.010f, 0.015f, MaterialParamFloat(asset, "normalMap2Tiling", 5.0f), 0.35f };
    cb.foamParams = {
        MaterialParamFloat(asset, "foamThreshold", 0.3f),
        MaterialParamFloat(asset, "foamFade", 0.5f),
        MaterialParamFloat(asset, "foamStrength", 0.6f),
        MaterialParamFloat(asset, "foamTiling", 5.0f)
    };
    cb.refractionFlowParams = {
        MaterialParamFloat(asset, "refractionStrength", 0.02f),
        MaterialParamFloat(asset, "flowSpeed", 0.3f),
        MaterialParamFloat(asset, "flowTiling", 1.0f),
        0.0f
    };
    return cb;
}

static bool EnsureThumbnailGpuResources(
    renderer::ResourceManager& resources,
    renderer::ResourceHandle<renderer::ShaderTag>& shader,
    renderer::ResourceHandle<renderer::PipelineStateTag>& pso,
    renderer::ResourceHandle<renderer::ConstantBufferTag>& frameCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag>& objectCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag>& materialCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag>& lightCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag>& shadowCB,
    bool requireFallbackMaterial)
{
    if (requireFallbackMaterial && !shader.IsValid())
        shader = resources.LoadShader("Assets/Shaders/Material/Surface/Lit.hlsl");
    if (requireFallbackMaterial && !shader.IsValid()) return false;

    if (!pso.IsValid()) {
        pso = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND,
            renderer::DepthMode::DEPTH_ON
        });
    }
    if (!frameCB.IsValid())
        frameCB = resources.CreateConstantBuffer(sizeof(scene::PerFrameCB));
    if (!objectCB.IsValid())
        objectCB = resources.CreateConstantBuffer(sizeof(scene::PerObjectCB));
    if (requireFallbackMaterial && !materialCB.IsValid())
        materialCB = resources.CreateConstantBuffer(sizeof(ThumbnailMaterialCB));
    if (!lightCB.IsValid())
        lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    if (!shadowCB.IsValid())
        shadowCB = resources.CreateConstantBuffer(sizeof(scene::ShadowConstantsCB));

    return pso.IsValid() && frameCB.IsValid() && objectCB.IsValid() &&
           (!requireFallbackMaterial || materialCB.IsValid()) && lightCB.IsValid() && shadowCB.IsValid();
}

static bool RenderMeshThumbnail(
    renderer::IRenderer& renderer,
    renderer::ResourceManager& resources,
    const renderer::Mesh& mesh,
    renderer::ResourceHandle<renderer::RenderTargetTag> rt,
    renderer::ResourceHandle<renderer::TextureTag> albedoTexture,
    ImVec4 albedoColor,
    renderer::ResourceHandle<renderer::ShaderTag> materialShader = {},
    renderer::ResourceHandle<renderer::ConstantBufferTag> materialCB = {},
    const std::vector<renderer::ResourceHandle<renderer::TextureTag>>* materialTextures = nullptr,
    ThumbnailShaderFlavor flavor = ThumbnailShaderFlavor::Surface,
    const asset::MaterialAsset* materialAsset = nullptr)
{
    static renderer::ResourceHandle<renderer::ShaderTag> s_shader;
    static renderer::ResourceHandle<renderer::PipelineStateTag> s_pso;
    static renderer::ResourceHandle<renderer::ConstantBufferTag> s_frameCB;
    static renderer::ResourceHandle<renderer::ConstantBufferTag> s_objectCB;
    static renderer::ResourceHandle<renderer::ConstantBufferTag> s_materialCB;
    static renderer::ResourceHandle<renderer::ConstantBufferTag> s_lightCB;
    static renderer::ResourceHandle<renderer::ConstantBufferTag> s_shadowCB;
    static renderer::ResourceHandle<renderer::ConstantBufferTag> s_terrainObjectCB;
    static renderer::ResourceHandle<renderer::ConstantBufferTag> s_waterObjectCB;
    static renderer::ResourceHandle<renderer::ConstantBufferTag> s_skinningCB;
    static renderer::ResourceHandle<renderer::TextureTag> s_whiteTexture;
    static renderer::ResourceHandle<renderer::TextureTag> s_blackTexture;
    static renderer::ResourceHandle<renderer::TextureTag> s_flatNormalTexture;

    if (!rt.IsValid() || !mesh.vertexBuffer.IsValid() || !mesh.indexBuffer.IsValid())
        return false;
    const bool useMaterialOverride = materialShader.IsValid();
    if (!EnsureThumbnailGpuResources(
            resources,
            s_shader,
            s_pso,
            s_frameCB,
            s_objectCB,
            s_materialCB,
            s_lightCB,
            s_shadowCB,
            !useMaterialOverride))
        return false;

    const math::Vector3 center = MeshBoundsCenter(mesh);
    const float radius = std::max(0.0001f, MeshBoundsRadius(mesh, center));
    const float cameraDistance = radius * 4.0f;

    renderer::Camera camera;
    // WHY: Unity の Material Preview に近い、少し上からの 3/4 ビューにする。
    //      真正面よりも球のハイライト・影・輪郭が読み取りやすくなる。
    camera.m_position = {
        center.x - radius * 2.12f,
        center.y + radius * 1.28f,
        center.z - cameraDistance
    };
    camera.m_aspect = 1.0f;
    camera.m_fovY = 30.0f;
    camera.m_near = 0.01f;
    camera.m_far = std::max(10.0f, cameraDistance + radius * 6.0f);
    camera.LookAt(center);

    scene::PerFrameCB frameData{};
    frameData.view = camera.GetViewMatrix();
    frameData.projection = camera.GetProjectionMatrix();
    frameData.viewProjection = camera.GetViewProjection();
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos = camera.m_position;
    frameData.nearZ = camera.m_near;
    frameData.farZ = camera.m_far;
    resources.Update(s_frameCB, &frameData, sizeof(frameData));

    scene::PerObjectCB objectData{};
    objectData.world = math::Matrix4::Identity();
    objectData.worldInvTranspose = math::Matrix4::Identity();
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCBForDraw = s_objectCB;
    if (flavor == ThumbnailShaderFlavor::Terrain) {
        if (!s_terrainObjectCB.IsValid())
            s_terrainObjectCB = resources.CreateConstantBuffer(sizeof(ThumbnailTerrainCB));
        if (!s_terrainObjectCB.IsValid()) return false;
        const ThumbnailTerrainCB terrainData = BuildThumbnailTerrainCB(materialAsset, frameData.viewProjection);
        resources.Update(s_terrainObjectCB, &terrainData, sizeof(terrainData));
        objectCBForDraw = s_terrainObjectCB;
    } else if (flavor == ThumbnailShaderFlavor::Water) {
        if (!s_waterObjectCB.IsValid())
            s_waterObjectCB = resources.CreateConstantBuffer(sizeof(ThumbnailWaterCB));
        if (!s_waterObjectCB.IsValid()) return false;
        const ThumbnailWaterCB waterData = BuildThumbnailWaterCB(materialAsset, frameData.viewProjection);
        resources.Update(s_waterObjectCB, &waterData, sizeof(waterData));
        objectCBForDraw = s_waterObjectCB;
    } else {
        resources.Update(s_objectCB, &objectData, sizeof(objectData));
    }

    if (flavor == ThumbnailShaderFlavor::Skinned) {
        if (!s_skinningCB.IsValid())
            s_skinningCB = resources.CreateConstantBuffer(sizeof(ThumbnailSkinningCB));
        if (!s_skinningCB.IsValid()) return false;
        ThumbnailSkinningCB skinningData{};
        for (auto& bone : skinningData.boneMatrices)
            bone = math::Matrix4::Identity();
        resources.Update(s_skinningCB, &skinningData, sizeof(skinningData));
    }

    if (!useMaterialOverride) {
        ThumbnailMaterialCB materialData{};
        materialData.albedo = {
            std::clamp(albedoColor.x, 0.0f, 1.0f),
            std::clamp(albedoColor.y, 0.0f, 1.0f),
            std::clamp(albedoColor.z, 0.0f, 1.0f),
            std::clamp(albedoColor.w, 0.0f, 1.0f)
        };
        materialData.textureMask = albedoTexture.IsValid() ? 1u : 0u;
        resources.Update(s_materialCB, &materialData, sizeof(materialData));
    }

    renderer::LightConstantsCB lightData{};
    const math::Vector3 keyLight = (camera.m_position + math::Vector3{ radius * 1.4f, radius * 1.8f, radius * 0.8f } - center).Normalized();
    lightData.lightDir = { -keyLight.x, -keyLight.y, -keyLight.z };
    lightData.lightColor = { 1.0f, 0.97f, 0.92f };
    lightData.lightIntensity = 1.65f;
    lightData.ambientColor = { 0.24f, 0.27f, 0.31f };
    resources.Update(s_lightCB, &lightData, sizeof(lightData));

    scene::ShadowConstantsCB shadowData{};
    shadowData.lightViewProjection = math::Matrix4::Translate({ 16.0f, 16.0f, 0.0f });
    shadowData.shadowMapTexelSize[0] = 0.0f;
    shadowData.shadowMapTexelSize[1] = 0.0f;
    shadowData.shadowBias = 1.0f;
    resources.Update(s_shadowCB, &shadowData, sizeof(shadowData));

    renderer.SetRenderTarget(rt, resources);
    renderer.Clear({ 0.030f, 0.032f, 0.038f, 1.0f });
    renderer.ClearDepth();
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::CLAMP_LINEAR);
    renderer.SetSampler(2, renderer::SamplerMode::CLAMP_ANISOTROPIC);

    renderer::DrawCall dc;
    dc.vertexBuffer = mesh.vertexBuffer;
    dc.indexBuffer = mesh.indexBuffer;
    dc.indexCount = mesh.indexCount;
    dc.vertexCount = mesh.vertexCount;
    dc.shader = useMaterialOverride ? materialShader : s_shader;
    dc.pipelineState = s_pso;
    dc.constantBuffers[0] = s_frameCB;
    dc.constantBuffers[1] = objectCBForDraw;
    dc.constantBuffers[2] = (useMaterialOverride && materialCB.IsValid()) ? materialCB : s_materialCB;
    dc.constantBuffers[3] = s_lightCB;
    dc.constantBuffers[4] = s_shadowCB;
    if (flavor == ThumbnailShaderFlavor::Skinned)
        dc.constantBuffers[7] = s_skinningCB;
    if (useMaterialOverride && materialTextures) {
        const size_t count = std::min(dc.textures.size(), materialTextures->size());
        for (size_t i = 0; i < count; ++i)
            dc.textures[i] = (*materialTextures)[i];
        if ((flavor == ThumbnailShaderFlavor::Terrain || flavor == ThumbnailShaderFlavor::Water) &&
            EnsureThumbnailDefaultTextures(resources, s_whiteTexture, s_blackTexture, s_flatNormalTexture)) {
            for (uint32_t i = 0; i < static_cast<uint32_t>(dc.textures.size()); ++i) {
                if (dc.textures[i].IsValid()) continue;
                dc.textures[i] = DefaultThumbnailTextureForSlot(flavor, i, s_whiteTexture, s_blackTexture, s_flatNormalTexture);
            }
        }
    } else {
        dc.textures[0] = albedoTexture;
    }
    renderer.Submit(dc, resources);

    // WHY: AssetBrowser の ImGui 描画中に一時 RT へ切り替えるため、生成後は必ずバックバッファへ戻す。
    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    return true;
}

static void DrawRenderTargetThumbnail(
    renderer::ResourceHandle<renderer::RenderTargetTag> rt,
    ImVec2 origin,
    float sz,
    EditorContext& ctx,
    bool hovered,
    const char* badge)
{
    DrawThumbnailFrame(origin, sz, hovered);
    if (!ctx.imguiRenderer || !ctx.resources || !rt.IsValid()) return;

    void* rawID = ctx.imguiRenderer->GetImTextureID(rt, *ctx.resources, 0);
    if (!rawID) return;

    const float frameH = sz;
    const float margin = std::max(4.0f, sz * 0.06f);
    ImGui::GetWindowDrawList()->AddImage(
        ToImTextureID(rawID),
        { origin.x + margin, origin.y + margin },
        { origin.x + sz - margin, origin.y + frameH - margin });

    const ImVec2 textSize = ImGui::CalcTextSize(badge);
    const ImVec2 badgeMin = { origin.x + sz - textSize.x - 12.0f, origin.y + frameH - textSize.y - 7.0f };
    const ImVec2 badgeMax = { origin.x + sz - 4.0f, origin.y + frameH - 3.0f };
    ImGui::GetWindowDrawList()->AddRectFilled(badgeMin, badgeMax, IM_COL32(20, 22, 26, 205), 3.0f);
    ImGui::GetWindowDrawList()->AddText({ badgeMin.x + 4.0f, badgeMin.y + 2.0f },
                                        IM_COL32(235, 240, 245, 230), badge);
}

} // namespace

ImVec4 AssetBrowserPanel::EntryColor(const Entry& e)
{
    if (e.isDir) return { 0.80f, 0.60f, 0.10f, 1.0f };
    if (const ExtGroup* g = FindGroup(e.ext)) return g->color;
    if (e.ext.empty()) return { 0.38f, 0.38f, 0.38f, 1.0f };
    // 未知拡張子: ハッシュで自動着色
    return ColorFromExt(e.ext);
}

const char* AssetBrowserPanel::EntryLabel(const Entry& e)
{
    if (e.isDir) return "DIR";
    if (const ExtGroup* g = FindGroup(e.ext)) return g->label;
    // 未知拡張子: 拡張子文字列をそのままラベルに使う (最大 6 文字、先頭の . を除く)
    // WHY: 静的バッファに詰めることでどんな拡張子でもラベル表示できる。
    //      ImGui はフレーム内で文字列を参照するため static thread_local を使う。
    static thread_local char buf[8];
    const char* src = e.ext.size() > 1 ? e.ext.c_str() + 1 : e.ext.c_str(); // skip '.'
    const std::string upper = util::StringUtils::ToUpper(src);
    const std::size_t len = std::min<std::size_t>(upper.size(), 6);
    std::memcpy(buf, upper.data(), len);
    buf[len] = '\0';
    return len > 0 ? buf : "FILE";
}

// ─── フォルダツリー (左ペイン) ───────────────────────────────────────────────

void AssetBrowserPanel::DrawFolderTree(const std::string& dirPath, EditorContext& ctx)
{
    std::vector<Entry> dirs;
    for (const auto& p : util::FileSystem::ListAll(dirPath)) {
        if (!util::FileSystem::IsDirectory(p)) continue;
        Entry e;
        e.path = util::FileSystem::NormalizePathSeparators(p);
        e.name = util::FileSystem::GetFilename(p);
        e.isDir = true;
        dirs.push_back(std::move(e));
    }
    if (util::FileSystem::SamePathText(dirPath, m_rootPath)) {
        for (const AssetMount& mount : m_mounts) {
            Entry e;
            e.path = mount.path;
            e.name = mount.name;
            e.isDir = true;
            e.isMount = true;
            dirs.push_back(std::move(e));
        }
    }
    std::stable_sort(dirs.begin(), dirs.end(), [](const Entry& a, const Entry& b) {
        return a.name < b.name;
    });

    for (const Entry& dir : dirs) {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (util::FileSystem::SamePathText(dir.path, m_currentPath)) flags |= ImGuiTreeNodeFlags_Selected;

        // WHY: 表示名は Assets 側の仮想名、ID は実パスにすることで同名マウントでも ImGui ID が衝突しない。
        bool open = ImGui::TreeNodeEx(dir.path.c_str(), flags, "%s", dir.name.c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            m_currentPath = dir.path;
            RefreshDirectory();
        }
        // ヒエラルキーエンティティをフォルダノードにドロップ → そのフォルダへ Prefab 保存
        if (ImGui::BeginDragDropTarget()) {
            if (SaveHierarchyPayloadAsPrefab(
                    ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, dir.path)) {
                RefreshDirectory();
            }
            ImGui::EndDragDropTarget();
        }
        if (open) {
            DrawFolderTree(dir.path, ctx);
            ImGui::TreePop();
        }
    }
}

// ─── アイコン描画ユーティリティ ──────────────────────────────────────────────

void AssetBrowserPanel::DrawFileIconAt(ImVec2 origin, float sz, const Entry& e, bool hovered)
{
    const ImVec4 base  = EntryColor(e);
    const ImU32 cFill  = ImGui::ColorConvertFloat4ToU32(hovered ? Lighten(base) : base);
    const ImU32 cDark  = ImGui::ColorConvertFloat4ToU32(
        { base.x * 0.50f, base.y * 0.50f, base.z * 0.50f, 1.0f });
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (e.isDir) {
        const float tabW  = sz * 0.48f;
        const float tabH  = sz * 0.14f;
        const float bodyY = origin.y + tabH;
        const float bodyH = sz * 0.82f;
        const float r     = sz * 0.07f;
        dl->AddRectFilled({ origin.x, origin.y }, { origin.x + tabW, bodyY + r }, cFill, r);
        dl->AddRectFilled({ origin.x, bodyY },    { origin.x + sz,   origin.y + bodyH }, cFill, r);
        dl->AddRect(      { origin.x, bodyY },    { origin.x + sz,   origin.y + bodyH }, cDark, r, 0, 1.0f);
    } else {
        const float bodyH = sz * 0.85f;
        const float dog   = sz * 0.22f;
        ImVec2 pts[5] = {
            { origin.x,        origin.y       },
            { origin.x+sz-dog, origin.y       },
            { origin.x+sz,     origin.y+dog   },
            { origin.x+sz,     origin.y+bodyH },
            { origin.x,        origin.y+bodyH },
        };
        dl->AddConvexPolyFilled(pts, 5, cFill);
        dl->AddPolyline(pts, 5, cDark, ImDrawFlags_Closed, 1.0f);
        ImVec2 tri[3] = {
            { origin.x+sz-dog, origin.y     },
            { origin.x+sz,     origin.y+dog },
            { origin.x+sz-dog, origin.y+dog },
        };
        dl->AddConvexPolyFilled(tri, 3, cDark);

        const char* lbl = EntryLabel(e);
        ImFont* font = ImGui::GetFont();
        const float labelSz = ImGui::GetFontSize() * std::max(1.0f, sz / 64.0f);
        const ImVec2 tsz = font->CalcTextSizeA(labelSz, FLT_MAX, 0.0f, lbl);
        dl->AddText(font, labelSz,
                    { origin.x + (sz - tsz.x) * 0.5f, origin.y + bodyH * 0.52f - tsz.y * 0.5f },
                    IM_COL32(255, 255, 255, 220), lbl);
    }
}

// ─── アイコン1個 (右ペイン) ──────────────────────────────────────────────────

void AssetBrowserPanel::DrawAssetPreviewIconAt(ImVec2 origin, float sz, const Entry& e, EditorContext& ctx, bool hovered)
{
    if (e.isDir) {
        DrawFileIconAt(origin, sz, e, hovered);
        return;
    }

    if (IsTextureExt(e.ext) && ctx.resources && ctx.imguiRenderer) {
        TexturePreview& preview = m_texturePreviews[e.path];
        if (!preview.handle.IsValid() && !preview.failed) {
            preview.handle = ctx.resources->LoadTexture(ToTextureLoadPath(e.path, ctx));
            if (preview.handle.IsValid()) {
                if (auto* texture = ctx.resources->Get(preview.handle)) {
                    preview.width = texture->GetWidth();
                    preview.height = texture->GetHeight();
                }
            } else {
                preview.failed = true;
            }
        }

        if (!preview.failed) {
            void* rawID = ctx.imguiRenderer->GetImTextureID(preview.handle, *ctx.resources);
            if (rawID) {
                DrawTextureThumbnail(rawID, preview.width, preview.height, origin, sz, hovered);
                return;
            }
        }
    }

    if (e.ext == ".fzmat") {
        MaterialPreview& preview = m_materialPreviews[e.path];
        const auto currentWriteTime = ReadLastWriteTime(e.path);
        if (!preview.loaded || currentWriteTime != preview.lastWriteTime) {
            preview.asset = {};
            preview.failed = !asset::LoadMaterialAssetFromFile(e.path, preview.asset);
            preview.loaded = true;
            preview.lastWriteTime = currentWriteTime;
            preview.previewTexture = {};
            preview.previewTexturePath.clear();
            preview.previewTextureWidth = 0;
            preview.previewTextureHeight = 0;
            preview.shaderPath.clear();
            preview.shader = {};
            preview.materialCB = {};
            preview.textures.clear();
            preview.paramData.clear();
            preview.thumbnailRendered = false;
        }

        if (!preview.failed && ctx.renderer && ctx.resources && ctx.imguiRenderer) {
            const std::string previewTexturePath = SelectMaterialPreviewTexture(preview.asset);
            if (previewTexturePath != preview.previewTexturePath) {
                preview.previewTexturePath = previewTexturePath;
                preview.previewTexture = {};
                preview.previewTextureWidth = 0;
                preview.previewTextureHeight = 0;
                preview.thumbnailRendered = false;
            }

            if (!previewTexturePath.empty()) {
                if (!preview.previewTexture.IsValid()) {
                    preview.previewTexture = ctx.resources->LoadTexture(ToTextureLoadPath(previewTexturePath, ctx));
                    if (auto* texture = ctx.resources->Get(preview.previewTexture)) {
                        preview.previewTextureWidth = texture->GetWidth();
                        preview.previewTextureHeight = texture->GetHeight();
                    }
                }
            }

            if (!preview.thumbnailRT.IsValid()) {
                preview.thumbnailRT = ctx.resources->CreateRenderTarget(128, 128);
                preview.thumbnailRendered = false;
            }
            if (!preview.thumbnailRendered && preview.thumbnailRT.IsValid()) {
                static std::shared_ptr<renderer::Mesh> s_materialSphere;
                static std::shared_ptr<renderer::Mesh> s_skinnedMaterialSphere;
                static std::shared_ptr<renderer::Mesh> s_waterMaterialSphere;
                if (!s_materialSphere)
                    s_materialSphere = renderer::PrimitiveMesh::Sphere(*ctx.resources, 64);
                if (!s_skinnedMaterialSphere)
                    s_skinnedMaterialSphere = CreateSkinnedPreviewSphere(*ctx.resources, 64);
                if (!s_waterMaterialSphere)
                    s_waterMaterialSphere = CreateWaterPreviewSphere(*ctx.resources, 64);
                const ThumbnailShaderFlavor flavor = DetectThumbnailShaderFlavor(preview.asset.shaderPath);
                const std::shared_ptr<renderer::Mesh>& previewMesh = (flavor == ThumbnailShaderFlavor::Skinned)
                    ? s_skinnedMaterialSphere
                    : (flavor == ThumbnailShaderFlavor::Water ? s_waterMaterialSphere : s_materialSphere);
                if (previewMesh && RebuildMaterialThumbnailGpuData(preview, ctx)) {
                    preview.thumbnailRendered = RenderMeshThumbnail(
                        *ctx.renderer,
                        *ctx.resources,
                        *previewMesh,
                        preview.thumbnailRT,
                        preview.previewTexture,
                        SelectMaterialColor(preview.asset),
                        preview.shader,
                        preview.materialCB,
                        &preview.textures,
                        flavor,
                        &preview.asset);
                }
            }
            if (preview.thumbnailRendered) {
                DrawRenderTargetThumbnail(preview.thumbnailRT, origin, sz, ctx, hovered, "MAT");
                return;
            }
        }
    }

    if (IsMeshExt(e.ext) && ctx.renderer && ctx.resources && ctx.imguiRenderer) {
        MeshPreview& preview = m_meshPreviews[e.path];
        const auto currentWriteTime = ReadLastWriteTime(e.path);
        if (currentWriteTime != preview.lastWriteTime) {
            preview.lastWriteTime = currentWriteTime;
            preview.model.reset();
            preview.thumbnailRendered = false;
            preview.failed = false;
        }
        if (!preview.thumbnailRT.IsValid()) {
            preview.thumbnailRT = ctx.resources->CreateRenderTarget(128, 128);
            preview.thumbnailRendered = false;
        }
        if (!preview.model && !preview.failed)
            preview.model = asset::AssetManager::Load<asset::Model>(e.path);
        if (!preview.thumbnailRendered && !preview.failed && preview.thumbnailRT.IsValid() &&
            preview.model && !preview.model->meshes.empty() && preview.model->meshes.front()) {
            preview.thumbnailRendered = RenderMeshThumbnail(
                *ctx.renderer,
                *ctx.resources,
                *preview.model->meshes.front(),
                preview.thumbnailRT,
                renderer::ResourceHandle<renderer::TextureTag>{},
                { 0.74f, 0.78f, 0.84f, 1.0f });
            preview.failed = !preview.thumbnailRendered;
        }
        if (preview.thumbnailRendered) {
            DrawRenderTargetThumbnail(preview.thumbnailRT, origin, sz, ctx, hovered, "MESH");
            return;
        }
    }

    DrawFileIconAt(origin, sz, e, hovered);
}

void AssetBrowserPanel::ResetAssetPreviewCache(const std::string& path)
{
    m_texturePreviews.erase(path);
    m_materialPreviews.erase(path);
    m_meshPreviews.erase(path);
}

void AssetBrowserPanel::DrawEntry(const Entry& e, EditorContext& ctx)
{
    ImGui::PushID(e.path.c_str());

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  sz     = m_iconSize;

    ImGui::InvisibleButton("##icon", { sz, sz });
    const bool hov = ImGui::IsItemHovered();

    DrawAssetPreviewIconAt(origin, sz, e, ctx, hov);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 選択ハイライト
    const bool isSelected = !e.isDir && (e.path == ctx.selectedAssetPath);
    if (isSelected) {
        ImGui::GetWindowDrawList()->AddRect(
            origin, { origin.x + sz, origin.y + sz },
            IM_COL32(255, 200, 80, 220), 3.0f, 0, 2.0f);
    }

    // ! バッジ: 未変換ファイル (FBX / PNG 等) に赤丸で警告表示
    if (!e.isDir && IsImportableRaw(e.ext))
    {
        const float r  = sz * 0.15f;
        const float cx = origin.x + sz - r;
        const float cy = origin.y + r;
        dl->AddCircleFilled({ cx, cy }, r, IM_COL32(220, 50, 50, 230));
        const ImVec2 bsz = ImGui::CalcTextSize("!");
        dl->AddText({ cx - bsz.x * 0.5f, cy - bsz.y * 0.5f },
                    IM_COL32(255, 255, 255, 255), "!");
    }

    // ドラッグソース (ファイルのみ)
    if (!e.isDir && ImGui::BeginDragDropSource()) {
        const std::string payloadPath = ToProjectAssetPath(e.path, ctx);
        ImGui::SetDragDropPayload("ASSET_PATH", payloadPath.c_str(), payloadPath.size() + 1);
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::EndDragDropSource();
    }
    // ドロップターゲット (ディレクトリのみ)
    // WHY: コンテンツエリア全体ではなく特定サブフォルダへ直接ドロップして保存先を選べるようにする。
    if (e.isDir && ImGui::BeginDragDropTarget()) {
        if (SaveHierarchyPayloadAsPrefab(
                ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, e.path)) {
            RefreshDirectory();
        }
        ImGui::EndDragDropTarget();
    }

    if (hov && m_renamingPath != e.path) {
        if (e.isMount)
            ImGui::SetTooltip("%s\n\nExternal source folder mounted under Assets", e.path.c_str());
        else if (e.ext == ".fnt")
            ImGui::SetTooltip("%s\n\nFont atlas metadata\nDrag and drop onto UI Text Font Path to assign it", e.path.c_str());
        else if (e.ext == ".ttf" || e.ext == ".otf")
            ImGui::SetTooltip("%s\n\nTTF font\nConvert it to a PNG + FNT atlas with gen_font_atlas.py before use", e.path.c_str());
        else
            ImGui::SetTooltip("%s", e.path.c_str());
    }

    // 右クリックコンテキストメニュー (リネーム / 削除)
    if (ImGui::BeginPopupContextItem("##entry_ctx")) {
        // 未変換ファイルは "Import" を最上位に表示する
        if (!e.isDir && IsImportableRaw(e.ext)) {
            if (ImGui::MenuItem("Import")) {
                bool found = false;
                for (const auto& p : m_pendingImports)
                    if (p.path == e.path) { found = true; break; }
                if (!found)
                    m_pendingImports.push_back({ e.path, PendingImport::Kind::Fbx });
                m_importAllRequested = true;
            }
            ImGui::Separator();
        }
        ImGui::BeginDisabled(e.isMount);
        if (ImGui::MenuItem("Rename")) {
            m_renamingPath    = e.path;
            const std::string stem = util::FileSystem::GetFilename(e.path);
            std::strncpy(m_renameBuffer, stem.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            m_renameNeedFocus = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) {
            const std::string path = e.path;
            ModalDialog::OpenConfirm("Delete",
                "Delete \"" + util::FileSystem::GetFilename(path) + "\"?",
                [this, path]() {
                    if (!util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(path))) {
                        FBZZ_LOG_ERROR("Delete failed: %s", path.c_str());
                    } else {
                        if (m_selectedFbxPath == path) {
                            m_selectedFbxPath.clear();
                            m_selectedModel.reset();
                        }
                        ResetAssetPreviewCache(path);
                        RefreshDirectory();
                    }
                });
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::BeginMenu("Create")) {
            DrawCreateMenu(ctx);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    // ホバー中は FBX 内容を即座に更新する。
    // WHY: 右側 Preview ペインは削除したが、FBX 内部の Mesh / Animation アイコンは
    //      AssetBrowser 下部の既存領域で確認・ドラッグできる必要がある。
    if (hov && !e.isDir) {
        if (IsMeshExt(e.ext) && m_selectedFbxPath != e.path) {
            m_selectedFbxPath = e.path;
            m_selectedModel.reset();
            if (renderer::ResourceManager::Active())
                m_selectedModel = asset::AssetManager::Load<asset::Model>(e.path);
        }
    }

    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !e.isDir) {
        ctx.selectedAssetPath = e.path;
    }

    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        // WHY: ダブルクリック後に Scene open / Prefab instantiate / directory navigation が走ると、
        //      AssetBrowser の m_entries が更新される可能性がある。
        //      Entry 参照 e を使い続けると、std::string 比較や string_view 内で無効参照を踏むため、
        //      分岐に入る前に必要な値をコピーしておく。
        const bool isDir = e.isDir;
        const std::string path = e.path;
        const std::string ext = e.ext;

        if (isDir) {
            m_pendingNavigate = path;
        }

        if (!isDir && ext == ".fbzz" && ctx.activeScene) {
            if (ctx.requestOpenScene) {
                ctx.requestOpenScene(path);
            } else if (SceneIO::Load(*ctx.activeScene, path)) {
                ctx.selectedEntities.clear();
                if (ctx.markSceneDirty) ctx.markSceneDirty();
                FBZZ_LOG_INFO("Opened scene: %s", path.c_str());
            } else {
                FBZZ_LOG_ERROR("Failed to open scene: %s", path.c_str());
            }
        } else if (!isDir && ext == ".fbzzprefab" && ctx.activeScene) {
            std::vector<scene::EntityID> roots;
            if (PrefabSerializer::Instantiate(*ctx.activeScene, path, roots)) {
                ctx.selectedEntities = roots;
                if (ctx.markSceneDirty) ctx.markSceneDirty();
            }
        }
    }

    // ファイル名 (リネーム中は InputText、通常は省略ラベル)
    if (m_renamingPath == e.path) {
        ImGui::SetNextItemWidth(m_iconSize);
        if (m_renameNeedFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameNeedFocus = false;
        }
        constexpr ImGuiInputTextFlags renameFlags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
        const bool enterPressed = ImGui::InputText("##rename", m_renameBuffer,
                                                   sizeof(m_renameBuffer), renameFlags);
        if (enterPressed) {
            if (m_renameBuffer[0] != '\0') {
                const std::string dir     = util::FileSystem::GetDirectory(e.path);
                const std::string newPath = dir + m_renameBuffer;
                if (newPath != e.path) {
                    if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(e.path),
                                                  util::FileSystem::PathFromUtf8(newPath))) {
                        FBZZ_LOG_ERROR("Rename failed: %s -> %s", e.path.c_str(), newPath.c_str());
                    } else {
                        if (m_selectedFbxPath == e.path) m_selectedFbxPath = newPath;
                        ResetAssetPreviewCache(e.path);
                        RefreshDirectory();
                    }
                }
            }
            m_renamingPath.clear();
        } else if (ImGui::IsItemDeactivated()) {
            // Escape またはフォーカス外れ → キャンセル
            m_renamingPath.clear();
        }
    } else {
        std::string display = e.name;
        while (display.size() > 2 &&
               ImGui::CalcTextSize(display.c_str()).x + ImGui::CalcTextSize("..").x > m_iconSize)
            display.pop_back();
        if (display.size() < e.name.size()) display += "..";

        float indent = (m_iconSize - ImGui::CalcTextSize(display.c_str()).x) * 0.5f;
        if (indent > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        ImGui::TextUnformatted(display.c_str());

        // F2 でリネーム開始 (ホバー中)
        if (!e.isMount && hov && ImGui::IsKeyPressed(ImGuiKey_F2)) {
            m_renamingPath    = e.path;
            const std::string stem = e.name;
            std::strncpy(m_renameBuffer, stem.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            m_renameNeedFocus = true;
        }
    }

    ImGui::PopID();
}

// ─── FBX 内容プレビュー (サブアセットアイコン) ────────────────────────────────

} // namespace fbzz::editor
