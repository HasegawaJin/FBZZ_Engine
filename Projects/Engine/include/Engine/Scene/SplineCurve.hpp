/// @file    SplineCurve.hpp
/// @brief   Generic Catmull-Rom curve evaluation shared by runtime and editor previews.
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>
#include <span>

namespace fbzz::scene {

/// @brief Evaluate a Catmull-Rom curve at normalized arc-parameter segment position.
/// @param points Owner-local control positions in authored order.
/// @param closed Whether the last point connects to the first point.
/// @param normalizedPosition Position in [0, 1]; values outside are clamped.
/// @return The owner-local curve position, or zero for no points.
[[nodiscard]] FBZZ_ENGINE_API math::Vector3 EvaluateSplineCurve(std::span<const math::Vector3> points,
    bool closed, float normalizedPosition);

/// @brief Evaluate the Catmull-Rom derivative with respect to normalized position.
/// @param points Owner-local control positions in authored order.
/// @param closed Whether the last point connects to the first point.
/// @param normalizedPosition Position in [0, 1]; values outside are clamped.
/// @return The owner-local tangent; its magnitude includes segment-count scaling.
[[nodiscard]] FBZZ_ENGINE_API math::Vector3 EvaluateSplineCurveTangent(std::span<const math::Vector3> points,
    bool closed, float normalizedPosition);

/// @brief Estimate curve length after signed componentwise world scaling.
/// @param points Owner-local control positions in authored order.
/// @param closed Whether the last point connects to the first point.
/// @param worldScale Signed world scale applied before measuring length.
/// @return Approximate world-space length with a nonzero lower bound.
[[nodiscard]] FBZZ_ENGINE_API float EstimateSplineCurveLength(std::span<const math::Vector3> points,
    bool closed, const math::Vector3& worldScale);

} /// @note namespace fbzz::scene
