// FBZZ Engine
// AssetBrowserItems.cpp | fbzz::editor
// AssetBrowser のフォルダツリーとファイルアイコン描画
#include "AssetBrowserCommon.hpp"
#include <Editor/Util/UndoStack.hpp>
#include <Windows.h>
#include <toml++/toml.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Util/Uuid.hpp>
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

class AssetDeleteCommand final : public ICommand {
public:
    struct Entry {
        std::string original;
        std::string backup;
    };

    AssetDeleteCommand(std::vector<Entry> entries, EditorContext* context)
        : m_entries(std::move(entries))
        , m_context(context)
    {
    }

    ~AssetDeleteCommand() override
    {
        // Undo されないまま履歴から消えた削除データだけを最終破棄する。
        if (!m_deleted) return;
        for (const Entry& entry : m_entries)
            util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(entry.backup));
    }

    void Execute() override
    {
        bool movedAny = false;
        for (const Entry& entry : m_entries) {
            if (!util::FileSystem::Exists(entry.original)) continue;
            util::FileSystem::EnsureDirectory(util::FileSystem::GetDirectory(entry.backup));
            movedAny |= util::FileSystem::Rename(
                util::FileSystem::PathFromUtf8(entry.original),
                util::FileSystem::PathFromUtf8(entry.backup));
        }
        m_deleted = movedAny || m_deleted;
        RequestRefresh();
    }

    void Undo() override
    {
        for (const Entry& entry : m_entries) {
            if (!util::FileSystem::Exists(entry.backup)) continue;
            util::FileSystem::EnsureDirectory(util::FileSystem::GetDirectory(entry.original));
            util::FileSystem::Rename(
                util::FileSystem::PathFromUtf8(entry.backup),
                util::FileSystem::PathFromUtf8(entry.original));
        }
        m_deleted = false;
        RequestRefresh();
    }

    std::string GetDescription() const override { return "Delete Asset"; }

private:
    void RequestRefresh()
    {
        if (m_context) m_context->requestAssetBrowserRefresh = true;
    }

    std::vector<Entry> m_entries;
    EditorContext*     m_context = nullptr;
    bool               m_deleted = false;
};

std::unique_ptr<ICommand> CreateAssetDeleteCommand(const std::vector<std::string>& paths,
                                                   EditorContext& ctx)
{
    const std::string undoRoot = ctx.projectRoot + "/.fbzz/Undo/" + util::GenerateUUID() + "/";
    std::vector<AssetDeleteCommand::Entry> entries;
    entries.reserve(paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i) {
        entries.push_back({
            paths[i],
            undoRoot + std::to_string(i) + "_" + util::FileSystem::GetFilename(paths[i])
        });
    }
    return std::make_unique<AssetDeleteCommand>(std::move(entries), &ctx);
}

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
    { { ".prefab", nullptr },                              { 0.25f, 0.65f, 0.75f, 1.0f }, "PREFAB"  },
    { { ".terrain", nullptr },                             { 0.35f, 0.70f, 0.30f, 1.0f }, "TERRAIN" },
    { { ".scene", nullptr },                                    { 0.60f, 0.15f, 0.70f, 1.0f }, "SCENE"   },
    { { ".animgraph", nullptr },                               { 0.75f, 0.40f, 0.85f, 1.0f }, "GRAPH"   },
{ { ".fzasset", nullptr },                               { 0.90f, 0.60f, 0.10f, 1.0f }, "FZASSET" },
    { { ".asset", nullptr },                                 { 0.85f, 0.55f, 0.08f, 1.0f }, "ASSET"   },
    { { ".anim", nullptr },                                  { 0.95f, 0.75f, 0.20f, 1.0f }, "ANIM"    },
    { { ".mat", nullptr },                                   { 0.20f, 0.70f, 0.80f, 1.0f }, "MAT"     },
    { { ".fzpp", nullptr },                                  { 0.65f, 0.35f, 0.85f, 1.0f }, "POST FX" },
    { { ".tex", nullptr },                                   { 0.40f, 0.80f, 0.90f, 1.0f }, "TEX"     },
    { { ".mesh", nullptr },                                  { 0.80f, 0.45f, 0.10f, 1.0f }, "MESH"    },
    { { ".animcontroller", ".animctrl", nullptr },           { 0.35f, 0.75f, 0.45f, 1.0f }, "CTRL"    },
    { { ".toml", ".json", ".yaml", ".yml", nullptr },           { 0.65f, 0.65f, 0.10f, 1.0f }, "DATA"    },
    { { ".wav", ".mp3", ".ogg", ".flac", nullptr },             { 0.70f, 0.20f, 0.50f, 1.0f }, "SFX"     },
    { { ".ttf", ".otf", nullptr },                             { 0.60f, 0.30f, 0.85f, 1.0f }, "FONT"    },
    { { ".fnt", nullptr },                                     { 0.50f, 0.20f, 0.75f, 1.0f }, "FNT"     },
    { { ".txt", ".md", ".rst", nullptr },                      { 0.55f, 0.55f, 0.55f, 1.0f }, "TEXT"    },
    { { ".py", ".lua", ".cs", nullptr },                       { 0.20f, 0.70f, 0.55f, 1.0f }, "SCRIPT"  },
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
           ext == ".glb" || ext == ".mesh" || ext == ".fzasset";
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

    // WHY: .mat 内のテクスチャ参照は Assets/ 相対で保存されるため、
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

// t0-t15 は標準 Material スロット。Terrain/Water は専用名で解決されるが、
// それ以外の汎用スロットはここで名前フォールバックが効く。
// WHY: preview.textures.assign(16,{}) と同サイズにすることで、
//      bind.slot が 8-15 の汎用スロットでも FindMaterialTexturePath が機能する。
constexpr std::array<const char*, 16> kMaterialTextureSlotNames = {
    "albedo",
    "normal",
    "metallic",
    "emissive",
    "ao",
    "tex5",
    "tex6",
    "tex7",
    "tex8",
    "tex9",
    "tex10",
    "tex11",
    "tex12",
    "tex13",
    "tex14",
    "tex15",
};

