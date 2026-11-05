/// @file    TerrainPaintAsset.hpp
/// @brief   地形へ塗るメッシュ素材と配置規則を保存する専用アセット。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <string>
#include <string_view>

namespace fbzz::asset {

enum class TerrainPaintCategory { GRASS, TREE, ROCK, OTHER };

/// @note 距離は world m、角度は度、大きさはモデル原寸への一様倍率。
struct TerrainPaintPlacement {
    float spacing = 6.0f;
    float density = 1.0f;
    float minScale = 0.8f, maxScale = 1.2f;
    float minYaw = 0.0f, maxYaw = 360.0f;
    float minSlope = 0.0f, maxSlope = 40.0f;
    bool alignToNormal = false;
    bool alignDownslope = false;
    float downslopeJitter = 0.0f;
    float embedDepth = 0.0f;
    float waterClearance = 0.0f;
    float pathClearance = 0.0f;
    bool operator==(const TerrainPaintPlacement&) const = default;
};

/// @note InstanceSetAsset の検証と snapshot を共用するが、.terrainpaint は DataAssetRegistry の型登録を使わない。
struct TerrainPaintAsset {
    std::string name;
    TerrainPaintCategory category = TerrainPaintCategory::OTHER;
    InstanceSetAsset definition;
    TerrainPaintPlacement placement;
};

[[nodiscard]] FBZZ_ENGINE_API const char* TerrainPaintCategoryName(TerrainPaintCategory category);
[[nodiscard]] FBZZ_ENGINE_API bool ValidateTerrainPaintAsset(const TerrainPaintAsset& asset);
[[nodiscard]] FBZZ_ENGINE_API bool TerrainPaintAssetsEqual(const TerrainPaintAsset& first, const TerrainPaintAsset& second);
/// @return 不正形式・未知バージョン・非有限値・I/O 失敗時は false。out は未変更。
[[nodiscard]] FBZZ_ENGINE_API bool LoadTerrainPaintAsset(std::string_view path, TerrainPaintAsset& out);
[[nodiscard]] FBZZ_ENGINE_API bool SaveTerrainPaintAsset(std::string_view path, const TerrainPaintAsset& asset);

class TerrainPaintImporter final : public IAssetImporter<TerrainPaintAsset> {
public:
    std::unique_ptr<TerrainPaintAsset> Import(const std::string& path, renderer::ResourceManager*) override
    {
        auto result = std::make_unique<TerrainPaintAsset>();
        return LoadTerrainPaintAsset(path, *result) ? std::move(result) : nullptr;
    }
    std::span<const std::string_view> SupportedExtensions() const override
    {
        static constexpr std::string_view EXTENSIONS[]{".terrainpaint"};
        return EXTENSIONS;
    }
};
} /// @note namespace fbzz::asset
