/// @file    Waterfall.hpp
/// @brief   河川断面の共有と滝の手続き生成。
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once
#include <Engine/Api.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstdint>
#include <functional>

namespace fbzz::scene {
class MeshBuilder;
class Scene;
class GameObject;
struct WaterSplineSnapshot;

struct WaterfallSection {
    math::Vector3 left, right;
    math::Vector3 insideLeft, insideRight;
    math::Vector3 velocity;
    float depth = 0.0f;
    bool operator==(const WaterfallSection&) const = default;
};

struct WaterfallGeometry {
    WaterfallSection source, receiver;
    float breakup = 1.0f;
    bool rockAttached = false;
    bool operator==(const WaterfallGeometry&) const = default;
};

/// @brief 同じ Spline River の元の両岸端点と内側の帯をサンプリングする。
/// @note 接続方向を反転すると両岸の順も逆になる。
[[nodiscard]] FBZZ_ENGINE_API bool GetWaterfallRiverSection(const WaterSplineSnapshot& snapshot,
    bool atStart, bool incoming, WaterfallSection& out);

/// @brief 上限付きの滝幕、落水点の泡、任意のワールド空間エフェクト位置を生成する。
/// @param[in] ground 問い合わせた XZ の最も高い地形面。地形外は負の無限大。
/// @return 接続が無効か、サンプリングした滝幕を地形より上に保てない場合は false。
[[nodiscard]] FBZZ_ENGINE_API bool BuildWaterfallGeometry(const WaterfallGeometry& geometry,
    const std::function<float(const math::Vector3&)>& ground, MeshBuilder& out,
    math::Vector3* midFall = nullptr, std::uint32_t* farIndexStart = nullptr);

/// @brief 制作時の水面参照を現在のシーンに対して解決する。
[[nodiscard]] FBZZ_ENGINE_API bool ResolveWaterfallGeometry(Scene& scene, GameObject& owner, WaterfallGeometry& out);

}
