/// @file    ModelAssetImporter.hpp
/// @brief   .fzasset バイナリ → ModelAsset ローダー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/ModelAsset.hpp>

namespace fbzz::asset {

struct BinaryReader;

class ModelAssetImporter final : public IAssetImporter<ModelAsset> {
public:
    [[nodiscard]] std::unique_ptr<ModelAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    /// @brief firstResidentLod より高品質な LOD のメッシュ本体を読まずに取り込む (部分 I/O)。
    /// @param firstResidentLod 最低品質の LOD で頭打ちにする。読まなかった LOD は submesh の枠と名前だけ持つ。
    /// @param outBytesRead 実際にディスクから読んだバイト数 (null 可)。
    /// @note .fzasset は submesh ヘッダーが頂点数・インデックス数・モーフ数を持つので、索引が無くても
    ///       本体の大きさが決まり、読まない LOD はシークで飛ばせる。
    [[nodiscard]] std::unique_ptr<ModelAsset> ImportPartial(
        const std::string&         absPath,
        renderer::ResourceManager* resources,
        uint32_t                   firstResidentLod,
        uint64_t*                  outBytesRead = nullptr);

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".fzasset" };
        return kExts;
    }

private:
    [[nodiscard]] static std::unique_ptr<ModelAsset> ImportFromReader(
        BinaryReader& reader, const std::string& absPath,
        renderer::ResourceManager* resources, uint32_t firstResidentLod);
};

} // namespace fbzz::asset
