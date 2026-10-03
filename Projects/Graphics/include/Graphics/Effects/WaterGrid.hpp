/// @file    WaterGrid.hpp
/// @brief   有限矩形の外周を保った水面格子の近景密度と位置を求める。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once

#include <Math/Matrix4.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace fbzz::renderer {

struct WaterGridAxisSample {
    float position = 0.0f;
    float cellSize = 0.0f;
};

/// @brief 水面すぐ上ではカメラ直下、俯瞰では視線が当たる水面へ格子の中心を寄せる。
/// @pre world の各軸は非ゼロ、cameraForward は正規化済み、resolution と nearCellSize は正。
/// @note 水平線付近の交点は遠すぎるため、高さと最密区間の幅で移動距離を制限する。
/// @note ビューごとに 1 回だけ求め、頂点配置とチャンク判定へ同じローカル座標を渡す。
/// @see Docs/design/water-waves.md 有限の矩形を保つカメラ集中グリッド。
[[nodiscard]] inline math::Vector2 ResolveWaterGridFocus(const math::Matrix4& world,
    const math::Vector3& cameraPosition, const math::Vector3& cameraForward,
    math::Vector2 resolution, float nearCellSize)
{
    const math::Vector3 origin = { world.m[0][3], world.m[1][3], world.m[2][3] };
    const math::Vector3 axisX = { world.m[0][0], world.m[1][0], world.m[2][0] };
    const math::Vector3 axisY = { world.m[0][1], world.m[1][1], world.m[2][1] };
    const math::Vector3 axisZ = { world.m[0][2], world.m[1][2], world.m[2][2] };
    const math::Vector3 normal = axisY.Normalized();
    math::Vector3 toFocus = cameraPosition - origin;
    const float height = math::Vector3::Dot(toFocus, normal);
    const float forwardNormal = math::Vector3::Dot(cameraForward, normal);
    if (height > 0.0f && forwardNormal < -0.001f) {
        const float radius = nearCellSize * (std::min)(resolution.x, resolution.y) * 0.25f;
        const math::Vector3 planarForward = cameraForward - normal * forwardNormal;
        const float travel = (std::min)(
            -height / forwardNormal,
            (std::max)(radius * 2.0f, height * 8.0f) / (std::max)(planarForward.Length(), 1.0e-4f));
        const float blend = (std::clamp)(height / (std::max)(radius * 0.5f, 1.0e-4f), 0.0f, 1.0f);
        toFocus += planarForward * (travel * blend);
    }
    return {
        math::Vector3::Dot(toFocus, axisX) / (std::max)(math::Vector3::Dot(axisX, axisX), 1.0e-8f),
        math::Vector3::Dot(toFocus, axisZ) / (std::max)(math::Vector3::Dot(axisZ, axisZ), 1.0e-8f)
    };
}

/// @brief 近景へ頂点を寄せた格子のローカル座標を求める。
/// @pre extent と nearCellSize は正の有限値、resolution は 1 以上、focus は有限値。
/// @note 近景の線形区間と遠景の三次区間は C2 連続。外周とテクスチャの基準矩形は動かさない。
/// @note focus は nearCellSize 単位に固定し、カメラの微小移動で格子が泳ぐのを抑える。
/// @warning Water.hlsl の WaterGridAxisPosition と同じ式・丸め順であること。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows/chapter-18-using-vertex-texture-displacement GPU Gems 2, 18.2 カメラ近傍へ頂点を集中する水面格子。
[[nodiscard]] inline float WaterGridAxisPosition(float coordinate, float extent, uint32_t resolution,
                                                 float focus, float nearCellSize)
{
    assert(std::isfinite(extent) && extent > 0.0f);
    assert(resolution > 0);
    assert(std::isfinite(focus));
    assert(std::isfinite(nearCellSize) && nearCellSize > 0.0f);
    const float half = extent * 0.5f;
    if (coordinate <= 0.0f) return -half;
    if (coordinate >= 1.0f) return half;
    if (extent / static_cast<float>(resolution) <= nearCellSize)
        return (coordinate * 2.0f - 1.0f) * half;
    const float fineLength = (std::min)(nearCellSize * static_cast<float>(resolution) * 0.5f, half);
    const float limitedFocus = (std::clamp)(focus, -half + fineLength, half - fineLength);
    const float snapped = std::floor(limitedFocus / nearCellSize + 0.5f) * nearCellSize;
    const float center = (std::clamp)(snapped, -half + fineLength, half - fineLength);
    const float signedCoordinate = coordinate * 2.0f - 1.0f;
    const float sign = signedCoordinate < 0.0f ? -1.0f : 1.0f;
    const float distance = std::abs(signedCoordinate);
    const float sideLength = half - sign * center;
    const float coarse = (std::max)(distance * 2.0f - 1.0f, 0.0f);
    return center + sign * (fineLength * distance
        + (sideLength - fineLength) * coarse * coarse * coarse);
}

/// @brief 実際の隣接頂点との最大間隔を返し、三次区間の境目でも波の過少標本化を防ぐ。
/// @note position と cellSize は extent と同じローカル単位。ワールドの間隔には軸スケールを掛ける。
/// @warning Water.hlsl の ResolveWaterGridAxis と同じ式であること。
[[nodiscard]] inline WaterGridAxisSample ResolveWaterGridAxis(float coordinate, float extent,
                                                             uint32_t resolution, float focus,
                                                             float nearCellSize)
{
    const float position = WaterGridAxisPosition(coordinate, extent, resolution, focus, nearCellSize);
    const float step = 1.0f / static_cast<float>(resolution);
    const float previous = WaterGridAxisPosition((std::max)(coordinate - step, 0.0f),
        extent, resolution, focus, nearCellSize);
    const float next = WaterGridAxisPosition((std::min)(coordinate + step, 1.0f),
        extent, resolution, focus, nearCellSize);
    return { position, (std::max)(position - previous, next - position) };
}

}
