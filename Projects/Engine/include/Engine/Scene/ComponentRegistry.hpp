// FBZZ Engine
// ComponentRegistry.hpp | fbzz::scene
// 全コンポーネント型を一箇所で登録する。新型を追加するときはここだけ編集する。
#pragma once
#include "Components/MeshRenderer.hpp"
#include "Components/ParticleEmitter.hpp"
#include "Components/ColliderComponent.hpp"
#include "Components/RigidBodyComponent.hpp"
#include "Components/VolumeComponent.hpp"
#include "Components/LightComponent.hpp"
#include "Components/CameraComponent.hpp"
#include "Components/AudioSourceComponent.hpp"
#include "Components/SkyRenderer.hpp"
#include "Components/UICanvas.hpp"
#include "Components/UIImage.hpp"
#include "Components/UIButton.hpp"
#include "Components/UIText.hpp"
#include "Components/UILayoutGroup.hpp"
#include "Components/UIAnimator.hpp"
#include "ScriptComponent.hpp"
#include <tuple>

namespace fbzz::scene {

using ComponentList = std::tuple<
    MeshRenderer,
    ParticleEmitter,
    ColliderComponent,
    RigidBodyComponent,
    VolumeComponent,
    LightComponent,
    CameraComponent,
    AudioSourceComponent,
    SkyRenderer,
    UICanvas,
    UIImage,
    UIButton,
    UIText,
    UILayoutGroup,
    UIAnimator,
    ScriptComponent
    // 新型はここに1行追加するだけ
>;

} // namespace fbzz::scene
