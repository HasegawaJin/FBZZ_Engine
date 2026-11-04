/// @file    ScriptPhysicsProxy.hpp
/// @brief   Script から物理コンポーネントへ転送するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::physics { class Collider; }

namespace fbzz::scene {

class GameObject;
class Script;

/// @brief physics::World のヒット情報を扱う軽量 DTO。GameObject を優先しつつ、必要なら
/// @note 低レベル Collider も参照できる。
struct RaycastHit {
    GameObject* gameObject = nullptr;
    const physics::Collider* collider = nullptr;
    math::Vector3 point = math::Vector3::ZERO;
    math::Vector3 normal = math::Vector3::UP;
    float distance = 0.0f;
};

struct ScriptPhysicsProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API void AddForce(const math::Vector3& v) const;
    FBZZ_ENGINE_API void AddImpulse(const math::Vector3& v) const;
    FBZZ_ENGINE_API void AddForceAtPoint(const math::Vector3& force, const math::Vector3& worldPoint) const;
    FBZZ_ENGINE_API bool HasRigidBody() const;
    FBZZ_ENGINE_API bool HasRigidBody(GameObject* go) const;
    FBZZ_ENGINE_API float GetMass() const;
    FBZZ_ENGINE_API void SetMass(float mass) const;
    FBZZ_ENGINE_API void SetStatic(bool isStatic) const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsStatic() const;
    /// @note 睡眠中の剛体は積分も衝突解決も行わない。力を加えても動かないため、
    /// @note 「押しても反応しない」の原因切り分けに要る。
    [[nodiscard]] FBZZ_ENGINE_API bool IsSleeping() const;
    /// @note 剛体ごとの重力倍率。World の重力ベクトルは共有したまま、この個体だけ効きを変える。
    /// @note 1 = 通常 / 0 = 無重力 / 負値 = 反重力。浮かせる演出の間だけ下げて、終わったら戻す。
    [[nodiscard]] FBZZ_ENGINE_API float GetGravityScale() const;
    FBZZ_ENGINE_API void SetGravityScale(float scale) const;
    /// @note 自分の剛体の時計倍率 [0,8]。1 で通常。
    FBZZ_ENGINE_API void SetLocalTimeScale(float scale) const;
    /// @note 流れ (Flow Field / 環境流) との結合係数 [1/s]。0 で風も水流も受けない。
    /// @note 既定 0: 剛体はゲームプレイの当事者なので、風に流されるかは受ける体が宣言する。
    /// @note «一時的に流されやすくする» (傘を広げる等) は体の状態なので、ここから触れる。
    FBZZ_ENGINE_API void SetFlowCoupling(float coupling) const;
    [[nodiscard]] FBZZ_ENGINE_API float GetFlowCoupling() const;
    /// @note World が全剛体へ与えている重力加速度 (m/s^2)。
    /// @note SetGravityScale は「これに対する倍率」なので、跳躍高さのように m/s^2 で
    /// @note 決めた値を倍率へ直すには基準となるこの大きさが要る。
    [[nodiscard]] FBZZ_ENGINE_API math::Vector3 GetWorldGravity() const;
    /// @note この 2 つのレイヤーがぶつかるか (ProjectSettings > Physics の衝突行列)。行列が
    /// @note 組まれていなければ常に true。
    /// @note 自前で当たりを組み立てるもの (BossHitboxRigComponent) は «自分の剛体とは
    /// @note 当たらない» 前提を組み立て前に確かめ、駄目なら安全側 (トリガー) へ倒すため。
    [[nodiscard]] FBZZ_ENGINE_API bool LayersCollide(int a, int b) const;
    FBZZ_ENGINE_API void SetVelocity(const math::Vector3& v) const;
    FBZZ_ENGINE_API math::Vector3 GetVelocity() const;
    FBZZ_ENGINE_API void SetVelocity(GameObject* go, const math::Vector3& v) const;
    FBZZ_ENGINE_API math::Vector3 GetVelocity(GameObject* go) const;
    FBZZ_ENGINE_API void AddImpulse(GameObject* go, const math::Vector3& v) const;
    FBZZ_ENGINE_API float GetMass(GameObject* go) const;
    FBZZ_ENGINE_API void SetAngularVelocity(const math::Vector3& v) const;
    FBZZ_ENGINE_API math::Vector3 GetAngularVelocity() const;
    FBZZ_ENGINE_API void AddTorque(const math::Vector3& v) const;
    FBZZ_ENGINE_API void SetFreezePosition(bool x, bool y, bool z) const;
    FBZZ_ENGINE_API void SetFreezeRotation(bool x, bool y, bool z) const;
    FBZZ_ENGINE_API bool Raycast(const math::Vector3& origin, const math::Vector3& dir, float dist, RaycastHit& hit) const;
    FBZZ_ENGINE_API std::vector<RaycastHit> RaycastAll(const math::Vector3& origin, const math::Vector3& dir, float dist) const;
    FBZZ_ENGINE_API bool SphereCast(const math::Vector3& center, float radius, const math::Vector3& dir, float dist, RaycastHit& hit) const;
    FBZZ_ENGINE_API std::vector<GameObject*> OverlapSphere(const math::Vector3& center, float radius) const;
};

} /// @note namespace fbzz::scene
