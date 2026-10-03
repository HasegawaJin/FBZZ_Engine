/// @file    ShaderCapabilities.cpp
/// @brief   シェーダー能力宣言の検証と更新時刻単位のキャッシュ。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <Engine/Asset/ShaderCapabilities.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fbzz::asset {
namespace {

struct CachedCapabilities {
    std::filesystem::file_time_type timestamp;
    renderer::ShaderCapabilities value;
};

renderer::ShaderCapabilities ReadCapabilities(const std::string& path)
{
    using namespace renderer;
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return {};
    auto parsed = toml::parse(text);
    if (!parsed) return {};
    const auto* declaration = parsed.table()["shader"].as_table();
    if (!declaration) return {};
    const auto& table = *declaration;
    if (!table["version"].is_integer() || table["version"].value_or(int64_t{0}) != 1) return {};
    if (table["preview"] && !table["preview"].is_string()) return {};
    ShaderCapabilities result;
    const std::string vertex = table["vertex"].value_or(std::string{});
    const std::string surface = table["surface"].value_or(std::string{});
    const std::string opacity = table["opacity"].value_or(std::string{});
    const std::string variants = table["variants"].value_or(std::string{});
    if (vertex == "standard_surface_v1") result.vertex = ShaderVertexContract::STANDARD_SURFACE;
    else if (vertex == "standard_skinned_v1") result.vertex = ShaderVertexContract::STANDARD_SKINNED;
    else if (vertex != "custom") return {};
    if (surface == "metallic_roughness_v1") result.surface = ShaderSurfaceContract::STANDARD_PBR;
    else if (surface == "lambert_v1") result.surface = ShaderSurfaceContract::LAMBERT;
    else if (surface == "unlit_v1") result.surface = ShaderSurfaceContract::UNLIT;
    else if (surface == "water_v1") result.surface = ShaderSurfaceContract::WATER;
    else if (surface == "terrain_v1") result.surface = ShaderSurfaceContract::TERRAIN;
    else if (surface != "custom") return {};
    if (opacity == "alpha_clip_v1") result.opacity = ShaderOpacityContract::ALPHA_CLIP;
    else if (opacity == "opaque_v1") result.opacity = ShaderOpacityContract::OPAQUE_OUTPUT;
    else if (opacity != "custom") return {};
    if (variants == "standard_surface_v1") result.variants = ShaderVariantSet::STANDARD_SURFACE;
    else if (variants == "standard_skinned_v1") result.variants = ShaderVariantSet::STANDARD_SKINNED;
    else if (variants != "none") return {};
    if (result.variants != ShaderVariantSet::NONE && !result.HasStandardVariants()) return {};
    const std::string preview = table["preview"].value_or(std::string{});
    if (preview.empty()) {
        if (result.surface == ShaderSurfaceContract::WATER) result.preview = ShaderPreviewKind::WATER;
        else if (result.surface == ShaderSurfaceContract::TERRAIN) result.preview = ShaderPreviewKind::TERRAIN;
        else if (result.vertex == ShaderVertexContract::STANDARD_SKINNED) result.preview = ShaderPreviewKind::SKINNED;
        else if (result.vertex == ShaderVertexContract::STANDARD_SURFACE) result.preview = ShaderPreviewKind::SURFACE;
    } else if (preview == "surface") result.preview = ShaderPreviewKind::SURFACE;
    else if (preview == "skinned") result.preview = ShaderPreviewKind::SKINNED;
    else if (preview == "water") result.preview = ShaderPreviewKind::WATER;
    else if (preview == "terrain") result.preview = ShaderPreviewKind::TERRAIN;
    else if (preview == "ui") result.preview = ShaderPreviewKind::UI;
    else if (preview == "particle") result.preview = ShaderPreviewKind::PARTICLE;
    else if (preview == "trail") result.preview = ShaderPreviewKind::TRAIL;
    else if (preview == "decal") result.preview = ShaderPreviewKind::DECAL;
    else if (preview == "post_process") result.preview = ShaderPreviewKind::POST_PROCESS;
    else if (preview == "mesh_trail") result.preview = ShaderPreviewKind::MESH_TRAIL;
    else if (preview == "gpu_particle") result.preview = ShaderPreviewKind::GPU_PARTICLE;
    else if (preview == "fiber_shell") result.preview = ShaderPreviewKind::FIBER_SHELL;
    else if (preview == "fiber_fin") result.preview = ShaderPreviewKind::FIBER_FIN;
    else if (preview == "fiber_blade") result.preview = ShaderPreviewKind::FIBER_BLADE;
    else return {};
    return result;
}

} /// @note namespace

renderer::ShaderCapabilities ResolveShaderCapabilities(std::string_view shaderReference)
{
    if (shaderReference.empty()) return {};
    const std::string reference(shaderReference);
    const std::string path = AssetDatabase::IsGuidRef(reference)
        ? AssetDatabase::PathFromGuid(AssetDatabase::GuidFromRef(reference))
        : AssetManager::ResolveAssetPath(reference);
    if (path.empty() || !util::FileSystem::Exists(path)) return {};
    const std::string metaPath = path + ".meta";
    std::error_code error;
    const auto timestamp = std::filesystem::last_write_time(std::filesystem::u8path(metaPath), error);
    if (error) return {};
    static std::unordered_map<std::string, CachedCapabilities> cache;
    static std::mutex cacheMutex;
    const std::lock_guard lock(cacheMutex);
    const auto found = cache.find(metaPath);
    if (found != cache.end() && found->second.timestamp == timestamp) return found->second.value;
    const auto capabilities = ReadCapabilities(metaPath);
    cache.insert_or_assign(metaPath, CachedCapabilities{timestamp, capabilities});
    return capabilities;
}

} /// @note namespace fbzz::asset
