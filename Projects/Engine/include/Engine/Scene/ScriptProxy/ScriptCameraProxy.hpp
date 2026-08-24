/// @file    ScriptCameraProxy.hpp
/// @brief   Script から CameraComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01

#pragma once

#include <Engine/Renderer/Camera.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
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
    /// 何も描かれていない画素に残る色。空を描くシーンでは空が上書きするため見えない。
    /// RGB は 1 を超えてよい (HDR ターゲットへそのまま入り、ブルームへ乗る)。
    void SetBackgroundColor(const math::Vector4& color) const;
    /// 描き始めのバッファ初期化。DepthOnly は «前に描かれた絵の上に重ねる» モードで、
    /// 重ねる相手が居ないカメラに指定すると前フレームの絵が残る。
    void SetClearMode(renderer::CameraClearMode mode) const;
    math::Vector3 WorldToScreenPoint(const math::Vector3& worldPos) const;
    math::Vector3 ScreenToWorldPoint(const math::Vector3& screenPos) const;

    float GetFOV() const;
    float GetNearZ() const;
    float GetFarZ() const;
    float GetAspectRatio() const;
    fbzz::LayerMask GetCullingMask() const;
    math::Vector4 GetBackgroundColor() const;
    renderer::CameraClearMode GetClearMode() const;
    // このカメラが描画に使われているか。SetAsMain() が他カメラを降ろすため、
    // 「今どのカメラが有効か」を切り替え側から確認できる必要がある。
    bool  IsMain() const;
    bool  IsVisible(const math::Vector3& worldPos) const;
    Ray   ScreenPointToRay(float screenX, float screenY) const;
};

} // namespace fbzz::scene
