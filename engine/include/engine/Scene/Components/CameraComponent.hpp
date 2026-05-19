// FBZZ Engine
// CameraComponent.hpp | fbzz::scene
// カメラパラメータを持つ Component。位置/回転は Transform から取得する
#pragma once

namespace fbzz::scene {

struct CameraComponent {
    float fovY    = 60.0f;
    float nearZ   = 0.1f;
    float farZ    = 1000.0f;
    bool  isMain  = true;
    bool  enabled = true;
};

} // namespace fbzz::scene
