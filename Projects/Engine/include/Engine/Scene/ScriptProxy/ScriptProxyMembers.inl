/// @file    ScriptProxyMembers.inl
/// @brief   Script のメンバーとして展開するプロキシの一覧。
/// @author  Hasegawa Jin
/// @date    2026-06-22
///
/// @note Script.hpp は取り込みの前後で展開マクロを定義・解除する。
/// @note Script* を受け取る型は FBZZ_PROXY_MEMBER で this を渡し、受け取らない型は FBZZ_PROXY_STANDALONE で既定の初期化を行う。
/// @note 追加時は宣言を AllScriptProxies.hpp から取り込み、領域に対応する Script*Proxies.cpp に実装を置く。
/// @note DLL ABI で既存プロキシのオフセットを維持するため、新しい項目は末尾へ追加する。

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
FBZZ_PROXY_MEMBER(ScriptVFXProxy,           vfx)
FBZZ_PROXY_MEMBER(ScriptGameplayProxy,      gameplay)
FBZZ_PROXY_MEMBER(ScriptSaveProxy,          save)
FBZZ_PROXY_MEMBER(ScriptEventProxy,         events)
FBZZ_PROXY_MEMBER(ScriptRandomProxy,        random)
FBZZ_PROXY_MEMBER(ScriptTweenProxy,         tween)
/// @note 環境設定は save と同じ API を持ち、別ファイルに保存する。
FBZZ_PROXY_MEMBER(ScriptConfigProxy,        config)
FBZZ_PROXY_MEMBER(ScriptDisplayProxy,       display)
FBZZ_PROXY_MEMBER(ScriptGraphicsProxy,      graphics)
FBZZ_PROXY_MEMBER(ScriptMotionWarpProxy,    motionWarp)
FBZZ_PROXY_MEMBER(ScriptSequenceProxy,      sequence)
FBZZ_PROXY_MEMBER(ScriptObjectMaskProxy,    objectMask)
FBZZ_PROXY_MEMBER(ScriptSpringBoneProxy,    springBone)
FBZZ_PROXY_MEMBER(ScriptRagdollProxy,       ragdoll)
/// @note 制約の実体は World が所有し、プロキシは JointComponent の宣言を扱う。
FBZZ_PROXY_MEMBER(ScriptJointProxy,         joint)
/// @note 非同期読み込み・先読みの要求は、スクリプトの破棄時に手放す。
FBZZ_PROXY_MEMBER(ScriptAssetProxy,         assets)
