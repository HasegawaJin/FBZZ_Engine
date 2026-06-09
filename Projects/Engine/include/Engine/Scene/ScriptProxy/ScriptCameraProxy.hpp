// FBZZ Engine
// ScriptCameraProxy.hpp | fbzz::scene
// Script から CameraComponent を操作するショートハンド
#pragma once

#include <Math/Vector3.hpp>
#include <Physics/Layer.hpp>

namespace fbzz::scene {

class Script;

struct ScriptCameraProxy {
    Script* script = nullptr;

    void SetAsMain() const;
    void SetFOV(float fovY) const;
    void SetAspectRatio(float aspectRatio) const;
    void SetNearFar(float nearZ, float farZ) const;
    void SetCullingMask(fbzz::LayerMask mask) const;
    math::Vector3 WorldToScreenPoint(const math::Vector3& worldPos) const;
    math::Vector3 ScreenToWorldPoint(const math::Vector3& screenPos) const;
};

} // namespace fbzz::scene
