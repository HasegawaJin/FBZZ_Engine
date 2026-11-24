/// @file    RenderWaterRipples.cpp
/// @brief   波紋の変化検出と波紋テクスチャの焼き直し。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderWaterResources.hpp"
#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace fbzz::scene::water_render_detail {

namespace {

/// @note モジュール読込時に一度だけ取得する。値が tiled に完全一致する場合だけタイル化し、未指定・full・未知の値には全走査を使う。
const bool USE_TILED_RIPPLE_BAKE = [] {
    const char* mode = std::getenv("FBZZ_WATER_RIPPLE_BAKE_MODE");
    return mode && std::string_view(mode) == "tiled";
}();

uint64_t ComputeRippleInputSignature(const WaterComponent& water, const math::Matrix4& surfaceToWorld,
    const math::Vector2& localExtent)
{
    uint64_t signature = 1469598103934665603ull;
    const auto mix = [&signature](float value) {
        signature ^= std::bit_cast<uint32_t>(value);
        signature *= 1099511628211ull;
    };
    for (const auto& row : surfaceToWorld.m) for (float value : row) mix(value);
    mix(localExtent.x); mix(localExtent.y);
    signature ^= water.ripples.size(); signature *= 1099511628211ull;
    for (const auto& ripple : water.ripples) {
        mix(ripple.center.x); mix(ripple.center.y); mix(ripple.radius);
        mix(ripple.width); mix(ripple.amplitude); mix(ripple.meshFade);
    }
    return signature;
}

/// @note RGB は法線 XY と正規化した頂点変位。meshFade は頂点変位にだけ掛ける。
/// @note 距離をワールド XZ で測り、断面は CPU と同じ WaterComponent::WaterRippleWave で評価する。
void BakeRipplePixels(WaterRippleState& state, const WaterComponent& water, const math::Matrix4& surfaceToWorld,
    const math::Vector2& localExtent)
{
    FBZZ_PROFILE_SCOPE("WaterRipple::Bake");
    const float inverseHeightScale = 1.0f / kWaterRippleHeightScale;
    struct RippleBounds {
        const WaterRipple* ripple = nullptr;
        float innerSquared = 0.0f;
        float outerSquared = 0.0f;
        bool bounded = false;
    };
    std::vector<RippleBounds> activeRipples;
    activeRipples.reserve(water.ripples.size());
    for (const auto& ripple : water.ripples) {
        if (ripple.amplitude <= 0.0f) continue;
        const float width = (std::max)(ripple.width, 1.0e-4f);
        const float inner = (std::max)(0.0f, ripple.radius - width * 6.0f - width * 16.0f);
        const float outer = (std::max)(0.0f, ripple.radius + width * 16.0f);
        const float innerSquared = inner * inner;
        const float outerSquared = outer * outer;
        const bool bounded = std::numeric_limits<float>::is_iec559
            && std::numeric_limits<float>::digits == 24 && std::isfinite(innerSquared)
            && std::isfinite(outerSquared) && std::isfinite(ripple.radius) && std::isfinite(ripple.width);
        activeRipples.push_back({&ripple, innerSquared, outerSquared, bounded});
    }
    const auto worldAt = [&](uint32_t x, uint32_t y) {
        const math::Vector2 uv = {
            (static_cast<float>(x) + 0.5f) / static_cast<float>(state.width),
            (static_cast<float>(y) + 0.5f) / static_cast<float>(state.height)
        };
        /// @note EmitWaterRipple と同じテクセル中心の対応式と演算順でサンプルする。
        const auto point = surfaceToWorld * math::Vector4{(uv.x - 0.5f) * localExtent.x,
            0.0f, (uv.y - 0.5f) * localExtent.y, 1.0f};
        return math::Vector2{point.x, point.z};
    };
    const auto bakeRegion = [&](uint32_t firstX, uint32_t firstY, uint32_t endX, uint32_t endY,
        std::span<const RippleBounds> candidates) {
        for (uint32_t y = firstY; y < endY; ++y) {
            for (uint32_t x = firstX; x < endX; ++x) {
                const auto worldXZ = worldAt(x, y);
                math::Vector2 normalOffset = math::Vector2::ZERO;
                float height = 0.0f;
                /// @note どちらの走査でも波紋の順と、単精度で寄与がゼロになった末尾の判定を揃える。
                for (const auto& bounds : candidates) {
                    const auto& ripple = *bounds.ripple;
                    const math::Vector2 rippleOffset = worldXZ - ripple.center;
                    const float distanceSquared = rippleOffset.LengthSq();
                    /// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/data-type-constants IEEE float range; WaterComponent::WaterRippleWave remains authoritative.
                    if (bounds.bounded && std::isfinite(distanceSquared)
                        && (distanceSquared < bounds.innerSquared || distanceSquared > bounds.outerSquared)) continue;
                    const float distance = std::sqrt(distanceSquared);
                    if (distance <= math::EPSILON) continue;

                    const float wave = WaterComponent::WaterRippleWave(ripple, distance) * ripple.amplitude;
                    normalOffset += (rippleOffset / distance) * wave;
                    height += wave * ripple.meshFade * kWaterRippleHeightAmplitude;
                }

                const size_t pixelOffset = (static_cast<size_t>(y) * state.width + x) * 4u;
                state.bakedPixels[pixelOffset + 0] = static_cast<uint8_t>(math::Clamp01(0.5f + normalOffset.x * 0.5f) * 255.0f);
                state.bakedPixels[pixelOffset + 1] = static_cast<uint8_t>(math::Clamp01(0.5f + normalOffset.y * 0.5f) * 255.0f);
                state.bakedPixels[pixelOffset + 2] =
                    static_cast<uint8_t>(math::Clamp01(0.5f + height * inverseHeightScale * 0.5f) * 255.0f);
                state.bakedPixels[pixelOffset + 3] = 255u;
            }
        }
    };
    if (!USE_TILED_RIPPLE_BAKE) {
        state.bakedPixels.assign(static_cast<size_t>(state.width) * static_cast<size_t>(state.height) * 4u, 128u);
        bakeRegion(0u, 0u, state.width, state.height, activeRipples);
    } else {
        const auto overlapsTile = [](const RippleBounds& bounds,
            const math::Vector2& tileMinimum, const math::Vector2& tileMaximum) {
            if (!bounds.bounded || !std::isfinite(tileMinimum.x) || !std::isfinite(tileMinimum.y)
                || !std::isfinite(tileMaximum.x) || !std::isfinite(tileMaximum.y)) return true;
            const math::Vector2 minimumOffset = tileMinimum - bounds.ripple->center;
            const math::Vector2 maximumOffset = tileMaximum - bounds.ripple->center;
            const math::Vector2 nearest{
                minimumOffset.x > 0.0f ? minimumOffset.x : (maximumOffset.x < 0.0f ? maximumOffset.x : 0.0f),
                minimumOffset.y > 0.0f ? minimumOffset.y : (maximumOffset.y < 0.0f ? maximumOffset.y : 0.0f)};
            const math::Vector2 farthest{
                (std::max)(std::abs(minimumOffset.x), std::abs(maximumOffset.x)),
                (std::max)(std::abs(minimumOffset.y), std::abs(maximumOffset.y))};
            const float nearSquared = std::nextafter(nearest.LengthSq(), 0.0f);
            const float farSquared = std::nextafter(farthest.LengthSq(), std::numeric_limits<float>::infinity());
            /// @note 距離を確定できない場合やオーバーフロー時は、従来のテクセル単位の計算を使う。
            return !std::isfinite(nearSquared) || !std::isfinite(farSquared)
                || (farSquared >= bounds.innerSquared && nearSquared <= bounds.outerSquared);
        };
        /// @note 最後の波紋が消えた直後のフレームも含め、寄与ゼロは切り捨て後の値 127 に符号化される。
        state.bakedPixels.assign(static_cast<size_t>(state.width) * static_cast<size_t>(state.height) * 4u, 127u);
        for (size_t alpha = 3; alpha < state.bakedPixels.size(); alpha += 4u) state.bakedPixels[alpha] = 255u;
        constexpr uint32_t RIPPLE_TILE_SIZE = 16u;
        const auto roundingMargin = [&](size_t axis) {
            const double magnitude = std::abs(static_cast<double>(surfaceToWorld.m[axis][0])) * std::abs(localExtent.x) * 0.5
                + std::abs(static_cast<double>(surfaceToWorld.m[axis][2])) * std::abs(localExtent.y) * 0.5
                + std::abs(static_cast<double>(surfaceToWorld.m[axis][3]));
            const double error = (magnitude + 1.0) * 32.0 * std::numeric_limits<float>::epsilon();
            if (!std::isfinite(error) || error > std::numeric_limits<float>::max())
                return std::numeric_limits<float>::infinity();
            return std::nextafter(static_cast<float>(error), std::numeric_limits<float>::infinity());
        };
        /// @note 大きな平行移動や桁落ちも含め、テクセル座標変換の丸め誤差を絶対値で見積もる。上限が不明なら全波紋を残す。
        const math::Vector2 roundingError{roundingMargin(0u), roundingMargin(2u)};
        std::vector<RippleBounds> tileRipples;
        tileRipples.reserve(activeRipples.size());
        for (uint32_t tileY = 0; tileY < state.height; tileY += RIPPLE_TILE_SIZE) {
            const uint32_t endY = (std::min)(tileY + RIPPLE_TILE_SIZE, state.height);
            for (uint32_t tileX = 0; tileX < state.width; tileX += RIPPLE_TILE_SIZE) {
                const uint32_t endX = (std::min)(tileX + RIPPLE_TILE_SIZE, state.width);
                const std::array corners{worldAt(tileX, tileY), worldAt(endX - 1u, tileY),
                    worldAt(tileX, endY - 1u), worldAt(endX - 1u, endY - 1u)};
                math::Vector2 tileMinimum = corners.front(), tileMaximum = corners.front();
                bool finiteCorners = true;
                for (const auto& corner : corners) {
                    finiteCorners = finiteCorners && std::isfinite(corner.x) && std::isfinite(corner.y);
                    tileMinimum.x = (std::min)(tileMinimum.x, corner.x); tileMinimum.y = (std::min)(tileMinimum.y, corner.y);
                    tileMaximum.x = (std::max)(tileMaximum.x, corner.x); tileMaximum.y = (std::max)(tileMaximum.y, corner.y);
                }
                tileMinimum.x = std::nextafter(tileMinimum.x - roundingError.x, -std::numeric_limits<float>::infinity());
                tileMinimum.y = std::nextafter(tileMinimum.y - roundingError.y, -std::numeric_limits<float>::infinity());
                tileMaximum.x = std::nextafter(tileMaximum.x + roundingError.x, std::numeric_limits<float>::infinity());
                tileMaximum.y = std::nextafter(tileMaximum.y + roundingError.y, std::numeric_limits<float>::infinity());
                tileRipples.clear();
                for (const auto& bounds : activeRipples)
                    if (!finiteCorners || overlapsTile(bounds, tileMinimum, tileMaximum)) tileRipples.push_back(bounds);
                if (tileRipples.empty()) continue;
                bakeRegion(tileX, tileY, endX, endY, tileRipples);
            }
        }
    }
}

void UploadRippleTexture(WaterRippleState& state, renderer::ResourceManager& resources)
{
    FBZZ_PROFILE_SCOPE("WaterRipple::Upload");
    if (!resources.Get(state.texture))
        state.texture = resources.CreateDynamicTexture(state.width, state.height, renderer::DynamicTextureFormat::RGBA8);
    auto* currentTexture = resources.Get(state.texture);
    const bool updated = currentTexture && currentTexture->UpdateRegion(0, 0, state.width, state.height,
        state.bakedPixels.data(), state.width * 4u);
    if (!updated) {
        if (state.texture.IsValid()) resources.Release(state.texture);
        state.texture = resources.CreateTexture(state.bakedPixels.data(), state.width, state.height);
    }
    state.signatureValid = resources.Get(state.texture) != nullptr;
}

}

void UpdateRippleState(WaterRippleState& state, const WaterComponent& water,
    const Transform& transform, renderer::ResourceManager& resources)
{
    const auto surfaceToWorld = transform.GetWorldMatrix();
    const auto localExtent = WaterSurfaceLocalExtent(water, WaterSurfaceReadView::SIMULATION);
    const uint64_t signature = ComputeRippleInputSignature(water, surfaceToWorld, localExtent);
    /// @note 波紋は全ビュー共通のワールド座標で扱い、入力が同じなら焼き込み済みテクスチャを再利用する。
    if (state.signatureValid && state.inputSignature == signature && resources.Get(state.texture)) {
        state.dirty = false;
        return;
    }

    BakeRipplePixels(state, water, surfaceToWorld, localExtent);
    UploadRippleTexture(state, resources);
    state.inputSignature = signature;
    state.dirty = !state.signatureValid;
    state.previousBakeHadRipples = !water.ripples.empty();
}

}
