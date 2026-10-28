/// @file    FiberPaintAsset.hpp
/// @brief   表面の密度・高さペイントを保存する CPU 専用アセット。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Graphics/Renderer/FiberPlacement.hpp>
#include <Engine/Asset/IAssetImporter.hpp>

namespace fbzz::asset {
struct FiberPaintAsset {
    renderer::FiberPaintData data;
    /// @note 読込・編集・Undo ごとに更新するランタイム世代。ファイルへ保存しない。
    uint64_t revision = 0;
};
void TouchFiberPaintAsset(FiberPaintAsset& asset);
/// @return 不正形式・上限超過・I/O 失敗時は false。out は未変更。
[[nodiscard]] bool LoadFiberPaintAssetFromFile(std::string_view path, FiberPaintAsset& out);
/// @note ディスク上のアセットをテンポラリから原子的に置き換える。
[[nodiscard]] bool SaveFiberPaintAssetToFile(std::string_view path, const FiberPaintAsset& asset);
class FiberPaintImporter final : public IAssetImporter<FiberPaintAsset> {
public:
    std::unique_ptr<FiberPaintAsset> Import(const std::string& path, renderer::ResourceManager*) override
    {
        auto result = std::make_unique<FiberPaintAsset>();
        if (!LoadFiberPaintAssetFromFile(path,*result)) return nullptr;
        return result;
    }
    std::span<const std::string_view> SupportedExtensions() const override
    {
        static constexpr std::string_view extensions[]{".fiberpaint"};
        return extensions;
    }
};
} /// @note namespace fbzz::asset
