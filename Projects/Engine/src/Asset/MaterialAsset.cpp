/// @file    MaterialAsset.cpp
/// @brief   .mat マテリアルアセットの TOML シリアライズ / デシリアライズ。
/// @author  Hasegawa Jin
/// @date    2026-06-07
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

// WHY 綴りの揺れを受けるか: 未知の綴りは Opaque へ落ちる。半透明のつもりで書いた .mat が
//     不透明で描かれても «濃く出る» だけなので、綴り違いだと気付けないまま調整を続けることになる。
//     手書きされる綴りは受け付けて、意図と結果がずれる経路を塞ぐ。
renderer::BlendMode BlendModeFromString(std::string_view value)
{
    if (value == "AlphaBlend" || value == "Alpha Blend" || value == "Alpha" || value == "Transparent")
        return renderer::BlendMode::ALPHA_BLEND;
    if (value == "Additive" || value == "Add")
        return renderer::BlendMode::ADDITIVE;
    if (value == "Premultiplied" || value == "PremultipliedAlpha")
        return renderer::BlendMode::PREMULTIPLIED;
    return renderer::BlendMode::OPAQUE_BLEND;
}

DepthTest DepthTestFromString(std::string_view value)
{
    if (value == "Always")       return DepthTest::Always;
    if (value == "Never")        return DepthTest::Never;
    if (value == "Less")         return DepthTest::Less;
    if (value == "Equal")        return DepthTest::Equal;
    if (value == "Greater")      return DepthTest::Greater;
    if (value == "GreaterEqual") return DepthTest::GreaterEqual;
    if (value == "NotEqual")     return DepthTest::NotEqual;
    return DepthTest::LessEqual;
}

const char* DepthTestToString(DepthTest test)
{
    switch (test) {
    case DepthTest::Always:       return "Always";
    case DepthTest::Never:        return "Never";
    case DepthTest::Less:         return "Less";
    case DepthTest::Equal:        return "Equal";
    case DepthTest::Greater:      return "Greater";
    case DepthTest::GreaterEqual: return "GreaterEqual";
    case DepthTest::NotEqual:     return "NotEqual";
    case DepthTest::LessEqual:
    default:                      return "LessEqual";
    }
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
    if (value == "post_process" || value == "PostProcess") return RenderPath::PostProcess;
    return RenderPath::Auto;
}

