/// @file    MeshInstancesAsset.hpp
/// @brief   メッシュ配置結果の専用形式と旧 InstanceSet 参照の互換解決。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/Import/IAssetImporter.hpp>
#include <Engine/Asset/Instances/InstanceSetAsset.hpp>
#include <string>
#include <string_view>

namespace fbzz::asset {

struct MeshInstancesAsset {
    InstanceSetAsset instances; ///< 配置時の見た目も保存し、素材定義の変更だけで既存の配置を作り直さない。
    std::string terrainPaintRef; ///< 各インスタンスの地形ペイント定義を解決するアセット参照。
};

/// @return 不正形式・未知バージョン・重複 ID・I/O 失敗時は false。out は未変更。
[[nodiscard]] FBZZ_ENGINE_API bool LoadMeshInstancesAsset(std::string_view path, MeshInstancesAsset& out);
/// @param[in] sourcePath TOML の診断に使うパス。ファイルは開かない。
/// @return 不正形式・未知バージョン・重複 ID の場合は false。out は未変更。
[[nodiscard]] FBZZ_ENGINE_API bool DeserializeMeshInstancesAsset(std::string_view text, std::string_view sourcePath, MeshInstancesAsset& out);
[[nodiscard]] FBZZ_ENGINE_API bool SaveMeshInstancesAsset(std::string_view path, const MeshInstancesAsset& asset);
[[nodiscard]] FBZZ_ENGINE_API bool MeshInstanceDefinitionsEqual(const InstanceSetAsset& first, const InstanceSetAsset& second);
[[nodiscard]] FBZZ_ENGINE_API bool MeshInstancesAssetsEqual(const MeshInstancesAsset& first, const MeshInstancesAsset& second);
/// @return 同期ロードした配置アセットの非所有参照。失敗時は nullptr、resolvedPath は未変更。
[[nodiscard]] FBZZ_ENGINE_API MeshInstancesAsset* LoadMeshInstancesReference(const std::string& reference,
    std::string* resolvedPath = nullptr);
/// @return 読込済み参照のインデックス上のパス。キャッシュに無ければ空文字列。
[[nodiscard]] FBZZ_ENGINE_API std::string FindLoadedMeshInstancesPath(const std::string& reference);
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
} // namespace fbzz::asset
