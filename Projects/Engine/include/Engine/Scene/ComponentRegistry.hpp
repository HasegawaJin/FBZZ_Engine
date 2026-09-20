/// @file    ComponentRegistry.hpp
/// @brief   コンポーネント型とEditor／永続化メタデータの単一登録表。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once

#include "Components/MeshRenderer.hpp"
#include "Components/MaterialComponent.hpp"
#include "Components/ParticleEmitter.hpp"
#include "Fields/FlowField.hpp"
#include "Components/WeatherComponent.hpp"
#include "Components/ColliderComponent.hpp"
#include "Components/RigidBodyComponent.hpp"
#include "Components/JointComponent.hpp"
#include "Components/ClothComponent.hpp"
#include "Components/VolumeComponent.hpp"
#include "Components/LightComponent.hpp"
#include "Components/CameraComponent.hpp"
#include "Components/AudioSourceComponent.hpp"
#include "Components/AudioListenerComponent.hpp"
#include "Components/LODGroupComponent.hpp"
#include "Components/SkyRenderer.hpp"
#include "Components/SunMoonRenderer.hpp"
#include "Components/AnimatorComponent.hpp"
#include "Components/SkinnedMeshRenderer.hpp"
#include "Components/BoneComponent.hpp"
#include "Components/UICanvas.hpp"
#include "Components/UICanvasGroup.hpp"
#include "Components/UIImage.hpp"
#include "Components/UIButton.hpp"
#include "Components/UIText.hpp"
#include "Components/UILayoutGroup.hpp"
#include "Components/UIAnimator.hpp"
#include "Components/DecalComponent.hpp"
#include "Components/EnvironmentLightComponent.hpp"
#include "Components/ReflectionProbeComponent.hpp"
#include "Components/AtmosphericScatteringComponent.hpp"
#include "Components/PostProcessVolumeComponent.hpp"
#include "Components/IKSolverComponent.hpp"
#include "Components/SpringBoneComponent.hpp"
#include "Components/RagdollComponent.hpp"
#include "Components/MotionWarpComponent.hpp"
#include "Components/CharacterControllerComponent.hpp"
#include "Components/TerrainComponent.hpp"
#include "Components/TerrainGridComponent.hpp"
#include "Components/WaterComponent.hpp"
#include "Components/VolumetricCloudComponent.hpp"
#include "Components/TrailComponent.hpp"
#include "Components/MeshTrailComponent.hpp"
#include "Components/LifetimeComponent.hpp"
#include "Components/VFXScreenEffect.hpp"
#include "Components/VFXComponent.hpp"
#include "Components/VFXElement.hpp"
#include "Components/VFXBeamComponent.hpp"
#include "Components/VFXLineComponent.hpp"
#include "Components/VFXAudioEnvelope.hpp"
#include "Components/PresentationComponents.hpp"
#include "Components/ConstraintComponents.hpp"
#include "Components/SplineComponents.hpp"
#include "Components/CameraRigComponents.hpp"
#include "Components/UIControls.hpp"
#include "Components/AudioSpatialComponents.hpp"
#include "Components/NavMeshSurfaceComponent.hpp"
#include "Components/NavMeshModifierComponent.hpp"
#include "Components/NavMeshAgentComponent.hpp"
#include "Components/NavMeshOffMeshLinkComponent.hpp"
#include "Components/NavMeshPatrolComponent.hpp"
#include "Components/NavMeshSensorComponent.hpp"
#include "Components/BehaviorTreeComponent.hpp"
#include "Components/ProceduralMeshComponent.hpp"
#include "Components/SequencePlayerComponent.hpp"
#include "Components/FiberComponent.hpp"
#include "Components/FiberInteractorComponent.hpp"
#include "Components/LightProbeVolumeComponent.hpp"
#include "ScriptComponent.hpp"

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace fbzz::scene {

/// @brief Editor 上の分類。依存方向を増やさず Engine 側の登録情報だけでメニューを構築する。
enum class ComponentCategory {
    Rendering,
    Lighting,
    Physics,
    Animation,
    Audio,
    Effects,
    Environment,
    Navigation,
    Terrain,
    UI,
    Misc,
    Internal
};

/// @brief Automatic は Reflect() から描画し、Custom は専用 Editor、Hidden は内部型として扱う。
enum class ComponentInspectorMode { Automatic, Custom, Hidden };

/// @brief Automatic は Reflect() で TOML 化し、Custom は既存の複合データ専用処理を使う。
enum class ComponentSerializationMode { Automatic, Custom };

/// @brief C++20 の文字列 NTTP。型名と表示名を登録型そのものへ保持する。
template<size_t N>
struct ComponentString {
    char value[N]{};

    constexpr ComponentString(const char (&text)[N])
    {
        for (size_t i = 0; i < N; ++i) value[i] = text[i];
    }
};

/// @brief 1 コンポーネント分の登録情報。単純型は Automatic を選ぶだけで全標準経路へ接続される。
template<typename T,
         ComponentCategory CategoryValue,
         ComponentString SerializedName,
         ComponentString DisplayName,
         ComponentInspectorMode InspectorModeValue = ComponentInspectorMode::Automatic,
         ComponentSerializationMode SerializationModeValue = ComponentSerializationMode::Automatic,
         bool AddableValue = true>
struct ComponentRegistration {
    using Type = T;
    static constexpr bool hasReflect = requires(T& component, IReflector& reflector) {
        component.Reflect(reflector);
    };
    static_assert(InspectorModeValue != ComponentInspectorMode::Automatic
                  || (hasReflect && std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>),
                  "Automatic Inspector requires Reflect() and a copyable component");
    static_assert(SerializationModeValue != ComponentSerializationMode::Automatic
                  || (hasReflect && std::is_default_constructible_v<T> && std::is_move_constructible_v<T>),
                  "Automatic serialization requires Reflect(), default construction, and move construction");
    static constexpr ComponentCategory category = CategoryValue;
    static constexpr ComponentInspectorMode inspectorMode = InspectorModeValue;
    static constexpr ComponentSerializationMode serializationMode = SerializationModeValue;
    static constexpr bool addable = AddableValue;
    static constexpr const char* serializedName = SerializedName.value;
    static constexpr const char* displayName = DisplayName.value;
};

#define FBZZ_COMPONENT(Type, Category, DisplayName) \
    ComponentRegistration<Type, ComponentCategory::Category, #Type, DisplayName>

#define FBZZ_CUSTOM_COMPONENT(Type, Category, DisplayName) \
    ComponentRegistration<Type, ComponentCategory::Category, #Type, DisplayName, \
        ComponentInspectorMode::Custom, ComponentSerializationMode::Custom>

#define FBZZ_AUTO_INSPECTOR_COMPONENT(Type, Category, DisplayName) \
    ComponentRegistration<Type, ComponentCategory::Category, #Type, DisplayName, \
        ComponentInspectorMode::Automatic, ComponentSerializationMode::Custom>

#define FBZZ_INTERNAL_COMPONENT(Type, DisplayName) \
    ComponentRegistration<Type, ComponentCategory::Internal, #Type, DisplayName, \
        ComponentInspectorMode::Hidden, ComponentSerializationMode::Custom, false>

/// @brief 登録順は Scene の SoA tuple 順と Script DLL ABI へ影響するため、既存順を維持する。
/// @note 新しい単純型は FBZZ_COMPONENT を 1 行追加すれば、標準の追加・Inspector・保存経路へ入る。
using ComponentRegistry = std::tuple<
    FBZZ_CUSTOM_COMPONENT(MeshRenderer, Rendering, "Mesh Renderer"),
    FBZZ_CUSTOM_COMPONENT(MaterialComponent, Rendering, "Material"),
    FBZZ_CUSTOM_COMPONENT(ParticleEmitter, Effects, "Particle Emitter"),
    FBZZ_CUSTOM_COMPONENT(FlowField, Effects, "Flow Field"),
    FBZZ_CUSTOM_COMPONENT(AabbColliderComponent, Physics, "AABB Collider"),
    FBZZ_CUSTOM_COMPONENT(BoxColliderComponent, Physics, "Box Collider"),
    FBZZ_CUSTOM_COMPONENT(SphereColliderComponent, Physics, "Sphere Collider"),
    FBZZ_CUSTOM_COMPONENT(CapsuleColliderComponent, Physics, "Capsule Collider"),
    FBZZ_CUSTOM_COMPONENT(MeshColliderComponent, Physics, "Mesh Collider"),
    FBZZ_CUSTOM_COMPONENT(ConvexHullColliderComponent, Physics, "Convex Hull Collider"),
    FBZZ_CUSTOM_COMPONENT(TerrainColliderComponent, Physics, "Terrain Collider"),
    FBZZ_CUSTOM_COMPONENT(RigidBodyComponent, Physics, "Rigid Body"),
    FBZZ_CUSTOM_COMPONENT(VolumeComponent, Physics, "Volume"),
    FBZZ_CUSTOM_COMPONENT(LightComponent, Lighting, "Light"),
    FBZZ_CUSTOM_COMPONENT(CameraComponent, Lighting, "Camera"),
    FBZZ_COMPONENT(AudioSourceComponent, Audio, "Audio Source"),
    FBZZ_COMPONENT(AudioListenerComponent, Audio, "Audio Listener"),
    FBZZ_CUSTOM_COMPONENT(LODGroupComponent, Rendering, "LOD Group"),
    FBZZ_CUSTOM_COMPONENT(SkyRenderer, Environment, "Sky Renderer"),
    FBZZ_CUSTOM_COMPONENT(SunMoonRenderer, Environment, "Sun Moon Renderer"),
    FBZZ_CUSTOM_COMPONENT(AnimatorComponent, Animation, "Animator"),
    FBZZ_CUSTOM_COMPONENT(SkinnedMeshRenderer, Rendering, "Skinned Mesh Renderer"),
    FBZZ_INTERNAL_COMPONENT(BoneComponent, "Bone"),
    FBZZ_COMPONENT(UICanvas, UI, "UI Canvas"),
    FBZZ_COMPONENT(UIImage, UI, "UI Image"),
    FBZZ_COMPONENT(UIButton, UI, "UI Button"),
    FBZZ_COMPONENT(UIText, UI, "UI Text"),
    FBZZ_COMPONENT(UILayoutGroup, UI, "UI Layout Group"),
    FBZZ_COMPONENT(UIAnimator, UI, "UI Animator"),
    FBZZ_INTERNAL_COMPONENT(ScriptComponent, "Scripts"),
    FBZZ_CUSTOM_COMPONENT(DecalComponent, Environment, "Decal"),
    FBZZ_CUSTOM_COMPONENT(IKSolverComponent, Animation, "IK Solver"),
    FBZZ_CUSTOM_COMPONENT(CharacterControllerComponent, Physics, "Character Controller"),
    FBZZ_CUSTOM_COMPONENT(TerrainComponent, Terrain, "Terrain"),
    FBZZ_CUSTOM_COMPONENT(TerrainGridComponent, Terrain, "Terrain Grid"),
    FBZZ_CUSTOM_COMPONENT(WaterComponent, Terrain, "Water"),
    FBZZ_CUSTOM_COMPONENT(VolumetricCloudComponent, Environment, "Volumetric Cloud"),
    FBZZ_CUSTOM_COMPONENT(TrailComponent, Effects, "Trail"),
    FBZZ_CUSTOM_COMPONENT(MeshTrailComponent, Effects, "Mesh Trail"),
    FBZZ_COMPONENT(LifetimeComponent, Effects, "Lifetime"),
    FBZZ_CUSTOM_COMPONENT(NavMeshSurfaceComponent, Navigation, "NavMesh Surface"),
    FBZZ_CUSTOM_COMPONENT(NavMeshModifierComponent, Navigation, "NavMesh Modifier"),
    FBZZ_CUSTOM_COMPONENT(NavMeshAgentComponent, Navigation, "NavMesh Agent"),
    FBZZ_CUSTOM_COMPONENT(NavMeshOffMeshLinkComponent, Navigation, "Off-Mesh Link"),
    FBZZ_CUSTOM_COMPONENT(NavMeshPatrolComponent, Navigation, "NavMesh Patrol"),
    FBZZ_CUSTOM_COMPONENT(NavMeshSensorComponent, Navigation, "NavMesh Sensor"),
    FBZZ_CUSTOM_COMPONENT(EnvironmentLightComponent, Environment, "Environment Light"),
    FBZZ_CUSTOM_COMPONENT(ReflectionProbeComponent, Environment, "Reflection Probe"),
    FBZZ_CUSTOM_COMPONENT(AtmosphericScatteringComponent, Environment, "Atmospheric Scattering"),
    FBZZ_CUSTOM_COMPONENT(PostProcessVolumeComponent, Environment, "Post Process Volume"),
    /// @note ComponentRegistry は Script DLL ABI へ影響するため、新規型は既存順を崩さず末尾へ追加する。
    /// @note VFXScreenEffect はかつて VFXGraphSystem が生成するだけの内部型だったが、.vfx がプレハブになった以上「シーンに置いて保存できる」必要がある (置けなければ変換で消える)。weight は VFXElement が毎フレーム書くランタイム値なので Reflect には含めない。
    FBZZ_COMPONENT(VFXScreenEffect, Effects, "VFX Screen Effect"),
    FBZZ_COMPONENT(VFXCameraShake, Effects, "VFX Camera Shake"),
    FBZZ_COMPONENT(VFXTimeScale, Effects, "VFX Time Scale"),
    FBZZ_COMPONENT(SpriteRendererComponent, Rendering, "Sprite Renderer"),
    FBZZ_COMPONENT(SortingGroupComponent, Rendering, "Sorting Group"),
    FBZZ_COMPONENT(LineRendererComponent, Rendering, "Line Renderer"),
    FBZZ_COMPONENT(BillboardComponent, Rendering, "Billboard"),
    FBZZ_COMPONENT(ProjectorComponent, Rendering, "Projector"),
    FBZZ_COMPONENT(SocketAttachmentComponent, Animation, "Socket Attachment"),
    FBZZ_COMPONENT(TransformConstraintComponent, Animation, "Transform Constraint"),
    FBZZ_COMPONENT(SplineComponent, Misc, "Spline"),
    FBZZ_COMPONENT(SplineFollowerComponent, Misc, "Spline Follower"),
    FBZZ_COMPONENT(VirtualCameraComponent, Rendering, "Virtual Camera"),
    FBZZ_COMPONENT(CameraFollowComponent, Rendering, "Camera Follow"),
    FBZZ_COMPONENT(CameraBlendComponent, Rendering, "Camera Blend"),
    FBZZ_COMPONENT(CameraShakeComponent, Rendering, "Camera Shake"),
    FBZZ_COMPONENT(UISlider, UI, "UI Slider"),
    FBZZ_COMPONENT(UIToggle, UI, "UI Toggle"),
    FBZZ_COMPONENT(UIScrollView, UI, "UI Scroll View"),
    FBZZ_COMPONENT(UIMask, UI, "UI Mask"),
    FBZZ_COMPONENT(UIInputField, UI, "UI Input Field"),
    FBZZ_COMPONENT(UIEventTrigger, UI, "UI Event Trigger"),
    FBZZ_COMPONENT(AudioReverbZoneComponent, Audio, "Audio Reverb Zone"),
    FBZZ_COMPONENT(AudioOcclusionComponent, Audio, "Audio Occlusion"),
    FBZZ_COMPONENT(AudioMixerSendComponent, Audio, "Audio Mixer Send"),
    /// @note ComponentRegistry の順序は Scene の SoA tuple 順と Script DLL ABI に影響するため、既存順を崩さず末尾へ追加する。
    FBZZ_COMPONENT(BehaviorTreeComponent, Navigation, "Behavior Tree"),
    FBZZ_CUSTOM_COMPONENT(CylinderColliderComponent, Physics, "Cylinder Collider"),
    FBZZ_CUSTOM_COMPONENT(SpringBoneComponent, Animation, "Spring Bone"),
    /// @note 寄せ先はスクリプトが毎回入れ替えるランタイム値で、保存する設定は enabled だけ。
    FBZZ_COMPONENT(MotionWarpComponent, Animation, "Motion Warp"),
    FBZZ_COMPONENT(WeatherComponent, Environment, "Weather"),
    /// @note 形の正本はスクリプトなので保存も手動追加もしない。mesh.Apply が付ける。
    FBZZ_INTERNAL_COMPONENT(ProceduralMeshComponent, "Procedural Mesh"),
    FBZZ_COMPONENT(SequencePlayerComponent, Misc, "Sequence Player"),
    /// @note .vfx プレハブの再生ヘッドと生存窓。
    /// @see Docs/design/vfx-prefab.md
    FBZZ_COMPONENT(VFXComponent, Effects, "VFX"),
    FBZZ_COMPONENT(VFXElement, Effects, "VFX Element"),
    FBZZ_COMPONENT(VFXLightEnvelope, Effects, "VFX Light Envelope"),
    FBZZ_COMPONENT(VFXTransformEnvelope, Effects, "VFX Transform Envelope"),
    FBZZ_COMPONENT(VFXMaterialEnvelope, Effects, "VFX Material Envelope"),
    FBZZ_COMPONENT(VFXDecalEnvelope, Effects, "VFX Decal Envelope"),
    FBZZ_COMPONENT(VFXAudioEnvelope, Effects, "VFX Audio Envelope"),
    /// @note 経路だけを持つ。見た目は同じ GameObject の TrailComponent が受け持つ。
    FBZZ_COMPONENT(VFXBeamComponent, Effects, "VFX Beam"),
    /// @note 配下の透明度と入力可否をまとめて変える。矩形は持たない。
    FBZZ_COMPONENT(UICanvasGroup, UI, "UI Canvas Group"),
    FBZZ_COMPONENT(UIContentSizeFitter, UI, "UI Content Size Fitter"),
    FBZZ_COMPONENT(UINavigation, UI, "UI Navigation"),
    FBZZ_COMPONENT(UIDragSource, UI, "UI Drag Source"),
    FBZZ_COMPONENT(UIDropTarget, UI, "UI Drop Target"),
    /// @note 実行状態 (質点・拘束・段階) は保存しないので保存は手動。設定は Reflect で足りる。
    FBZZ_AUTO_INSPECTOR_COMPONENT(RagdollComponent, Animation, "Ragdoll"),
    /// @note 雷・ビーム。保存は Reflect のまま、Inspector はプリセットと形のプレビューを持つ専用 UI。
    ComponentRegistration<VFXLineComponent, ComponentCategory::Effects, "VFXLineComponent", "VFX Line",
                          ComponentInspectorMode::Custom, ComponentSerializationMode::Automatic>,
    /// @note 剛体の関節 (ロープ・鎖・ヒンジ)。保存は Reflect のまま、Inspector は種別で出し分ける専用 UI (張れているか / 実効距離は Reflect では表せない)。
    ComponentRegistration<JointComponent, ComponentCategory::Physics, "JointComponent", "Joint",
                          ComponentInspectorMode::Custom, ComponentSerializationMode::Automatic>,
    FBZZ_COMPONENT(FiberComponent, Rendering, "Fiber (Fur / Grass)"),
    FBZZ_COMPONENT(ClothComponent, Physics, "Cloth"),
    FBZZ_COMPONENT(FiberInteractorComponent, Rendering, "Fiber Interactor"),
    /// @note 保存は Reflect のまま、Inspector は焼きの進捗と Bake ボタンを持つ専用 UI。
    ComponentRegistration<LightProbeVolumeComponent, ComponentCategory::Environment, "LightProbeVolumeComponent",
                          "Light Probe Volume", ComponentInspectorMode::Custom, ComponentSerializationMode::Automatic>
>;

template<typename Registry>
struct ComponentTypeList;

template<typename... Registrations>
struct ComponentTypeList<std::tuple<Registrations...>> {
    using Type = std::tuple<typename Registrations::Type...>;
};

/// @brief Scene ストレージと既存 API が参照する純粋な型リストは Registry から自動生成する。
using ComponentList = typename ComponentTypeList<ComponentRegistry>::Type;

template<typename Fn, typename... Registrations>
constexpr void ForEachRegisteredComponentImpl(Fn&& fn, std::tuple<Registrations...>*)
{
    (fn.template operator()<typename Registrations::Type, Registrations>(), ...);
}

/// @brief Editor / Serializer が同じ登録情報を型安全に反復する入口。
template<typename Fn>
constexpr void ForEachRegisteredComponent(Fn&& fn)
{
    ForEachRegisteredComponentImpl(std::forward<Fn>(fn), static_cast<ComponentRegistry*>(nullptr));
}

#undef FBZZ_INTERNAL_COMPONENT
#undef FBZZ_AUTO_INSPECTOR_COMPONENT
#undef FBZZ_CUSTOM_COMPONENT
#undef FBZZ_COMPONENT

} // namespace fbzz::scene