const char* RenderPathToString(RenderPath rp)
{
    switch (rp) {
    case RenderPath::Particle: return "particle";
    case RenderPath::Trail:    return "trail";
    case RenderPath::UI:       return "ui";
    case RenderPath::Decal:    return "decal";
    case RenderPath::PostProcess: return "post_process";
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

// ── [particle] テーブル ──
// WHY 列挙を文字列で持つか: .mat は人が読み書きするので、alpha_source = 1 より
//     alpha_source = "luminance" の方が «何が起きるか» が読んで分かる。
//     未知の綴りは既定へ落とす (壊れた .mat でも描画は続く)。

scene::ParticleAlphaSource AlphaSourceFromString(std::string_view value)
{
    if (value == "luminance")          return scene::ParticleAlphaSource::Luminance;
    if (value == "luminance_inverted") return scene::ParticleAlphaSource::LuminanceInverted;
    if (value == "red")                return scene::ParticleAlphaSource::Red;
    if (value == "green")              return scene::ParticleAlphaSource::Green;
    if (value == "blue")               return scene::ParticleAlphaSource::Blue;
    if (value == "alpha_inverted")     return scene::ParticleAlphaSource::AlphaInverted;
    return scene::ParticleAlphaSource::TextureAlpha;
}

const char* AlphaSourceToString(scene::ParticleAlphaSource value)
{
    switch (value) {
    case scene::ParticleAlphaSource::Luminance:         return "luminance";
    case scene::ParticleAlphaSource::LuminanceInverted: return "luminance_inverted";
    case scene::ParticleAlphaSource::Red:               return "red";
    case scene::ParticleAlphaSource::Green:             return "green";
    case scene::ParticleAlphaSource::Blue:              return "blue";
    case scene::ParticleAlphaSource::AlphaInverted:     return "alpha_inverted";
    case scene::ParticleAlphaSource::TextureAlpha:
    default:                                            return "texture_alpha";
    }
}

scene::ParticleFlipbookMode FlipbookModeFromString(std::string_view value)
{
    if (value == "fps")         return scene::ParticleFlipbookMode::FramesPerSecond;
    if (value == "random")      return scene::ParticleFlipbookMode::RandomFrame;
    if (value == "ping_pong")   return scene::ParticleFlipbookMode::PingPong;
    return scene::ParticleFlipbookMode::Lifetime;
}

const char* FlipbookModeToString(scene::ParticleFlipbookMode value)
{
    switch (value) {
    case scene::ParticleFlipbookMode::FramesPerSecond: return "fps";
    case scene::ParticleFlipbookMode::RandomFrame:     return "random";
    case scene::ParticleFlipbookMode::PingPong:        return "ping_pong";
    case scene::ParticleFlipbookMode::Lifetime:
    default:                                           return "lifetime";
    }
}

void ReadParticleTable(const toml::table& table, ParticleMaterialSettings& out)
{
    const ParticleMaterialSettings d{};
    const auto flt = [&](const char* key, float fallback) {
        return static_cast<float>(table[key].value_or(static_cast<double>(fallback)));
    };
    const auto integer = [&](const char* key, int fallback) {
        return static_cast<int>(table[key].value_or(static_cast<int64_t>(fallback)));
    };

    out.alphaSource = AlphaSourceFromString(
        table["alpha_source"].value_or(std::string{ AlphaSourceToString(d.alphaSource) }));

    out.flipbook.spriteColumns          = integer("sprite_columns", d.flipbook.spriteColumns);
    out.flipbook.spriteRows             = integer("sprite_rows", d.flipbook.spriteRows);
    out.flipbook.spriteStartFrame       = integer("sprite_start_frame", d.flipbook.spriteStartFrame);
    out.flipbook.spriteEndFrame         = integer("sprite_end_frame", d.flipbook.spriteEndFrame);
    out.flipbook.flipbookMode           = FlipbookModeFromString(
        table["flipbook_mode"].value_or(std::string{ FlipbookModeToString(d.flipbook.flipbookMode) }));
    out.flipbook.flipbookFramesPerSecond = flt("flipbook_fps", d.flipbook.flipbookFramesPerSecond);
    out.flipbook.flipbookFrameBlending  = table["flipbook_frame_blending"].value_or(d.flipbook.flipbookFrameBlending);
    out.flipbook.spriteRandomStartFrame = table["sprite_random_start_frame"].value_or(d.flipbook.spriteRandomStartFrame);
    out.flipbook.spriteRandomRow        = table["sprite_random_row"].value_or(d.flipbook.spriteRandomRow);
    out.flipbook.motionVectorFlipbook   = table["motion_vector_flipbook"].value_or(d.flipbook.motionVectorFlipbook);
    out.flipbook.motionVectorStrength   = flt("motion_vector_strength", d.flipbook.motionVectorStrength);

    out.softParticles            = table["soft_particles"].value_or(d.softParticles);
    out.softParticleFadeDistance = flt("soft_particle_fade_distance", d.softParticleFadeDistance);
    out.cameraFadeNear           = flt("camera_fade_near", d.cameraFadeNear);
    out.cameraFadeFar            = flt("camera_fade_far", d.cameraFadeFar);

    out.distortion          = table["distortion"].value_or(d.distortion);
    out.distortionStrength  = flt("distortion_strength", d.distortionStrength);
    out.distortionChromatic = flt("distortion_chromatic", d.distortionChromatic);

    out.sixWayLighting        = table["six_way_lighting"].value_or(d.sixWayLighting);
    out.lightingStrength      = flt("lighting_strength", d.lightingStrength);
    out.smokeWrap             = flt("smoke_wrap", d.smokeWrap);
    out.smokeTransmission     = flt("smoke_transmission", d.smokeTransmission);
    out.smokeBackScatterPower = flt("smoke_back_scatter_power", d.smokeBackScatterPower);
    out.sixWayMaps            = table["six_way_maps"].value_or(d.sixWayMaps);
    if (const toml::array* color = table["six_way_emission_color"].as_array(); color != nullptr && color->size() >= 3) {
        out.sixWayEmissionColor = { static_cast<float>((*color)[0].value_or(0.0)),
                                    static_cast<float>((*color)[1].value_or(0.0)),
                                    static_cast<float>((*color)[2].value_or(0.0)) };
    }
    out.punctualLighting      = table["punctual_lighting"].value_or(d.punctualLighting);

    out.volumetric           = table["volumetric"].value_or(d.volumetric);
    out.volumetricSteps      = integer("volumetric_steps", d.volumetricSteps);
    out.volumetricDensity    = flt("volumetric_density", d.volumetricDensity);
    out.volumetricAnisotropy = flt("volumetric_anisotropy", d.volumetricAnisotropy);
    out.volumetricNoiseScale = flt("volumetric_noise_scale", d.volumetricNoiseScale);

    out.receiveShadows     = table["receive_shadows"].value_or(d.receiveShadows);
    out.shadowStrength     = flt("shadow_strength", d.shadowStrength);
    out.selfShadowStrength = flt("self_shadow_strength", d.selfShadowStrength);

    out.emissiveScale = flt("emissive_scale", d.emissiveScale);
}

// 既定値と同じものは書かない。
// WHY: 31 個を全部書き出すと、手で開いたときに «この素材で実際に効いている設定» が
//      既定値の海に埋もれる。差分だけ残せば .mat が意図の記録になる。
void WriteParticleTable(const ParticleMaterialSettings& value, toml::table& out)
{
    const ParticleMaterialSettings d{};
    const auto putF = [&](const char* key, float v, float def) {
        if (v != def) out.insert(key, static_cast<double>(v));
    };
    const auto putI = [&](const char* key, int v, int def) {
        if (v != def) out.insert(key, static_cast<int64_t>(v));
    };
    const auto putB = [&](const char* key, bool v, bool def) {
        if (v != def) out.insert(key, v);
    };

    if (value.alphaSource != d.alphaSource)
        out.insert("alpha_source", AlphaSourceToString(value.alphaSource));

    putI("sprite_columns", value.flipbook.spriteColumns, d.flipbook.spriteColumns);
    putI("sprite_rows", value.flipbook.spriteRows, d.flipbook.spriteRows);
    putI("sprite_start_frame", value.flipbook.spriteStartFrame, d.flipbook.spriteStartFrame);
    putI("sprite_end_frame", value.flipbook.spriteEndFrame, d.flipbook.spriteEndFrame);
    if (value.flipbook.flipbookMode != d.flipbook.flipbookMode)
        out.insert("flipbook_mode", FlipbookModeToString(value.flipbook.flipbookMode));
    putF("flipbook_fps", value.flipbook.flipbookFramesPerSecond, d.flipbook.flipbookFramesPerSecond);
    putB("flipbook_frame_blending", value.flipbook.flipbookFrameBlending, d.flipbook.flipbookFrameBlending);
    putB("sprite_random_start_frame", value.flipbook.spriteRandomStartFrame, d.flipbook.spriteRandomStartFrame);
    putB("sprite_random_row", value.flipbook.spriteRandomRow, d.flipbook.spriteRandomRow);
    putB("motion_vector_flipbook", value.flipbook.motionVectorFlipbook, d.flipbook.motionVectorFlipbook);
    putF("motion_vector_strength", value.flipbook.motionVectorStrength, d.flipbook.motionVectorStrength);

    putB("soft_particles", value.softParticles, d.softParticles);
    putF("soft_particle_fade_distance", value.softParticleFadeDistance, d.softParticleFadeDistance);
    putF("camera_fade_near", value.cameraFadeNear, d.cameraFadeNear);
    putF("camera_fade_far", value.cameraFadeFar, d.cameraFadeFar);

    putB("distortion", value.distortion, d.distortion);
    putF("distortion_strength", value.distortionStrength, d.distortionStrength);
    putF("distortion_chromatic", value.distortionChromatic, d.distortionChromatic);

    putB("six_way_lighting", value.sixWayLighting, d.sixWayLighting);
    putF("lighting_strength", value.lightingStrength, d.lightingStrength);
    putF("smoke_wrap", value.smokeWrap, d.smokeWrap);
    putF("smoke_transmission", value.smokeTransmission, d.smokeTransmission);
    putF("smoke_back_scatter_power", value.smokeBackScatterPower, d.smokeBackScatterPower);
    putB("six_way_maps", value.sixWayMaps, d.sixWayMaps);
    if (value.sixWayEmissionColor.x != d.sixWayEmissionColor.x || value.sixWayEmissionColor.y != d.sixWayEmissionColor.y
        || value.sixWayEmissionColor.z != d.sixWayEmissionColor.z) {
        out.insert("six_way_emission_color", toml::array{ static_cast<double>(value.sixWayEmissionColor.x),
                                                          static_cast<double>(value.sixWayEmissionColor.y),
                                                          static_cast<double>(value.sixWayEmissionColor.z) });
    }
    putB("punctual_lighting", value.punctualLighting, d.punctualLighting);

    putB("volumetric", value.volumetric, d.volumetric);
    putI("volumetric_steps", value.volumetricSteps, d.volumetricSteps);
    putF("volumetric_density", value.volumetricDensity, d.volumetricDensity);
    putF("volumetric_anisotropy", value.volumetricAnisotropy, d.volumetricAnisotropy);
    putF("volumetric_noise_scale", value.volumetricNoiseScale, d.volumetricNoiseScale);

    putB("receive_shadows", value.receiveShadows, d.receiveShadows);
    putF("shadow_strength", value.shadowStrength, d.shadowStrength);
    putF("self_shadow_strength", value.selfShadowStrength, d.selfShadowStrength);

    putF("emissive_scale", value.emissiveScale, d.emissiveScale);
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
    asset.depthWrite  = table["depth_write"].value_or(true);
    asset.depthTest   = DepthTestFromString(table["depth_test"].value_or(std::string{ "LessEqual" }));
    asset.renderQueue = static_cast<int32_t>(table["render_queue"].value_or(
        static_cast<int64_t>(renderer::RenderQueue::GEOMETRY)));
    asset.renderPath = RenderPathFromString(table["render_path"].value_or(std::string{ "auto" }));
    asset.meshType   = MeshTypeFromString(table["mesh_type"].value_or(std::string{ "any" }));

    if (auto* keywords = table["keywords"].as_array()) {
        for (const auto& node : *keywords) {
            if (const auto keyword = node.value<std::string>(); keyword && !keyword->empty())
                asset.keywords.push_back(*keyword);
        }
    }

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

    // 未記載のキーは既定値のまま (テーブルごと無くても壊れない)。
    if (auto* particle = table["particle"].as_table())
        ReadParticleTable(*particle, asset.particle);

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
    table.insert("depth_write", asset.depthWrite);
    table.insert("depth_test",  DepthTestToString(asset.depthTest));
    table.insert("render_queue", static_cast<int64_t>(asset.renderQueue));
    table.insert("render_path", RenderPathToString(asset.renderPath));
    table.insert("mesh_type",   MeshTypeToString(asset.meshType));

    if (!asset.keywords.empty()) {
        toml::array keywords;
        for (const std::string& keyword : asset.keywords)
            keywords.push_back(keyword);
        table.insert("keywords", std::move(keywords));
    }

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

    // パーティクル用の .mat だけ [particle] を書く。既定のままの項目は省く。
    if (asset.renderPath == RenderPath::Particle) {
        toml::table particle;
        WriteParticleTable(asset.particle, particle);
        if (!particle.empty()) table.insert("particle", std::move(particle));
    }

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
