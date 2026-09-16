/// @file    JointSync.hpp
/// @brief   JointComponent → physics::Constraint の張り直しと解除。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// WHY 独立した ISystem にしないか:
///   制約は World の BeginSceneSync / EndSceneSync の «窓» の中で申告しないと、
///   その場で «申告が途切れた» と見なされて破棄される。窓を開けて Step まで走らせるのは
///   PhysicsSystem::Update の中だけなので、別 System として並べると順番が保証できない。
///   ColliderSync と同じく、手順だけを独立モジュールへ切り出して PhysicsSystem から呼ぶ。
#pragma once

namespace fbzz::physics { class World; class RigidBody; }

namespace fbzz::scene {

class Scene;
class GameObject;
struct JointComponent;

/// ペア関節の相手になる剛体。connectedBody が空なら祖先方向へたどる。
/// 相手が居ない / 剛体が無い / 無効なら nullptr。Chain では常に nullptr。
///
/// WHY 公開するか: 探し方は «張るとき» と «スクリプトが今の間隔を問うとき» で
///     一致していないと、Inspector に出る距離と物理が効いている距離が食い違う。
[[nodiscard]] physics::RigidBody* ResolveJointPartner(Scene& scene,
                                                      GameObject& go,
                                                      const JointComponent& joint);

/// 全 JointComponent を physics::World へ申告する。
/// world.BeginSceneSync() の後、world.EndSceneSync() の前に呼ぶこと。
///
/// 相手の剛体が見つからない / GameObject が非アクティブ / enabled が false のものは
/// 申告せず、既に張ってあれば解除する (RigidBodyComponent と同じ有効条件)。
void SyncJointComponents(Scene& scene, physics::World& world);

} // namespace fbzz::scene
