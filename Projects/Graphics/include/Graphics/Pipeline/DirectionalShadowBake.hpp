/// @file    DirectionalShadowBake.hpp
/// @brief   ワールド空間に固定した方向ライトの静的影ベイクと再利用。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Graphics/Pipeline/ViewPreparation.hpp>

namespace fbzz::renderer {

struct RenderMeshItem;
struct RenderObject;
struct RenderScene;
struct RenderTerrainInput;

/// @note 通常 Z の float 深度を行優先 RGBA バイト列として格納する。R が最下位バイト。
struct DirectionalShadowBakeData {
    math::Vector3 lightDirection{};
    ShadowBounds coverage;
    uint32_t resolution = 0;
    uint32_t cullingMask = 0;
    std::vector<uint8_t> depthRgba;
};

[[nodiscard]] FBZZ_GRAPHICS_API bool IsDirectionalShadowBakeMesh(
    const RenderObject& object, const RenderMeshItem& item);
[[nodiscard]] FBZZ_GRAPHICS_API bool IsDirectionalShadowBakeTerrain(const RenderTerrainInput& terrain);
/// @note 粗い広域ベイクでは小さなメッシュキャスターを常にリアルタイム描画する。
[[nodiscard]] FBZZ_GRAPHICS_API bool DirectionalShadowBakeIncludesMeshes(const ShadowBounds& coverage, uint32_t resolution);

/// @pre ネイティブ描画フレーム外。呼び出し中、抽出済みの全静的資源を保持する。
/// @return 失敗時は出力を変更しない。
[[nodiscard]] FBZZ_GRAPHICS_API bool BakeDirectionalStaticShadow(IRenderer& renderer, ResourceManager& resources,
    const RenderScene& scene, const ShadowBounds& coverage, const math::Vector3& direction,
    uint32_t resolution, uint32_t cullingMask, DirectionalShadowBakeData& out);

/// @return 成功時は呼び出し側が Release する。
[[nodiscard]] FBZZ_GRAPHICS_API ResourceHandle<TextureTag> UploadDirectionalShadowBake(
    ResourceManager& resources, const DirectionalShadowBakeData& data);

/// @pre ホストが現在の静的キャスターと保存データの版の一致を確認済みであること。
/// @return 最遠カスケードへの深度復元が可能なら true。失敗時に出力は未変更。
[[nodiscard]] FBZZ_GRAPHICS_API bool PrepareDirectionalShadowBakeView(ResourceManager& resources,
    const DirectionalShadowBakeData& data, ResourceHandle<TextureTag> texture, const ShadowBounds& currentBounds,
    const math::Vector3& currentDirection, uint32_t cullingMask, float shadowBias,
    const PreparedShadows& shadows, DirectionalShadowBakeView& out);

} /// @note namespace fbzz::renderer
