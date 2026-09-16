/// @file    ScriptPhysicsProxy.hpp
/// @brief   Script から物理コンポーネントへ転送するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once

#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::physics { class Collider; }

namespace fbzz::scene {

class GameObject;
class Script;

// RaycastHit — Script から physics::World のヒット情報を扱うための軽量 DTO。
// WHY: Script では GameObject を優先して扱えるようにしつつ、必要なら低レベル Collider も参照できる。
struct RaycastHit {
    GameObject* gameObject = nullptr;
    const physics::Collider* collider = nullptr;
    math::Vector3 point = math::Vector3::ZERO;
    math::Vector3 normal = math::Vector3::UP;
    float distance = 0.0f;
};

struct ScriptPhysicsProxy {
    Script* script = nullptr;

    void AddForce(const math::Vector3& v) const;
    void AddImpulse(const math::Vector3& v) const;
    void AddForceAtPoint(const math::Vector3& force, const math::Vector3& worldPoint) const;
    bool HasRigidBody() const;
    bool HasRigidBody(GameObject* go) const;
    float GetMass() const;
    void SetMass(float mass) const;
    void SetStatic(bool isStatic) const;
    [[nodiscard]] bool IsStatic() const;
    // 睡眠中の剛体は積分も衝突解決も行わない。力を加えても動かないため、
    // 「押しても反応しない」の原因切り分けに要る。
    [[nodiscard]] bool IsSleeping() const;
    // 剛体ごとの重力倍率。World の重力ベクトルは共有したまま、この個体だけ効きを変える。
    // 1 = 通常 / 0 = 無重力 / 負値 = 反重力。浮かせる演出の間だけ下げて、終わったら戻す。
    [[nodiscard]] float GetGravityScale() const;
    void SetGravityScale(float scale) const;
    /// 自分の剛体の時計倍率 [0,8]。1 で通常。
    void SetLocalTimeScale(float scale) const;
    // World が全剛体へ与えている重力加速度 (m/s^2)。
    // SetGravityScale は「これに対する倍率」なので、跳躍高さのように m/s^2 で
    // 決めた値を倍率へ直すには基準となるこの大きさが要る。
    [[nodiscard]] math::Vector3 GetWorldGravity() const;
    // この 2 つのレイヤーがぶつかるか (ProjectSettings > Physics の衝突行列)。
    // 行列が組まれていなければ常に true。
    //
    // WHY スクリプトから引けるようにするか: 自前で当たりを組み立てるもの
    // (BossHitboxRigComponent) は «自分の剛体とは当たらない» ことを前提にしている。
    // 設定が外れると «押され続けて勝手に動く» という原因の見えない壊れ方をするので、
    // 組み立てる前に前提を確かめて、駄目なら安全側 (トリガー) へ倒せるようにする。
    [[nodiscard]] bool LayersCollide(int a, int b) const;
    void SetVelocity(const math::Vector3& v) const;
    math::Vector3 GetVelocity() const;
    void SetVelocity(GameObject* go, const math::Vector3& v) const;
    math::Vector3 GetVelocity(GameObject* go) const;
    void AddImpulse(GameObject* go, const math::Vector3& v) const;
    float GetMass(GameObject* go) const;
    void SetAngularVelocity(const math::Vector3& v) const;
    math::Vector3 GetAngularVelocity() const;
    void AddTorque(const math::Vector3& v) const;
    void SetFreezePosition(bool x, bool y, bool z) const;
    void SetFreezeRotation(bool x, bool y, bool z) const;
    bool Raycast(const math::Vector3& origin, const math::Vector3& dir, float dist, RaycastHit& hit) const;
    std::vector<RaycastHit> RaycastAll(const math::Vector3& origin, const math::Vector3& dir, float dist) const;
    bool SphereCast(const math::Vector3& center, float radius, const math::Vector3& dir, float dist, RaycastHit& hit) const;
    std::vector<GameObject*> OverlapSphere(const math::Vector3& center, float radius) const;
};

} // namespace fbzz::scene
