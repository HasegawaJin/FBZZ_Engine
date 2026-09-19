/// @file    ScriptCameraProxy.hpp
/// @brief   Script から CameraComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01

#pragma once

#include <Engine/Renderer/Camera.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Physics/Layer.hpp>

namespace fbzz::scene {

class Script;
class GameObject;

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
    /// 左上原点の pixel 座標と DirectX 深度。投影不能時は ZERO。
    /// カメラ選択は TryWorldToViewportPoint と共通。
    math::Vector3 WorldToScreenPoint(const math::Vector3& worldPos) const;
    math::Vector3 ScreenToWorldPoint(const math::Vector3& screenPos) const;

    /// 左上原点の UV と DirectX 深度を返す。画面外も投影する。
    /// cameraObject 未指定時は自分のカメラ、無ければメインカメラを使う。
    /// カメラ不在・背後・不正値は false、outViewport は未変更。
    [[nodiscard]] bool TryWorldToViewportPoint(const math::Vector3& worldPos,
        math::Vector3& outViewport, GameObject* cameraObject = nullptr) const;
    /// UV とカメラ前方への距離 [m、正数] からワールド座標を返す。
    /// カメラ選択は上記と同じ。失敗時は false、outWorld は未変更。
    [[nodiscard]] bool TryViewportToWorldPoint(const math::Vector2& uv, float depth,
        math::Vector3& outWorld, GameObject* cameraObject = nullptr) const;

    float GetFOV() const;
    float GetNearZ() const;
    float GetFarZ() const;
    float GetAspectRatio() const;
    fbzz::LayerMask GetCullingMask() const;
    math::Vector4 GetBackgroundColor() const;
    renderer::CameraClearMode GetClearMode() const;
    /// @return このカメラが描画に使われているか。SetAsMain() が他カメラを降ろすため、
    ///         切り替え側が「今どのカメラが有効か」を確認できる。
    bool  IsMain() const;
    bool  IsVisible(const math::Vector3& worldPos) const;
    /// 投影不能時は direction が ZERO のレイを返す。
    Ray   ScreenPointToRay(float screenX, float screenY) const;
};

} // namespace fbzz::scene
