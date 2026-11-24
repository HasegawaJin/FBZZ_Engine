/// @file    ComputedSkinningDemand.hpp
/// @brief   現在のカメラと影が計算済みスキニング出力を必要とするか判定する。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Graphics/Api.hpp>

namespace fbzz::renderer {
struct RenderPassContext;
struct RenderObject;

/// @note 候補は現在の world bounds と layer/LOD を捕捉した値。材質未解決なら色・影の候補を保守的に残す。
/// @note 未知 consumer・Ray・mask・不明な bounds は全処理へ戻す。資源照会と統計の変更を行わない。
[[nodiscard]] FBZZ_GRAPHICS_API bool RequiresComputedSkinning(const RenderPassContext& context,
    const RenderObject& object, bool unknownConsumers = false);
}
