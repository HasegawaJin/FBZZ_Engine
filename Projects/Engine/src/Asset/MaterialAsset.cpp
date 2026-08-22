// FBZZ Engine
// MaterialAsset.cpp | fbzz::asset
// .mat マテリアルアセットの TOML シリアライズ / デシリアライズ
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <sstream>

namespace fbzz::asset {

namespace {

// GeometryPassHelpers の kTextureSlotNames (t0-t7) と一致させる。
// t5-t7 は "tex5"/"tex6"/"tex7" という汎用キーでカスタムシェーダーが自由に利用できる。
constexpr std::array<const char*, 8> kTextureSlots = {
    "albedo",
    "normal",
    "metallic",
    "emissive",
    "ao",
    "tex5",
    "tex6",
    "tex7",
};

std::string ResolveTexturePath(std::string_view materialPath, std::string value)
{
    if (value.empty()) return value;
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.starts_with("Assets/") || value.starts_with("assets/"))
        return value;
    if (value.find('/') != std::string::npos)
        return value;

    // WHY: FBX インポートは materials/<MaterialName>.mat と textures/foo.png を sibling に出す。
    //      旧エクスポーターは basename だけを保存していたため、ここで絶対パスへ補完する。
    const std::filesystem::path materialDir = std::filesystem::path(std::string(materialPath)).parent_path();
    return (materialDir.parent_path() / "textures" / value).string();
}

renderer::BlendMode BlendModeFromString(std::string_view value)
{
    if (value == "AlphaBlend" || value == "Alpha Blend" || value == "Transparent")
        return renderer::BlendMode::ALPHA_BLEND;
    if (value == "Additive")
        return renderer::BlendMode::ADDITIVE;
    if (value == "Premultiplied" || value == "PremultipliedAlpha")
        return renderer::BlendMode::PREMULTIPLIED;
    return renderer::BlendMode::OPAQUE_BLEND;
}

const char* BlendModeToString(renderer::BlendMode mode)
{
    switch (mode) {
    case renderer::BlendMode::ALPHA_BLEND:  return "AlphaBlend";
    case renderer::BlendMode::ADDITIVE:     return "Additive";
    case renderer::BlendMode::PREMULTIPLIED: return "Premultiplied";
    case renderer::BlendMode::OPAQUE_BLEND:
    default:                                return "Opaque";
    }
}

RenderPath RenderPathFromString(std::string_view value)
{
    if (value == "auto" || value == "Auto") return RenderPath::Auto;
    if (value == "particle" || value == "Particle") return RenderPath::Particle;
    if (value == "trail" || value == "Trail") return RenderPath::Trail;
    if (value == "ui" || value == "UI") return RenderPath::UI;
    if (value == "decal" || value == "Decal") return RenderPath::Decal;
    return RenderPath::Auto;
}

const char* RenderPathToString(RenderPath rp)
{
    switch (rp) {
    case RenderPath::Particle: return "particle";
    case RenderPath::Trail:    return "trail";
    case RenderPath::UI:       return "ui";
    case RenderPath::Decal:    return "decal";
    case RenderPath::Auto:
    default:                   return "auto";
    }
}

MeshType MeshTypeFromString(std::string_view value)
{
    if (value == "surface" || value == "Surface") return MeshType::Surface;
    if (value == "skinned" || value == "Skinned") return MeshType::Skinned;
    return MeshType::Any;
}

const char* MeshTypeToString(MeshType mt)
{
    switch (mt) {
    case MeshType::Surface: return "surface";
    case MeshType::Skinned: return "skinned";
    case MeshType::Any:
    default:                return "any";
    }
}

void ReadFloatParam(const toml::table& table, const char* key, MaterialAsset& asset)
{
    if (const auto value = table[key].value<double>())
        asset.params[key] = { static_cast<float>(*value) };
    else if (const auto value = table[key].value<int64_t>())
        asset.params[key] = { static_cast<float>(*value) };
}

void ReadFloatArrayParam(const toml::table& table, const char* key, MaterialAsset& asset)
{
    auto* arr = table[key].as_array();
    if (!arr) return;

    std::vector<float> values;
    values.reserve(arr->size());
    for (const auto& node : *arr) {
        if (const auto value = node.value<double>())
            values.push_back(static_cast<float>(*value));
        else if (const auto value = node.value<int64_t>())
            values.push_back(static_cast<float>(*value));
    }
    if (!values.empty())
        asset.params[key] = std::move(values);
}

void ReadParamsTable(const toml::table& table, MaterialAsset& asset)
{
    for (const auto& [key, node] : table) {
        const std::string name = std::string(key);
        if (const auto value = node.value<double>()) {
            asset.params[name] = { static_cast<float>(*value) };
        } else if (const auto value = node.value<int64_t>()) {
            asset.params[name] = { static_cast<float>(*value) };
        } else if (auto* arr = node.as_array()) {
            std::vector<float> values;
            values.reserve(arr->size());
            for (const auto& item : *arr) {
                if (const auto f = item.value<double>())
                    values.push_back(static_cast<float>(*f));
                else if (const auto i = item.value<int64_t>())
                    values.push_back(static_cast<float>(*i));
            }
            if (!values.empty())
                asset.params[name] = std::move(values);
        }
    }
}

toml::array FloatArrayToToml(const std::vector<float>& values)
{
    toml::array arr;
    for (const float value : values)
        arr.push_back(static_cast<double>(value));
    return arr;
}

} // namespace

