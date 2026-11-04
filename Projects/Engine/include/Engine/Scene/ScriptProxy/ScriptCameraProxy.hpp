/// @file    ScriptCameraProxy.hpp
/// @brief   Script から CameraComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01

#pragma once
#include <Engine/Api.hpp>

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

    FBZZ_ENGINE_API void SetAsMain() const;
    FBZZ_ENGINE_API void SetFOV(float fovY) const;
    FBZZ_ENGINE_API void SetAspectRatio(float aspectRatio) const;
    FBZZ_ENGINE_API void SetNearFar(float nearZ, float farZ) const;
    FBZZ_ENGINE_API void SetCullingMask(fbzz::LayerMask mask) const;
    /// @note 何も描かれていない画素に残る色。空を描くシーンでは空が上書きするため見えない。
    /// @note RGB は 1 を超えてよい (HDR ターゲットへそのまま入り、ブルームへ乗る)。
    FBZZ_ENGINE_API void SetBackgroundColor(const math::Vector4& color) const;
    /// @note 描き始めのバッファ初期化。DepthOnly は «前に描かれた絵の上に重ねる» モードで、
    /// @note 重ねる相手が居ないカメラに指定すると前フレームの絵が残る。
    FBZZ_ENGINE_API void SetClearMode(renderer::CameraClearMode mode) const;
    /// @note 左上原点の pixel 座標と DirectX 深度。投影不能時は ZERO。
    /// @note カメラ選択は TryWorldToViewportPoint と共通。
    FBZZ_ENGINE_API math::Vector3 WorldToScreenPoint(const math::Vector3& worldPos) const;
    FBZZ_ENGINE_API math::Vector3 ScreenToWorldPoint(const math::Vector3& screenPos) const;

    /// @note 左上原点の UV と DirectX 深度を返す。画面外も投影する。
    /// @note cameraObject 未指定時は自分のカメラ、無ければメインカメラを使う。
    /// @note カメラ不在・背後・不正値は false、outViewport は未変更。
    [[nodiscard]] FBZZ_ENGINE_API bool TryWorldToViewportPoint(const math::Vector3& worldPos,
        math::Vector3& outViewport, GameObject* cameraObject = nullptr) const;
    /// @note UV とカメラ前方への距離 [m、正数] からワールド座標を返す。
    /// @note カメラ選択は上記と同じ。失敗時は false、outWorld は未変更。
    [[nodiscard]] FBZZ_ENGINE_API bool TryViewportToWorldPoint(const math::Vector2& uv, float depth,
        math::Vector3& outWorld, GameObject* cameraObject = nullptr) const;

    FBZZ_ENGINE_API float GetFOV() const;
    FBZZ_ENGINE_API float GetNearZ() const;
    FBZZ_ENGINE_API float GetFarZ() const;
    FBZZ_ENGINE_API float GetAspectRatio() const;
    FBZZ_ENGINE_API fbzz::LayerMask GetCullingMask() const;
    FBZZ_ENGINE_API math::Vector4 GetBackgroundColor() const;
    FBZZ_ENGINE_API renderer::CameraClearMode GetClearMode() const;
    /// @return このカメラが描画に使われているか。SetAsMain() が他カメラを降ろすため、
    /// @note 切り替え側が「今どのカメラが有効か」を確認できる。
    FBZZ_ENGINE_API bool  IsMain() const;
    FBZZ_ENGINE_API bool  IsVisible(const math::Vector3& worldPos) const;
    /// @note 投影不能時は direction が ZERO のレイを返す。
    FBZZ_ENGINE_API Ray   ScreenPointToRay(float screenX, float screenY) const;
};

} /// @note namespace fbzz::scene
