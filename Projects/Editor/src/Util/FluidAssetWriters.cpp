/// @file    FluidAssetWriters.cpp
/// @brief   焼いたフリップブックを .mat と単層・複数層の .vfx へ書き出す。
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Editor/Util/FluidAssetWriters.hpp>

#include <Editor/Util/AssetPath.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/Uuid.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <locale>
#include <system_error>
#include <utility>

namespace fbzz::editor {
namespace {

// .mat からの参照は guid で書く。パスで書くと素材を別フォルダーへ移した瞬間に外れる。
std::string GuidReference(const std::string& diskPath)
{
    const std::string guid = asset::AssetDatabase::GuidFromPath(diskPath);
    if (guid.empty()) return NormalizeAssetPath(diskPath);
    return std::string(asset::AssetDatabase::kGuidPrefix) + guid;
}

bool EndsWith(const std::string& text, const std::string& suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string TomlEscape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

void ApplyFlat(const asset::FluidBakeResult& bake, asset::MaterialAsset& material)
{
    material.renderPath = asset::RenderPath::Particle;
    material.blendMode  = bake.blendMode;
    material.textures["albedo"] = GuidReference(bake.albedoPath);
    if (!bake.motionVectorPath.empty()) material.textures["tex5"] = GuidReference(bake.motionVectorPath);
    else                                material.textures.erase("tex5");

    asset::ParticleFlipbookSettings& flipbook = material.particle.flipbook;
    flipbook.spriteColumns           = bake.columns;
    flipbook.spriteRows              = bake.rows;
    flipbook.spriteStartFrame        = 0;
    flipbook.spriteEndFrame          = 0;
    flipbook.spriteRandomRow         = false;
    flipbook.flipbookMode            = bake.flipbookMode;
    flipbook.flipbookFramesPerSecond = bake.framesPerSecond;
    flipbook.flipbookFrameBlending   = true;
    flipbook.motionVectorFlipbook    = !bake.motionVectorPath.empty();
    flipbook.motionVectorStrength    = bake.motionVectorStrength;
    material.particle.distortion     = bake.distortion;
    material.particle.emissiveScale  = bake.emissiveScale;
    material.particle.alphaSource    = scene::ParticleAlphaSource::TextureAlpha;
}

void ApplyVolume(const FluidMaterialSource& source, asset::MaterialAsset& material)
{
    const asset::VolumeFlipbookBakeResult& result = source.volume;
    material.textures["albedo"] = NormalizeAssetPath(result.colorPath);
    // 歪みマップは MV を焼かない。前回の MV を残すと、別の流れで warp される。
    if (!result.motionPath.empty()) material.textures["tex5"] = NormalizeAssetPath(result.motionPath);
    else                            material.textures.erase("tex5");
    // 歪みは 2D の Distortion と同じく通常のアルファ合成で、albedo の RG を曲げる向きとして読む。
    material.blendMode =
        source.volumeDistortion ? renderer::BlendMode::ALPHA_BLEND : renderer::BlendMode::PREMULTIPLIED;
    auto& particle = material.particle;
    particle.alphaSource = scene::ParticleAlphaSource::TextureAlpha;
    particle.flipbook.spriteColumns = result.columns;
    particle.flipbook.spriteRows = result.rows;
    particle.flipbook.spriteStartFrame = 0;
    particle.flipbook.spriteEndFrame = result.frameCount - 1;
    particle.flipbook.spriteRandomRow = false;
    particle.flipbook.spriteRandomStartFrame = false;
    // MV は spriteBlend が 0 だと一切効かない。
    particle.flipbook.flipbookFrameBlending = true;
    particle.flipbook.motionVectorFlipbook = !result.motionPath.empty();
    particle.flipbook.motionVectorStrength = result.motionPath.empty() ? 0.0f : result.recommendedStrength;
    particle.emissiveScale = result.suggestedEmissiveScale;
    particle.distortion = source.volumeDistortion;
    if (!source.volumeDistortion && !result.sixWayPositivePath.empty() && !result.sixWayNegativePath.empty()) {
        // 6 方向マップを焼いたなら、色の Atlas ではなくマップで陰影を付ける (光の向きに追従する)。
        // マップはストレートの明るさなので、合成も通常のアルファへ戻す。
        material.textures["albedo"] = NormalizeAssetPath(result.sixWayPositivePath);
        material.textures["emissive"] = NormalizeAssetPath(result.sixWayNegativePath);
        material.blendMode = renderer::BlendMode::ALPHA_BLEND;
        particle.sixWayMaps = true;
        particle.sixWayLighting = false;
        particle.volumetric = false;
        particle.punctualLighting = true;
        particle.sixWayEmissionColor = result.sixWayEmissionColor;
        particle.emissiveScale = 1.0f;
    } else {
        particle.sixWayMaps = false;
        // 前回 6-way で焼いた _6wayN が emissive に残ると、6-way を切った焼き直しでも光って見える。
        const auto emissive = material.textures.find("emissive");
        if (emissive != material.textures.end()
            && (EndsWith(emissive->second, "_6wayN.dds") || EndsWith(emissive->second, "_6wayN.png")))
            material.textures.erase(emissive);
    }
    if (source.volumeLoops) {
        particle.flipbook.flipbookMode = scene::ParticleFlipbookMode::FramesPerSecond;
        particle.flipbook.flipbookFramesPerSecond = source.volumeFramesPerSecond;
    } else {
        particle.flipbook.flipbookMode = scene::ParticleFlipbookMode::Lifetime;
    }
}

} // namespace

FluidMaterialSource FluidMaterialSource::FromFlat(const asset::FluidBakeResult& result)
{
    FluidMaterialSource source;
    source.kind = Kind::Flat2D;
    source.flat = result;
    return source;
}

FluidMaterialSource FluidMaterialSource::FromVolume(const asset::VolumeFlipbookBakeResult& result,
                                                    const asset::VolumeFlipbookBakeSettings& settings)
{
    FluidMaterialSource source;
    source.kind = Kind::Volume3D;
    source.volume = result;
    source.volumeLoops = asset::VolumeBakeLoops(settings);
    source.volumeFramesPerSecond = 1.0f / (std::max)(settings.source.frameDt, 1.0e-4f);
    source.volumeDistortion = settings.distortion;
    return source;
}

std::string SiblingMaterialPath(const std::string& fluidPath)
{
    std::filesystem::path path = util::FileSystem::PathFromUtf8(fluidPath);
    path.replace_extension(".mat");
    return util::FileSystem::PathToUtf8(path);
}

asset::MaterialAsset NewFluidParticleMaterial()
{
    asset::MaterialAsset material;
    material.doubleSided = true;
    material.depthWrite  = false;
    material.renderQueue = 3000;
    material.meshType    = asset::MeshType::Surface;
    material.renderPath  = asset::RenderPath::Particle;
    return material;
}

void ApplyFluidBakeToMaterial(const FluidMaterialSource& source, asset::MaterialAsset& material)
{
    if (source.kind == FluidMaterialSource::Kind::Flat2D) ApplyFlat(source.flat, material);
    else                                                  ApplyVolume(source, material);
}

bool WriteFluidParticleMaterial(const std::string& materialPath, const FluidMaterialSource& source,
                                bool& outCreated, std::string& outError)
{
    outCreated = false;
    asset::MaterialAsset material;
    if (util::FileSystem::Exists(materialPath)) {
        // 読めない .mat を既定で上書きすると、人が手で直している途中のファイルを消す。
        if (!asset::LoadMaterialAssetFromFile(materialPath, material)) {
            outError = "既存のマテリアルを読み込めません: " + materialPath;
            return false;
        }
    } else {
        material = NewFluidParticleMaterial();
        outCreated = true;
    }
    ApplyFluidBakeToMaterial(source, material);
    if (!asset::SaveMaterialAssetToFile(materialPath, material)) {
        outError = "マテリアルを書き出せません: " + materialPath;
        return false;
    }
    (void)asset::AssetDatabase::GuidFromPath(materialPath);
    return true;
}

const char* FluidEffectTemplateName(FluidEffectTemplate preset)
{
    switch (preset) {
    case FluidEffectTemplate::LANDING: return "Landing";
    case FluidEffectTemplate::CHARGE_RELEASE: return "Charge Release";
    case FluidEffectTemplate::MAGIC_ERUPTION: return "Magic Eruption";
    default: return "";
    }
}

std::vector<FluidEffectLayer> MakeFluidEffectLayers(FluidEffectTemplate preset)
{
    std::vector<FluidEffectLayer> layers;
    const auto add = [&](const char* name, asset::FluidPreset material, float delay, float size) {
        FluidEffectLayer layer;
        layer.name = name;
        layer.recipe = asset::MakeFluidPreset(material);
        layer.startDelay = delay;
        layer.size = size;
        // 組み合わせの初回作成は平面アトラスで揃える。各素材は後から 3D で焼き直せる。
        layer.recipe.bake.mode = asset::FluidBakeMode::Flat2D;
        layer.recipe.output.frameSize = 128;
        layer.recipe.output.columns = 8;
        layer.recipe.output.rows = 4;
        layer.recipe.output.supersampling = 2;
        layer.recipe.output.loop = false;
        layer.recipe.output.warmup = 0.0f;
        layer.recipe.output.vectorField = false;
        layers.push_back(std::move(layer));
    };
    switch (preset) {
    case FluidEffectTemplate::LANDING:
        add("ImpactRing", asset::FluidPreset::GroundRing, 0.0f, 3.0f);
        add("Dust", asset::FluidPreset::DustBurst, 0.08f, 2.6f);
        add("AfterMist", asset::FluidPreset::ColdMist, 0.25f, 2.0f);
        break;
    case FluidEffectTemplate::CHARGE_RELEASE:
        add("Charge", asset::FluidPreset::ChargeVortex, 0.0f, 2.0f);
        add("Release", asset::FluidPreset::PlasmaBurst, 0.85f, 2.8f);
        add("Embers", asset::FluidPreset::EmberBurst, 0.95f, 2.4f);
        break;
    case FluidEffectTemplate::MAGIC_ERUPTION:
        add("Seal", asset::FluidPreset::SigilFlare, 0.0f, 2.6f);
        add("Core", asset::FluidPreset::PlasmaBurst, 0.25f, 1.8f);
        add("Mist", asset::FluidPreset::ColdMist, 0.5f, 3.0f);
        break;
    default: break;
    }
    return layers;
}

bool WriteLayeredFluidVfx(const std::filesystem::path& file, const std::string& rootName,
    std::span<const FluidEffectLayer> layers, std::span<const std::string> materialPaths, std::string& outError)
{
    const auto fail = [&](const std::string& message) {
        outError = message;
        return false;
    };
    std::error_code error;
    if (std::filesystem::exists(file, error) || error)
        return fail("既存の VFX は上書きしません: " + util::FileSystem::PathToUtf8(file));
    if (layers.empty() || layers.size() != materialPaths.size()) return fail("素材とレイヤーの数が一致しません");
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const auto& layer = layers[i];
        if (!std::isfinite(layer.startDelay) || layer.startDelay < 0.0f
            || !std::isfinite(layer.size) || layer.size <= 0.0f
            || !std::isfinite(layer.recipe.output.duration) || layer.recipe.output.duration <= 0.0f
            || !std::isfinite(layer.position.x) || !std::isfinite(layer.position.y) || !std::isfinite(layer.position.z))
            return fail("レイヤーの時間・大きさ・位置が不正です");
        if (!util::FileSystem::Exists(materialPaths[i])) return fail("素材がありません: " + materialPaths[i]);
    }
    const std::string rootId = util::GenerateUUID();
    const auto temporary = util::FileSystem::PathFromUtf8(
        util::FileSystem::PathToUtf8(file) + "." + rootId + ".tmp");
    std::ofstream out(temporary, std::ios::binary);
    if (!out) return fail("VFX を書き出せません");
    out.imbue(std::locale::classic());
    out << "[scene]\nformat_version = 1\n\n[prefab]\nformat_version = 1\nroot_count = 1\n";
    const auto node = [&](const std::string& name, const std::string& id, const std::string& parent,
                          const std::string& parentId, const math::Vector3& position) {
        out << "\n[[gameobjects]]\nname = \"" << TomlEscape(name) << "\"\ninstanceId = \"" << id
            << "\"\nactive = true\ntag = \"Untagged\"\nlayer = 0\nparent = \"" << TomlEscape(parent)
            << "\"\nparentInstanceId = \"" << parentId << "\"\n"
            << "[gameobjects.transform]\nposition = [" << position.x << ", " << position.y << ", " << position.z
            << "]\nrotation = [0.0, 0.0, 0.0, 1.0]\nscale = [1.0, 1.0, 1.0]\n";
    };
    node(rootName, rootId, "", "", {});
    out << "[gameobjects.VFXComponent]\nenabled = true\nplayOnAwake = true\nloop = false\n"
           "speed = 1.0\nduration = 0.0\nautoDestroy = false\n";
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const auto& layer = layers[i];
        node(layer.name, util::GenerateUUID(), rootName, rootId, layer.position);
        out << "[gameobjects.ParticleEmitter]\ncullingEnabled = false\nlodEnabled = false\n"
            << "startDelay = " << layer.startDelay << "\nduration = " << layer.recipe.output.duration
            << "\nlifetime = " << layer.recipe.output.duration << "\nlifetimeRandom = 0.0\n"
            << "loop = false\nemitRate = 0.0\nmaxParticles = 1\nshape = 0\n"
            << "sizeStart = " << layer.size << "\nsizeEnd = " << layer.size << "\nsizeCurvePower = 1.0\n"
            << "emitVelocity = [0.0, 0.0, 0.0]\nvelocitySpread = 0.0\ngravity = [0.0, 0.0, 0.0]\n"
            << "materialPath = \"" << TomlEscape(GuidReference(materialPaths[i])) << "\"\n"
            << "renderMode = 0\nsortMode = 1\nangularVelocityMin = 0.0\nangularVelocityMax = 0.0\n"
            << "colorVariation = 0.0\ncolorStart = [1.0, 1.0, 1.0, 1.0]\ncolorEnd = [1.0, 1.0, 1.0, 0.0]\n"
            << "bursts = [{ time = 0.0, count = 1, cycles = 1, interval = 0.0, probability = 1.0 }]\n";
    }
    out.close();
    const bool written = static_cast<bool>(out);
    if (written) std::filesystem::rename(temporary, file, error);
    if (!written || error) {
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
        return fail("VFX の保存を完了できません: " + util::FileSystem::PathToUtf8(file));
    }
    (void)asset::AssetDatabase::GuidFromPath(util::FileSystem::PathToUtf8(file));
    outError.clear();
    return true;
}

