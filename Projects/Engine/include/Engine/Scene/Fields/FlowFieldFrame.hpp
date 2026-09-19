/// @file    FlowFieldFrame.hpp
/// @brief   Scene が 1 フレーム分だけ持つ «流れ» のキャッシュ。保存しない。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// @note 置き場の前例は WaterComponent::waves / current — «保存しない・System が毎フレーム
///       書く・描画も浮力も水中判定もスクリプトも読む»。同じ形をシーン全体へ広げただけ。
/// @see Docs/design/flow-field.md
#pragma once
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <cstdint>
#include <memory>
#include <vector>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

/// @brief «このシーンの風» の要約。粒子以外 (雲・水面・草) は点ごとに積分せず、
/// @brief 方向と速さだけを自分のシェーダーへ渡すので 1 本ぶんへ畳んだ値を受け取る。
struct AmbientWind {
    bool          active = false;
    math::Vector3 direction = { 0.7071f, 0.0f, 0.7071f }; ///< @brief 正規化済み
    float         speed = 0.0f;      ///< @brief [m/s]
    float         turbulence = 0.0f; ///< @brief [m/s]
    float         pulseFrequency = 1.0f;
};

/// @brief 1 フレーム分に解決した流れ一式。Scene が 1 つ持ち、消費者は全員ここを読む。
///
/// @note ランタイム専用。シーンに保存しない。
struct FlowFieldFrame {
    /// @note 公開後は不変。水面・物理・描画が同じ配列を共有し、次の収集で差し替える。
    std::shared_ptr<const std::vector<ActiveFlowField>> fields =
        std::make_shared<const std::vector<ActiveFlowField>>();
    AmbientWind                  ambient;
    /// @brief fields の先頭からシーンに置いた FlowField が並ぶ本数。残りは ambient を場の形にした環境流。
    /// @note 環境流を別経路 (AmbientWind) で受け取る消費者が二重に足さないための境界。
    size_t                       sceneFieldCount = 0;
    /// @brief 埋めたフレーム番号 (Time::frameCount)。未収集は kNeverFilled。
    uint64_t                     frame = kNeverFilled;

    /// @brief 一度も収集していないことを表す番号。
    /// @note 0 にすると «起動直後の frameCount 0» と区別が付かない。
    static constexpr uint64_t kNeverFilled = ~static_cast<uint64_t>(0);
};

} // namespace fbzz::scene
