/// @file    MaterialPreviewCache.hpp
/// @brief   マテリアルプレビューの入力値と資源の寿命を比較する値キー。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once

#include <Editor/Panels/MaterialPreviewCore.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer { class ITexture; }

namespace fbzz::editor::matpreview {

/// @note GPU ハンドルを再利用できる所有者と reset 世代。Shader・asset・project の変更とは別に判定する。
struct PreviewResourceLifetime {
    std::uint64_t instanceId = 0;
    std::uint64_t resetVersion = 0;
    [[nodiscard]] static PreviewResourceLifetime Read(const renderer::ResourceManager& resources);
    bool operator==(const PreviewResourceLifetime&) const = default;
};

struct PreviewResourceKey {
    std::uint64_t instanceId = 0;
    std::uint64_t resetVersion = 0;
    std::uint64_t shaderVersion = 0;
    int assetGeneration = 0;
    std::string projectRoot;
    [[nodiscard]] PreviewResourceLifetime Lifetime() const { return {instanceId, resetVersion}; }
    bool operator==(const PreviewResourceKey&) const = default;
};

/// @note ハンドルを維持するテクスチャ再読み込みも実体と内容版で検出する。
struct PreviewTextureKey {
    renderer::ResourceHandle<renderer::TextureTag> handle;
    const renderer::ITexture* object = nullptr;
    std::uint64_t contentVersion = 0;
    bool operator==(const PreviewTextureKey&) const = default;
};

struct PreviewGpuKey {
    PreviewResourceKey resources;
    std::vector<PreviewTextureKey> textures;
    bool operator==(const PreviewGpuKey&) const = default;
};

struct PreviewFrameKey {
    std::uint64_t sourceRevision = 0;
    renderer::ResourceHandle<renderer::RenderTargetTag> target;
    Flavor flavor = Flavor::Surface;
    Shape shape = Shape::Sphere;
    Channel channel = Channel::Shaded;
    LightPreset lightPreset = LightPreset::Studio;
    FiberMode fiberMode = FiberMode::Shell;
    int fiberShellCount = 16;
    bool animate = true;
    bool showParallax = true;
    std::array<std::uint32_t, 10> values{};
    bool operator==(const PreviewFrameKey&) const = default;
};

/// @note MaterialAsset の公開フィールドへの直書きを検出する。float は NaN を含めビット単位で比較する。
[[nodiscard]] bool MaterialPreviewValuesEqual(const asset::MaterialAssetAuthoring& a,
                                              const asset::MaterialAssetAuthoring& b);

/// @note 時刻はキーへ含めず、時間依存 Flavor の継続描画を UsesPreviewTime で判定する。
[[nodiscard]] PreviewFrameKey MakePreviewFrameKey(std::uint64_t sourceRevision,
                                                 const RenderDesc& desc, Shape shape,
                                                 bool animate);
[[nodiscard]] bool UsesPreviewTime(Flavor flavor, bool animate);

} /// @note namespace fbzz::editor::matpreview