bool WriteSingleEmitterVfx(const std::filesystem::path& file, const std::string& rootName,
                           const std::string& materialAssetPath, float lifetime)
{
    const std::string rootId = util::GenerateUUID();
    const std::string emitterId = util::GenerateUUID();
    const float life = (std::max)(lifetime, 0.01f);
    const std::string root = TomlEscape(rootName);
    char numbers[160]{};
    // 1 粒ずつ、寿命いっぱいでアトラスを最後まで再生させる (Lifetime モード)。
    std::snprintf(numbers, sizeof(numbers), "duration = %.4f\nemitRate = %.4f\nlifetime = %.4f\n",
                  life, 1.0f / life, life);

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "# FBZZ Engine\n"
        << "# 焼いたフリップブックを 1 層だけ再生するプレビュー用 .vfx。焼き直すたびに上書きされる。\n"
        << "[scene]\nformat_version = 1\n\n"
        << "[prefab]\nformat_version = 1\nroot_count = 1\n\n"
        << "[[gameobjects]]\nname = \"" << root << "\"\ninstanceId = \"" << rootId << "\"\n"
        << "tag = \"Untagged\"\nlayer = 0\nactive = true\nprefabAssetPath = \"\"\nprefabSourceId = \"\"\n"
        << "parent = \"\"\nparentInstanceId = \"\"\n\n"
        << "[gameobjects.transform]\nposition = [0.0, 0.0, 0.0]\nrotation = [0.0, 0.0, 0.0, 1.0]\n"
        << "scale = [1.0, 1.0, 1.0]\n\n"
        << "[gameobjects.VFXComponent]\nenabled = true\nplayOnAwake = true\nloop = true\nspeed = 1.0\n"
        << "duration = 0.0\nautoDestroy = false\n\n"
        << "[[gameobjects]]\nname = \"Flipbook\"\ninstanceId = \"" << emitterId << "\"\n"
        << "tag = \"Untagged\"\nlayer = 0\nactive = true\nprefabAssetPath = \"\"\nprefabSourceId = \"\"\n"
        << "parent = \"" << root << "\"\nparentInstanceId = \"" << rootId << "\"\n\n"
        << "[gameobjects.transform]\nposition = [0.0, 1.0, 0.0]\nrotation = [0.0, 0.0, 0.0, 1.0]\n"
        << "scale = [1.0, 1.0, 1.0]\n\n"
        << "[gameobjects.ParticleEmitter]\ncullingEnabled = false\nlodEnabled = false\n"
        << numbers
        << "loop = true\nmaxParticles = 1\nshape = 0\nsizeStart = 2.0\nsizeEnd = 2.0\nsizeCurvePower = 1.0\n"
        << "lifetimeRandom = 0.0\nemitVelocity = [0.0, 0.0, 0.0]\nvelocitySpread = 0.0\n"
        << "velocityDamping = 0.0\ngravity = [0.0, 0.0, 0.0]\n"
        << "materialPath = \"" << TomlEscape(materialAssetPath) << "\"\n"
        << "renderMode = 0\nsortMode = 1\nangularVelocityMin = 0.0\nangularVelocityMax = 0.0\n"
        << "colorVariation = 0.0\ncolorStart = [1.0, 1.0, 1.0, 1.0]\ncolorEnd = [1.0, 1.0, 1.0, 1.0]\n"
        << "startDelay = 0.0\n";
    return static_cast<bool>(out);
}

} // namespace fbzz::editor
