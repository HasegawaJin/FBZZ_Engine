// FBZZ Engine
// DebugCamera.hpp | fbzz::renderer
// Scene View 風のデバッグカメラ
// Input を読んで Camera を動かす操作用ラッパー。
// 本番カメラデータとは分け、エディタやデバッグ描画の視点制御に使う。
#pragma once

#include <Engine/Renderer/Camera.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::renderer {

// 操作
//   右ドラッグ           : 視点回転 (FPS ルック)
//   右クリック + WASD/QE : フライ移動 (Shift で高速)
//   中ドラッグ           : パン (平行移動)
//   スクロール           : ドリー (前後移動)
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

private:
    float         m_yaw           = 0.0f;
    float         m_pitch         = 0.0f;
    float         m_focusDistance = 10.0f;
    math::Vector3 m_pivot         = {};
    float         m_dollyVelocity = 0.0f;  // 秒あたりの移動量

    void ApplyRotation();
    void ApplyDolly(float dt);
};

} // namespace fbzz::renderer
