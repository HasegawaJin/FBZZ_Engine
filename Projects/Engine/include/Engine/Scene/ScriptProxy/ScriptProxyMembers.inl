/// @file    ScriptProxyMembers.inl
/// @brief   ScriptProxyMembers.inl — Script クラスが持つプロキシメンバーの X-macro リスト。
/// @author  Hasegawa Jin
/// @date    2026-06-22
///
/// 使い方 (Script.hpp 内):
/// #define FBZZ_PROXY_MEMBER(Type, Name)     Type Name { this };
/// #define FBZZ_PROXY_STANDALONE(Type, Name) Type Name;
/// #include <Engine/Scene/ScriptProxy/ScriptProxyMembers.inl>
/// #undef FBZZ_PROXY_MEMBER
/// #undef FBZZ_PROXY_STANDALONE
///
/// 新しいプロキシを追加するには:
/// 1. ScriptProxy/XxxProxy.hpp を作成
/// 2. AllScriptProxies.hpp に #include を追加
/// 3. ここにエントリを 1 行追加 (コンストラクタが Script* を受け取る → MEMBER, そうでない → STANDALONE)
/// 4. ScriptProxies.cpp に実装を追加

/// FBZZ_PROXY_MEMBER     — Script* { this } で初期化するプロキシ (大多数)
/// FBZZ_PROXY_STANDALONE — デフォルト初期化するプロキシ (Script* を受け取らない設計のもの)
FBZZ_PROXY_MEMBER(ScriptTransformProxy,   transform)
FBZZ_PROXY_MEMBER(ScriptInputProxy,       input)
FBZZ_PROXY_MEMBER(ScriptCursorProxy,      cursor)
FBZZ_PROXY_MEMBER(ScriptApplicationProxy, app)
FBZZ_PROXY_MEMBER(ScriptTimeProxy,        time)
FBZZ_PROXY_MEMBER(ScriptPhysicsProxy,     physics)
FBZZ_PROXY_MEMBER(ScriptColliderProxy,    collider)
FBZZ_PROXY_MEMBER(ScriptAudioProxy,       audio)
FBZZ_PROXY_MEMBER(ScriptLightProxy,       light)
FBZZ_PROXY_MEMBER(ScriptCameraProxy,      camera)
FBZZ_PROXY_MEMBER(ScriptMaterialProxy,    material)
FBZZ_PROXY_MEMBER(ScriptParticleProxy,    particle)
FBZZ_PROXY_MEMBER(ScriptFlowFieldProxy, flowField)
FBZZ_PROXY_MEMBER(ScriptCloudProxy,       cloud)
FBZZ_PROXY_MEMBER(ScriptSunMoonProxy,     sunMoon)
FBZZ_PROXY_MEMBER(ScriptPatrolProxy,      patrol)
FBZZ_PROXY_MEMBER(ScriptWindProxy,        wind)
FBZZ_PROXY_MEMBER(ScriptTrailProxy,       trail)
FBZZ_PROXY_MEMBER(ScriptMeshTrailProxy,   meshTrail)
FBZZ_PROXY_MEMBER(ScriptSceneProxy,       scene)
FBZZ_PROXY_MEMBER(ScriptAnimatorProxy,    animator)
FBZZ_PROXY_MEMBER(ScriptDebugProxy,       debug)
FBZZ_PROXY_STANDALONE(GizmoProxy,         gizmo)
FBZZ_PROXY_MEMBER(ScriptPostProcessProxy, postprocess)
FBZZ_PROXY_MEMBER(ScriptMemoryProxy,      memory)
FBZZ_PROXY_MEMBER(ScriptUIProxy,          ui)
FBZZ_PROXY_MEMBER(ScriptUIAnimatorProxy,  uiAnimator)
FBZZ_PROXY_MEMBER(ScriptNavigationProxy,     navigation)
FBZZ_PROXY_MEMBER(ScriptCharacterProxy,     character)
FBZZ_PROXY_MEMBER(ScriptMeshProxy,          mesh)
FBZZ_PROXY_MEMBER(ScriptIKProxy,            ik)
FBZZ_PROXY_MEMBER(ScriptWaterProxy,         water)
FBZZ_PROXY_MEMBER(ScriptTerrainProxy,       terrain)
FBZZ_PROXY_MEMBER(ScriptEnvironmentProxy,   environment)
FBZZ_PROXY_MEMBER(ScriptDecalProxy,         decal)
FBZZ_PROXY_MEMBER(ScriptVolumeProxy,        volume)
FBZZ_PROXY_MEMBER(ScriptReflectionProbeProxy, reflectionProbe)
FBZZ_PROXY_MEMBER(ScriptLifetimeProxy,      lifetime)
/// @note Script の DLL ABI で既存 Proxy のオフセットを維持するため、新規 Proxy は末尾へ追加する。
FBZZ_PROXY_MEMBER(ScriptVFXProxy,           vfx)
FBZZ_PROXY_MEMBER(ScriptGameplayProxy,      gameplay)
FBZZ_PROXY_MEMBER(ScriptSaveProxy,          save)
FBZZ_PROXY_MEMBER(ScriptEventProxy,         events)
FBZZ_PROXY_MEMBER(ScriptRandomProxy,        random)
FBZZ_PROXY_MEMBER(ScriptTweenProxy,         tween)
/// 環境設定 (Option)。save と同じ API で、保存先だけが別ファイル。
FBZZ_PROXY_MEMBER(ScriptConfigProxy,        config)
FBZZ_PROXY_MEMBER(ScriptDisplayProxy,       display)
FBZZ_PROXY_MEMBER(ScriptGraphicsProxy,      graphics)
FBZZ_PROXY_MEMBER(ScriptMotionWarpProxy,    motionWarp)
FBZZ_PROXY_MEMBER(ScriptSequenceProxy,      sequence)
FBZZ_PROXY_MEMBER(ScriptObjectMaskProxy,    objectMask)
FBZZ_PROXY_MEMBER(ScriptSpringBoneProxy,    springBone)
FBZZ_PROXY_MEMBER(ScriptRagdollProxy,       ragdoll)
/// 剛体の関節。制約の実体は World が持つので、ここからは宣言 (JointComponent) を触る。
FBZZ_PROXY_MEMBER(ScriptJointProxy,         joint)
