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
#include <utility>
#include <vector>

namespace fbzz::asset {
namespace {

struct CachedCapabilities {
    std::filesystem::file_time_type timestamp;
    renderer::ShaderCapabilities value;
};

struct PreparedShaderReference {
    std::string reference;
    std::string guid;
    std::string path;
    std::string metaPath;
    std::filesystem::path nativeSourcePath;
    std::filesystem::path nativeMetaPath;
    uint64_t databaseRevision = 0;
    bool hasDatabaseRevision = false;
};

/// @note Only reference parsing and native path conversion are retained; existence and metadata timestamps are queried on every resolution.
class PreparedShaderReferences {
public:
    PreparedShaderReference& Select(std::string_view reference)
    {
        for (auto& entry : m_entries)
            if (entry.reference == reference) return entry;
        PreparedShaderReference prepared;
        prepared.reference = reference;
        if (AssetDatabase::IsGuidRef(reference)) prepared.guid = AssetDatabase::GuidFromRef(reference);
        if (m_entries.size() < MAXIMUM_REFERENCES) {
            if (m_entries.empty()) m_entries.reserve(MAXIMUM_REFERENCES);
            m_entries.push_back(std::move(prepared));
            return m_entries.back();
        }
        auto& entry = m_entries[m_nextEviction];
        m_nextEviction = (m_nextEviction + 1u) % MAXIMUM_REFERENCES;
        entry = std::move(prepared);
        return entry;
    }
private:
    static constexpr size_t MAXIMUM_REFERENCES = 32;
    std::vector<PreparedShaderReference> m_entries;
    size_t m_nextEviction = 0;
};

PreparedShaderReference& PrepareShaderReference(std::string_view reference)
{
    const bool guidReference = AssetDatabase::IsGuidRef(reference);
    /// @note Reporting callbacks can edit the caller's input; retain the original non-GUID bytes before resolution and cache selection.
    std::string ownedReference;
    std::string resolvedPath;
    if (!guidReference) {
        ownedReference.assign(reference);
        reference = ownedReference;
        resolvedPath = AssetManager::ResolveAssetPath(ownedReference);
    }
    thread_local PreparedShaderReferences preparedReferences;
    auto& prepared = preparedReferences.Select(reference);
    if (guidReference) {
        /// @note Database revision advances for all GUID mapping/ambiguity changes, including Init and Shutdown; metadata freshness remains independent.
        const uint64_t revision = AssetDatabase::Revision();
        if (prepared.hasDatabaseRevision && prepared.databaseRevision == revision) return prepared;
        resolvedPath = AssetDatabase::PathFromGuid(prepared.guid);
        /// @note Tag with the revision read before PathFromGuid; a concurrent mapping edit forces the next call to resolve again.
        prepared.databaseRevision = revision;
        prepared.hasDatabaseRevision = true;
    }
    if (prepared.path != resolvedPath) {
        prepared.path = std::move(resolvedPath);
        prepared.metaPath = prepared.path + ".meta";
        prepared.nativeSourcePath = util::FileSystem::PathFromUtf8(prepared.path);
        prepared.nativeMetaPath.clear();
    }
    return prepared;
}

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
    else if (vertex == "water_surface_v1") result.vertex = ShaderVertexContract::WATER_SURFACE;
    else if (vertex == "fiber_shell_v1") result.vertex = ShaderVertexContract::FIBER_SHELL;
    else if (vertex == "fiber_fin_v1") result.vertex = ShaderVertexContract::FIBER_FIN;
    else if (vertex == "fiber_blade_v1") result.vertex = ShaderVertexContract::FIBER_BLADE;
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
    auto& prepared = PrepareShaderReference(shaderReference);
    /// @note The native-path overload uses the same GetFileAttributesW existence contract, including source symlinks.
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileattributesw GetFileAttributesW symbolic-link behavior.
    if (prepared.path.empty() || !util::FileSystem::Exists(prepared.nativeSourcePath)) return {};
    /// @note Preserve conversion order for absent sources; an empty native metadata path records only that conversion has not run.
    if (prepared.nativeMetaPath.empty()) {
        prepared.nativeMetaPath = std::filesystem::u8path(prepared.metaPath);
    }
    std::error_code error;
    /// @note Keep standard-library target timestamp and sharing-violation handling; no filesystem result enters the prepared-reference cache.
    /// @see https://github.com/microsoft/STL/blob/main/stl/src/filesystem.cpp __std_fs_get_stats follows reparse points and handles sharing violations.
    const auto timestamp = std::filesystem::last_write_time(prepared.nativeMetaPath, error);
    if (error) return {};
    static std::unordered_map<std::string, CachedCapabilities> cache;
    static std::mutex cacheMutex;
    const std::lock_guard lock(cacheMutex);
    const auto found = cache.find(prepared.metaPath);
    if (found != cache.end() && found->second.timestamp == timestamp) return found->second.value;
    const auto capabilities = ReadCapabilities(prepared.metaPath);
    cache.insert_or_assign(prepared.metaPath, CachedCapabilities{timestamp, capabilities});
    return capabilities;
}

} /// @note namespace fbzz::asset
