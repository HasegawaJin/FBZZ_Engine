/// @file    ComponentRegistry.hpp
/// @brief   コンポーネント型とEditor／永続化メタデータの単一登録表。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Engine/Scene/ComponentMetadata.hpp>

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
#include "Components/PostProcessVolumeComponent.hpp"
#include "Components/IKSolverComponent.hpp"
#include "Components/SpringBoneComponent.hpp"
#include "Components/RagdollComponent.hpp"
#include "Components/MotionWarpComponent.hpp"
#include "Components/CharacterControllerComponent.hpp"
#include "Components/TerrainComponent.hpp"
#include "Components/TerrainGridComponent.hpp"
#include "Components/WaterComponent.hpp"
#include "Components/WaterZoneComponent.hpp"
#include "Components/WaterExclusionComponent.hpp"
#include "Components/WorldPartitionComponent.hpp"
#include "Components/WorldPcgAuthoringComponent.hpp"
#include "Components/TargetPointComponent.hpp"
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
#include "Components/InstancedMeshComponent.hpp"
#include "Components/FiberInteractorComponent.hpp"
#include "Components/LightProbeVolumeComponent.hpp"
#include "ScriptComponent.hpp"

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace fbzz::scene {

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

/// @brief 登録順は Scene の SoA tuple の並びに影響するため、既存順を維持する。
/// @note 新しい単純型は FBZZ_COMPONENT を 1 行追加すれば、標準の追加・Inspector・保存経路へ入る。
using ComponentRegistry = std::tuple<
#define FBZZ_COMPONENT_FIRST(Kind, Type, ...) __VA_ARGS__
#define FBZZ_COMPONENT_ENTRY(Kind, Type, ...) , __VA_ARGS__
#include "ComponentRegistry.inl"
#undef FBZZ_COMPONENT_ENTRY
#undef FBZZ_COMPONENT_FIRST
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

} /// @note namespace fbzz::scene
