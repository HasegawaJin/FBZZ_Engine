// FBZZ Engine
// DebugCamera.hpp | fbzz::renderer
// Unity Scene View 風デバッグカメラ（入力制御付き Camera ラッパー）
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
    float mouseSens      = 0.15f;  // degrees per pixel
    float panSensitivity = 0.003f; // world units per pixel per focus-distance
    float scrollSpeed    = 1.0f;   // 速度加算量 (focus-distance 比)
    float scrollDamping  = 10.0f;  // 速度減衰係数 (大きいほど素早く停止)

    // 指定ターゲットへの向きから yaw / pitch とピボットを初期化する
    void LookAt(const math::Vector3& target);

    // ゲームループ毎に呼ぶ
    void Update(float dt);

private:
    float         m_yaw           = 0.0f;
    float         m_pitch         = 0.0f;
    float         m_focusDistance = 10.0f;
    math::Vector3 m_pivot         = {};
    float         m_dollyVelocity = 0.0f;  // world units per second

    void ApplyRotation();
    void ApplyDolly(float dt);
};

} // namespace fbzz::renderer
