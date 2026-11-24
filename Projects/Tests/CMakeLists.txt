# @file    CMakeLists.txt
# @brief   テストスイートの明示的なソース登録。
# @author  Hasegawa Jin
# @date    2026-05-15
# @note 自動テストと手動テストの構成。規約は Docs/conventions/test.md を参照。
#
# @note ソースは file(GLOB) で拾わない。新規ファイルが CMake 再実行まで拾われず、
# @note 「書いたのに一度も実行されていないテスト」が生まれるため。

add_subdirectory(TestKit)

add_test(NAME AgentLintCoroutineBoundaries
    COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/DevTools/AgentLintCoroutineTests.mjs")
set_tests_properties(AgentLintCoroutineBoundaries PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 30 LABELS "Build")

# @note 実際のゲームテンプレートをリンクし、内部 Engine ヘッダーを必要としない起動境界を検証する。
add_executable(FBZZStandaloneBoundarySmoke EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/Projects/GameHub/Templates/standard/Src/AppMain.cpp")
target_link_libraries(FBZZStandaloneBoundarySmoke PRIVATE FBZZEngine)
set_target_properties(FBZZStandaloneBoundarySmoke PROPERTIES FOLDER "Tests")

include("${CMAKE_SOURCE_DIR}/CMake/FBZZTests.cmake")

# @note Runtime packaging is a CMake contract, so validate its isolated fixture through the same CTest entry point.
add_test(NAME SharedSDKTemplates
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}"
        "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
        "-DTEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DTEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        -P "${CMAKE_SOURCE_DIR}/CMake/SDK/TestSharedSDKTemplates.cmake")
set_tests_properties(SharedSDKTemplates PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 120 LABELS "Build")

add_test(NAME ProjectShaderCompilation
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CMake/ProjectShaderCompilation.cmake")
set_tests_properties(ProjectShaderCompilation PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 60 LABELS "Build")

add_test(NAME EngineModuleBoundaries
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CMake/EngineModuleBoundaries.cmake")
set_tests_properties(EngineModuleBoundaries PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 30 LABELS "Build")
add_test(NAME SandboxProjectPreservation
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CMake/SandboxProjectPreservation.cmake")
set_tests_properties(SandboxProjectPreservation PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 30 LABELS "Build")

# @note --- Math -------------------------------------------------------------------
fbzz_add_test_suite(MathAuto
    SOURCES
        Math/Auto/Geometry/FrustumTests.cpp
        Math/Auto/Geometry/PlaneTests.cpp
        Math/Auto/Geometry/RayTests.cpp
        Math/Auto/Geometry/SegmentTests.cpp
        Math/Auto/LinearAlgebra/Matrix3Tests.cpp
        Math/Auto/LinearAlgebra/Matrix4Tests.cpp
        Math/Auto/LinearAlgebra/QuaternionTests.cpp
        Math/Auto/LinearAlgebra/Vector2Tests.cpp
        Math/Auto/LinearAlgebra/Vector3Tests.cpp
        Math/Auto/LinearAlgebra/Vector4Tests.cpp
        Math/Auto/Scalar/MathUtilsTests.cpp
        Math/Auto/Scalar/ScalarParityTests.cpp
)

# @note --- Core -------------------------------------------------------------------
fbzz_add_test_suite(CoreAuto
    SOURCES
        Core/Auto/Memory/LinearAllocatorTests.cpp
        Core/Auto/Memory/StackAllocatorTests.cpp
        Core/Auto/Memory/PoolAllocatorTests.cpp
        Core/Auto/Memory/AllocatorMoveTests.cpp
        Core/Auto/Memory/FrameAllocatorTests.cpp
        Core/Auto/Memory/MemoryTrackerTests.cpp
        Core/Auto/Memory/MemoryDebugTests.cpp
        Core/Auto/Memory/MemorySystemTests.cpp
        Core/Auto/Signal/SignalTests.cpp
        Core/Auto/Logger/LoggerTests.cpp
        Core/Auto/Profiler/ProfilerTests.cpp
        Core/Auto/Profiler/ProfileRecorderTests.cpp
        Core/Auto/Util/FileSystemTests.cpp
        Core/Auto/Util/TempDirTests.cpp
        Core/Auto/CrashHandler/CrashHandlerTests.cpp
        Core/Auto/CrashHandler/CrashHandlerInstallTests.cpp
        Core/Auto/CrashHandler/CrashLogTailTests.cpp
        Core/Auto/CrashHandler/CrashReportContentTests.cpp
        Core/Auto/CrashHandler/CrashHandlerFatalTests.cpp
        Core/Auto/DeveloperMode/DeveloperModeTests.cpp
        Core/Auto/Time/TimeConfigurationTests.cpp
        Core/Auto/Time/TimeLockstepTests.cpp
        Core/Auto/Time/TimeTickTests.cpp
        Core/Auto/Concurrency/TaskSystemSubmitTests.cpp
        Core/Auto/Concurrency/TaskSystemConcurrencyTests.cpp
        Core/Auto/Scheduler/SchedulerDeclarationTests.cpp
        Core/Auto/Scheduler/SchedulerExecutionTests.cpp
        Core/Auto/Scheduler/SchedulerBatchingTests.cpp
    LINKS
        FBZZTestKitEngine
        FBZZPhysics   # @note SchedulerExecutionTests がコンポーネント型として参照する
)

# @note --- Physics ----------------------------------------------------------------
# @note ログ末尾と開発者モードの起動引数解析は Engine の内部契約を参照する。
target_include_directories(FBZZTestsCoreAutoObj PRIVATE "${CMAKE_SOURCE_DIR}/Projects/Engine/src")

fbzz_add_test_suite(PhysicsAuto
    SOURCES
        Physics/Auto/Cloth/ClothAdvancedTests.cpp
        Physics/Auto/Cloth/ClothInterCollisionTests.cpp
        Physics/Auto/Cloth/ClothSolverTests.cpp
        Physics/Auto/Cloth/ClothWindTests.cpp
        Physics/Auto/Collision/BroadPhase/BroadPhaseIncrementalTests.cpp
        Physics/Auto/Collision/BroadPhase/BroadPhaseTests.cpp
        Physics/Auto/Collision/NarrowPhase/CCDTests.cpp
        Physics/Auto/Collision/NarrowPhase/CylinderGroundRegressionTests.cpp
        Physics/Auto/Collision/NarrowPhase/EPATests.cpp
        Physics/Auto/Collision/NarrowPhase/GJKTests.cpp
        Physics/Auto/Collision/NarrowPhase/NarrowPhaseContactTests.cpp
        Physics/Auto/Collision/Shapes/AABBTests.cpp
        Physics/Auto/Collision/Shapes/BVHTests.cpp
        Physics/Auto/Collision/Shapes/ColliderTests.cpp
        Physics/Auto/Collision/Shapes/ConvexHullTests.cpp
        Physics/Auto/Collision/Shapes/MeshColliderTests.cpp
        Physics/Auto/Constraints/ConstraintTests.cpp
        Physics/Auto/Debug/ColliderDebugGeometryTests.cpp
        Physics/Auto/Debug/ConstraintDebugGeometryTests.cpp
        Physics/Auto/Dynamics/PhysicsMaterialTests.cpp
        Physics/Auto/Dynamics/RigidBodySerializerTests.cpp
        Physics/Auto/Dynamics/RigidBodyTests.cpp
        Physics/Auto/Queries/WorldQueryIndexTests.cpp
        Physics/Auto/Queries/WorldQueryTests.cpp
        Physics/Auto/Queries/WorldRaycastShapeTests.cpp
        Physics/Auto/Solver/ContactCacheTests.cpp
        Physics/Auto/Solver/ContactIslandTests.cpp
        Physics/Auto/Solver/ContactResolveTests.cpp
        Physics/Auto/Solver/XPBD/XPBDBodyDriveTests.cpp
        Physics/Auto/Solver/XPBD/XPBDContactTests.cpp
        Physics/Auto/Solver/XPBD/XPBDGripTests.cpp
        Physics/Auto/Solver/XPBD/XPBDJointTests.cpp
        Physics/Auto/Solver/XPBD/XPBDJointVelocityTests.cpp
        Physics/Auto/Solver/XPBD/XPBDPlaneContactTests.cpp
        Physics/Auto/Solver/XPBD/XPBDPoseAnchorTests.cpp
        Physics/Auto/Solver/XPBD/XPBDSolverTests.cpp
        Physics/Auto/Volumes/ColliderVolumeTests.cpp
        Physics/Auto/Volumes/FlowVolumeTests.cpp
        Physics/Auto/Volumes/FluidVolumeTests.cpp
        Physics/Auto/World/CollisionMatrixTests.cpp
        Physics/Auto/World/LayerTests.cpp
        Physics/Auto/World/WorldCollisionEventTests.cpp
        Physics/Auto/World/WorldSceneSyncTests.cpp
        Physics/Auto/World/WorldStepTests.cpp
    LINKS
        FBZZTestKitPhysics
)

# @note --- Fluid ------------------------------------------------------------------
# @note 専用の TestKit を作らないか: Fluid は Math しか見ない。FBZZTestKit が既に FBZZMath を
# @note 公開でリンクしているので、足すのは FBZZFluid 1 つで足りる。
# @note .fluid の読み書き (FileSystem / TOML) や焼き (FluidBaker) に触るテストは
# @note Engine 依存が残るため EngineAuto に置いたまま。
fbzz_add_test_suite(FluidAuto
    SOURCES
        Fluid/Auto/Fields/GerstnerWavesTests.cpp
        Fluid/Auto/Operators/FluidOperatorEvalTests.cpp
        Fluid/Auto/Stepping/FluidGpuStepTests.cpp
    LINKS
        FBZZFluid
)

# @note --- Engine -----------------------------------------------------------------
fbzz_add_test_suite(EngineAuto
    SOURCES
        Engine/Auto/Scene/World/WorldCellTests.cpp
        Engine/Auto/Scene/World/WorldPcgTests.cpp
        Engine/Auto/Scene/World/WorldHlodTests.cpp
        Engine/Auto/Scene/World/WorldPartitionManifestTests.cpp
        Engine/Auto/Scene/World/WorldPartitionTests.cpp
        Engine/Auto/Scene/World/CellAssetStreamingTests.cpp
        Engine/Auto/Scene/Hierarchy/SceneForestTests.cpp
        Engine/Auto/Scene/Ragdoll/RagdollRigTests.cpp
        Engine/Auto/Scene/Ragdoll/RagdollDefinitionTests.cpp
        Engine/Auto/Scene/Ragdoll/RagdollControlsTests.cpp
        Engine/Auto/Scene/Ragdoll/StandingPoseTests.cpp
        Engine/Auto/Scene/Systems/Physics/RagdollSystemTests.cpp
    Engine/Auto/Scene/Systems/Physics/ClothSystemTests.cpp
    Engine/Auto/Scene/Systems/Physics/ClothContactSystemTests.cpp
    Engine/Auto/Scene/Systems/Physics/ClothFlowFieldTests.cpp
    Engine/Auto/Scene/Hierarchy/InactiveHierarchyTests.cpp
        Engine/Auto/Scene/Systems/Physics/ClothAssetTests.cpp
        Engine/Auto/Scene/Systems/Physics/ClothRenderBindingTests.cpp
        Engine/Auto/Asset/Registry/AssetDatabaseTests.cpp
        Engine/Auto/Asset/Streaming/AssetStreamingTests.cpp
        Engine/Auto/Scene/Serialization/SceneAssetReferencesTests.cpp
        Engine/Auto/Scene/Managed/ManagedScriptAssetPersistenceTests.cpp
        Engine/Auto/Asset/Registry/AssetPartialIoTests.cpp
        Engine/Auto/Asset/Texture/TextureMipmapTests.cpp
        Engine/Auto/Asset/Streaming/AssetStreamingIntegrationTests.cpp
        Engine/Auto/Core/Events/EventBusTests.cpp
        Engine/Auto/Core/Diagnostics/PixCaptureTests.cpp
        Engine/Auto/Util/Files/FileSystemPathTests.cpp
    Engine/Auto/Scene/Managed/ManagedWorldFamilyServicesTests.cpp
        Engine/Auto/Scene/UI/CanvasGeometryGuardTests.cpp
        Engine/Auto/Scene/Hierarchy/SceneCapacityTests.cpp
        Engine/Auto/Scene/Hierarchy/SceneBulkDestroyTests.cpp
    Engine/Auto/Rendering/Resources/SceneRendererIndexTests.cpp
        Engine/Auto/Scene/Systems/Navigation/NavMeshBakeConcurrencyTests.cpp
        Engine/Auto/Scene/ControlPoints/NavigationControlPointTests.cpp
        Engine/Auto/Scene/Systems/Navigation/NavigationWorkspaceTests.cpp
        Engine/Auto/Scene/Systems/Animation/IKTargetReferenceTests.cpp
        Engine/Auto/Scene/Systems/Animation/FootIKTests.cpp
        Engine/Auto/Scene/Serialization/AdditionalTargetPointPersistenceTests.cpp
        Engine/Auto/Scene/Managed/ManagedTargetPointRuntimeTests.cpp
        Engine/Auto/Scene/Systems/Animation/AdditionalTargetPointUsageTests.cpp
        Engine/Auto/Scene/Systems/Transform/TransformSystemTests.cpp
        Engine/Auto/Scene/ControlPoints/ControlPointResolverTests.cpp
        Engine/Auto/Scene/Hierarchy/SceneCapacityIntegrationTests.cpp
        Engine/Auto/Rendering/Shaders/ShaderCapabilitiesTests.cpp
        Engine/Auto/Scene/Prefab/PrefabIdentityTests.cpp
        Engine/Auto/Scene/Managed/ManagedScriptRuntimeTests.cpp
        Engine/Auto/Scene/Managed/ManagedInputBridgeTests.cpp
        Engine/Auto/Scene/Managed/ManagedRequiredReferenceTests.cpp
        Engine/Auto/Scene/Managed/ManagedMathParityTests.cpp
    Engine/Auto/Scene/Managed/GeneratedManagedBridgeTests.cpp
    Engine/Auto/Scene/Managed/ManagedObjectBatchTests.cpp
        Engine/Auto/Scene/Managed/ManagedTransformPoseTests.cpp
        Engine/Auto/Scene/Managed/ManagedTransformPositionTests.cpp
        Engine/Auto/Scene/Managed/ManagedObjectLifecycleTests.cpp
        Engine/Auto/Scene/Managed/ManagedObjectControlsTests.cpp
        Engine/Auto/Scene/Managed/ManagedObjectControlsRuntimeTests.cpp
        Engine/Auto/Scene/Managed/ManagedGamePackageTests.cpp
        Engine/Auto/Scene/Managed/ManagedCompletionTests.cpp
        Engine/Auto/Scene/Managed/ManagedPhysicsBridgeTests.cpp
        Engine/Auto/Scene/Managed/ManagedServicesBridgeTests.cpp
        Engine/Auto/Scene/Managed/ManagedServicesRuntimeTests.cpp
        Engine/Auto/Scene/Managed/ManagedPhysicsRuntimeTests.cpp
        Engine/Auto/Scene/Managed/ManagedCollisionTests.cpp
        Engine/Auto/Scene/Managed/ManagedEngineNotificationTests.cpp
        Engine/Auto/Scene/Managed/ManagedRenderTransfersTests.cpp
        Engine/Auto/Scene/Managed/ManagedComponentServicesTests.cpp
        Engine/Auto/Rendering/Geometry/PresentationGeometryTests.cpp
        Engine/Auto/Scene/Managed/ManagedRenderComponentsTests.cpp
        Engine/Auto/Scene/Managed/ManagedParticleServicesTests.cpp
        Engine/Auto/Scene/Managed/ManagedComponentServicesRuntimeTests.cpp
        Engine/Auto/Util/Math/ColorTests.cpp
        Engine/Auto/Util/Math/EasingTests.cpp
        Engine/Auto/Asset/References/GuidRefCodecTests.cpp
        Engine/Auto/Asset/References/SpriteReferenceTests.cpp
        Engine/Auto/Util/Text/StringUtilsTests.cpp
        Engine/Auto/Scene/Hierarchy/TransformTests.cpp
        Engine/Auto/Util/Text/Utf8Tests.cpp
        Engine/Auto/Util/Types/UuidTests.cpp
        Engine/Auto/Asset/Animation/AnimationSamplingTests.cpp
        Engine/Auto/Asset/Animation/BakedBoneAnimationTests.cpp
        Engine/Auto/Audio/AudioBusTests.cpp
        Engine/Auto/Audio/AudioStreamingTests.cpp
        Engine/Auto/Audio/AudioSourceTests.cpp
        Engine/Auto/AI/BlackboardTests.cpp
        Engine/Auto/AI/BehaviorTreeRuntimeTests.cpp
        Engine/Auto/AI/BehaviorTreeSystemTests.cpp
        Engine/Auto/Scene/Systems/Navigation/CrowdSimulationTests.cpp
        Engine/Auto/Scene/Managed/ManagedAiCrowdTransferTests.cpp
        Engine/Auto/Scene/Managed/ManagedAiPoseTransferTests.cpp
        Engine/Auto/Scene/Managed/ManagedAiCohortRuntimeTests.cpp
        Engine/Auto/Scene/Components/ComponentArrayTests.cpp
        Engine/Auto/Asset/Format/FzAssetFormatTests.cpp
        Engine/Auto/Input/InputActionMapTests.cpp
        Engine/Auto/Core/Platform/CursorControlPolicyTests.cpp
        Engine/Auto/Scene/UI/UIInputPolicyTests.cpp
        Engine/Auto/Rendering/Pipeline/RenderPipelineAssetTests.cpp
        Engine/Auto/Scene/Systems/Physics/JointComponentTests.cpp
        Engine/Auto/Asset/VFX/ParticleCodecTests.cpp
        Engine/Auto/Asset/VFX/ParticleCurveTests.cpp
        Engine/Auto/Asset/Fluid/VectorFieldTests.cpp
        Engine/Auto/Asset/Registry/AssetTypeBoundaryTests.cpp
    Engine/Auto/Project/SharedSDKAssetTests.cpp
        Engine/Auto/Rendering/Resources/TransientGpuStateTests.cpp
        Engine/Auto/Rendering/Resources/RuntimeMeshOwnershipTests.cpp
        Engine/Auto/Rendering/Resources/HostResourceBindingTests.cpp
        Engine/Auto/Scene/UI/UIInteractionTests.cpp
        Engine/Auto/Asset/Flipbook/FlipbookMotionVectorEncodingTests.cpp
        Engine/Auto/Asset/Flipbook/FlipbookMotionVectorsTests.cpp
        Engine/Auto/Asset/Flipbook/VolumeFlipbookAnalyticTests.cpp
        Engine/Auto/Asset/Flipbook/VolumeFlipbookFramingTests.cpp
        Engine/Auto/Scene/Components/ComponentReflectionParityTests.cpp
        Engine/Auto/Scene/Fields/FlowFieldFrameTests.cpp
        Engine/Auto/Scene/VFX/VFXEnvelopeTests.cpp
        Engine/Auto/Scene/VFX/VFXSubEmitterScopeTests.cpp
        Engine/Auto/Asset/VFX/ParticleMaterialPathTests.cpp
        Engine/Auto/Rendering/Fiber/FiberTests.cpp
        Engine/Auto/Rendering/Terrain/TerrainFiberStreamingTests.cpp
    Engine/Auto/Rendering/Fiber/FiberPlacementTests.cpp
    Engine/Auto/Asset/VFX/ParticleFlipbookTests.cpp
    Engine/Auto/Scene/VFX/ParticleLoopInspectionTests.cpp
        Engine/Auto/Asset/Fluid/FluidSolverTests.cpp
        Engine/Auto/Asset/Material/SixWayLightingTests.cpp
        Engine/Auto/Scene/VFX/ParticleLightSelectionTests.cpp
        Engine/Auto/Scene/VFX/ParticleGpuFallbackTests.cpp
        Engine/Auto/Asset/Flipbook/VolumeFlipbookFluidTests.cpp
        Engine/Auto/Asset/Flipbook/FlipbookMipsTests.cpp
        Engine/Auto/Scene/VFX/VFXLineGeometryTests.cpp
        Engine/Auto/Asset/Fluid/FluidLiquid3DTests.cpp
        Engine/Auto/Scene/Water/WaterfallFluidClipTests.cpp
        Engine/Auto/Asset/Fluid/FluidRecipeBakeTests.cpp
        Engine/Auto/Asset/Fluid/FluidBakeSafetyTests.cpp
        Engine/Auto/Asset/Fluid/FluidDeterminismTests.cpp
        Engine/Auto/Asset/Fluid/FluidSegmentShapeTests.cpp
        Engine/Auto/Asset/Fluid/FluidAmountEnvelopeTests.cpp
        Engine/Auto/Asset/Fluid/FluidGpuLiquidTests.cpp
        Engine/Auto/Rendering/Pipeline/GeometryRouteTests.cpp
        Engine/Auto/Rendering/Extraction/GeometryMaterialExtractionTests.cpp
        Engine/Auto/Rendering/Pipeline/GeometryPipelineTests.cpp
        Engine/Auto/Rendering/Pipeline/RenderVisibilityTests.cpp
        Engine/Auto/Rendering/Extraction/RenderSceneExtractorTests.cpp
        Engine/Auto/Rendering/Extraction/RenderEffectExtractionTests.cpp
        Engine/Auto/Rendering/Geometry/StaticMeshRenderReuseTests.cpp
        Engine/Auto/Rendering/Geometry/StaticMeshRenderMotionTests.cpp
        Engine/Auto/Rendering/Extraction/RenderLightExtractorTests.cpp
        Engine/Auto/Rendering/Geometry/InstanceBatchTests.cpp
        Engine/Auto/Rendering/Geometry/InstanceSetTests.cpp
        Engine/Auto/Rendering/Pipeline/RenderGraphTests.cpp
        Engine/Auto/Rendering/Pipeline/RenderPipelineTests.cpp
        Engine/Auto/Rendering/Pipeline/RenderPassCaptureTests.cpp
        Engine/Auto/Rendering/Shaders/ShaderReloadTests.cpp
        Engine/Auto/Asset/Material/MaterialReflectionTests.cpp
        Engine/Auto/Rendering/Pipeline/RenderGraphLifetimeTests.cpp
        Engine/Auto/Rendering/Pipeline/RenderSettingsTests.cpp
        Engine/Auto/Rendering/RayTracing/RayParticleCoverageTests.cpp
        Engine/Auto/Rendering/Extraction/RayLightExtractionTests.cpp
        Engine/Auto/Rendering/Pipeline/RenderModeSettingsTests.cpp
        Engine/Auto/Rendering/Diagnostics/DebugDrawBatchTests.cpp
        Engine/Auto/Scene/Serialization/SceneSerializerTests.cpp
        Engine/Auto/Rendering/Terrain/TerrainSplatTests.cpp
        Engine/Auto/Rendering/Terrain/TerrainPatchLodTests.cpp
        Engine/Auto/Rendering/Terrain/TerrainRenderResourceTests.cpp
        Engine/Auto/Asset/Fluid/VelocityFieldAtlasOwnershipTests.cpp
        Engine/Auto/Rendering/Resources/MeshTrailRenderResourceTests.cpp
        Engine/Auto/Scene/VFX/ParticlePlaybackTests.cpp
        Engine/Auto/Scene/VFX/ParticleGpuOwnershipTests.cpp
        Engine/Auto/Asset/Fluid/BakeGpuOwnershipTests.cpp
        Engine/Auto/Asset/Animation/AnimatorControllerDefinitionTests.cpp
        Engine/Auto/Reflection/ReflectionStandaloneTests.cpp
        Engine/Auto/Util/Files/SaveStoreReflectionTests.cpp
        Engine/Auto/Scene/Scripting/TweenCoroutineTests.cpp
        Engine/Auto/Scene/UI/UIAnimatorSchedulingTests.cpp
        Engine/Auto/Scene/VFX/TrailMultiKeyTests.cpp
        Engine/Auto/Scene/UI/UIRectTests.cpp
        Engine/Auto/Scene/Water/WaterWaveTests.cpp
        Engine/Auto/Scene/Water/WaterGridTests.cpp
        Engine/Auto/Scene/Water/WaterSurfaceQueryTests.cpp
    Engine/Auto/Scene/Water/WaterBuoyancyClockTests.cpp
    Engine/Auto/Scene/UI/UIThemeAssetTests.cpp
        Engine/Auto/Scene/Water/WaterRenderResourceTests.cpp
        Engine/Auto/Scene/Water/WaterTerrainInfoTests.cpp
        Engine/Auto/Scene/Water/WaterWavesAssetTests.cpp
        Engine/Auto/Scene/Water/WaterDepthFieldTests.cpp
        Engine/Auto/Scene/Water/WaterSplineTests.cpp
        Engine/Auto/Scene/Water/WaterZoneTests.cpp
        Engine/Auto/Scene/Water/WaterExclusionTests.cpp
        Engine/Auto/Scene/Water/WaterInteractionTests.cpp
        Engine/Auto/Scene/Water/WaterZonePersistenceTests.cpp
        Engine/Auto/Scene/Water/WaterSplineMeshTests.cpp
        Engine/Auto/Scene/Water/WaterOptimizationTests.cpp
        Engine/Auto/Scene/Water/WaterVortexTests.cpp
        Engine/Auto/Scene/Water/WaterNoiseBakeTests.cpp
    LINKS
        FBZZTestKitEngine
        FBZZPhysics
)

# @note Engine/src を足すか: 水面のさざ波タイルのような «GPU へ渡す前の CPU ベイク» は
# @note ヘッダーオンリーで src 側に閉じている。公開ヘッダーへ出すと、焼き方を知る必要のない
# @note 利用側にまで実装が露出する。テストだけが実装ディレクトリを覗く。
target_include_directories(FBZZTestsEngineAutoObj PRIVATE "${CMAKE_SOURCE_DIR}/Projects/Engine/src")
target_compile_definitions(FBZZTestsEngineAutoObj PRIVATE FBZZ_ENGINE_SHADER_ROOT="${CMAKE_SOURCE_DIR}/Assets/Shaders")

# @note 実 DX12 資源を使う GPU skinning と HDR 抽出だけを明示実行へ分ける。CPU 抽出・設定は EngineAuto に残す。
fbzz_add_test_suite(RayTracingEngine OPT_IN
    SOURCES
        Engine/Auto/Rendering/Geometry/BakedBoneReplayTests.cpp
        Engine/Auto/Rendering/RayTracing/RaySkinningTests.cpp
        Engine/Auto/Rendering/Extraction/RayEnvironmentExtractionTests.cpp
        Engine/Auto/Rendering/Geometry/RegularMeshRenderPerformanceTests.cpp
    LINKS
        FBZZTestKitEngine
        FBZZPhysics
)
target_include_directories(FBZZTestsRayTracingEngineObj PRIVATE "${CMAKE_SOURCE_DIR}/Projects/Engine/src")
target_compile_definitions(FBZZTestsRayTracingEngineObj PRIVATE FBZZ_ENGINE_SHADER_ROOT="${CMAKE_SOURCE_DIR}/Assets/Shaders")

# @note --- Editor -----------------------------------------------------------------
# @note Editor をリンクするか: import 判定まわりは «起動のたびに全アセットを焼き直す»
# @note という形でしか表に出ず、しかも直ったかどうかがローカルの mtime に左右される。
# @note 再現条件をテストへ固定できないと、同じ回帰を何度も踏む。
# @note FBZZEditor は STATIC なので、テストが参照するオブジェクトだけが引き込まれる。
fbzz_add_test_suite(EditorAuto
  SOURCES
    Editor/Auto/Tools/World/WorldPartitionOperatorTests.cpp
    Editor/Auto/Tools/World/WorldPcgAuthoringTests.cpp
    Editor/Auto/Tools/World/WorldCellBakingTests.cpp
    Editor/Auto/Panels/Animation/AnimatorMaskAuditTests.cpp
    Editor/Auto/Project/AppIconWriterTests.cpp
    Editor/Auto/Asset/Browser/AssetBrowserExplorerTests.cpp
    Editor/Auto/Diagnostics/AnalysisProfilerGpuTests.cpp
    Editor/Auto/Diagnostics/ProfilerDisplayRowsTests.cpp
    Editor/Auto/Asset/Browser/AssetReferenceScanTests.cpp
    Editor/Auto/Prefab/PrefabStructureMergeTests.cpp
    Editor/Auto/Play/PlayChangesAdoptionTests.cpp
    Editor/Auto/Panels/Animation/SequenceCurveTests.cpp
    Editor/Auto/Asset/Browser/AssetBrowserThumbnailTests.cpp
    Editor/Auto/Asset/Storage/AssetDirtyRegistryTests.cpp
    Editor/Auto/Op/HotkeyManagerTests.cpp
    Editor/Auto/BuildPipeline/BuildOutputTests.cpp
    Editor/Auto/Inspector/Widgets/PropertyControlsTests.cpp
    Editor/Auto/Panels/EditorPanelToolbarTests.cpp
    Editor/Auto/Inspector/Editing/AssetInspectorPersistenceTests.cpp
    Editor/Auto/Asset/Storage/MetaFilePersistenceTests.cpp
    Editor/Auto/Asset/Storage/AssetPathTests.cpp
    Editor/Auto/Asset/Browser/AssetPickerTests.cpp
    Editor/Auto/Tools/Cloth/ClothPinBrushTests.cpp
    Editor/Auto/Tools/Fiber/FiberPaintToolTests.cpp
    Editor/Auto/Tools/ControlPoints/ControlPointEditingTests.cpp
    Editor/Auto/Panels/Viewport/ControlPointViewportTests.cpp
    Editor/Auto/Inspector/Editing/ControlPointInspectorTests.cpp
    Editor/Auto/Tools/ControlPoints/ControlPointOperatorTests.cpp
        Editor/Auto/Tools/ControlPoints/AdditionalControlPointOperatorTests.cpp
        Editor/Auto/Tools/ControlPoints/TargetPointReferenceOperatorTests.cpp
    Editor/Auto/BuildPipeline/BuildSettingsOutputPathTests.cpp
    Editor/Auto/BuildPipeline/PackageBuildTests.cpp
    Editor/Auto/Play/StandalonePlaySessionTests.cpp
    Editor/Auto/Diagnostics/SpatialDebugVisualizationTests.cpp
    Editor/Auto/Ai/Bus/BusContractParityTests.cpp
    Editor/Auto/Ai/Bus/EditorBusDispatcherTests.cpp
    Editor/Auto/Ai/Bus/EditorBusProtocolTests.cpp
    Editor/Auto/Ai/Bus/NamedPipeServerTests.cpp
    Editor/Auto/Project/EditorProjectFileTests.cpp
    Editor/Auto/Project/EditorSettingsTests.cpp
    Editor/Auto/Op/EditorMenuCatalogTests.cpp
    Editor/Auto/Op/ObjectPresetMenuTests.cpp
    Editor/Auto/App/EditorIdleWorkTests.cpp
    Editor/Auto/Asset/Browser/EditorQueryCacheTests.cpp
    Editor/Auto/Inspector/Editing/InspectorValueSnapshotTests.cpp
    Editor/Auto/Panels/Animation/AnimationTrackTableTests.cpp
    Editor/Auto/GraphEditor/BehaviorTreeChildCountsTests.cpp
    Editor/Auto/Panels/Preview/MaterialPreviewCacheTests.cpp
    Editor/Auto/Panels/Preview/MaterialPreviewToolbarTests.cpp
    Editor/Auto/Fluid/Bake/FluidAssetWritersTests.cpp
    Editor/Auto/Fluid/Authoring/FluidDocumentTests.cpp
    Editor/Auto/Fluid/Authoring/FluidEffectTemplateTests.cpp
    Editor/Auto/Fluid/Authoring/FluidPartClipboardTests.cpp
    Editor/Auto/Fluid/Authoring/FluidPartOverlayTests.cpp
    Editor/Auto/Fluid/Preview/FluidPreviewCacheTests.cpp
    Editor/Auto/GraphEditor/GraphLayoutTests.cpp
    Editor/Auto/Panels/Preview/ImageCompareTests.cpp
    Editor/Auto/Fluid/Preview/FluidJetFlamePlaybackTests.cpp
    Editor/Auto/Import/Settings/ImportCacheStoreTests.cpp
    Editor/Auto/Import/ImportFingerprintTests.cpp
    Editor/Auto/Import/ModelImportOperatorTests.cpp
    Editor/Auto/Project/ProjectSettingsOperatorTests.cpp
    Editor/Auto/Op/InputBindingOperatorTests.cpp
    Editor/Auto/Import/TextureImportOperatorTests.cpp
    Editor/Auto/Project/EditorSettingsDocumentTests.cpp
    Editor/Auto/Project/EditorViewSettingsTests.cpp
    Editor/Auto/Panels/Viewport/ViewportCaptureSizingTests.cpp
    Editor/Auto/Panels/Animation/SequenceOperatorTests.cpp
    Editor/Auto/Panels/Preview/IblOperatorTests.cpp
    Editor/Auto/Asset/Authoring/PlacementOperatorTests.cpp
    Editor/Auto/Fluid/Bake/VolumeFlipbookOperatorTests.cpp
    Editor/Auto/Op/DirectPanelOperatorTests.cpp
    Editor/Auto/Asset/Storage/AssetFileOperatorTests.cpp
    Editor/Auto/Diagnostics/RenderPassOperatorTests.cpp
    Editor/Auto/Playtest/InputRecorderTests.cpp
    Editor/Auto/Ai/Serialization/JsonTests.cpp
    Editor/Auto/Panels/Preview/MaterialPreviewFiberTests.cpp
    Editor/Auto/Playtest/PlaytestJsonTests.cpp
    Editor/Auto/Playtest/PlaytestRunnerTests.cpp
        Editor/Auto/Script/Managed/ManagedScriptInspectionTests.cpp
        Editor/Auto/Script/Managed/ManagedScriptBuildTests.cpp
    Editor/Auto/Script/Managed/ManagedSdkBindingTests.cpp
        Editor/Auto/Script/Managed/ManagedCompletionTests.cpp
    Editor/Auto/Scene/Editing/SceneFindPaginationTests.cpp
        Editor/Auto/Script/Managed/ManagedDataAssetAuthoringTests.cpp
    Editor/Auto/Diagnostics/PixOperatorTests.cpp
    Editor/Auto/Diagnostics/ProfilerBenchmarkTests.cpp
    Editor/Auto/Prefab/PrefabOverrideTests.cpp
        Editor/Auto/Prefab/PrefabReferenceOverrideTests.cpp
        Editor/Auto/Scene/Editing/SceneEditCapacityTests.cpp
    Editor/Auto/Scene/Editing/SceneEditReferenceCloneTests.cpp
    Editor/Auto/Prefab/PrefabSaveTests.cpp
    Editor/Auto/Scene/Persistence/SceneDirtyTrackerTests.cpp
    Editor/Auto/Script/Managed/ManagedScriptMetadataTests.cpp
    Editor/Auto/Script/Authoring/SourceOpenTests.cpp
    Editor/Auto/Asset/Authoring/SpriteFieldFilterTests.cpp
    Editor/Auto/Asset/Authoring/SpriteSlicerTests.cpp
    Editor/Auto/Tools/Terrain/TerrainBrushTests.cpp
        Editor/Auto/Tools/Terrain/TerrainAuthoringTests.cpp
        Editor/Auto/Tools/Terrain/TerrainGrassOperatorTests.cpp
        Editor/Auto/Tools/Terrain/TerrainScatterTests.cpp
        Editor/Auto/Tools/Terrain/TerrainPaintAssetTests.cpp
        Editor/Auto/Tools/Terrain/TerrainToolAuthoringTests.cpp
    Editor/Auto/Scene/Editing/UndoStackTests.cpp
    Editor/Auto/Inspector/Editing/WaterInspectorRuntimeTests.cpp
  LINKS
    FBZZTestKitEditor
)

# @note Editor の一部の道具はヘッダごと src/ に置かれている (公開 API ではないため)。
# @note テストからだけ覗くか: TerrainBrush の減衰カーブや座標変換は «なんとなく変» という
# @note 形でしか壊れず、目視では追えない。公開範囲を広げる代わりに、テスト側だけが
# @note 内部ヘッダを見られるようにする。
target_include_directories(FBZZTestsEditorAutoObj
    PRIVATE "${CMAKE_SOURCE_DIR}/Projects/Editor/src"
    PRIVATE "${CMAKE_SOURCE_DIR}/Projects/Engine/src/Profiler")

# @note ソースの場所を渡すか: BusContractParityTests が MCP 側の契約ファイル (TypeScript) を読んで
# @note Editor のハンドラー表と突き合わせる。実行ディレクトリからの相対では CI とローカルで食い違う。
target_compile_definitions(FBZZTestsEditorAutoObj
    PRIVATE FBZZ_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        FBZZ_MANAGED_HOST_TEST_DIRECTORY="${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/Managed"
        FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY="${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/ManagedFixtures/V1/Fbzz.ManagedFixtures.dll")

# @note --- ビジュアル検証ベンチ ---------------------------------------------------
# @note CTest には載せない。合否をコードで判定しないため。
# @note テスト実行機能を持たせないか: 自動テストの正は CTest / VS のテストエクスプローラー。
# @note ここでも走らせられるようにすると «どちらの結果が正しいのか» が分からなくなる。
add_executable(FBZZTestBench
    Bench/main.cpp
    Bench/AnomalyLog.cpp
    Bench/BenchApp.cpp
    Bench/Measure.cpp
    Bench/MicroCases.cpp
    Bench/WaterControlPointCases.cpp
    Bench/Viewport2D.cpp
    Bench/Scenes/XPBDConvergenceScene.cpp
    Bench/Scenes/XPBDJointChainScene.cpp
    Bench/Scenes/ContactScene.cpp
    Bench/Scenes/BVHQueryScene.cpp
    Bench/Scenes/CCDScene.cpp
    Bench/Scenes/ConvexHullScene.cpp
    Bench/Scenes/RagdollScene.cpp
    # @note コンソール制御だけ TestKit と共有する。
    # @note リンクせずソースを直接足すか: FBZZTestKit は gtest / gmock を引き連れてくる。
    # @note ベンチはテストフレームワークに依存させたくないので、この 1 ファイルだけ借りる。
    TestKit/src/Output/Console.cpp
)
target_include_directories(FBZZTestBench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/TestKit/include")
target_link_libraries(FBZZTestBench PRIVATE FBZZEngine FBZZPhysics FBZZMath ImGui)
# @note --measure の JSON に測定条件として書く。exe は自分がどの構成で作られたかを知る手段が他に無い。
target_compile_definitions(FBZZTestBench PRIVATE FBZZ_BENCH_CONFIG="$<CONFIG>")
set_target_properties(FBZZTestBench PROPERTIES FOLDER "Tests")
fbzz_set_test_output_dir(FBZZTestBench)
fbzz_apply_app_manifest(FBZZTestBench)
fbzz_stage_test_runtime(FBZZTestBench)

# @note BroadPhaseのCPU時間だけを測る。正しさの回帰はPhysicsAutoが担う。
add_executable(FBZZPhysicsBroadPhaseBench Bench/PhysicsBroadPhaseMeasure.cpp)
target_link_libraries(FBZZPhysicsBroadPhaseBench PRIVATE FBZZPhysics FBZZMath)
set_target_properties(FBZZPhysicsBroadPhaseBench PROPERTIES FOLDER "Tests")
fbzz_set_test_output_dir(FBZZPhysicsBroadPhaseBench)
fbzz_stage_test_runtime(FBZZPhysicsBroadPhaseBench)

add_executable(FBZZRenderSynchronizationBench Bench/RenderSynchronizationMeasure.cpp)
target_link_libraries(FBZZRenderSynchronizationBench PRIVATE FBZZGraphics ole32 user32)
set_target_properties(FBZZRenderSynchronizationBench PROPERTIES FOLDER "Tests")
fbzz_set_test_output_dir(FBZZRenderSynchronizationBench)
fbzz_stage_test_runtime(FBZZRenderSynchronizationBench)

# @note Fixed CPU-only Before/After measurements; no window, GPU submission or implicit benchmark in CTest.
add_executable(FBZZAlgorithmOptimizationBench Bench/AlgorithmOptimizationMeasure.cpp)
target_include_directories(FBZZAlgorithmOptimizationBench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/TestKit/include")
target_link_libraries(FBZZAlgorithmOptimizationBench PRIVATE FBZZEngine psapi d3d12 dxgi)
set_target_properties(FBZZAlgorithmOptimizationBench PROPERTIES FOLDER "Tests")
fbzz_set_test_output_dir(FBZZAlgorithmOptimizationBench)
fbzz_stage_test_runtime(FBZZAlgorithmOptimizationBench)

# @note Explicit CPU-only target; production APIs are identical in baseline and candidate builds, with no CTest registration.
add_executable(FBZZScenePreparationBench EXCLUDE_FROM_ALL Bench/ScenePreparationMeasure.cpp)
target_link_libraries(FBZZScenePreparationBench PRIVATE FBZZEditor FBZZEngine)
target_compile_definitions(FBZZScenePreparationBench PRIVATE FBZZ_SCENE_PREPARATION_CONFIG="$<CONFIG>")
set_target_properties(FBZZScenePreparationBench PROPERTIES FOLDER "Tests")
fbzz_set_test_output_dir(FBZZScenePreparationBench)
fbzz_stage_test_runtime(FBZZScenePreparationBench)

# @note Explicit paired CPU predicate benchmark; both production implementations run in one binary without CTest registration.
add_executable(FBZZMaterialVisibilityBench EXCLUDE_FROM_ALL Bench/MaterialVisibilityMeasure.cpp)
target_link_libraries(FBZZMaterialVisibilityBench PRIVATE FBZZEngine)
target_compile_definitions(FBZZMaterialVisibilityBench PRIVATE FBZZ_MATERIAL_VISIBILITY_CONFIG="$<CONFIG>")
set_target_properties(FBZZMaterialVisibilityBench PROPERTIES FOLDER "Tests")
fbzz_set_test_output_dir(FBZZMaterialVisibilityBench)
fbzz_stage_test_runtime(FBZZMaterialVisibilityBench)

add_executable(FBZZStaticMeshReuseBench Bench/StaticMeshReuseMeasure.cpp)
target_link_libraries(FBZZStaticMeshReuseBench PRIVATE FBZZEngine)
set_target_properties(FBZZStaticMeshReuseBench PROPERTIES FOLDER "Tests")
fbzz_set_test_output_dir(FBZZStaticMeshReuseBench)
fbzz_stage_test_runtime(FBZZStaticMeshReuseBench)

# @note --- 手動テスト -------------------------------------------------------------
# @note Window / Cursor は OS の状態を書き換えるため CTest に登録しない。
# @note 開発者が Visual Studio から意図的に起動したときだけ実行する。
fbzz_add_test_suite(CoreManual MANUAL
    SOURCES
        Core/ManualTest/WindowManualTests.cpp
        Core/ManualTest/CursorManualTests.cpp
    LINKS
        FBZZTestKitEngine
)

# @note 描画計画・履歴・キャッシュは CPU の値や fake backend で検証し、通常 CTest / CI に残す。
fbzz_add_test_suite(GraphicsAuto
    SOURCES
        Graphics/Auto/Diagnostics/GpuProfilerTests.cpp
        Graphics/Auto/Diagnostics/PixEventStackTests.cpp
        Graphics/Auto/Diagnostics/RecordedGpuProfilerViewTests.cpp
        Graphics/Auto/Geometry/MeshletGeometryBuilderTests.cpp
        Graphics/Auto/Geometry/OcclusionCullerTests.cpp
        Graphics/Auto/Geometry/RenderMeshSpatialIndexTests.cpp
        Graphics/Auto/Geometry/RenderSceneSpatialQueriesTests.cpp
        Graphics/Auto/Geometry/SurfaceFarMeshTests.cpp
        Graphics/Auto/Geometry/SurfacePatchLodTests.cpp
        Graphics/Auto/Geometry/SurfacePatchMorphTests.cpp
        Graphics/Auto/Lighting/ProbeCaptureBudgetTests.cpp
        Graphics/Auto/Pipeline/ResolvedRenderPlanTests.cpp
        Graphics/Auto/RayTracing/RayEnvironmentTests.cpp
        Graphics/Auto/RayTracing/RayGameContractTests.cpp
        Graphics/Auto/RayTracing/RayPathHistoryTests.cpp
        Graphics/Auto/RayTracing/RayPathSceneTests.cpp
        Graphics/Auto/RayTracing/RayReflectionMediumCacheTests.cpp
        Graphics/Auto/RayTracing/RayRenderPlanCpuTests.cpp
        Graphics/Auto/RayTracing/RaySceneBuilderTests.cpp
        Graphics/Auto/Resources/DX12UploadArenaSizingTests.cpp
        Graphics/Auto/Resources/DeferredDirtySetTests.cpp
        Graphics/Auto/Standalone/GraphicsStandaloneCpuTests.cpp
        Graphics/Auto/Threading/RenderThreadTests.cpp
        Graphics/Auto/Water/WaterFarRenderPassTests.cpp
    LINKS FBZZGraphics ImGui
)

target_include_directories(FBZZTestsGraphicsAutoObj PRIVATE "${CMAKE_SOURCE_DIR}/Projects/Graphics/src")

# @note 実 DX12 デバイス・描画・GPU 読戻しを要するテストは通常 CTest / CI から外す。
fbzz_add_test_suite(GraphicsStandalone OPT_IN
    SOURCES
        Editor/Auto/Panels/Preview/MaterialPreviewShaderTests.cpp
        Graphics/Auto/Geometry/FiberRenderPerfTests.cpp
        Graphics/Auto/Geometry/GpuGeometrySelectionTests.cpp
        Graphics/Auto/Geometry/InstancedMeshTests.cpp
        Graphics/Auto/Geometry/MeshletGeometryShaderTests.cpp
        Graphics/Auto/Geometry/TerrainInstanceShaderTests.cpp
        Graphics/Auto/Lighting/DeferredEmissionTests.cpp
        Graphics/Auto/Lighting/ManyLightShaderTests.cpp
        Graphics/Auto/Lighting/ReflectionProbeCaptureTests.cpp
        Graphics/Auto/Pipeline/GraphicsQualityLifetimeTests.cpp
        Graphics/Auto/Pipeline/GraphicsQualityShaderTests.cpp
        Graphics/Auto/Resources/BufferReadTests.cpp
        Graphics/Auto/Standalone/GraphicsStandaloneTests.cpp
        Graphics/Auto/Threading/ThreadedRenderingTests.cpp
        Graphics/Auto/Water/UnderwaterShaderTests.cpp
        Graphics/Auto/Water/WaterMediumShaderTests.cpp
        Graphics/Auto/Water/WaterRenderSyncTests.cpp
        Graphics/Auto/Water/WaterShaderTests.cpp
        Graphics/Fixtures/GraphicsQualityFixture.cpp
    LINKS
        FBZZGraphics
        FBZZTestKitEngine
        FBZZTestKitEditor
)

# @note Paint Library の GPU 検証は本番の非公開 Editor UI を描く。
target_include_directories(FBZZTestsGraphicsStandaloneObj PRIVATE "${CMAKE_SOURCE_DIR}/Projects/Editor/src")

# @note DXR / Hybrid / PathTracing は合否を自動判定し、選択したときだけビルド・実行する。
fbzz_add_test_suite(RayTracing OPT_IN
    SOURCES
        Graphics/Auto/RayTracing/HybridHistoryTests.cpp
        Graphics/Auto/RayTracing/RayGameTests.cpp
        Graphics/Auto/RayTracing/RayHybridDiffuseGITests.cpp
        Graphics/Auto/RayTracing/RayHybridGlassTraversalTests.cpp
        Graphics/Auto/RayTracing/RayPathTracingTests.cpp
        Graphics/Auto/RayTracing/RayReflectionReconstructionTests.cpp
        Graphics/Auto/RayTracing/RayReflectionTests.cpp
        Graphics/Auto/RayTracing/RayRenderPlanTests.cpp
        Graphics/Auto/RayTracing/RayTextureTests.cpp
        Graphics/Auto/RayTracing/RayTracingTests.cpp
    LINKS FBZZGraphics
)
configure_file(Graphics/Fixtures/StandaloneTriangle.hlsl StandaloneTriangle.hlsl COPYONLY)
configure_file(Graphics/Fixtures/GpuGeometrySelectionProbe.hlsl GpuGeometrySelectionProbe.hlsl COPYONLY)
configure_file(Graphics/Fixtures/GpuGeometrySelectionProbe.ms.hlsl GpuGeometrySelectionProbe.ms.hlsl COPYONLY)
foreach(graphicsSuite GraphicsAuto GraphicsStandalone RayTracing)
    target_compile_definitions(FBZZTests${graphicsSuite}Obj PRIVATE
        FBZZ_GRAPHICS_SMOKE_SHADER="${CMAKE_CURRENT_BINARY_DIR}/StandaloneTriangle.hlsl"
        FBZZ_GPU_GEOMETRY_SELECTION_VS_SHADER="${CMAKE_CURRENT_BINARY_DIR}/GpuGeometrySelectionProbe.hlsl"
        FBZZ_GPU_GEOMETRY_SELECTION_MS_SHADER="${CMAKE_CURRENT_BINARY_DIR}/GpuGeometrySelectionProbe.ms.hlsl"
        FBZZ_GPU_HI_Z_PROBE_SHADER="${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Fixtures/GpuHiZProbe.hlsl"
        FBZZ_GRAPHICS_SHADER_ROOT="${CMAKE_SOURCE_DIR}/Assets/Shaders"
        FBZZ_RAY_INTERSECTION_SHADER="${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Shaders/RayIntersection.cs.hlsl")
endforeach()
# @note DrawsAndReadsBackWithoutEngine は実行時に .hlsl をコンパイルするので dxcompiler.dll が exe の隣に要る。
# @warning DXC が見つからない構成では «シェーダーが読めない» だけの失敗になる。原因は configure 時の dxc.exe 探索 (Projects/Graphics/CMakeLists.txt)。
if(FBZZ_DXC_RUNTIME_DIRECTORY)
    foreach(graphicsSuite GraphicsStandalone RayTracing RayTracingEngine)
        foreach(FBZZ_DXC_RUNTIME_DLL dxcompiler.dll dxil.dll)
            if(EXISTS "${FBZZ_DXC_RUNTIME_DIRECTORY}/${FBZZ_DXC_RUNTIME_DLL}")
                add_custom_command(TARGET FBZZTests${graphicsSuite} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${FBZZ_DXC_RUNTIME_DIRECTORY}/${FBZZ_DXC_RUNTIME_DLL}"
                        "$<TARGET_FILE_DIR:FBZZTests${graphicsSuite}>")
            endif()
        endforeach()
    endforeach()
endif()

# @note gtest_discover_tests が登録した後に、実測で 30 秒を超える統合ケースだけの上限を適用する。
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/CMake/IntegrationTimeouts.cmake")
