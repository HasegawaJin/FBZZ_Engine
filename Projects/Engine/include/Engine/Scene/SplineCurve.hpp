/// @file    SplineCurve.hpp
/// @brief   ランタイムとエディターのプレビューで共用する Catmull-Rom 曲線評価。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>
#include <span>

namespace fbzz::scene {

/// @brief 区間を正規化した曲線パラメーターで Catmull-Rom 曲線を評価する。
/// @param points 制作時の並び順で格納した所有者ローカル座標の制御点。
/// @param closed 最後の点を最初の点に接続するか。
/// @param normalizedPosition 範囲は [0, 1]。範囲外の値は端に丸める。
/// @return 所有者ローカル空間の曲線上の位置。点がなければゼロ。
[[nodiscard]] FBZZ_ENGINE_API math::Vector3 EvaluateSplineCurve(std::span<const math::Vector3> points,
    bool closed, float normalizedPosition);

/// @brief 正規化位置に関する Catmull-Rom 曲線の導関数を求める。
/// @param points 制作時の並び順で格納した所有者ローカル座標の制御点。
/// @param closed 最後の点を最初の点に接続するか。
/// @param normalizedPosition 範囲は [0, 1]。範囲外の値は端に丸める。
/// @return 所有者ローカル空間の接線。大きさは区間数による倍率を含む。
[[nodiscard]] FBZZ_ENGINE_API math::Vector3 EvaluateSplineCurveTangent(std::span<const math::Vector3> points,
    bool closed, float normalizedPosition);

/// @brief 軸ごとに符号付きワールドスケールを掛けた曲線の長さを近似する。
/// @param points 制作時の並び順で格納した所有者ローカル座標の制御点。
/// @param closed 最後の点を最初の点に接続するか。
/// @param worldScale 測長前に適用する符号付きワールドスケール。
/// @return ワールド空間での近似長。下限はゼロより大きい。
[[nodiscard]] FBZZ_ENGINE_API float EstimateSplineCurveLength(std::span<const math::Vector3> points,
    bool closed, const math::Vector3& worldScale);

}
