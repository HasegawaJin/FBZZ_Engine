/// @file    ShadowCasterVisibility.hpp
/// @brief   ShadowPass と事前 Skinning 需要が共有する caster mask の判定。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include "RenderScenePassHelpers.hpp"
#include <algorithm>

namespace fbzz::renderer::shadow_visibility_detail {

/// @brief IsShadowRelevant — この caster がこのカスケード上で意味のある大きさを持つか。
/// @note 影ボリュームは正射影なのでワールド半径がテクセル数に一意換算できる。直径が数テクセル
/// @note 未満の caster は PCF (既定 5x5) で均されて絵に出ないため、DrawCall・頂点・ラスタライズの
/// @note コストが無駄になる前に落とす。しきい値は PCF カーネルより小さく、影が消える方向の破綻は無い。
/// @note 判定はカスケードごとに行う。遠いカスケードで切り捨てられる小物も、手前では十分な大きさを持つ。
inline bool IsShadowRelevant(const ShadowCascade& cascade, const WorldBounds& bounds)
{
    /// @note テクセル情報が無ければカリングしない
    if (cascade.texelWorldSize <= 0.0f) return true;
    /// @note bounds 未計算メッシュも同様
    if (bounds.radius <= 0.0f)          return true;

    /// @note 直径が 2 テクセル未満なら影として解像できない。
    constexpr float MIN_TEXEL_DIAMETER = 2.0f;
    return (bounds.radius * 2.0f) >= cascade.texelWorldSize * MIN_TEXEL_DIAMETER;
}

/// @note ComputeCascadeMask — この bounds がどのカスケードへ提出されるかを一度に判定する。
inline uint32_t ComputeCascadeMask(const RenderPassContext& ctx, int cascadeCount,
                            const WorldBounds& bounds, bool hasBounds)
{
    if (!hasBounds) {
        /// @note bounds が取れないメッシュはカリングせず全カスケードへ出す (従来の安全側)。
        return (1u << cascadeCount) - 1u;
    }

    uint32_t mask = 0;
    for (int i = 0; i < cascadeCount; ++i) {
        const ShadowCascade& cascade = ctx.shadowCascades[i];
        if (!cascade.frustum.IntersectsSphere(bounds.center, bounds.radius)) continue;
        if (!IsShadowRelevant(cascade, bounds)) continue;
        mask |= (1u << i);
    }
    return mask;
}

/// @brief ComputePunctualMask — この bounds がどの Spot / Point スロットへ提出されるか。
/// @note 透視投影では 1 テクセルの覆うワールド距離がライトからの距離に比例し、カスケードの
/// @note ような固定 texelWorldSize を持てないため、caster の見かけの角半径 (radius/distance)
/// @note とテクセルの張る角度を直接比べる、深度に依存しない規則でカリングする。
inline uint32_t ComputePunctualMask(const RenderPassContext& ctx,
                             const WorldBounds& bounds, bool hasBounds)
{
    const int count = std::clamp(ctx.punctualShadowViewCount, 0, kMaxPunctualShadows);
    if (count == 0) return 0;

    uint32_t mask = 0;
    for (int i = 0; i < count; ++i) {
        const PunctualShadowView& view = ctx.punctualShadowViews[i];
        if (hasBounds) {
            if (!view.frustum.IntersectsSphere(bounds.center, bounds.radius)) continue;
            if (bounds.radius > 0.0f && view.texelAngularSize > 0.0f) {
                const math::Vector3 toLight = bounds.center - view.eyePos;
                const float dist = toLight.Length();
                /// @note 直径が 2 テクセルに満たない caster は PCF で完全に均されて絵に出ない。
                /// @note ライトの内側にいる (dist <= radius) 場合は必ず描く。
                if (dist > bounds.radius) {
                    const float angularDiameter = 2.0f * bounds.radius / dist;
                    if (angularDiameter < view.texelAngularSize * 2.0f) continue;
                }
            }
        }
        /// @note bounds が取れないメッシュはカリングせず全スロットへ出す (従来の安全側)。
        mask |= (1u << i);
    }
    return mask;
}

}