static const std::vector<float>* FindMaterialParam(const asset::MaterialAsset& asset, std::string_view shaderVarName)
{
    auto it = asset.params.find(std::string(shaderVarName));
    if (it != asset.params.end()) return &it->second;

    // WHY: .mat は PBR 寄りの名前、HLSL は shader ごとの短い変数名を使う場合がある。
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

        // Terrain レイヤーの fzmat はプレフィックスなし ("diffuse", "ao_roughness") を使う。
        // WHY: Fallback/Surface シェーダーが slot 0="albedo", slot 4="ao" を期待するが、
        //      レイヤー fzmat にはこれらが存在しないため別名でフォールバックする。
        const std::string_view standard = kMaterialTextureSlotNames[bind.slot];
        if (standard == "albedo") {
            auto it = asset.textures.find("diffuse");
            if (it != asset.textures.end() && !it->second.empty()) return it->second;
        }
        if (standard == "ao") {
            auto it = asset.textures.find("ao_roughness");
            if (it != asset.textures.end() && !it->second.empty()) return it->second;
        }
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
        if (ctx.resources && preview.materialCB.IsValid()) {
            ctx.resources->Release(preview.materialCB);
            preview.materialCB = {};
        }
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

// サムネイルレンダリング用の共有 GPU リソースをまとめて保持する構造体。
// WHY: 以前は RenderMeshThumbnail / DrawAssetPreviewIconAt の function-local static
//      として暗黙的に共有されていた。構造体に昇格させて意図を明示する。
struct ThumbnailRenderer {
    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  pso;
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> materialCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> terrainObjectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> waterObjectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB;
    renderer::ResourceHandle<renderer::TextureTag>        whiteTexture;
    renderer::ResourceHandle<renderer::TextureTag>        blackTexture;
    renderer::ResourceHandle<renderer::TextureTag>        flatNormalTexture;
    renderer::Mesh*                                        materialSphere = nullptr;
    renderer::Mesh*                                        skinnedMaterialSphere = nullptr;
    renderer::Mesh*                                        waterMaterialSphere = nullptr;
};
static ThumbnailRenderer s_tr;

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

static renderer::Mesh* CreateSkinnedPreviewSphere(renderer::ResourceManager& resources, int segments)
{
    static std::unordered_map<int, std::shared_ptr<renderer::Mesh>> s_cache;
    if (auto it = s_cache.find(segments); it != s_cache.end()) return it->second.get();

    auto* surface = renderer::PrimitiveMesh::Sphere(resources, segments);
    if (!surface) return nullptr;

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

    auto mesh = std::shared_ptr<renderer::Mesh>(new renderer::Mesh());
    mesh->vertexBuffer = resources.CreateVertexBuffer(verts.data(), verts.size() * sizeof(renderer::SkinnedVertex), sizeof(renderer::SkinnedVertex));
    mesh->indexBuffer = resources.CreateIndexBuffer(surface->cpuIndices.data(), static_cast<uint32_t>(surface->cpuIndices.size()));
    mesh->vertexCount = static_cast<uint32_t>(verts.size());
    mesh->indexCount = static_cast<uint32_t>(surface->cpuIndices.size());
    mesh->isSkinned = true;
    mesh->cpuSkinnedVertices = std::move(verts);
    mesh->cpuIndices = surface->cpuIndices;
    mesh->ComputeBounds();
    s_cache[segments] = mesh;
    return mesh.get();
}

static renderer::Mesh* CreateWaterPreviewSphere(renderer::ResourceManager& resources, int segments)
{
    static std::unordered_map<int, std::shared_ptr<renderer::Mesh>> s_cache;
    if (auto it = s_cache.find(segments); it != s_cache.end()) return it->second.get();

    auto* surface = renderer::PrimitiveMesh::Sphere(resources, segments);
    if (!surface) return nullptr;

    std::vector<ThumbnailWaterVertex> verts;
    verts.reserve(surface->cpuVertices.size());
    for (const auto& v : surface->cpuVertices)
        verts.push_back({ v.position, v.uv });

    auto mesh = std::shared_ptr<renderer::Mesh>(new renderer::Mesh());
    mesh->vertexBuffer = resources.CreateVertexBuffer(verts.data(), verts.size() * sizeof(ThumbnailWaterVertex), sizeof(ThumbnailWaterVertex));
    mesh->indexBuffer = resources.CreateIndexBuffer(surface->cpuIndices.data(), static_cast<uint32_t>(surface->cpuIndices.size()));
    mesh->vertexCount = static_cast<uint32_t>(verts.size());
    mesh->indexCount = static_cast<uint32_t>(surface->cpuIndices.size());
    mesh->cpuVertices = surface->cpuVertices;
    mesh->cpuIndices = surface->cpuIndices;
    mesh->ComputeBounds();
    s_cache[segments] = mesh;
    return mesh.get();
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

static bool EnsureThumbnailGpuResources(renderer::ResourceManager& resources,
                                         ThumbnailRenderer& tr,
                                         bool requireFallbackMaterial)
{
    if (requireFallbackMaterial && !tr.shader.IsValid())
        tr.shader = resources.LoadShader("Assets/Shaders/Material/Surface/Lit.hlsl");
    if (requireFallbackMaterial && !tr.shader.IsValid()) return false;

    if (!tr.pso.IsValid()) {
        tr.pso = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND,
            renderer::DepthMode::DEPTH_ON
        });
    }
    if (!tr.frameCB.IsValid())
        tr.frameCB = resources.CreateConstantBuffer(sizeof(scene::PerFrameCB));
    if (!tr.objectCB.IsValid())
        tr.objectCB = resources.CreateConstantBuffer(sizeof(scene::PerObjectCB));
    if (requireFallbackMaterial && !tr.materialCB.IsValid())
        tr.materialCB = resources.CreateConstantBuffer(sizeof(ThumbnailMaterialCB));
    if (!tr.lightCB.IsValid())
        tr.lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    if (!tr.shadowCB.IsValid())
        tr.shadowCB = resources.CreateConstantBuffer(sizeof(scene::ShadowConstantsCB));

    return tr.pso.IsValid() && tr.frameCB.IsValid() && tr.objectCB.IsValid() &&
           (!requireFallbackMaterial || tr.materialCB.IsValid()) && tr.lightCB.IsValid() && tr.shadowCB.IsValid();
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
    const asset::MaterialAsset* materialAsset = nullptr,
    bool clearRT = true,
    math::Vector3 overrideCenter = {},
    float overrideRadius = -1.0f)  // <0 = use mesh bounds
{
    if (!rt.IsValid() || !mesh.vertexBuffer.IsValid() || !mesh.indexBuffer.IsValid())
        return false;
    const bool useMaterialOverride = materialShader.IsValid();
    if (!EnsureThumbnailGpuResources(resources, s_tr, !useMaterialOverride))
        return false;

    const math::Vector3 center = (overrideRadius >= 0.0f) ? overrideCenter : MeshBoundsCenter(mesh);
    const float radius = std::max(0.0001f, (overrideRadius >= 0.0f) ? overrideRadius : MeshBoundsRadius(mesh, center));
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
    resources.Update(s_tr.frameCB, &frameData, sizeof(frameData));

    scene::PerObjectCB objectData{};
    objectData.world = math::Matrix4::Identity();
    objectData.worldInvTranspose = math::Matrix4::Identity();
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCBForDraw = s_tr.objectCB;
    if (flavor == ThumbnailShaderFlavor::Terrain) {
        if (!s_tr.terrainObjectCB.IsValid())
            s_tr.terrainObjectCB = resources.CreateConstantBuffer(sizeof(ThumbnailTerrainCB));
        if (!s_tr.terrainObjectCB.IsValid()) return false;
        const ThumbnailTerrainCB terrainData = BuildThumbnailTerrainCB(materialAsset, frameData.viewProjection);
        resources.Update(s_tr.terrainObjectCB, &terrainData, sizeof(terrainData));
        objectCBForDraw = s_tr.terrainObjectCB;
    } else if (flavor == ThumbnailShaderFlavor::Water) {
        if (!s_tr.waterObjectCB.IsValid())
            s_tr.waterObjectCB = resources.CreateConstantBuffer(sizeof(ThumbnailWaterCB));
        if (!s_tr.waterObjectCB.IsValid()) return false;
        const ThumbnailWaterCB waterData = BuildThumbnailWaterCB(materialAsset, frameData.viewProjection);
        resources.Update(s_tr.waterObjectCB, &waterData, sizeof(waterData));
        objectCBForDraw = s_tr.waterObjectCB;
    } else {
        resources.Update(s_tr.objectCB, &objectData, sizeof(objectData));
    }

    if (flavor == ThumbnailShaderFlavor::Skinned) {
        if (!s_tr.skinningCB.IsValid())
            s_tr.skinningCB = resources.CreateConstantBuffer(sizeof(ThumbnailSkinningCB));
        if (!s_tr.skinningCB.IsValid()) return false;
        ThumbnailSkinningCB skinningData{};
        for (auto& bone : skinningData.boneMatrices)
            bone = math::Matrix4::Identity();
        resources.Update(s_tr.skinningCB, &skinningData, sizeof(skinningData));
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
        resources.Update(s_tr.materialCB, &materialData, sizeof(materialData));
    }

    renderer::LightConstantsCB lightData{};
    const math::Vector3 keyLight = (camera.m_position + math::Vector3{ radius * 1.4f, radius * 1.8f, radius * 0.8f } - center).Normalized();
    lightData.lightDir = { -keyLight.x, -keyLight.y, -keyLight.z };
    lightData.lightColor = { 1.0f, 0.97f, 0.92f };
    lightData.lightIntensity = 1.65f;
    lightData.ambientColor = { 0.24f, 0.27f, 0.31f };
    resources.Update(s_tr.lightCB, &lightData, sizeof(lightData));

    scene::ShadowConstantsCB shadowData{};
    shadowData.lightViewProjection = math::Matrix4::Translate({ 16.0f, 16.0f, 0.0f });
    shadowData.shadowMapTexelSize[0] = 0.0f;
    shadowData.shadowMapTexelSize[1] = 0.0f;
    // NDC 深度最大値 (1.0) をバイアスにすることで depth - bias <= 0 が常に成立し、
    // SampleCmpLevelZero が必ず 1.0 (照らされている) を返してサムネイル描画でのシャドウを無効化する。
    shadowData.shadowBias = 1.0f;
    resources.Update(s_tr.shadowCB, &shadowData, sizeof(shadowData));

    renderer.SetRenderTarget(rt, resources);
    if (clearRT) {
        renderer.Clear({ 0.030f, 0.032f, 0.038f, 1.0f });
        renderer.ClearDepth();
    }
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::CLAMP_LINEAR);
    renderer.SetSampler(2, renderer::SamplerMode::CLAMP_ANISOTROPIC);

    renderer::DrawCall dc;
    dc.vertexBuffer = mesh.vertexBuffer;
    dc.indexBuffer = mesh.indexBuffer;
    dc.indexCount = mesh.indexCount;
    dc.vertexCount = mesh.vertexCount;
    dc.shader = useMaterialOverride ? materialShader : s_tr.shader;
    dc.pipelineState = s_tr.pso;
    dc.constantBuffers[0] = s_tr.frameCB;
    dc.constantBuffers[1] = objectCBForDraw;
    dc.constantBuffers[2] = (useMaterialOverride && materialCB.IsValid()) ? materialCB : s_tr.materialCB;
    dc.constantBuffers[3] = s_tr.lightCB;
    dc.constantBuffers[4] = s_tr.shadowCB;
    if (flavor == ThumbnailShaderFlavor::Skinned)
        dc.constantBuffers[7] = s_tr.skinningCB;
    if (useMaterialOverride && materialTextures) {
        const size_t count = std::min(dc.textures.size(), materialTextures->size());
        for (size_t i = 0; i < count; ++i)
            dc.textures[i] = (*materialTextures)[i];
        if ((flavor == ThumbnailShaderFlavor::Terrain || flavor == ThumbnailShaderFlavor::Water) &&
            EnsureThumbnailDefaultTextures(resources, s_tr.whiteTexture, s_tr.blackTexture, s_tr.flatNormalTexture)) {
            for (uint32_t i = 0; i < static_cast<uint32_t>(dc.textures.size()); ++i) {
                if (dc.textures[i].IsValid()) continue;
                dc.textures[i] = DefaultThumbnailTextureForSlot(flavor, i, s_tr.whiteTexture, s_tr.blackTexture, s_tr.flatNormalTexture);
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

static void DrawThumbnailLabel(ImDrawList* dl, ImVec2 origin, float sz, const char* badge)
{
    const ImVec2 tsz     = ImGui::CalcTextSize(badge);
    const ImVec2 bMin    = { origin.x + sz - tsz.x - 12.0f, origin.y + sz - tsz.y - 7.0f };
    const ImVec2 bMax    = { origin.x + sz - 4.0f,           origin.y + sz - 3.0f         };
    dl->AddRectFilled(bMin, bMax, IM_COL32(20, 22, 26, 205), 3.0f);
    dl->AddText({ bMin.x + 4.0f, bMin.y + 2.0f }, IM_COL32(235, 240, 245, 230), badge);
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

template<typename T>
static void EnsureThumbnailRT(T& t, EditorContext& ctx) {
    if (!t.thumbnailRT.IsValid()) {
        t.thumbnailRT = ctx.resources->CreateRenderTarget(128, 128);
        t.thumbnailRendered = false;
    }
}

// Returns true if the thumbnail was drawn; caller should `return` immediately.
template<typename T>
static bool DrawThumbnailIfReady(T& t, ImVec2 origin, float sz,
                                  EditorContext& ctx, bool hovered, const char* badge) {
    if (!t.thumbnailRendered) return false;
    DrawRenderTargetThumbnail(t.thumbnailRT, origin, sz, ctx, hovered, badge);
    return true;
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
    const std::string normDir = util::FileSystem::NormalizePathSeparators(dirPath);
    auto it = m_treeCache.find(normDir);
    if (it == m_treeCache.end()) {
        std::vector<Entry> newDirs;
        for (const auto& p : util::FileSystem::ListAll(normDir)) {
            if (!util::FileSystem::IsDirectory(p)) continue;
            Entry e;
            e.path = util::FileSystem::NormalizePathSeparators(p);
            e.name = util::FileSystem::GetFilename(p);
            e.isDir = true;
            if (!ShouldDisplayEntry(e.path, e.name, true)) continue;
            newDirs.push_back(std::move(e));
        }
        if (util::FileSystem::SamePathText(normDir, m_rootPath)) {
            for (const AssetMount& mount : m_mounts) {
                Entry e;
                e.path = mount.path;
                e.name = mount.name;
                e.isDir = true;
                e.isMount = true;
                newDirs.push_back(std::move(e));
            }
        }
        std::stable_sort(newDirs.begin(), newDirs.end(), [](const Entry& a, const Entry& b) {
            return a.name < b.name;
        });
        it = m_treeCache.emplace(normDir, std::move(newDirs)).first;
    }
    const std::vector<Entry>& dirs = it->second;

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
        if (ImGui::BeginPopupContextItem()) {
            auto& bks = ctx.assetBrowserBookmarks;
            const bool already = std::find(bks.begin(), bks.end(), dir.path) != bks.end();
            if (!already && ImGui::MenuItem("\xe2\x98\x85 Add to Favorites"))
                bks.push_back(dir.path);
            if (already && ImGui::MenuItem("Remove from Favorites"))
                bks.erase(std::remove(bks.begin(), bks.end(), dir.path), bks.end());
            ImGui::EndPopup();
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
        DrawFileIconPolygon(dl, origin, sz, cFill, cDark);
        const char* lbl = EntryLabel(e);
        ImFont* font = ImGui::GetFont();
        const float labelSz = ImGui::GetFontSize() * std::max(1.0f, sz / 64.0f);
        const ImVec2 tsz = font->CalcTextSizeA(labelSz, FLT_MAX, 0.0f, lbl);
        dl->AddText(font, labelSz,
                    { origin.x + (sz - tsz.x) * 0.5f, origin.y + sz * 0.85f * 0.52f - tsz.y * 0.5f },
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
        if (!preview.handle.IsValid() && !preview.failed && !preview.queued) {
            preview.queued = true;
            m_texLoadQueue.push_back(e.path);
        }

        if (!preview.failed) {
            void* rawID = ctx.imguiRenderer->GetImTextureID(preview.handle, *ctx.resources);
            if (rawID) {
                DrawTextureThumbnail(rawID, preview.width, preview.height, origin, sz, hovered);
                return;
            }
        }
    }

    // .tex descriptor: ImageImporter 経由でロードして GPU テクスチャを表示
    if (e.ext == ".tex" && ctx.resources && ctx.imguiRenderer) {
        TexDescPreview& preview = m_texDescPreviews[e.path];
        const auto currentWriteTime = ReadLastWriteTime(e.path);
        if (currentWriteTime != preview.lastWriteTime) {
            preview.lastWriteTime = currentWriteTime;
            preview.handle = {};
            preview.width = preview.height = 0;
            preview.failed = false;
        }
        if (!preview.handle.IsValid() && !preview.failed) {
            preview.handle = asset::AssetManager::Load<asset::TextureAsset>(e.path);
            if (preview.handle.IsValid()) {
                if (const auto* ta = asset::AssetManager::Get(preview.handle)) {
                    if (const auto* tex = ctx.resources->Get(ta->gpuHandle)) {
                        preview.width  = tex->GetWidth();
                        preview.height = tex->GetHeight();
                    }
                }
            } else {
                preview.failed = true;
            }
        }
        if (!preview.failed && preview.handle.IsValid()) {
            if (const auto* ta = asset::AssetManager::Get(preview.handle)) {
                void* rawID = ctx.imguiRenderer->GetImTextureID(ta->gpuHandle, *ctx.resources);
                if (rawID) {
                    DrawTextureThumbnail(rawID, preview.width, preview.height, origin, sz, hovered);
                    return;
                }
            }
        }
    }

    if (e.ext == ".mat") {
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
            if (m_resources && preview.materialCB.IsValid()) {
                m_resources->Release(preview.materialCB);
                preview.materialCB = {};
            }
            preview.textures.clear();
            preview.paramData.clear();
            preview.thumbnailRendered = false;
        }

        // シェーダーファイルが変更された場合もサムネイルをリセットする
        if (preview.loaded && !preview.failed && !preview.asset.shaderPath.empty()) {
            const std::string resolvedShader = preview.asset.shaderPath.empty()
                ? "Assets/Shaders/Material/Surface/Fallback.hlsl"
                : preview.asset.shaderPath;
            const auto shaderWriteTime = ReadLastWriteTime(
                util::FileSystem::NormalizePathSeparators(ctx.projectRoot + "/" + resolvedShader));
            if (shaderWriteTime != preview.shaderLastWriteTime && shaderWriteTime != std::filesystem::file_time_type{}) {
                preview.shaderLastWriteTime = shaderWriteTime;
                preview.shaderPath.clear();
                preview.shader = {};
                if (m_resources && preview.materialCB.IsValid()) {
                    m_resources->Release(preview.materialCB);
                    preview.materialCB = {};
                }
                preview.textures.clear();
                preview.paramData.clear();
                preview.thumbnailRendered = false;
            }
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

            EnsureThumbnailRT(preview, ctx);
            if (!preview.thumbnailRendered && preview.thumbnailRT.IsValid()) {
                if (!s_tr.materialSphere)
                    s_tr.materialSphere = renderer::PrimitiveMesh::Sphere(*ctx.resources, 64);
                if (!s_tr.skinnedMaterialSphere)
                    s_tr.skinnedMaterialSphere = CreateSkinnedPreviewSphere(*ctx.resources, 64);
                if (!s_tr.waterMaterialSphere)
                    s_tr.waterMaterialSphere = CreateWaterPreviewSphere(*ctx.resources, 64);
                const ThumbnailShaderFlavor flavor = DetectThumbnailShaderFlavor(preview.asset.shaderPath);
                renderer::Mesh* previewMesh = (flavor == ThumbnailShaderFlavor::Skinned)
                    ? s_tr.skinnedMaterialSphere
                    : (flavor == ThumbnailShaderFlavor::Water ? s_tr.waterMaterialSphere : s_tr.materialSphere);
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
            if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, "MAT")) return;
        }
    }

    // .fzasset: ModelAssetImporter (新 API) 経由でロードして LOD0 サブメッシュを 3D プレビュー
    if (e.ext == ".fzasset" && ctx.renderer && ctx.resources && ctx.imguiRenderer) {
        ModelAssetPreview& preview = m_modelAssetPreviews[e.path];
        const auto currentWriteTime = ReadLastWriteTime(e.path);
        if (currentWriteTime != preview.lastWriteTime) {
            if (m_resources)
                for (auto& mp : preview.slotMaterials)
                    if (mp.materialCB.IsValid())
                        m_resources->Release(mp.materialCB);
            preview.slotMaterials.clear();
            preview.materialsLoaded = false;
            preview.lastWriteTime = currentWriteTime;
            preview.handle = {};
            preview.thumbnailRendered = false;
            preview.failed = false;
        }
        EnsureThumbnailRT(preview, ctx);
        if (!preview.handle.IsValid() && !preview.failed) {
            FBZZ_LOG_INFO("AssetBrowserItems: Load<ModelAsset> [%s]", e.path.c_str());
            // WHY: インポート直後やファイル監視直後は、生成前に一度 Load して Null が
            //      AssetManager にキャッシュされることがある。サムネイル再試行時は失敗 cache を掃除する。
            asset::AssetManager::FlushFailed();
            preview.handle = asset::AssetManager::Load<asset::ModelAsset>(e.path);
            if (!preview.handle.IsValid()) {
                FBZZ_LOG_ERROR("AssetBrowserItems: ModelAsset load failed [%s]", e.path.c_str());
                preview.failed = true;
            }
        }
        // マテリアルスロットを初回ロード (materials/slotName.mat -> per-slot MaterialPreview)
        if (preview.handle.IsValid() && !preview.materialsLoaded) {
            preview.materialsLoaded = true;
            const std::filesystem::path pkgDir =
                util::FileSystem::PathFromUtf8(e.path).parent_path();
            const std::string matDir = util::FileSystem::NormalizePathSeparators(
                util::FileSystem::PathToUtf8(pkgDir / "materials"));
            if (const asset::ModelAsset* m0 = asset::AssetManager::Get(preview.handle)) {
                preview.slotMaterials.resize(m0->materialSlotNames.size());
                for (size_t si = 0; si < m0->materialSlotNames.size(); ++si) {
                    MaterialPreview& mp = preview.slotMaterials[si];
                    const std::string matPath = matDir + "/" + m0->materialSlotNames[si] + ".mat";
                    mp.failed = !asset::LoadMaterialAssetFromFile(matPath, mp.asset);
                    mp.loaded = true;
                }
            }
        }
        if (!preview.thumbnailRendered && !preview.failed && preview.thumbnailRT.IsValid()) {
            const asset::ModelAsset* m = asset::AssetManager::Get(preview.handle);
            if (m && !m->lods.empty() && !m->lods[0].submeshes.empty()) {
                // 全サブメッシュの AABB から共通カメラを計算 (Unity 同様すべてのメッシュが写る)
                constexpr float kInf = std::numeric_limits<float>::max();
                math::Vector3 bMin = { kInf,  kInf,  kInf  };
                math::Vector3 bMax = { -kInf, -kInf, -kInf };
                for (const auto& sub : m->lods[0].submeshes) {
                    if (!sub.mesh) continue;
                    const math::Vector3 c = sub.mesh->boundsCenter;
                    const float r = sub.mesh->boundsRadius;
                    bMin.x = std::min(bMin.x, c.x - r);  bMax.x = std::max(bMax.x, c.x + r);
                    bMin.y = std::min(bMin.y, c.y - r);  bMax.y = std::max(bMax.y, c.y + r);
                    bMin.z = std::min(bMin.z, c.z - r);  bMax.z = std::max(bMax.z, c.z + r);
                }
                const math::Vector3 combinedCenter = {
                    (bMin.x + bMax.x) * 0.5f, (bMin.y + bMax.y) * 0.5f, (bMin.z + bMax.z) * 0.5f };
                const float combinedRadius = std::max({ bMax.x - bMin.x,
                                                        bMax.y - bMin.y,
                                                        bMax.z - bMin.z }) * 0.5f;

                bool firstDraw = true;
                for (const auto& sub : m->lods[0].submeshes) {
                    if (!sub.mesh) continue;
                    auto* mesh = sub.mesh.get();
                    // WHY: resources 未初期化時にロードされた場合 GPU バッファが未作成。CPU データから lazily 作成。
                    if (!mesh->vertexBuffer.IsValid()) {
                        if (!mesh->cpuVertices.empty())
                            mesh->vertexBuffer = ctx.resources->CreateVertexBuffer(
                                mesh->cpuVertices.data(),
                                mesh->cpuVertices.size() * sizeof(renderer::Vertex),
                                sizeof(renderer::Vertex));
                        else if (!mesh->cpuSkinnedVertices.empty())
                            mesh->vertexBuffer = ctx.resources->CreateVertexBuffer(
                                mesh->cpuSkinnedVertices.data(),
                                mesh->cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex),
                                sizeof(renderer::SkinnedVertex));
                    }
                    if (!mesh->indexBuffer.IsValid() && !mesh->cpuIndices.empty())
                        mesh->indexBuffer = ctx.resources->CreateIndexBuffer(
                            mesh->cpuIndices.data(), static_cast<uint32_t>(mesh->cpuIndices.size()));

                    MaterialPreview* matPrev = nullptr;
                    if (sub.materialSlotIndex < preview.slotMaterials.size() &&
                        !preview.slotMaterials[sub.materialSlotIndex].failed) {
                        matPrev = &preview.slotMaterials[sub.materialSlotIndex];
                        if (!RebuildMaterialThumbnailGpuData(*matPrev, ctx))
                            matPrev = nullptr;
                    }
                    const bool ok = RenderMeshThumbnail(
                        *ctx.renderer, *ctx.resources,
                        *mesh, preview.thumbnailRT,
                        renderer::ResourceHandle<renderer::TextureTag>{},
                        matPrev ? SelectMaterialColor(matPrev->asset) : ImVec4{ 0.74f, 0.78f, 0.84f, 1.0f },
                        matPrev ? matPrev->shader : renderer::ResourceHandle<renderer::ShaderTag>{},
                        matPrev ? matPrev->materialCB : renderer::ResourceHandle<renderer::ConstantBufferTag>{},
                        matPrev ? &matPrev->textures : nullptr,
                        matPrev ? DetectThumbnailShaderFlavor(matPrev->shaderPath) : ThumbnailShaderFlavor::Surface,
                        matPrev ? &matPrev->asset : nullptr,
                        firstDraw, combinedCenter, combinedRadius);
                    if (ok) { preview.thumbnailRendered = true; firstDraw = false; }
                }
            }
            if (!preview.thumbnailRendered) preview.failed = true;
        }
        if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, "FZASSET")) return;
    }

    // 仮想 .mesh サブアセット (::mesh:: 合成パス): .fzasset 内の特定サブメッシュを 3D プレビュー
    {
        const auto mark = e.path.find("::mesh::");
        if (e.ext == ".mesh" && e.isSubAsset && mark != std::string::npos &&
            ctx.renderer && ctx.resources && ctx.imguiRenderer) {
            const std::string parentPath = e.path.substr(0, mark);
            const auto submeshIdx = static_cast<size_t>(std::stoi(e.path.substr(mark + 8)));

            ModelAssetPreview& preview = m_modelAssetPreviews[e.path];
            const auto parentWriteTime = ReadLastWriteTime(parentPath);
            if (parentWriteTime != preview.lastWriteTime) {
                preview.lastWriteTime    = parentWriteTime;
                preview.handle           = {};
                preview.thumbnailRendered = false;
                preview.failed           = false;
            }
            EnsureThumbnailRT(preview, ctx);
            if (!preview.handle.IsValid() && !preview.failed) {
                asset::AssetManager::FlushFailed();
                preview.handle = asset::AssetManager::Load<asset::ModelAsset>(parentPath);
                if (!preview.handle.IsValid()) preview.failed = true;
            }
            if (!preview.thumbnailRendered && !preview.failed && preview.thumbnailRT.IsValid()) {
                const asset::ModelAsset* m = asset::AssetManager::Get(preview.handle);
                if (m && !m->lods.empty() && submeshIdx < m->lods[0].submeshes.size() &&
                    m->lods[0].submeshes[submeshIdx].mesh) {
                    auto* mesh = m->lods[0].submeshes[submeshIdx].mesh.get();
                    if (!mesh->vertexBuffer.IsValid()) {
                        if (!mesh->cpuVertices.empty())
                            mesh->vertexBuffer = ctx.resources->CreateVertexBuffer(
                                mesh->cpuVertices.data(),
                                mesh->cpuVertices.size() * sizeof(renderer::Vertex),
                                sizeof(renderer::Vertex));
                        else if (!mesh->cpuSkinnedVertices.empty())
                            mesh->vertexBuffer = ctx.resources->CreateVertexBuffer(
                                mesh->cpuSkinnedVertices.data(),
                                mesh->cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex),
                                sizeof(renderer::SkinnedVertex));
                    }
                    if (!mesh->indexBuffer.IsValid() && !mesh->cpuIndices.empty())
                        mesh->indexBuffer = ctx.resources->CreateIndexBuffer(
                            mesh->cpuIndices.data(), static_cast<uint32_t>(mesh->cpuIndices.size()));
                    preview.thumbnailRendered = RenderMeshThumbnail(
                        *ctx.renderer, *ctx.resources,
                        *mesh, preview.thumbnailRT,
                        renderer::ResourceHandle<renderer::TextureTag>{},
                        { 0.74f, 0.78f, 0.84f, 1.0f });
                }
                if (!preview.thumbnailRendered) preview.failed = true;
            }
            if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, "MESH")) return;
        }
    }

    if (IsMeshExt(e.ext) && e.path.find("::mesh::") == std::string::npos && ctx.renderer && ctx.resources && ctx.imguiRenderer) {
        MeshPreview& preview = m_meshPreviews[e.path];
        const auto currentWriteTime = ReadLastWriteTime(e.path);
        if (currentWriteTime != preview.lastWriteTime) {
            preview.lastWriteTime = currentWriteTime;
            preview.model = nullptr;
            preview.thumbnailRendered = false;
            preview.failed = false;
        }
        EnsureThumbnailRT(preview, ctx);
        if (!preview.model && !preview.failed)
            preview.model = asset::AssetManager::LoadModel(e.path);
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
        const char* badge = (e.ext == ".asset") ? "ASSET" : "MESH";
        if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, badge)) return;
    }

    // .prefab: TOML を解析してメッシュを持つ場合は 3D サムネイル、なければキューブアイコン
    if (e.ext == ".prefab") {
        if (ctx.renderer && ctx.resources && ctx.imguiRenderer) {
            PrefabPreview& preview = m_prefabPreviews[e.path];
            const auto currentWriteTime = ReadLastWriteTime(e.path);
            if (currentWriteTime != preview.lastWriteTime) {
                preview = {};
                preview.lastWriteTime = currentWriteTime;
            }
            if (!preview.parsed) {
                preview.parsed = true;
                std::string text;
                if (util::FileSystem::ReadText(e.path, text)) {
                    toml::parse_result result = toml::parse(text);
                    if (result) {
                        if (auto* gos = result.table()["gameobjects"].as_array()) {
                            for (const auto& item : *gos) {
                                const auto* goTbl = item.as_table();
                                if (!goTbl) continue;
                                // SkinnedMeshRenderer を優先 (フルモデルパス)
                                if (auto* smrTbl = (*goTbl)["SkinnedMeshRenderer"].as_table()) {
                                    const std::string mp = (*smrTbl)["modelPath"].value_or(std::string{});
                                    if (!mp.empty()) {
                                        preview.meshPath = mp;
                                        preview.hasMesh = true;
                                        break;
                                    }
                                }
                                if (auto* mrTbl = (*goTbl)["MeshRenderer"].as_table()) {
                                    const std::string mp = (*mrTbl)["mesh"].value_or(std::string{});
                                    if (!mp.empty()) {
                                        preview.meshPath = mp;
                                        preview.hasMesh = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (preview.hasMesh && !preview.failed) {
                EnsureThumbnailRT(preview, ctx);
                if (!preview.model) {
                    std::string absPath = preview.meshPath;
                    if (absPath.starts_with("Assets/") && !ctx.projectRoot.empty())
                        absPath = ctx.projectRoot + "/" + absPath;
                    preview.model = asset::AssetManager::LoadModel(absPath);
                }
                if (!preview.thumbnailRendered && preview.thumbnailRT.IsValid() &&
                    preview.model && !preview.model->meshes.empty() && preview.model->meshes.front()) {
                    preview.thumbnailRendered = RenderMeshThumbnail(
                        *ctx.renderer, *ctx.resources,
                        *preview.model->meshes.front(),
                        preview.thumbnailRT,
                        renderer::ResourceHandle<renderer::TextureTag>{},
                        { 0.35f, 0.82f, 0.95f, 1.0f });
                    preview.failed = !preview.thumbnailRendered;
                }
                if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, "PREFAB")) return;
            }
        }
        // フォールバック: アイソメトリックキューブアイコン
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 bg    = IM_COL32( 28,  68,  84, 255);
            const ImU32 front = IM_COL32( 50, 140, 168, 255);
            const ImU32 top   = IM_COL32( 72, 172, 200, 255);
            const ImU32 right = IM_COL32( 36, 108, 132, 255);
            const ImU32 brd   = IM_COL32(110, 215, 235, 255);
            dl->AddRectFilled({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, bg, sz * 0.08f);
            dl->AddRect      ({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, brd, sz * 0.08f, 0, 1.0f);

            const float cx = origin.x + sz * 0.48f;
            const float cy = origin.y + sz * 0.54f;
            const float hw = sz * 0.22f; // front face half-width
            const float hh = sz * 0.20f; // front face half-height
            const float dx = sz * 0.14f; // depth x-offset
            const float dy = sz * 0.09f; // depth y-offset

            // front face
            ImVec2 frontFace[4] = {
                { cx - hw,      cy - hh },
                { cx + hw,      cy - hh },
                { cx + hw,      cy + hh },
                { cx - hw,      cy + hh },
            };
            dl->AddConvexPolyFilled(frontFace, 4, front);

            // top face
            ImVec2 topFace[4] = {
                { cx - hw,      cy - hh      },
                { cx + hw,      cy - hh      },
                { cx + hw + dx, cy - hh - dy },
                { cx - hw + dx, cy - hh - dy },
            };
            dl->AddConvexPolyFilled(topFace, 4, top);

            // right face
            ImVec2 rightFace[4] = {
                { cx + hw,      cy - hh      },
                { cx + hw + dx, cy - hh - dy },
                { cx + hw + dx, cy + hh - dy },
                { cx + hw,      cy + hh      },
            };
            dl->AddConvexPolyFilled(rightFace, 4, right);

            dl->AddPolyline(frontFace, 4, brd, ImDrawFlags_Closed, 1.0f);
            dl->AddPolyline(topFace,   4, brd, ImDrawFlags_Closed, 1.0f);
            dl->AddPolyline(rightFace, 4, brd, ImDrawFlags_Closed, 1.0f);
            DrawThumbnailLabel(dl, origin, sz, "PREFAB");
            return;
        }
    }

    // .animcontroller: ステートマシン風アイコン (3ノード + 矢印)
    if (e.ext == ".animcontroller") {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bg   = IM_COL32( 50,  90,  65, 255);
        const ImU32 node = IM_COL32( 80, 200, 120, 255);
        const ImU32 edge = IM_COL32(200, 255, 200, 180);
        const ImU32 brd  = IM_COL32(160, 220, 160, 255);
        dl->AddRectFilled({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, bg, sz * 0.08f);
        dl->AddRect      ({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, brd, sz * 0.08f, 0, 1.0f);
        // node positions: left-mid, top-right, bottom-right
        const float r = sz * 0.10f;
        const ImVec2 n0 = { origin.x + sz * 0.22f, origin.y + sz * 0.50f };
        const ImVec2 n1 = { origin.x + sz * 0.65f, origin.y + sz * 0.28f };
        const ImVec2 n2 = { origin.x + sz * 0.65f, origin.y + sz * 0.72f };
        dl->AddLine(n0, n1, edge, 1.0f);
        dl->AddLine(n0, n2, edge, 1.0f);
        dl->AddLine(n1, n2, edge, 1.0f);
        dl->AddCircleFilled(n0, r * 1.2f, node);
        dl->AddCircleFilled(n1, r,        node);
        dl->AddCircleFilled(n2, r,        node);
        DrawThumbnailLabel(dl, origin, sz, "CTRL");
        return;
    }

    // .terrain: layerMaterials[0] を読み取って layer0 diffuse でサムネイル、なければ丘アイコン
    if (e.ext == ".terrain") {
        if (ctx.renderer && ctx.resources && ctx.imguiRenderer) {
            TerrainPreview& preview = m_terrainPreviews[e.path];
            const auto currentWriteTime = ReadLastWriteTime(e.path);
            if (currentWriteTime != preview.lastWriteTime) {
                preview = {};
                preview.lastWriteTime = currentWriteTime;
            }
            if (!preview.parsed) {
                preview.parsed = true;
                std::string text;
                if (util::FileSystem::ReadText(e.path, text)) {
                    toml::parse_result result = toml::parse(text);
                    if (result) {
                        const toml::table* terrainTbl = result.table()["terrain"].as_table();
                        if (!terrainTbl) terrainTbl = &result.table();
                        std::string matPath;
                        if (const auto* layerArr = (*terrainTbl)["layerMaterials"].as_array();
                            layerArr && !layerArr->empty())
                            matPath = (*layerArr)[0].value_or(std::string{});
                        if (!matPath.empty()) {
                            std::string absMatPath = matPath;
                            if (absMatPath.starts_with("Assets/") && !ctx.projectRoot.empty())
                                absMatPath = ctx.projectRoot + "/" + absMatPath;
                            preview.hasMaterial = asset::LoadMaterialAssetFromFile(absMatPath, preview.mat.asset);
                            preview.mat.loaded = true;
                            preview.mat.lastWriteTime = ReadLastWriteTime(absMatPath);
                        }
                    }
                }
            }
            if (preview.hasMaterial && !preview.mat.failed) {
                EnsureThumbnailRT(preview.mat, ctx);
                if (!preview.mat.thumbnailRendered) {
                    if (!s_tr.materialSphere)
                        s_tr.materialSphere = renderer::PrimitiveMesh::Sphere(*ctx.resources, 64);
                    if (s_tr.materialSphere && RebuildMaterialThumbnailGpuData(preview.mat, ctx)) {
                        preview.mat.thumbnailRendered = RenderMeshThumbnail(
                            *ctx.renderer, *ctx.resources,
                            *s_tr.materialSphere,
                            preview.mat.thumbnailRT,
                            preview.mat.previewTexture,
                            SelectMaterialColor(preview.mat.asset),
                            preview.mat.shader,
                            preview.mat.materialCB,
                            &preview.mat.textures,
                            ThumbnailShaderFlavor::Terrain,
                            &preview.mat.asset);
                        preview.mat.failed = !preview.mat.thumbnailRendered;
                    }
                }
                if (DrawThumbnailIfReady(preview.mat, origin, sz, ctx, hovered, "TERRAIN")) return;
            }
        }
        // フォールバック: 丘シルエットアイコン
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 sky  = IM_COL32( 60, 100,  60, 255);
            const ImU32 land = IM_COL32( 80, 160,  70, 255);
            const ImU32 brd  = IM_COL32(140, 210, 120, 255);
            dl->AddRectFilled({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, sky, sz * 0.08f);
            dl->AddRect      ({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, brd, sz * 0.08f, 0, 1.0f);
            const float base = origin.y + sz * 0.95f;
            const float h    = sz * 0.40f;
            ImVec2 terrain[8] = {
                { origin.x,           base          },
                { origin.x + sz*0.0f, base          },
                { origin.x + sz*0.2f, base - h*0.5f },
                { origin.x + sz*0.4f, base - h      },
                { origin.x + sz*0.6f, base - h*0.6f },
                { origin.x + sz*0.8f, base - h*0.8f },
                { origin.x + sz,      base - h*0.3f },
                { origin.x + sz,      base          },
            };
            dl->AddConvexPolyFilled(terrain, 8, land);
            DrawThumbnailLabel(dl, origin, sz, "TERRAIN");
            return;
        }
    }

    DrawFileIconAt(origin, sz, e, hovered);
}

void AssetBrowserPanel::DrainTexLoadQueue(EditorContext& ctx)
{
    if (!ctx.resources) return;
    constexpr int kMaxPerFrame = 3;
    for (int i = 0; i < kMaxPerFrame && !m_texLoadQueue.empty(); ++i) {
        const std::string path = std::move(m_texLoadQueue.front());
        m_texLoadQueue.pop_front();
        auto it = m_texturePreviews.find(path);
        if (it == m_texturePreviews.end()) continue;
        TexturePreview& preview = it->second;
        if (preview.handle.IsValid() || preview.failed) continue;
        preview.handle = ctx.resources->LoadTexture(ToTextureLoadPath(path, ctx));
        if (preview.handle.IsValid()) {
            if (auto* texture = ctx.resources->Get(preview.handle)) {
                preview.width  = texture->GetWidth();
                preview.height = texture->GetHeight();
            }
        } else {
            preview.failed = true;
        }
    }
}

void AssetBrowserPanel::ResetAssetPreviewCache(const std::string& path)
{
    m_texturePreviews.erase(path);
    auto releaseAndErase = [&](auto& map) {
        auto it = map.find(path);
        if (it == map.end()) return;
        if (m_resources) {
            if constexpr (requires { it->second.thumbnailRT; }) {
                if (it->second.thumbnailRT.IsValid())
                    m_resources->Release(it->second.thumbnailRT);
                if constexpr (requires { it->second.materialCB; }) {
                    if (it->second.materialCB.IsValid())
                        m_resources->Release(it->second.materialCB);
                }
            } else if constexpr (requires { it->second.mat.thumbnailRT; }) {
                if (it->second.mat.thumbnailRT.IsValid())
                    m_resources->Release(it->second.mat.thumbnailRT);
                if (it->second.mat.materialCB.IsValid())
                    m_resources->Release(it->second.mat.materialCB);
            }
        }
        map.erase(it);
    };
    releaseAndErase(m_materialPreviews);
    releaseAndErase(m_meshPreviews);
    releaseAndErase(m_prefabPreviews);
    releaseAndErase(m_terrainPreviews);
    {
        auto it = m_modelAssetPreviews.find(path);
        if (it != m_modelAssetPreviews.end() && m_resources)
            for (auto& mp : it->second.slotMaterials)
                if (mp.materialCB.IsValid())
                    m_resources->Release(mp.materialCB);
    }
    releaseAndErase(m_modelAssetPreviews);
    // 合成パス (path::mesh::N) で登録されたサブメッシュプレビューもクリア
    {
        const std::string synthPrefix = path + "::mesh::";
        for (auto it = m_modelAssetPreviews.begin(); it != m_modelAssetPreviews.end(); ) {
            if (it->first.starts_with(synthPrefix)) {
                if (m_resources) {
                    if (it->second.thumbnailRT.IsValid())
                        m_resources->Release(it->second.thumbnailRT);
                    for (auto& mp : it->second.slotMaterials)
                        if (mp.materialCB.IsValid())
                            m_resources->Release(mp.materialCB);
                }
                it = m_modelAssetPreviews.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_texDescPreviews.erase(path);
}

// ── DrawEntry サブメソッド ──────────────────────────────────────────────────────

void AssetBrowserPanel::DrawEntryBadges(ImDrawList* dl, ImVec2 origin, float sz, const Entry& e)
{
    // 選択ハイライト (単一 = 金枠、複数追加 = 薄い青枠)
    if (!e.isDir && e.path == m_lastClickedPath && m_selectedPaths.empty()) {
        // primary は DrawEntry から渡された ctx に依存するため外側で描画済み
    }
    // ! バッジ: 未変換モデルファイルに赤丸で警告表示
    if (!e.isDir && IsImportableRaw(e.ext)) {
        const float r  = sz * 0.15f;
        const float cx = origin.x + sz - r;
        const float cy = origin.y + r;
        dl->AddCircleFilled({ cx, cy }, r, IM_COL32(220, 50, 50, 230));
        const ImVec2 bsz = ImGui::CalcTextSize("!");
        dl->AddText({ cx - bsz.x * 0.5f, cy - bsz.y * 0.5f }, IM_COL32(255, 255, 255, 255), "!");
    }
    // ↻ バッジ: .asset は存在するが元ファイルが新しい (再インポートが必要)
    if (!e.isDir && IsImportableRaw(e.ext) && m_outdatedPaths.count(e.path) > 0) {
        const float r  = sz * 0.15f;
        const float cx = origin.x + sz - r;
        const float cy = origin.y + r;
        dl->AddCircleFilled({ cx, cy }, r, IM_COL32(220, 130, 20, 230));
        const ImVec2 bsz = ImGui::CalcTextSize("\xe2\x86\xbb");
        dl->AddText({ cx - bsz.x * 0.5f, cy - bsz.y * 0.5f },
                    IM_COL32(255, 255, 255, 255), "\xe2\x86\xbb");
    }
    // 橙ドット: 未保存変更があるアセット
    if (!e.isDir && AssetDirtyRegistry::IsDirty(e.path)) {
        const float r  = sz * 0.10f;
        const float cx = origin.x + r + 2.0f;
        const float cy = origin.y + r + 2.0f;
        dl->AddCircleFilled({ cx, cy }, r, IM_COL32(255, 160, 30, 230));
    }
    // ▶/▼ 展開トグル: fzasset はサブアセットを持つ
    if (!e.isDir && e.ext == ".fzasset") {
        const bool expanded = m_expandedAssets.count(e.path) > 0;
        const float ts  = sz * 0.18f; // 三角サイズ
        const float bx  = origin.x + 2.0f;
        const float by  = origin.y + sz - ts - 2.0f;
        const ImU32 col = IM_COL32(255, 220, 80, 230);
        if (expanded) {
            // ▼ (pointing down)
            dl->AddTriangleFilled(
                { bx,        by },
                { bx + ts,   by },
                { bx + ts * 0.5f, by + ts },
                col);
        } else {
            // ▶ (pointing right)
            dl->AddTriangleFilled(
                { bx,        by },
                { bx,        by + ts },
                { bx + ts,   by + ts * 0.5f },
                col);
        }
    }
    // 金の左ボーダー: サブアセットエントリ
    if (e.isSubAsset) {
        dl->AddRectFilled(
            { origin.x,        origin.y },
            { origin.x + 3.0f, origin.y + sz },
            IM_COL32(255, 200, 50, 200));
    }
}

void AssetBrowserPanel::HandleEntryClick(const Entry& e, EditorContext& ctx, bool hov)
{
    // マウス押下フレーム: ドラッグ・ダブルクリックフラグをリセット (選択はまだしない)
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_entryDragStarted    = false;
        m_doubleClickConsumed = false;
    }

    // 選択確定はマウスリリース時 (Unity スタイル: D&D 開始後はスキップ)
    if (!hov || !ImGui::IsMouseReleased(ImGuiMouseButton_Left) || e.isDir) return;
    if (m_entryDragStarted) return;
    if (m_doubleClickConsumed) { m_doubleClickConsumed = false; return; }

    const bool ctrl  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);

    if (ctrl) {
        if (m_selectedPaths.count(e.path)) m_selectedPaths.erase(e.path);
        else                                m_selectedPaths.insert(e.path);
        ctx.selectedAssetPath = e.path;
        m_lastClickedPath     = e.path;
        m_pendingRenamePath.clear();
    } else if (shift && !m_lastClickedPath.empty()) {
        m_selectedPaths.clear();
        bool inside = false;
        for (const Entry& entry : m_entries) {
            if (entry.isDir) continue;
            if (entry.path == m_lastClickedPath || entry.path == e.path) {
                inside = !inside;
                m_selectedPaths.insert(entry.path);
            } else if (inside) {
                m_selectedPaths.insert(entry.path);
            }
        }
        ctx.selectedAssetPath = e.path;
        m_pendingRenamePath.clear();
    } else {
        // 選択済み & 単体選択状態での再クリック → 遅延リネーム (Unity スタイル)
        // 合成パス (::mesh:: 仮想サブアセット) はリネーム不可
        const bool canRename = !e.isMount && !e.isPackageAsset &&
            e.path.find("::mesh::") == std::string::npos;
        if (canRename && ctx.selectedAssetPath == e.path && m_selectedPaths.empty()) {
            m_pendingRenamePath  = e.path;
            m_pendingRenameTimer = static_cast<float>(ImGui::GetTime());
        } else {
            m_selectedPaths.clear();
            ctx.selectedAssetPath = e.path;
            m_lastClickedPath     = e.path;
            m_pendingRenamePath.clear();
            // FBX コンテンツ更新をクリック時に実施 (ホバーから移行)
            if (IsMeshExt(e.ext)) {
                m_selectedFbxPath = e.path;
                m_selectedModel   = nullptr;
                if (renderer::ResourceManager::Active())
                    m_selectedModel = asset::AssetManager::LoadModel(e.path);
            } else {
                m_selectedFbxPath.clear();
                m_selectedModel = nullptr;
            }
        }
    }
}

void AssetBrowserPanel::HandleEntryDoubleClick(const Entry& e, EditorContext& ctx, bool hov)
{
    if (!hov || !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) return;
    m_pendingRenamePath.clear();   // ダブルクリックは遅延リネームをキャンセル
    m_doubleClickConsumed = true;  // 2回目リリースで HandleEntryClick をスキップ

    // WHY: ダブルクリック後に entries が更新される可能性があるため値をコピーする。
    const bool        isDir = e.isDir;
    const std::string path  = e.path;
    const std::string ext   = e.ext;

    if (isDir) { m_pendingNavigate = path; return; }

    if (ext == ".scene" && ctx.activeScene) {
        if (ctx.requestOpenScene) {
            ctx.requestOpenScene(path);
        } else if (SceneIO::Load(*ctx.activeScene, path)) {
            ctx.selectedEntities.clear();
            if (ctx.undoStack) ctx.undoStack->Clear();
            if (ctx.markSceneDirty) ctx.markSceneDirty();
            FBZZ_LOG_INFO("Opened scene: %s", path.c_str());
        } else {
            FBZZ_LOG_ERROR("Failed to open scene: %s", path.c_str());
        }
    } else if (ext == ".prefab" && ctx.activeScene) {
        const bool canRecordUndo =
            ctx.undoStack != nullptr && ctx.undoStack->IsRecordingEnabled();
        const std::string before = canRecordUndo
            ? SceneIO::Serialize(*ctx.activeScene)
            : std::string{};
        std::vector<scene::EntityID> roots;
        if (PrefabSerializer::Instantiate(*ctx.activeScene, path, roots)) {
            ctx.selectedEntities = roots;
            const std::string after = canRecordUndo
                ? SceneIO::Serialize(*ctx.activeScene)
                : std::string{};
            if (canRecordUndo && before != after) {
                scene::Scene* scene = ctx.activeScene;
                EditorContext* context = &ctx;
                const auto markDirty = ctx.markSceneDirty;
                auto restore = [scene, context, markDirty](const std::string& snapshot) {
                    if (SceneIO::Deserialize(*scene, snapshot)) {
                        context->selectedEntities.clear();
                        if (markDirty) markDirty();
                    }
                };
                ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                    "Instantiate Prefab",
                    [restore, after]() { restore(after); },
                    [restore, before]() { restore(before); }));
            }
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
    } else if (ext == ".animcontroller") {
        ctx.requestOpenAnimationGraph = true;
    }
}

void AssetBrowserPanel::DrawEntryContextMenu(const Entry& e, EditorContext& ctx)
{
    if (!ImGui::BeginPopupContextItem("##entry_ctx")) return;

    // 複数選択時の一括操作
    const bool multiSel = m_selectedPaths.size() > 1 && m_selectedPaths.count(e.path);
    if (multiSel) {
        const int n = static_cast<int>(m_selectedPaths.size());
        char label[64];
        bool includesPackageAsset = false;
        for (const auto& path : m_selectedPaths) {
            if (m_packageAssetPaths.count(path) > 0) {
                includesPackageAsset = true;
                break;
            }
        }

        std::snprintf(label, sizeof(label), "Duplicate %d items", n);
        ImGui::BeginDisabled(includesPackageAsset);
        if (ImGui::MenuItem(label)) {
            std::vector<std::string> paths(m_selectedPaths.begin(), m_selectedPaths.end());
            auto command = std::make_unique<CompositeCommand>("Duplicate Assets");
            for (const auto& srcPath : paths) {
                if (util::FileSystem::IsDirectory(srcPath)) continue;
                const std::string dir  = util::FileSystem::GetDirectory(srcPath);
                const std::string stem = std::filesystem::path(srcPath).stem().string();
                const std::string ext  = util::StringUtils::ToLower(
                    util::FileSystem::GetExtension(srcPath));
                std::string dstPath;
                for (int k = 2; k <= 999; ++k) {
                    dstPath = util::FileSystem::NormalizePathSeparators(
                        dir + stem + "(" + std::to_string(k) + ")" + ext);
                    if (!util::FileSystem::Exists(dstPath)) break;
                }
                if (!dstPath.empty() && util::FileSystem::CopyFile(
                        util::FileSystem::PathFromUtf8(srcPath),
                        util::FileSystem::PathFromUtf8(dstPath))) {
                    EditorContext* context = &ctx;
                    command->Add(std::make_unique<LambdaCommand>(
                        "Duplicate Asset",
                        [srcPath, dstPath, context]() {
                            util::FileSystem::CopyFile(
                                util::FileSystem::PathFromUtf8(srcPath),
                                util::FileSystem::PathFromUtf8(dstPath));
                            context->requestAssetBrowserRefresh = true;
                        },
                        [dstPath, context]() {
                            util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(dstPath));
                            context->requestAssetBrowserRefresh = true;
                        }));
                }
            }
            if (ctx.undoStack && !command->Empty())
                ctx.undoStack->Push(std::move(command));
            RefreshDirectory();
        }
        ImGui::EndDisabled();

        std::snprintf(label, sizeof(label), "Delete %d items", n);
        ImGui::BeginDisabled(includesPackageAsset);
        if (ImGui::MenuItem(label)) {
            std::vector<std::string> paths(m_selectedPaths.begin(), m_selectedPaths.end());
            EditorContext* context = &ctx;
            ModalDialog::OpenConfirm("Delete",
                "Delete " + std::to_string(n) + " selected items?",
                [this, paths, context]() {
                    if (context->undoStack)
                        context->undoStack->Execute(CreateAssetDeleteCommand(paths, *context));
                    m_selectedPaths.clear();
                    RefreshDirectory();
                });
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        if (ImGui::BeginMenu("Create")) { DrawCreateMenu(ctx); ImGui::EndMenu(); }
        ImGui::EndPopup();
        return;
    }

    if (!e.isDir && IsImportableRaw(e.ext)) {
        const bool outdated = m_outdatedPaths.count(e.path) > 0;
        if (outdated) {
            if (ImGui::MenuItem("\xe2\x86\xbb Re-import")) {
                m_pendingImports.push_back({ e.path, {} });
                m_outdatedPaths.erase(e.path);
                m_importAllRequested = true;
            }
        } else {
            if (ImGui::MenuItem("Import")) {
                bool found = false;
                for (const auto& p : m_pendingImports)
                    if (p.path == e.path) { found = true; break; }
                if (!found)
                    m_pendingImports.push_back({ e.path, {} });
                m_importAllRequested = true;
            }
        }
        if (ImGui::MenuItem("Import with Settings...")) {
            m_importSettings.path      = e.path;
            m_importSettings.options   = {};
            m_importSettings.open      = true;
            m_importSettings.visible   = true;
            m_importSettings.needsInit = true;
            m_importSettings.isTexture = false;
        }
        ImGui::Separator();
    }
    if (!e.isDir && IsTextureRaw(e.ext)) {
        if (ImGui::MenuItem("Import Settings...")) {
            m_textureImportSettings.path        = e.path;
            m_textureImportSettings.open        = true;
            m_textureImportSettings.visible     = true;
            m_textureImportSettings.needsInit   = true;
            m_textureImportSettings.fromWatcher = false;
        }
        ImGui::Separator();
    }
    if (!e.isDir && !e.isPackageAsset && ImGui::MenuItem("Duplicate")) {
        const std::string dir  = util::FileSystem::GetDirectory(e.path);
        const std::string name = std::filesystem::path(e.path).stem().string();
        const std::string ext  = e.ext;
        std::string dstPath;
        for (int n = 2; ; ++n) {
            dstPath = util::FileSystem::NormalizePathSeparators(
                dir + name + "(" + std::to_string(n) + ")" + ext);
            if (!util::FileSystem::Exists(dstPath)) break;
            if (n > 999) { dstPath.clear(); break; }
        }
        if (!dstPath.empty()) {
            if (util::FileSystem::CopyFile(
                    util::FileSystem::PathFromUtf8(e.path),
                    util::FileSystem::PathFromUtf8(dstPath))) {
                if (ctx.undoStack) {
                    const std::string srcPath = e.path;
                    EditorContext* context = &ctx;
                    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                        "Duplicate Asset",
                        [srcPath, dstPath, context]() {
                            util::FileSystem::CopyFile(
                                util::FileSystem::PathFromUtf8(srcPath),
                                util::FileSystem::PathFromUtf8(dstPath));
                            context->requestAssetBrowserRefresh = true;
                        },
                        [dstPath, context]() {
                            util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(dstPath));
                            context->requestAssetBrowserRefresh = true;
                        }));
                }
                RefreshDirectory();
            } else {
                FBZZ_LOG_ERROR("Duplicate failed: %s", e.path.c_str());
            }
        }
    }
    ImGui::Separator();
    if (!e.isDir && ImGui::MenuItem("Find References...")) {
        m_findRefs.targetPath = e.path;
        m_findRefs.results.clear();
        m_findRefs.open = true;

        // プロジェクトファイルをスキャンして参照を検索
        const std::string filename = util::FileSystem::GetFilename(e.path);
        const std::string stem     = std::filesystem::path(e.path).stem().string();
        for (const auto& p : util::FileSystem::ListFilesRecursive(
                util::FileSystem::PathFromUtf8(m_rootPath))) {
            const std::string scanPath = util::FileSystem::PathToUtf8(p);
            const std::string scanExt  = util::StringUtils::ToLower(
                util::FileSystem::GetExtension(scanPath));
            if (scanExt != ".scene" && scanExt != ".mat" && scanExt != ".prefab"
                && scanExt != ".animcontroller") continue;
            std::string content;
            util::FileSystem::ReadText(scanPath, content);
            if (content.find(filename) != std::string::npos ||
                content.find(stem)     != std::string::npos) {
                m_findRefs.results.push_back(
                    util::FileSystem::NormalizePathSeparators(scanPath));
            }
        }
    }
    if (ImGui::MenuItem("Reveal in Explorer")) {
        const std::wstring wpath = util::FileSystem::PathFromUtf8(e.path).wstring();
        const std::wstring args  = L"/select," + wpath;
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    ImGui::Separator();
    ImGui::BeginDisabled(e.isMount || e.isPackageAsset);
    if (ImGui::MenuItem("Rename")) {
        m_renamingPath = e.path;
        std::strncpy(m_renameBuffer, e.name.c_str(), sizeof(m_renameBuffer) - 1);
        m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
        m_renameNeedFocus = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete")) {
        const std::string path = e.path;
        EditorContext* context = &ctx;
        ModalDialog::OpenConfirm("Delete",
            "Delete \"" + util::FileSystem::GetFilename(path) + "\"?",
            [this, path, context]() {
                if (context->undoStack)
                    context->undoStack->Execute(CreateAssetDeleteCommand({ path }, *context));
                if (m_selectedFbxPath == path) { m_selectedFbxPath.clear(); m_selectedModel = nullptr; }
                m_selectedPaths.erase(path);
                ResetAssetPreviewCache(path);
                RefreshDirectory();
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

void AssetBrowserPanel::DrawEntryRenameLabel(const Entry& e, EditorContext& ctx)
{
    if (m_renamingPath == e.path) {
        ImGui::SetNextItemWidth(m_iconSize);
        if (m_renameNeedFocus) { ImGui::SetKeyboardFocusHere(); m_renameNeedFocus = false; }
        constexpr ImGuiInputTextFlags renameFlags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
        const bool enterPressed = ImGui::InputText("##rename", m_renameBuffer,
                                                   sizeof(m_renameBuffer), renameFlags);
        if (enterPressed) {
            if (m_renameBuffer[0] != '\0') {
                const std::string dir     = util::FileSystem::GetDirectory(e.path);
                const std::string newPath = dir + m_renameBuffer;
                if (newPath != e.path) {
                    auto doRename = [this, oldPath = e.path, newPath, context = &ctx]() {
                        if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(oldPath),
                                                      util::FileSystem::PathFromUtf8(newPath))) {
                            FBZZ_LOG_ERROR("Rename failed: %s -> %s", oldPath.c_str(), newPath.c_str());
                        } else {
                            if (context->undoStack) {
                                const auto refresh = [context]() {
                                    context->requestAssetBrowserRefresh = true;
                                };
                                context->undoStack->Push(std::make_unique<LambdaCommand>(
                                    "Rename Asset",
                                    [oldPath, newPath, refresh]() {
                                        if (util::FileSystem::Exists(oldPath))
                                            util::FileSystem::Rename(
                                                util::FileSystem::PathFromUtf8(oldPath),
                                                util::FileSystem::PathFromUtf8(newPath));
                                        refresh();
                                    },
                                    [oldPath, newPath, refresh]() {
                                        if (util::FileSystem::Exists(newPath))
                                            util::FileSystem::Rename(
                                                util::FileSystem::PathFromUtf8(newPath),
                                                util::FileSystem::PathFromUtf8(oldPath));
                                        refresh();
                                    }));
                            }
                            if (m_selectedFbxPath == oldPath) m_selectedFbxPath = newPath;
                            ResetAssetPreviewCache(oldPath);
                            RefreshDirectory();
                        }
                    };
                    if (util::FileSystem::Exists(newPath)) {
                        ModalDialog::OpenConfirm("Rename",
                            "\"" + util::FileSystem::GetFilename(newPath) + "\" already exists. Overwrite?",
                            std::move(doRename));
                    } else {
                        doRename();
                    }
                }
            }
            m_renamingPath.clear();
        } else if (ImGui::IsItemDeactivated()) {
            m_renamingPath.clear();
        }
    } else {
        std::string display = e.name;
        while (display.size() > 2 &&
               ImGui::CalcTextSize(display.c_str()).x + ImGui::CalcTextSize("..").x > m_iconSize)
            display.pop_back();
        if (display.size() < e.name.size()) display += "..";

        const float indent = (m_iconSize - ImGui::CalcTextSize(display.c_str()).x) * 0.5f;
        if (indent > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        ImGui::TextUnformatted(display.c_str());

        if (!e.isMount && ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
            m_renamingPath = e.path;
            std::strncpy(m_renameBuffer, e.name.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            m_renameNeedFocus = true;
        }
    }
}

void AssetBrowserPanel::DrawEntry(const Entry& e, EditorContext& ctx)
{
    // 遅延リネームタイマー: ダブルクリック判定後 0.5s 経過でリネーム開始
    if (!m_pendingRenamePath.empty() && m_pendingRenamePath == e.path) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_pendingRenamePath.clear();
        } else if (!e.isMount &&
                   static_cast<float>(ImGui::GetTime()) - m_pendingRenameTimer > 0.5f) {
            m_renamingPath = m_pendingRenamePath;
            std::strncpy(m_renameBuffer, e.name.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            m_renameNeedFocus   = true;
            m_pendingRenamePath.clear();
        }
    }

    ImGui::PushID(e.path.c_str());

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  sz     = m_iconSize;

    ImGui::InvisibleButton("##icon", { sz, sz });
    const bool hov = ImGui::IsItemHovered();

    DrawAssetPreviewIconAt(origin, sz, e, ctx, hov);

    // 選択ハイライト
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!e.isDir && e.path == ctx.selectedAssetPath) {
        dl->AddRect(origin, { origin.x + sz, origin.y + sz }, IM_COL32(255, 200, 80, 220), 3.0f, 0, 2.0f);
    } else if (!e.isDir && m_selectedPaths.count(e.path) > 0) {
        dl->AddRect(origin, { origin.x + sz, origin.y + sz }, IM_COL32(80, 160, 255, 180), 3.0f, 0, 1.5f);
    }
    DrawEntryBadges(dl, origin, sz, e);

    // ドラッグソース (ファイルのみ)
    if (!e.isDir && ImGui::BeginDragDropSource()) {
        m_entryDragStarted = true;  // ドラッグ中はリリース時の選択変更を抑制
        const std::string payloadPath = ToProjectAssetPath(e.path, ctx);
        ImGui::SetDragDropPayload("ASSET_PATH", payloadPath.c_str(), payloadPath.size() + 1);
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::EndDragDropSource();
    }
    // ドロップターゲット (ディレクトリのみ)
    if (e.isDir && ImGui::BeginDragDropTarget()) {
        if (SaveHierarchyPayloadAsPrefab(
                ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, e.path))
            RefreshDirectory();
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            const std::string srcRelPath(static_cast<const char*>(p->Data), p->DataSize - 1);
            const std::string srcAbs = util::FileSystem::NormalizePathSeparators(ctx.projectRoot + "/" + srcRelPath);
            const std::string dstAbs = util::FileSystem::NormalizePathSeparators(
                e.path + "/" + util::FileSystem::GetFilename(srcAbs));
            if (srcAbs != dstAbs && !util::FileSystem::Exists(dstAbs)) {
                if (util::FileSystem::Rename(util::FileSystem::PathFromUtf8(srcAbs),
                                             util::FileSystem::PathFromUtf8(dstAbs))) {
                    if (ctx.undoStack) {
                        EditorContext* context = &ctx;
                        auto applyMove = [context](const std::string& from, const std::string& to) {
                            if (util::FileSystem::Exists(from) && !util::FileSystem::Exists(to))
                                util::FileSystem::Rename(
                                    util::FileSystem::PathFromUtf8(from),
                                    util::FileSystem::PathFromUtf8(to));
                            context->requestAssetBrowserRefresh = true;
                        };
                        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                            "Move Asset",
                            [applyMove, srcAbs, dstAbs]() { applyMove(srcAbs, dstAbs); },
                            [applyMove, srcAbs, dstAbs]() { applyMove(dstAbs, srcAbs); }));
                    }
                    if (ctx.selectedAssetPath == srcAbs) ctx.selectedAssetPath.clear();
                    ResetAssetPreviewCache(srcAbs);
                    InvalidateTreeCache(util::FileSystem::GetDirectory(srcAbs));
                    RefreshDirectory();
                } else {
                    FBZZ_LOG_ERROR("Move failed: %s -> %s", srcAbs.c_str(), dstAbs.c_str());
                }
            }
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

    DrawEntryContextMenu(e, ctx);

    // fzasset の ▶/▼ 三角クリックで展開トグル
    if (hov && e.ext == ".fzasset" && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const float ts  = sz * 0.18f;
        const float bx  = origin.x + 2.0f;
        const float by  = origin.y + sz - ts - 2.0f;
        const ImVec2 mp = ImGui::GetIO().MousePos;
        if (mp.x >= bx && mp.x <= bx + ts && mp.y >= by && mp.y <= by + ts) {
            if (m_expandedAssets.count(e.path))
                m_expandedAssets.erase(e.path);
            else
                m_expandedAssets.insert(e.path);
            m_assetExpandDirty = true;
            ImGui::PopID();
            return;
        }
    }
    HandleEntryClick(e, ctx, hov);
    HandleEntryDoubleClick(e, ctx, hov);
    DrawEntryRenameLabel(e, ctx);

    // FindRefs ポップアップは1つのエントリが最初にレンダリングされた後に開く
    if (m_findRefs.open) {
        ImGui::OpenPopup("##find_refs");
        m_findRefs.open = false;
    }
    DrawFindRefsPopup();

    ImGui::PopID();
}

void AssetBrowserPanel::DrawFindRefsPopup()
{
    ImGui::SetNextWindowSize({ 480.0f, 300.0f }, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##find_refs", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) return;

    ImGui::TextUnformatted("Find References");
    ImGui::SameLine();
    ImGui::TextDisabled("— %s", util::FileSystem::GetFilename(m_findRefs.targetPath).c_str());
    ImGui::Separator();

    if (m_findRefs.results.empty()) {
        ImGui::TextDisabled("(no references found)");
    } else {
        ImGui::TextDisabled("%zu file(s) reference this asset:", m_findRefs.results.size());
        ImGui::Spacing();
        const float avail = ImGui::GetContentRegionAvail().y - 34.0f;
        ImGui::BeginChild("##refs_list", { 0.0f, avail }, true);
        for (const auto& ref : m_findRefs.results) {
            const std::string label = util::FileSystem::GetFilename(ref);
            if (ImGui::Selectable(label.c_str())) {
                // クリックで親フォルダへナビゲート
                m_pendingNavigate = util::FileSystem::GetDirectory(ref);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ref.c_str());
        }
        ImGui::EndChild();
    }

    ImGui::Separator();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ─── FBX 内容プレビュー (サブアセットアイコン) ────────────────────────────────

} // namespace fbzz::editor
