/// @file    ScriptNavigationProxy.hpp
/// @brief   Script から NavMeshAgentComponent へ転送するショートハンド (Unity の NavMeshAgent 相当)。
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;
class GameObject;

struct ScriptNavigationProxy {
    Script* script = nullptr;

    /// @brief NavMesh 上のパスを計算してその目的地へ移動を開始する。
    FBZZ_ENGINE_API void SetDestination(const math::Vector3& worldPos) const;
    /// @brief パスを完全に放棄して停止する (ResetPath 相当)。
    FBZZ_ENGINE_API void CancelPath() const;
    /// @brief パスは保持したまま移動・回転だけ一時停止する。
    FBZZ_ENGINE_API void Pause() const;
    /// @brief Pause() で止めた移動を同じパスから再開する。
    FBZZ_ENGINE_API void Resume() const;
    /// @brief パス計算をせずその場へ瞬間移動し、保持していたパスを破棄する。
    FBZZ_ENGINE_API void Warp(const math::Vector3& worldPos) const;

    /// @brief target の現在位置を目的地として追跡し続ける (敵 AI のプレイヤー追跡等に使う)。
    /// @note interval ごと、または target が一定距離動くたびに NavigationSystem が自動で再パスする。
    FBZZ_ENGINE_API void SetTarget(EntityID target, float repathInterval = 0.5f) const;
    FBZZ_ENGINE_API void SetTarget(const GameObject& target, float repathInterval = 0.5f) const;
    /// @brief 追跡を解除する (現在のパスはそのまま、新たな再パスは行われなくなる)。
    FBZZ_ENGINE_API void ClearTarget() const;
    FBZZ_ENGINE_API bool IsFollowingTarget() const;

    /// @brief agentTypeId が一致する NavMeshSurfaceComponent の再ベイクをリクエストする。
    FBZZ_ENGINE_API void BakeNavMesh() const;

    FBZZ_ENGINE_API bool IsPaused() const;
    FBZZ_ENGINE_API bool IsMoving() const;
    FBZZ_ENGINE_API bool HasPath() const;
    FBZZ_ENGINE_API bool HasArrived() const;
    FBZZ_ENGINE_API bool IsStuck() const;
    FBZZ_ENGINE_API float GetRemainingDistance() const;
    FBZZ_ENGINE_API math::Vector3 GetVelocity() const;

    /// @brief 最大移動速度を動的に変更する。
    FBZZ_ENGINE_API void  SetSpeed(float speed) const;
    FBZZ_ENGINE_API float GetSpeed() const;
    /// @brief 旋回速度 [deg/s] を動的に変更する。
    FBZZ_ENGINE_API void  SetAngularSpeed(float degPerSec) const;
    FBZZ_ENGINE_API float GetAngularSpeed() const;
    /// @return 現在の目的地。SetTarget() による追跡中は追跡先の最新パス目標になる。
    /// @note 目的地を持たないときは自身の現在位置ではなく Vector3::ZERO を返す (HasPath() で判別)。
    FBZZ_ENGINE_API math::Vector3 GetDestination() const;
    /// @brief 到達と見なす最終ウェイポイントからの距離 [m]。
    FBZZ_ENGINE_API float GetStoppingDistance() const;
    FBZZ_ENGINE_API void  SetStoppingDistance(float distance) const;

    /// @brief 通過可能なエリアタイプのビットマスク (-1 = すべて)。
    FBZZ_ENGINE_API int  GetAreaMask() const;
    FBZZ_ENGINE_API void SetAreaMask(int mask) const;

    /// @brief NavMeshSensorComponent (付いていれば) の検知結果を参照するショートハンド。
    FBZZ_ENGINE_API bool CanSeeTarget() const;
    FBZZ_ENGINE_API GameObject* GetDetectedTarget() const;
};

} /// @note namespace fbzz::scene
