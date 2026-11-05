/// @file    MeshInstancesAsset.hpp
/// @brief   メッシュ配置結果の専用形式と旧 InstanceSet 参照の互換解決。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <string>
#include <string_view>

namespace fbzz::asset {

struct MeshInstancesAsset {
    /// @note 配置時の見た目も保存し、素材定義の変更だけで既存の配置を作り直さない。
    InstanceSetAsset instances;
    std::string terrainPaintRef;
};

/// @return 不正形式・未知バージョン・重複 ID・I/O 失敗時は false。out は未変更。
[[nodiscard]] FBZZ_ENGINE_API bool LoadMeshInstancesAsset(std::string_view path, MeshInstancesAsset& out);
[[nodiscard]] FBZZ_ENGINE_API bool SaveMeshInstancesAsset(std::string_view path, const MeshInstancesAsset& asset);
[[nodiscard]] FBZZ_ENGINE_API bool MeshInstanceDefinitionsEqual(const InstanceSetAsset& first, const InstanceSetAsset& second);
[[nodiscard]] FBZZ_ENGINE_API bool MeshInstancesAssetsEqual(const MeshInstancesAsset& first, const MeshInstancesAsset& second);
/// @note .meshinstances/.terrainpaint は型付き Importer、旧 .fzdata は従来の Registry を使う。戻り値は非所有参照。
[[nodiscard]] FBZZ_ENGINE_API const InstanceSetAsset* ResolveMeshInstanceDefinition(const std::string& reference);

class MeshInstancesImporter final : public IAssetImporter<MeshInstancesAsset> {
public:
    std::unique_ptr<MeshInstancesAsset> Import(const std::string& path, renderer::ResourceManager*) override
    {
        auto result = std::make_unique<MeshInstancesAsset>();
        return LoadMeshInstancesAsset(path, *result) ? std::move(result) : nullptr;
    }
    std::span<const std::string_view> SupportedExtensions() const override
    {
        static constexpr std::string_view EXTENSIONS[]{".meshinstances"};
        return EXTENSIONS;
    }
};
} /// @note namespace fbzz::asset
