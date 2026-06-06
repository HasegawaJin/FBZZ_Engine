// FBZZ Engine
// ComponentRegistry.hpp | fbzz::scene
// Scene が扱う全コンポーネント型の登録点
// 新しい Component 型を追加するときの編集箇所を一箇所に集約する。
// SceneSerializer や Inspector が同じ型一覧を参照できるようにする。
#pragma once
#include "Components/MeshRenderer.hpp"
#include "Components/MaterialComponent.hpp"
#include "Components/ParticleEmitter.hpp"
#include "Components/ColliderComponent.hpp"
#include "Components/RigidBodyComponent.hpp"
#include "Components/VolumeComponent.hpp"
#include "Components/LightComponent.hpp"
#include "Components/CameraComponent.hpp"
#include "Components/AudioSourceComponent.hpp"
#include "Components/SkyRenderer.hpp"
#include "Components/AnimatorComponent.hpp"
#include "Components/SkinnedMeshRenderer.hpp"
#include "Components/BoneComponent.hpp"
#include "Components/UICanvas.hpp"
#include "Components/UIImage.hpp"
#include "Components/UIButton.hpp"
#include "Components/UIText.hpp"
#include "Components/UILayoutGroup.hpp"
#include "Components/UIAnimator.hpp"
#include "Components/DecalComponent.hpp"
#include "Components/IKSolverComponent.hpp"
#include "Components/CharacterControllerComponent.hpp"
#include "Components/TerrainComponent.hpp"
#include "Components/WaterComponent.hpp"
#include "Components/TrailComponent.hpp"
#include "Components/MeshTrailComponent.hpp"
#include "ScriptComponent.hpp"
#include <tuple>

namespace fbzz::scene {

using ComponentList = std::tuple<
    MeshRenderer,
    MaterialComponent,
    ParticleEmitter,
    AabbColliderComponent,
    BoxColliderComponent,
    SphereColliderComponent,
    CapsuleColliderComponent,
    MeshColliderComponent,
    ConvexHullColliderComponent,
    RigidBodyComponent,
    VolumeComponent,
    LightComponent,
    CameraComponent,
    AudioSourceComponent,
    SkyRenderer,
    AnimatorComponent,
    SkinnedMeshRenderer,
    BoneComponent,
    UICanvas,
    UIImage,
    UIButton,
    UIText,
    UILayoutGroup,
    UIAnimator,
    ScriptComponent,
    DecalComponent,
    IKSolverComponent,
    CharacterControllerComponent,
    TerrainComponent,
    WaterComponent,
    TrailComponent,
    MeshTrailComponent
    // 新型はここに1行追加するだけ
>;

} // namespace fbzz::scene
