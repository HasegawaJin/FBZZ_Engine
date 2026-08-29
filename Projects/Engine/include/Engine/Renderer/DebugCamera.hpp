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

// 操作
//   右ドラッグ           : 視点回転 (FPS ルック)
//   右クリック + WASD/QE : フライ移動 (Shift で高速)
//   中ドラッグ           : パン (平行移動)
//   スクロール           : ドリー (前後移動 / 正投影ではズーム)
//   Alt + 左ドラッグ     : ピボット周回 (オービット)
class DebugCamera {
public:
    Camera camera;

    float moveSpeed      = 5.0f;
    float fastMultiplier = 3.0f;
    float mouseSens      = 0.15f;  // 1 ピクセルあたりの回転角度
    float panSensitivity = 0.003f; // focusDistance を基準にした 1 ピクセルあたりの移動量
    float scrollSpeed    = 1.0f;   // 速度加算量 (focus-distance 比)
    float scrollDamping  = 10.0f;  // 速度減衰係数 (大きいほど素早く停止)

    // 指定ターゲットへの向きから yaw / pitch とピボットを初期化する
    void LookAt(const math::Vector3& target);

    // 位置と回転を瞬時に移動し、内部の yaw/pitch/pivot も同期する
    void Teleport(const math::Vector3& pos, const math::Quaternion& rot);

    // ゲームループ毎に呼ぶ。viewportHovered が false のときマウスホイールを無視する。
    void Update(float dt, bool viewportHovered = true);

    // 射影を切り替える。向きとピボットは保ち、見かけの大きさも引き継ぐ。
    void SetProjection(ProjectionMode mode);

    // ピボットからカメラを引く距離 (= 視点の前後位置)。
    //
    // WHY 正投影で余分に引くか: 平行投影は前後に動いても見え方が変わらない代わりに、
    //     near 平面より手前のものは消える。見えている幅ぶん引いておけば、
    //     真横から見ても手前の壁が突然欠けない。
    // WHY それでも far の半分まで引かないか: シャドウカスケードも距離カリングも
    //     「カメラからの距離」で効く。数百単位も引くと、視界の中身が全部
    //     シャドウ距離の外へ出て影が丸ごと消える。ズームに比例させて必要最小限に留める。
    [[nodiscard]] float ViewDistance() const noexcept {
        if (camera.m_projection != ProjectionMode::Orthographic) return m_focusDistance;
        const float pullback = camera.m_orthoHeight * 2.0f;
        return pullback > m_focusDistance ? pullback : m_focusDistance;
    }

    // オービットの中心と注視距離。
    // WHY: エディタのナビゲーションギズモは「今のピボットを保ったまま視点だけ回す」必要があり、
    //      外から同じ中心・同じ距離を再現できないと Teleport のたびに寄り引きが変わってしまう。
    [[nodiscard]] const math::Vector3& Pivot()         const noexcept { return m_pivot; }
    [[nodiscard]] float                FocusDistance() const noexcept { return m_focusDistance; }

private:
    float         m_yaw           = 0.0f;
    float         m_pitch         = 0.0f;
    float         m_focusDistance = 10.0f;
    math::Vector3 m_pivot         = {};
    float         m_dollyVelocity = 0.0f;  // 秒あたりの移動量

    void ApplyRotation();
    void ApplyDolly(float dt);
    // ピボットと向きからカメラ位置を組み直す (射影によって引く距離が変わる)。
    void RepositionFromPivot();
};

} // namespace fbzz::renderer
