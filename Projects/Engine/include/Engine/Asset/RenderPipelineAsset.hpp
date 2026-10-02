/// @file    RenderPipelineAsset.hpp
/// @brief   Shared, persistent non-Volume rendering configuration.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <Engine/Asset/DataAsset.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::asset {

struct RenderPipelineAssetCodec;

/// @note Volume looks, environment ownership, player options and editor diagnostics remain outside this asset.
/// @see https://docs.unity3d.com/6000.1/Documentation/Manual/srp-setting-render-pipeline-asset.html Unity render pipeline configuration assets.
class RenderPipelineAsset final : public DataAsset {
public:
    static constexpr const char* TYPE_NAME = "RenderPipelineAsset";
    static constexpr int SCHEMA_VERSION = 1;
    const char* GetTypeName() const override { return TYPE_NAME; }
    void Reflect(scene::IReflector& reflector) override;

    /// @return Invalid persistent values leave this asset unchanged.
    [[nodiscard]] bool Capture(const renderer::RenderSettings& source);
    /// @note Only asset-owned fields are replaced; Volume, environment, runtime and diagnostic fields survive.
    void ApplyTo(renderer::RenderSettings& target) const;
    [[nodiscard]] const renderer::RenderSettings& Settings() const { return m_settings; }

private:
    friend struct RenderPipelineAssetCodec;
    renderer::RenderSettings m_settings;
    /// @note Unknown schema-1 fields survive editing and saving without leaking TOML types into the public API.
    std::string m_preservedToml;
    /// @note Structural edits keep each original entry's identity; new entries use a sentinel and renamed entries do not inherit another pass's fields.
    std::vector<size_t> m_preservedPassIndices;
};

/// @pre AssetManager has been initialized for project-relative or GUID references.
/// @return True selects a valid asset; false copies inlineFallback unchanged, including an empty assignment.
[[nodiscard]] bool ResolveRenderPipelineSettings(const renderer::RenderSettings& inlineFallback,
    std::string_view assetReference, renderer::RenderSettings& out);

/// @note Never overwrites an existing file. Validation precedes the first atomic write; failure does not change a caller's assignment.
/// @return The caller registers the resulting .meta/GUID through the existing asset browser refresh after success.
[[nodiscard]] bool CreateRenderPipelineAsset(std::string_view path, const renderer::RenderSettings& source);

} /// @note namespace fbzz::asset
