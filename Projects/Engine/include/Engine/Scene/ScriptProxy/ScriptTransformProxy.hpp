/// @file    ScriptTransformProxy.hpp
/// @brief   Script から Transform 操作へ転送するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// __declspec(property) により transform.position / transform.worldPosition を
/// -> 不要でアクセスできる。position=ローカル、worldPosition=ワールド。
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

    /// @name ワールド空間 getter (TransformSystem が毎フレーム更新)
    /// @{
    math::Vector3    _GetWPos() const;
    void             _SetWPos(const math::Vector3& v);  ///< 物理・IK 専用
    math::Quaternion _GetWRot() const;                  ///< 読み取り専用
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
    void    Translate   (const math::Vector3& v)               const;
    void    Rotate      (const math::Vector3& axis, float deg) const;
    void    LookAt      (const math::Vector3& target)          const;
    float   DistanceTo  (const GameObject& other)              const;
    math::Vector3 DirectionTo(const GameObject& other)         const;
    /// @}

};

} // namespace fbzz::scene
