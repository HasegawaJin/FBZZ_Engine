/// @file    SceneEnvironment.hpp
/// @brief   シーン全体に一律で流れる環境流。GameObject 探索をやめたシーン設定。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// @note 重力が ProjectSettings にあるのと同じ位置づけ。«半径 0 の Uniform を環境風と読む»
///       という暗黙の規約は、GameObject の並び順で勝者が決まる沈黙のバグだった。
/// @see Docs/design/flow-field.md
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::scene {

struct AmbientWind;
struct ActiveFlowField;

/// 環境流。雲・水面・草・粒子がこの 1 本を読む。
struct SceneEnvironment {
    /// 環境流を使うか。false ならコンポーネント固有の設定 (雲の windDirection 等) へ戻る。
    bool          enabled = false;
    /// 流れの向き。正規化していなくてよい (読む側が正規化する)。
    math::Vector3 direction = { 0.7071f, 0.0f, 0.7071f };
    /// 流速 [m/s]。
    float         speed = 0.0f;
    /// 乱れの強さ [m/s]。カールノイズの振幅として効く。
    float         turbulence = 0.0f;
    /// 乱れの脈動の速さ [1/s]。カールノイズの時間スクロール速度。
    float         pulseFrequency = 1.0f;

    void Reflect(IReflector& r);
};

/// 環境流を «方向 1 つと速さ 1 つ» の要約へ解決する。
/// @note direction は正規化済みで返る。長さ 0 の指定は +Y へ倒す。
[[nodiscard]] AmbientWind ResolveAmbientWind(const SceneEnvironment& environment);

/// 環境流を «半径なしの Uniform (+ Curl)» としてフレームキャッシュへ足す。
/// @note 粒子は 1 点ごとに流速を積むので要約では受け取れない。雲や水面が読む AmbientWind と
///       同じ値がここでも場の形になっていないと、«風は吹いているのに粒子が動かない» になる。
void AppendEnvironmentFlow(const AmbientWind& wind, std::vector<ActiveFlowField>& out);

} // namespace fbzz::scene
