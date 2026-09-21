/// @file    GeometryRoute.hpp
/// @brief   抽出済みの材質能力による GBuffer / Forward の振り分け。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Graphics/Renderer/RenderState.hpp>
#include <cstdint>

namespace fbzz::renderer {

enum class GeometryRoute : std::uint8_t {
    GBuffer,
    ForwardOpaque,
    ForwardTransparent,
};

/// @note submesh の材質スロット単位の値。Scene・アセットへの参照を保持しない。
/// @note 描画方式を含まないため、同じ抽出結果を異なる方式のビューで使用できる。
struct GeometryMaterialInput {
    BlendMode blend = BlendMode::OPAQUE_BLEND;
    /// @note 未解決・未知の材質は false。標準 PBR へ勝手に置換しない。
    bool gbufferEquivalentShader = false;
    /// @note clearcoat / sheen / anisotropy / cloth は現在の GBuffer に収まらない。
    bool advancedLobe = false;
};

/// @note 半透明を最優先し、材質ごとにカラー経路をちょうど 1 本へ決める。
/// @note スキニング方式では振り分けない。同じモデル内の異なる材質も個別に評価する。
/// @see Docs/design/pipeline-boundary.md §2
[[nodiscard]] constexpr GeometryRoute ResolveGeometryRoute(
    const GeometryMaterialInput& material, bool gbufferPipeline)
{
    if (material.blend != BlendMode::OPAQUE_BLEND) return GeometryRoute::ForwardTransparent;
    if (!gbufferPipeline || !material.gbufferEquivalentShader || material.advancedLobe)
        return GeometryRoute::ForwardOpaque;
    return GeometryRoute::GBuffer;
}

} /// @note namespace fbzz::renderer
