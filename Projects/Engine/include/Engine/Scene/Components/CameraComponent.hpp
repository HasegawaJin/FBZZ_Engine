// FBZZ Engine
// CameraComponent.hpp | fbzz::scene
// カメラパラメータを持つ Component。位置/回転は Transform から取得する
#pragma once
#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

struct CameraComponent {
    float fovY    = 60.0f;
    float nearZ   = 0.1f;
    float farZ    = 1000.0f;
    bool  isMain  = true;
    bool  enabled = true;

    const char* GetTypeName() const { return "Camera"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("isMain", isMain);
        r.Field("fovY", fovY);
        r.Field("nearZ", nearZ);
        r.Field("farZ", farZ);
    }
};

} // namespace fbzz::scene
