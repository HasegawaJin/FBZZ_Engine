/// @file    AllScriptProxies.hpp
/// @brief   全 ScriptProxy ヘッダーのアンブレラインクルード。
/// @author  Hasegawa Jin
/// @date    2026-06-22
///
/// Script.hpp はここを 1 行インクルードするだけでよい。
/// 新しいプロキシを追加する際はこのファイルに include を 1 行追加すること。
#pragma once

#include <Engine/Scene/ScriptProxy/Scene/ScriptTransformProxy.hpp>
#include <Engine/Scene/ScriptProxy/Input/ScriptInputProxy.hpp>
#include <Engine/Scene/ScriptProxy/Input/ScriptCursorProxy.hpp>
#include <Engine/Scene/ScriptProxy/Runtime/ScriptApplicationProxy.hpp>
#include <Engine/Scene/ScriptProxy/Runtime/ScriptTimeProxy.hpp>
#include <Engine/Scene/ScriptProxy/Physics/ScriptPhysicsProxy.hpp>
#include <Engine/Scene/ScriptProxy/Physics/ScriptColliderProxy.hpp>
#include <Engine/Scene/ScriptProxy/Audio/ScriptAudioProxy.hpp>
#include <Engine/Scene/ScriptProxy/Rendering/ScriptLightProxy.hpp>
#include <Engine/Scene/ScriptProxy/Rendering/ScriptCameraProxy.hpp>
#include <Engine/Scene/ScriptProxy/Assets/ScriptMaterialProxy.hpp>
#include <Engine/Scene/ScriptProxy/Effects/ScriptParticleProxy.hpp>
#include <Engine/Scene/ScriptProxy/Effects/ScriptVFXProxy.hpp>
#include <Engine/Scene/ScriptProxy/Environment/ScriptFlowFieldProxy.hpp>
#include <Engine/Scene/ScriptProxy/Environment/ScriptCloudProxy.hpp>
#include <Engine/Scene/ScriptProxy/Environment/ScriptSunMoonProxy.hpp>
#include <Engine/Scene/ScriptProxy/Navigation/ScriptPatrolProxy.hpp>
#include <Engine/Scene/ScriptProxy/Environment/ScriptWindProxy.hpp>
#include <Engine/Scene/ScriptProxy/Effects/ScriptTrailProxy.hpp>
#include <Engine/Scene/ScriptProxy/Effects/ScriptMeshTrailProxy.hpp>
#include <Engine/Scene/ScriptProxy/Scene/ScriptSceneProxy.hpp>
#include <Engine/Scene/ScriptProxy/Animation/ScriptAnimatorProxy.hpp>
#include <Engine/Scene/ScriptProxy/Runtime/ScriptDebugProxy.hpp>
#include <Engine/Scene/ScriptProxy/Runtime/GizmoProxy.hpp>
#include <Engine/Scene/ScriptProxy/Rendering/ScriptPostProcessProxy.hpp>
#include <Engine/Scene/ScriptProxy/Runtime/ScriptMemoryProxy.hpp>
#include <Engine/Scene/ScriptProxy/UI/ScriptUIProxy.hpp>
#include <Engine/Scene/ScriptProxy/UI/ScriptUIAnimatorProxy.hpp>
#include <Engine/Scene/ScriptProxy/Navigation/ScriptNavigationProxy.hpp>
#include <Engine/Scene/ScriptProxy/Physics/ScriptCharacterProxy.hpp>
#include <Engine/Scene/ScriptProxy/Assets/ScriptMeshProxy.hpp>
#include <Engine/Scene/ScriptProxy/Animation/ScriptIKProxy.hpp>
#include <Engine/Scene/ScriptProxy/Environment/ScriptWaterProxy.hpp>
#include <Engine/Scene/ScriptProxy/Environment/ScriptTerrainProxy.hpp>
#include <Engine/Scene/ScriptProxy/Environment/ScriptEnvironmentProxy.hpp>
#include <Engine/Scene/ScriptProxy/Effects/ScriptDecalProxy.hpp>
#include <Engine/Scene/ScriptProxy/Environment/ScriptVolumeProxy.hpp>
#include <Engine/Scene/ScriptProxy/Rendering/ScriptReflectionProbeProxy.hpp>
#include <Engine/Scene/ScriptProxy/Runtime/ScriptLifetimeProxy.hpp>
#include <Engine/Scene/ScriptProxy/Runtime/ScriptGameplayProxy.hpp>
#include <Engine/Scene/ScriptProxy/Runtime/ScriptSaveProxy.hpp>
#include <Engine/Scene/ScriptProxy/Runtime/ScriptEventProxy.hpp>
#include <Engine/Scene/ScriptProxy/Runtime/ScriptRandomProxy.hpp>
#include <Engine/Scene/ScriptProxy/Runtime/ScriptTweenProxy.hpp>
#include <Engine/Scene/ScriptProxy/Rendering/ScriptDisplayProxy.hpp>
#include <Engine/Scene/ScriptProxy/Rendering/ScriptGraphicsProxy.hpp>
#include <Engine/Scene/ScriptProxy/Animation/ScriptMotionWarpProxy.hpp>
#include <Engine/Scene/ScriptProxy/Animation/ScriptSequenceProxy.hpp>
#include <Engine/Scene/ScriptProxy/Rendering/ScriptObjectMaskProxy.hpp>
#include <Engine/Scene/ScriptProxy/Animation/ScriptSpringBoneProxy.hpp>
#include <Engine/Scene/ScriptProxy/Physics/ScriptRagdollProxy.hpp>
#include <Engine/Scene/ScriptProxy/Physics/ScriptJointProxy.hpp>
#include <Engine/Scene/ScriptProxy/Assets/ScriptAssetProxy.hpp>
