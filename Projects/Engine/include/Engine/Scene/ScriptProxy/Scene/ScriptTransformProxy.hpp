/// @file    ScriptTransformProxy.hpp
/// @brief   Script から Transform 操作へ転送するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01

/// @note position / rotation / scale はローカル空間。書き込みは対象と子孫のワールド姿勢へ即時反映する。
/// @see Docs/design/script-transform-contract.md
#pragma once
#include <Engine/Api.hpp>

#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>

namespace fbzz::scene {

struct Transform;
class GameObject;
class Script;

struct ScriptTransformProxy {
    Script* script = nullptr; ///< Transform操作で実行スコープを照合する、非所有の Script 参照。

    FBZZ_ENGINE_API Transform* Get() const;
    explicit operator bool() const { return Get() != nullptr; }

    /// @name ローカル空間 getter / setter (スクリプトが読み書きする)
    /// @{
    FBZZ_ENGINE_API math::Vector3    _GetPos()  const;
    FBZZ_ENGINE_API void             _SetPos(const math::Vector3& v);
    FBZZ_ENGINE_API math::Quaternion _GetRot()  const;
    FBZZ_ENGINE_API void             _SetRot(const math::Quaternion& v);
    FBZZ_ENGINE_API math::Vector3    _GetScl()  const;
    FBZZ_ENGINE_API void             _SetScl(const math::Vector3& v);
    /// @}

    /// @name ワールド空間
    /// @{
    FBZZ_ENGINE_API math::Vector3    _GetWPos() const;
    /// @note 親からローカル位置を逆算し、回転とスケールは保つ。親のゼロスケール軸は実現可能な位置へ射影する。
    FBZZ_ENGINE_API void             _SetWPos(const math::Vector3& v);
    FBZZ_ENGINE_API math::Quaternion _GetWRot() const;
    /// @}

    /// @name 算出値 (worldRotation から算出)
    /// @{
    FBZZ_ENGINE_API math::Vector3 _GetFwd()   const;
    FBZZ_ENGINE_API math::Vector3 _GetUp()    const;
    FBZZ_ENGINE_API math::Vector3 _GetRight() const;
    /// @}

    /// @name declspec(property) — transform.position = v; のように使う
    /// @{
    __declspec(property(get=_GetPos,  put=_SetPos))  math::Vector3    position;
    __declspec(property(get=_GetRot,  put=_SetRot))  math::Quaternion rotation; ///< 親を基準とするローカル回転。
    __declspec(property(get=_GetScl,  put=_SetScl))  math::Vector3    scale; ///< 親を基準とするローカルスケール。
    __declspec(property(get=_GetWPos, put=_SetWPos)) math::Vector3    worldPosition; ///< ワールド空間の位置 [m]。
    __declspec(property(get=_GetWRot))               math::Quaternion worldRotation; ///< ワールド空間の回転。
    __declspec(property(get=_GetFwd))                math::Vector3    forward; ///< Transform の正面を向くワールド方向。
    __declspec(property(get=_GetUp))                 math::Vector3    up; ///< Transform の上を向くワールド方向。
    __declspec(property(get=_GetRight))              math::Vector3    right; ///< Transform の右を向くワールド方向。
    /// @}

    /// @name メソッド
    /// @{
    /// @note 移動量は自分のローカル軸。書き込み後はワールド姿勢も同期する。
    FBZZ_ENGINE_API void    Translate   (const math::Vector3& v)               const;
    /// @param[in] deg 回転角 [deg]。長さゼロの軸は無視する。
    FBZZ_ENGINE_API void    Rotate      (const math::Vector3& axis, float deg) const;
    /// @note target はワールド空間。自分と重なる対象は回転を変えない。
    FBZZ_ENGINE_API void    LookAt      (const math::Vector3& target)          const;
    FBZZ_ENGINE_API float   DistanceTo  (const GameObject& other)              const;
    FBZZ_ENGINE_API math::Vector3 DirectionTo(const GameObject& other)         const;
    /// @}

};

} // namespace fbzz::scene
