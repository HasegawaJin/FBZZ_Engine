/// @file    JointSync.hpp
/// @brief   JointComponent → physics::Constraint の張り直しと解除。
/// @author  Hasegawa Jin
/// @date    2026-09-12

/// @note 独立 ISystem にしない理由: 制約は World の BeginSceneSync/EndSceneSync の窓の中で
/// @note 申告しないと «申告が途切れた» とみなされ破棄される。窓を開けて Step まで走らせるのは
/// @note PhysicsSystem::Update の中だけなので、別 System にすると順序を保証できない。
#pragma once
#include <Engine/Api.hpp>

namespace fbzz::physics { class World; class RigidBody; }

namespace fbzz::scene {

class Scene;
class GameObject;
struct JointComponent;

/// @note ペア関節の相手になる剛体。connectedBody が空なら祖先方向へたどる。
/// @return 相手が居ない / 剛体が無い / 無効なら nullptr。Chain では常に nullptr。
/// @note 探し方を「張るとき」と「スクリプトが間隔を問うとき」で分けると、Inspector の距離と
/// @note 物理が効いている距離が食い違うため、ここへ公開して両方から呼ぶ。
[[nodiscard]] FBZZ_ENGINE_API physics::RigidBody* ResolveJointPartner(Scene& scene,
                                                      GameObject& go,
                                                      const JointComponent& joint);

/// @note 全 JointComponent を physics::World へ申告する。
/// @note world.BeginSceneSync() の後、world.EndSceneSync() の前に呼ぶこと。

/// @note 相手の剛体が見つからない / GameObject が非アクティブ / enabled が false のものは
/// @note 申告せず、既に張ってあれば解除する (RigidBodyComponent と同じ有効条件)。
FBZZ_ENGINE_API void SyncJointComponents(Scene& scene, physics::World& world);

} /// @note namespace fbzz::scene
