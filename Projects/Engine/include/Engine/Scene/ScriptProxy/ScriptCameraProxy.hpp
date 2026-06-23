// FBZZ Engine
// ScriptCameraProxy.hpp | fbzz::scene
// Script から CameraComponent を操作するショートハンド
#pragma once

#include <Math/Vector3.hpp>
#include <Physics/Layer.hpp>

namespace fbzz::scene {

class Script;

struct Ray {
    math::Vector3 origin;
    math::Vector3 direction;
};

struct ScriptCameraProxy {
    Script* script = nullptr;

    void SetAsMain() const;
    void SetFOV(float fovY) const;
    void SetAspectRatio(float aspectRatio) const;
    void SetNearFar(float nearZ, float farZ) const;
    void SetCullingMask(fbzz::LayerMask mask) const;
    math::Vector3 WorldToScreenPoint(const math::Vector3& worldPos) const;
    math::Vector3 ScreenToWorldPoint(const math::Vector3& screenPos) const;

    float GetFOV() const;
    float GetNearZ() const;
    float GetFarZ() const;
    bool  IsVisible(const math::Vector3& worldPos) const;
    Ray   ScreenPointToRay(float screenX, float screenY) const;
};

} // namespace fbzz::scene