bool LoadMaterialAssetFromFile(std::string_view path, MaterialAsset& outAsset)
{
    const std::string pathString(path);
    std::string text;
    if (!util::FileSystem::ReadText(pathString, text)) {
        FBZZ_LOG_WARN("MaterialAsset: cannot open [%s]", pathString.c_str());
        return false;
    }

    // WHY: toml::parse_file(std::string_view) に Editor 側の一時パス表現を直接渡すと、
    //      Windows パス / string_view の寿命 / 終端 NUL の前提が呼び出し先へ漏れる。
    //      Engine の FileSystem で UTF-8/Win32 パスを解決してから本文を parse する。
    toml::parse_result parsed = toml::parse(text, pathString);
    if (!parsed) {
        FBZZ_LOG_WARN("MaterialAsset: parse failed [%s]", pathString.c_str());
        return false;
    }

    MaterialAsset asset;
    // guid: 参照を "Assets/..." パスへ戻してから読む (ランタイムは常にパスを持つ)。
    DecodeGuidRefs(parsed.table());
    const toml::table& table = parsed.table();
    asset.shaderPath = table["shader"].value_or(std::string{});
    asset.blendMode = BlendModeFromString(table["blend_mode"].value_or(std::string{ "Opaque" }));
    asset.doubleSided = table["double_sided"].value_or(false);
    asset.renderQueue = static_cast<int32_t>(table["render_queue"].value_or(
        static_cast<int64_t>(renderer::RenderQueue::GEOMETRY)));
    asset.renderPath = RenderPathFromString(table["render_path"].value_or(std::string{ "auto" }));
    asset.meshType   = MeshTypeFromString(table["mesh_type"].value_or(std::string{ "any" }));

    for (const char* slot : kTextureSlots)
        asset.textures[slot] = {};

    if (auto* textures = table["textures"].as_table()) {
        // WHY: Terrain / Water などの専用シェーダーは標準 t0-t7 以外の意味名
        //      (layer0_diffuse, normalMap1 など) を .mat に保存する。
        //      固定スロットだけを読むと、専用マテリアルを Inspector で保存した時に
        //      テクスチャ参照が消えるため、textures テーブルの全キーを保持する。
        for (const auto& [key, node] : *textures) {
            const std::string name = std::string(key);
            const auto texturePath = node.value<std::string>();
            asset.textures[name] = ResolveTexturePath(path, texturePath ? *texturePath : std::string{});
        }
    }

    if (auto* params = table["params"].as_table())
        ReadParamsTable(*params, asset);

    outAsset = std::move(asset);
    return true;
}

bool SaveMaterialAssetToFile(std::string_view path, const MaterialAsset& asset)
{
    const std::string pathString(path);
    toml::table table;
    table.insert("version", int64_t{ 1 });
    table.insert("shader", asset.shaderPath);
    table.insert("blend_mode", BlendModeToString(asset.blendMode));
    table.insert("double_sided", asset.doubleSided);
    table.insert("render_queue", static_cast<int64_t>(asset.renderQueue));
    table.insert("render_path", RenderPathToString(asset.renderPath));
    table.insert("mesh_type",   MeshTypeToString(asset.meshType));

    toml::table textures;
    // WHY: ロードと同じく、標準スロットに限定せず MaterialAsset が持つ全キーを保存する。
    //      これにより Terrain / Water の意味名テクスチャを generic .mat と同じ保存 API で扱える。
    for (const auto& [slot, texturePath] : asset.textures)
        textures.insert(slot, texturePath);
    for (const char* slot : kTextureSlots) {
        if (!textures.contains(slot))
            textures.insert(slot, std::string{});
    }
    table.insert("textures", std::move(textures));

    toml::table params;
    for (const auto& [name, values] : asset.params) {
        if (values.size() == 1)
            params.insert(name, static_cast<double>(values[0]));
        else
            params.insert(name, FloatArrayToToml(values));
    }
    table.insert("params", std::move(params));

    // ディスク上のテクスチャ / シェーダー参照は guid: 形式にする (リネーム・移動耐性)。
    EncodeGuidRefs(table);

    std::ostringstream out;
    out << table << '\n';
    if (!util::FileSystem::WriteText(pathString, out.str())) {
        FBZZ_LOG_ERROR("MaterialAsset: cannot open [%s]", pathString.c_str());
        return false;
    }
    return true;
}

} // namespace fbzz::asset
