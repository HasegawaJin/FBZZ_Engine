/// @file    DebugCamera.hpp
/// @brief   Scene View 風のデバッグカメラ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Input を読んで Camera を動かす操作用ラッパー。
/// 本番カメラデータとは分け、エディタやデバッグ描画の視点制御に使う。
#pragma once

#include <Engine/Renderer/Camera.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::renderer {

/// @brief Scene View 風の操作 (右ドラッグ=回転 / 右クリック+WASD+QE=フライ移動 (Shift で高速) /
///        中ドラッグ=パン / スクロール=ドリー・ズーム / Alt+左ドラッグ=オービット)。
class DebugCamera {
public:
    Camera camera;

    float moveSpeed      = 5.0f;
    float fastMultiplier = 3.0f;
    float mouseSens      = 0.15f;  ///< 1 ピクセルあたりの回転角度
    float panSensitivity = 0.003f; ///< focusDistance を基準にした 1 ピクセルあたりの移動量
    float scrollSpeed    = 1.0f;   ///< 速度加算量 (focus-distance 比)
    float scrollDamping  = 10.0f;  ///< 速度減衰係数 (大きいほど素早く停止)

    /// @brief 指定ターゲットへの向きから yaw / pitch とピボットを初期化する。
    void LookAt(const math::Vector3& target);

    /// @brief 位置と回転を瞬時に移動し、内部の yaw/pitch/pivot も同期する。
    void Teleport(const math::Vector3& pos, const math::Quaternion& rot);

    /// @brief ゲームループ毎に呼ぶ。
    /// @note viewportHovered が false のときマウスホイールを無視する。
    void Update(float dt, bool viewportHovered = true);

    /// @brief 射影を切り替える。向きとピボットは保ち、見かけの大きさも引き継ぐ。
    void SetProjection(ProjectionMode mode);

    /// @brief ピボットからカメラを引く距離 (= 視点の前後位置)。
    /// @note 正投影は near 平面より手前が消えるため、見えている幅ぶん余分に引いて手前の壁が
    ///       突然欠けないようにする。ただし far の半分までは引かない: シャドウカスケードや
    ///       距離カリングは「カメラからの距離」で効くため、数百単位引くと影が丸ごと消える。
    [[nodiscard]] float ViewDistance() const noexcept {
        if (camera.m_projection != ProjectionMode::Orthographic) return m_focusDistance;
        const float pullback = camera.m_orthoHeight * 2.0f;
        return pullback > m_focusDistance ? pullback : m_focusDistance;
    }

    /// @brief オービットの中心と注視距離。
    /// @note ナビゲーションギズモが「今のピボットを保ったまま視点だけ回す」ために外部から参照
    ///       する。無いと Teleport のたびに寄り引きが変わってしまう。
    [[nodiscard]] const math::Vector3& Pivot()         const noexcept { return m_pivot; }
    [[nodiscard]] float                FocusDistance() const noexcept { return m_focusDistance; }

private:
    float         m_yaw           = 0.0f;
    float         m_pitch         = 0.0f;
    float         m_focusDistance = 10.0f;
    math::Vector3 m_pivot         = {};
    float         m_dollyVelocity = 0.0f;  ///< 秒あたりの移動量

    void ApplyRotation();
    void ApplyDolly(float dt);
    /// @brief ピボットと向きからカメラ位置を組み直す (射影によって引く距離が変わる)。
    void RepositionFromPivot();
};

} // namespace fbzz::renderer
