/// @file    ScriptTransformProxy.hpp
/// @brief   Script から Transform 操作へ転送するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// @note position / rotation / scale はローカル空間。書き込みは対象と子孫のワールド姿勢へ即時反映する。
/// @see Docs/design/script-transform-contract.md
#pragma once

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct Transform;
class GameObject;
class Script;

struct ScriptTransformProxy {
    Script* script = nullptr;

    Transform* Get() const;
    explicit operator bool() const { return Get() != nullptr; }

    /// @name ローカル空間 getter / setter (スクリプトが読み書きする)
    /// @{
    math::Vector3    _GetPos()  const;
    void             _SetPos(const math::Vector3& v);
    math::Quaternion _GetRot()  const;
    void             _SetRot(const math::Quaternion& v);
    math::Vector3    _GetScl()  const;
    void             _SetScl(const math::Vector3& v);
    /// @}

    /// @name ワールド空間
    /// @{
    math::Vector3    _GetWPos() const;
    /// @note 親からローカル位置を逆算し、回転とスケールは保つ。親のゼロスケール軸は実現可能な位置へ射影する。
    void             _SetWPos(const math::Vector3& v);
    math::Quaternion _GetWRot() const;
    /// @}

    /// @name 算出値 (worldRotation から算出)
    /// @{
    math::Vector3 _GetFwd()   const;
    math::Vector3 _GetUp()    const;
    math::Vector3 _GetRight() const;
    /// @}

    /// @name declspec(property) — transform.position = v; のように使う
    /// @{
    __declspec(property(get=_GetPos,  put=_SetPos))  math::Vector3    position;
    __declspec(property(get=_GetRot,  put=_SetRot))  math::Quaternion rotation;
    __declspec(property(get=_GetScl,  put=_SetScl))  math::Vector3    scale;
    __declspec(property(get=_GetWPos, put=_SetWPos)) math::Vector3    worldPosition;
    __declspec(property(get=_GetWRot))               math::Quaternion worldRotation;
    __declspec(property(get=_GetFwd))                math::Vector3    forward;
    __declspec(property(get=_GetUp))                 math::Vector3    up;
    __declspec(property(get=_GetRight))              math::Vector3    right;
    /// @}

    /// @name メソッド
    /// @{
    /// @note 移動量は自分のローカル軸。書き込み後はワールド姿勢も同期する。
    void    Translate   (const math::Vector3& v)               const;
    /// @param deg 回転角 [deg]。長さゼロの軸は無視する。
    void    Rotate      (const math::Vector3& axis, float deg) const;
    /// @note target はワールド空間。自分と重なる対象は回転を変えない。
    void    LookAt      (const math::Vector3& target)          const;
    float   DistanceTo  (const GameObject& other)              const;
    math::Vector3 DirectionTo(const GameObject& other)         const;
    /// @}

};

} /// @note namespace fbzz::scene
