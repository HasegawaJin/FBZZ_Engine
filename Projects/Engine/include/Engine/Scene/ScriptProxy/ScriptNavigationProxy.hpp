// FBZZ Engine
// ScriptNavigationProxy.hpp | fbzz::scene
// Script から NavMeshAgentComponent へ転送するショートハンド (Unity の NavMeshAgent 相当)
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;
class GameObject;

struct ScriptNavigationProxy {
    Script* script = nullptr;

    // NavMesh 上のパスを計算してその目的地へ移動を開始する。
    void SetDestination(const math::Vector3& worldPos) const;
    // パスを完全に放棄して停止する (ResetPath 相当)。
    void CancelPath() const;
    // パスは保持したまま移動・回転だけ一時停止する。
    void Pause() const;
    // Pause() で止めた移動を同じパスから再開する。
    void Resume() const;
    // パス計算をせずその場へ瞬間移動し、保持していたパスを破棄する。
    void Warp(const math::Vector3& worldPos) const;

    // target の現在位置を目的地として追跡し続ける (敵 AI のプレイヤー追跡などに使う)。
    // interval ごと、または target が一定距離動くたびに NavigationSystem が自動で再パスする。
    void SetTarget(EntityID target, float repathInterval = 0.5f) const;
    void SetTarget(const GameObject& target, float repathInterval = 0.5f) const;
    // 追跡を解除する (現在のパスはそのまま、新たな再パスは行われなくなる)。
    void ClearTarget() const;
    bool IsFollowingTarget() const;

    // agentTypeId が一致する NavMeshSurfaceComponent の再ベイクをリクエストする。
    void BakeNavMesh() const;

    bool IsPaused() const;
    bool IsMoving() const;
    bool HasPath() const;
    bool HasArrived() const;
    bool IsStuck() const;
    float GetRemainingDistance() const;
    math::Vector3 GetVelocity() const;

    // 最大移動速度を動的に変更する。
    void SetSpeed(float speed) const;
    // 旋回速度 [deg/s] を動的に変更する。
    void SetAngularSpeed(float degPerSec) const;

    // 通過可能なエリアタイプのビットマスク (-1 = すべて)。
    int  GetAreaMask() const;
    void SetAreaMask(int mask) const;

    // NavMeshSensorComponent (付いていれば) の検知結果を参照するショートハンド。
    bool CanSeeTarget() const;
    GameObject* GetDetectedTarget() const;
};

} // namespace fbzz::scene
