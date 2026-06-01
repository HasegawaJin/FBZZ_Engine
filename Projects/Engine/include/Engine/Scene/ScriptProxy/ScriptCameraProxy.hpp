// FBZZ Engine
// ScriptCameraProxy.hpp | fbzz::scene
// Script から CameraComponent を操作するショートハンド
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptCameraProxy {
    Script* script = nullptr;

    void SetAsMain() const;
    void SetFOV(float fovY) const;
    void SetNearFar(float nearZ, float farZ) const;
    math::Vector3 WorldToScreenPoint(const math::Vector3& worldPos) const;
    math::Vector3 ScreenToWorldPoint(const math::Vector3& screenPos) const;
};

} // namespace fbzz::scene
