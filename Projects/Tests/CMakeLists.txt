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
        Math/Auto/Vector2Tests.cpp
        Math/Auto/Vector3Tests.cpp
        Math/Auto/Vector4Tests.cpp
        Math/Auto/QuaternionTests.cpp
        Math/Auto/Matrix3Tests.cpp
        Math/Auto/Matrix4Tests.cpp
        Math/Auto/MathUtilsTests.cpp
        Math/Auto/SegmentTests.cpp
        Math/Auto/PlaneTests.cpp
        Math/Auto/RayTests.cpp
        Math/Auto/FrustumTests.cpp
        Math/Auto/ScalarParityTests.cpp
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
        Core/Auto/DeferredDirtySetTests.cpp
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
        Physics/Auto/AABBTests.cpp
        Physics/Auto/ColliderTests.cpp
        Physics/Auto/GJKTests.cpp
        Physics/Auto/EPATests.cpp
        Physics/Auto/BVHTests.cpp
        Physics/Auto/XPBDSolverTests.cpp
        Physics/Auto/ClothSolverTests.cpp
    Physics/Auto/ClothAdvancedTests.cpp
    Physics/Auto/ClothInterCollisionTests.cpp
    Physics/Auto/ClothWindTests.cpp
        Physics/Auto/XPBDJointTests.cpp
        Physics/Auto/XPBDJointVelocityTests.cpp
        Physics/Auto/RigidBodyTests.cpp
        Physics/Auto/BroadPhaseTests.cpp
        Physics/Auto/BroadPhaseIncrementalTests.cpp
        Physics/Auto/CollisionMatrixTests.cpp
        Physics/Auto/CylinderGroundRegressionTests.cpp
        Physics/Auto/CCDTests.cpp
        Physics/Auto/ConvexHullTests.cpp
        Physics/Auto/WorldQueryTests.cpp
        Physics/Auto/WorldQueryIndexTests.cpp
        Physics/Auto/WorldRaycastShapeTests.cpp
        Physics/Auto/WorldSceneSyncTests.cpp
        Physics/Auto/WorldStepTests.cpp
        Physics/Auto/WorldCollisionEventTests.cpp
        Physics/Auto/ContactResolveTests.cpp
        Physics/Auto/ContactIslandTests.cpp
        Physics/Auto/NarrowPhaseContactTests.cpp
        Physics/Auto/MeshColliderTests.cpp
        Physics/Auto/RigidBodySerializerTests.cpp
        Physics/Auto/XPBDContactTests.cpp
        Physics/Auto/XPBDBodyDriveTests.cpp
        Physics/Auto/XPBDGripTests.cpp
        Physics/Auto/XPBDPlaneContactTests.cpp
        Physics/Auto/XPBDPoseAnchorTests.cpp
        Physics/Auto/ConstraintTests.cpp
        Physics/Auto/ContactCacheTests.cpp
        Physics/Auto/PhysicsMaterialTests.cpp
        Physics/Auto/LayerTests.cpp
        Physics/Auto/ColliderVolumeTests.cpp
        Physics/Auto/FluidVolumeTests.cpp
        Physics/Auto/FlowVolumeTests.cpp
        Physics/Auto/ColliderDebugGeometryTests.cpp
        Physics/Auto/ConstraintDebugGeometryTests.cpp
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
        Fluid/Auto/FluidOperatorEvalTests.cpp
        Fluid/Auto/FluidGpuStepTests.cpp
        Fluid/Auto/GerstnerWavesTests.cpp
    LINKS
        FBZZFluid
)

# @note --- Engine -----------------------------------------------------------------
fbzz_add_test_suite(EngineAuto
    SOURCES
        Engine/Auto/WorldCellTests.cpp
        Engine/Auto/WorldPcgTests.cpp
        Engine/Auto/WorldHlodTests.cpp
        Engine/Auto/WorldPartitionManifestTests.cpp
        Engine/Auto/WorldPartitionTests.cpp
        Engine/Auto/CellAssetStreamingTests.cpp
        Engine/Auto/SceneForestTests.cpp
        Engine/Auto/RagdollRigTests.cpp
        Engine/Auto/RagdollDefinitionTests.cpp
        Engine/Auto/RagdollControlsTests.cpp
        Engine/Auto/StandingPoseTests.cpp
        Engine/Auto/RagdollSystemTests.cpp
    Engine/Auto/ClothSystemTests.cpp
    Engine/Auto/ClothContactSystemTests.cpp
    Engine/Auto/ClothFlowFieldTests.cpp
    Engine/Auto/InactiveHierarchyTests.cpp
        Engine/Auto/ClothAssetTests.cpp
        Engine/Auto/ClothRenderBindingTests.cpp
        Engine/Auto/AssetDatabaseTests.cpp
        Engine/Auto/AssetStreamingTests.cpp
        Engine/Auto/SceneAssetReferencesTests.cpp
        Engine/Auto/ManagedScriptAssetPersistenceTests.cpp
        Engine/Auto/AssetPartialIoTests.cpp
        Engine/Auto/AssetStreamingIntegrationTests.cpp
        Engine/Auto/EventBusTests.cpp
        Engine/Auto/PixCaptureTests.cpp
        Engine/Auto/FileSystemPathTests.cpp
    Engine/Auto/ManagedWorldFamilyServicesTests.cpp
        Engine/Auto/CanvasGeometryGuardTests.cpp
        Engine/Auto/SceneCapacityTests.cpp
        Engine/Auto/SceneBulkDestroyTests.cpp
    Engine/Auto/SceneRendererIndexTests.cpp
        Engine/Auto/NavMeshBakeConcurrencyTests.cpp
        Engine/Auto/NavigationControlPointTests.cpp
        Engine/Auto/NavigationWorkspaceTests.cpp
        Engine/Auto/IKTargetReferenceTests.cpp
        Engine/Auto/FootIKTests.cpp
        Engine/Auto/AdditionalTargetPointPersistenceTests.cpp
        Engine/Auto/ManagedTargetPointRuntimeTests.cpp
        Engine/Auto/AdditionalTargetPointUsageTests.cpp
        Engine/Auto/TransformSystemTests.cpp
        Engine/Auto/ControlPointResolverTests.cpp
        Engine/Auto/SceneCapacityIntegrationTests.cpp
        Engine/Auto/ShaderCapabilitiesTests.cpp
        Engine/Auto/PrefabIdentityTests.cpp
        Engine/Auto/ManagedScriptRuntimeTests.cpp
        Engine/Auto/ManagedInputBridgeTests.cpp
        Engine/Auto/ManagedRequiredReferenceTests.cpp
        Engine/Auto/ManagedMathParityTests.cpp
    Engine/Auto/GeneratedManagedBridgeTests.cpp
    Engine/Auto/ManagedObjectBatchTests.cpp
        Engine/Auto/ManagedTransformPoseTests.cpp
        Engine/Auto/ManagedTransformPositionTests.cpp
        Engine/Auto/ManagedObjectLifecycleTests.cpp
        Engine/Auto/ManagedObjectControlsTests.cpp
        Engine/Auto/ManagedObjectControlsRuntimeTests.cpp
        Engine/Auto/ManagedGamePackageTests.cpp
        Engine/Auto/ManagedCompletionTests.cpp
        Engine/Auto/ManagedPhysicsBridgeTests.cpp
        Engine/Auto/ManagedServicesBridgeTests.cpp
        Engine/Auto/ManagedServicesRuntimeTests.cpp
        Engine/Auto/ManagedPhysicsRuntimeTests.cpp
        Engine/Auto/ManagedCollisionTests.cpp
        Engine/Auto/ManagedEngineNotificationTests.cpp
        Engine/Auto/ManagedRenderTransfersTests.cpp
        Engine/Auto/ManagedComponentServicesTests.cpp
        Engine/Auto/PresentationGeometryTests.cpp
        Engine/Auto/ManagedRenderComponentsTests.cpp
        Engine/Auto/ManagedParticleServicesTests.cpp
        Engine/Auto/ManagedComponentServicesRuntimeTests.cpp
        Engine/Auto/ColorTests.cpp
        Engine/Auto/EasingTests.cpp
        Engine/Auto/GuidRefCodecTests.cpp
        Engine/Auto/SpriteReferenceTests.cpp
        Engine/Auto/StringUtilsTests.cpp
        Engine/Auto/TransformTests.cpp
        Engine/Auto/Utf8Tests.cpp
        Engine/Auto/UuidTests.cpp
        Engine/Auto/AnimationSamplingTests.cpp
        Engine/Auto/BakedBoneAnimationTests.cpp
        Engine/Auto/AudioBusTests.cpp
        Engine/Auto/AudioStreamingTests.cpp
        Engine/Auto/AudioSourceTests.cpp
        Engine/Auto/BlackboardTests.cpp
        Engine/Auto/BehaviorTreeRuntimeTests.cpp
        Engine/Auto/BehaviorTreeSystemTests.cpp
        Engine/Auto/CrowdSimulationTests.cpp
        Engine/Auto/ManagedAiCrowdTransferTests.cpp
        Engine/Auto/ManagedAiPoseTransferTests.cpp
        Engine/Auto/ManagedAiCohortRuntimeTests.cpp
        Engine/Auto/ComponentArrayTests.cpp
        Engine/Auto/FzAssetFormatTests.cpp
        Engine/Auto/InputActionMapTests.cpp
        Engine/Auto/CursorControlPolicyTests.cpp
        Engine/Auto/UIInputPolicyTests.cpp
        Engine/Auto/RenderPipelineAssetTests.cpp
        Engine/Auto/JointComponentTests.cpp
        Engine/Auto/ParticleCodecTests.cpp
        Engine/Auto/ParticleCurveTests.cpp
        Engine/Auto/VectorFieldTests.cpp
        Engine/Auto/AssetTypeBoundaryTests.cpp
    Engine/Auto/SharedSDKAssetTests.cpp
        Engine/Auto/TransientGpuStateTests.cpp
        Engine/Auto/RuntimeMeshOwnershipTests.cpp
        Engine/Auto/HostResourceBindingTests.cpp
        Engine/Auto/UIInteractionTests.cpp
        Engine/Auto/FlipbookMotionVectorEncodingTests.cpp
        Engine/Auto/FlipbookMotionVectorsTests.cpp
        Engine/Auto/VolumeFlipbookAnalyticTests.cpp
        Engine/Auto/VolumeFlipbookFramingTests.cpp
        Engine/Auto/ComponentReflectionParityTests.cpp
        Engine/Auto/FlowFieldFrameTests.cpp
        Engine/Auto/VFXEnvelopeTests.cpp
        Engine/Auto/VFXSubEmitterScopeTests.cpp
        Engine/Auto/ParticleMaterialPathTests.cpp
        Engine/Auto/FiberTests.cpp
        Engine/Auto/TerrainFiberStreamingTests.cpp
    Engine/Auto/FiberPlacementTests.cpp
    Engine/Auto/ParticleFlipbookTests.cpp
    Engine/Auto/ParticleLoopInspectionTests.cpp
        Engine/Auto/FluidSolverTests.cpp
        Engine/Auto/SixWayLightingTests.cpp
        Engine/Auto/ParticleLightSelectionTests.cpp
        Engine/Auto/ParticleGpuFallbackTests.cpp
        Engine/Auto/VolumeFlipbookFluidTests.cpp
        Engine/Auto/FlipbookMipsTests.cpp
        Engine/Auto/VFXLineGeometryTests.cpp
        Engine/Auto/FluidLiquid3DTests.cpp
        Engine/Auto/WaterfallFluidClipTests.cpp
        Engine/Auto/FluidRecipeBakeTests.cpp
        Engine/Auto/FluidBakeSafetyTests.cpp
        Engine/Auto/FluidDeterminismTests.cpp
        Engine/Auto/FluidSegmentShapeTests.cpp
        Engine/Auto/FluidAmountEnvelopeTests.cpp
        Engine/Auto/FluidGpuLiquidTests.cpp
        Engine/Auto/GeometryRouteTests.cpp
        Engine/Auto/GeometryMaterialExtractionTests.cpp
        Engine/Auto/GeometryPipelineTests.cpp
        Engine/Auto/RenderVisibilityTests.cpp
        Engine/Auto/RenderSceneExtractorTests.cpp
        Engine/Auto/StaticMeshRenderReuseTests.cpp
        Engine/Auto/StaticMeshRenderMotionTests.cpp
        Engine/Auto/RenderLightExtractorTests.cpp
        Engine/Auto/InstanceBatchTests.cpp
        Engine/Auto/InstanceSetTests.cpp
        Engine/Auto/RenderGraphTests.cpp
        Engine/Auto/RenderPipelineTests.cpp
        Engine/Auto/RenderPassCaptureTests.cpp
        Engine/Auto/ShaderReloadTests.cpp
        Engine/Auto/MaterialReflectionTests.cpp
        Engine/Auto/RenderGraphLifetimeTests.cpp
        Engine/Auto/RenderSettingsTests.cpp
        Engine/Auto/RayParticleCoverageTests.cpp
        Engine/Auto/RayLightExtractionTests.cpp
        Engine/Auto/RenderModeSettingsTests.cpp
        Engine/Auto/DebugDrawBatchTests.cpp
        Engine/Auto/SceneSerializerTests.cpp
        Engine/Auto/TerrainSplatTests.cpp
        Engine/Auto/TerrainPatchLodTests.cpp
        Engine/Auto/TerrainRenderResourceTests.cpp
        Engine/Auto/VelocityFieldAtlasOwnershipTests.cpp
        Engine/Auto/MeshTrailRenderResourceTests.cpp
        Engine/Auto/ParticlePlaybackTests.cpp
        Engine/Auto/ParticleGpuOwnershipTests.cpp
        Engine/Auto/BakeGpuOwnershipTests.cpp
        Engine/Auto/AnimatorControllerDefinitionTests.cpp
        Engine/Auto/ReflectionStandaloneTests.cpp
        Engine/Auto/SaveStoreReflectionTests.cpp
        Engine/Auto/TweenCoroutineTests.cpp
        Engine/Auto/UIAnimatorSchedulingTests.cpp
        Engine/Auto/TrailMultiKeyTests.cpp
        Engine/Auto/UIRectTests.cpp
        Engine/Auto/WaterWaveTests.cpp
        Engine/Auto/WaterGridTests.cpp
        Engine/Auto/WaterSurfaceQueryTests.cpp
    Engine/Auto/WaterBuoyancyClockTests.cpp
    Engine/Auto/UIThemeAssetTests.cpp
        Engine/Auto/WaterRenderResourceTests.cpp
        Engine/Auto/WaterTerrainInfoTests.cpp
        Engine/Auto/WaterWavesAssetTests.cpp
        Engine/Auto/WaterDepthFieldTests.cpp
        Engine/Auto/WaterSplineTests.cpp
        Engine/Auto/WaterZoneTests.cpp
        Engine/Auto/WaterExclusionTests.cpp
        Engine/Auto/WaterInteractionTests.cpp
        Engine/Auto/WaterZonePersistenceTests.cpp
        Engine/Auto/WaterSplineMeshTests.cpp
        Engine/Auto/WaterOptimizationTests.cpp
        Engine/Auto/WaterVortexTests.cpp
        Engine/Auto/WaterNoiseBakeTests.cpp
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
        Engine/Auto/BakedBoneReplayTests.cpp
        Engine/Auto/RaySkinningTests.cpp
        Engine/Auto/RayEnvironmentExtractionTests.cpp
        Engine/Auto/RegularMeshRenderPerformanceTests.cpp
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
    Editor/Auto/WorldPartitionOperatorTests.cpp
    Editor/Auto/WorldPcgAuthoringTests.cpp
    Editor/Auto/WorldCellBakingTests.cpp
    Editor/Auto/AnimatorMaskAuditTests.cpp
    Editor/Auto/AppIconWriterTests.cpp
    Editor/Auto/AssetBrowserExplorerTests.cpp
    Editor/Auto/AnalysisProfilerGpuTests.cpp
    Editor/Auto/ProfilerDisplayRowsTests.cpp
    Editor/Auto/AssetReferenceScanTests.cpp
    Editor/Auto/PrefabStructureMergeTests.cpp
    Editor/Auto/PlayChangesAdoptionTests.cpp
    Editor/Auto/SequenceCurveTests.cpp
    Editor/Auto/AssetBrowserThumbnailTests.cpp
    Editor/Auto/AssetDirtyRegistryTests.cpp
    Editor/Auto/AssetInspectorPersistenceTests.cpp
    Editor/Auto/MetaFilePersistenceTests.cpp
    Editor/Auto/AssetPathTests.cpp
    Editor/Auto/AssetPickerTests.cpp
    Editor/Auto/ClothPinBrushTests.cpp
    Editor/Auto/FiberPaintToolTests.cpp
    Editor/Auto/ControlPointEditingTests.cpp
    Editor/Auto/ControlPointViewportTests.cpp
    Editor/Auto/ControlPointInspectorTests.cpp
    Editor/Auto/ControlPointOperatorTests.cpp
        Editor/Auto/AdditionalControlPointOperatorTests.cpp
        Editor/Auto/TargetPointReferenceOperatorTests.cpp
    Editor/Auto/BuildSettingsOutputPathTests.cpp
    Editor/Auto/PackageBuildTests.cpp
    Editor/Auto/StandalonePlaySessionTests.cpp
    Editor/Auto/SpatialDebugVisualizationTests.cpp
    Editor/Auto/BusContractParityTests.cpp
    Editor/Auto/EditorBusDispatcherTests.cpp
    Editor/Auto/EditorBusProtocolTests.cpp
    Editor/Auto/NamedPipeServerTests.cpp
    Editor/Auto/EditorProjectFileTests.cpp
    Editor/Auto/EditorSettingsTests.cpp
    Editor/Auto/EditorMenuCatalogTests.cpp
    Editor/Auto/ObjectPresetMenuTests.cpp
    Editor/Auto/EditorIdleWorkTests.cpp
    Editor/Auto/EditorQueryCacheTests.cpp
    Editor/Auto/InspectorValueSnapshotTests.cpp
    Editor/Auto/AnimationTrackTableTests.cpp
    Editor/Auto/BehaviorTreeChildCountsTests.cpp
    Editor/Auto/MaterialPreviewCacheTests.cpp
    Editor/Auto/FluidAssetWritersTests.cpp
    Editor/Auto/FluidDocumentTests.cpp
    Editor/Auto/FluidEffectTemplateTests.cpp
    Editor/Auto/FluidPartClipboardTests.cpp
    Editor/Auto/FluidPartOverlayTests.cpp
    Editor/Auto/FluidPreviewCacheTests.cpp
    Editor/Auto/GraphLayoutTests.cpp
    Editor/Auto/ImageCompareTests.cpp
    Editor/Auto/FluidJetFlamePlaybackTests.cpp
    Editor/Auto/ImportCacheStoreTests.cpp
    Editor/Auto/ImportFingerprintTests.cpp
    Editor/Auto/ModelImportOperatorTests.cpp
    Editor/Auto/ProjectSettingsOperatorTests.cpp
    Editor/Auto/InputBindingOperatorTests.cpp
    Editor/Auto/TextureImportOperatorTests.cpp
    Editor/Auto/EditorSettingsDocumentTests.cpp
    Editor/Auto/SequenceOperatorTests.cpp
    Editor/Auto/IblOperatorTests.cpp
    Editor/Auto/PlacementOperatorTests.cpp
    Editor/Auto/VolumeFlipbookOperatorTests.cpp
    Editor/Auto/DirectPanelOperatorTests.cpp
    Editor/Auto/AssetFileOperatorTests.cpp
    Editor/Auto/RenderPassOperatorTests.cpp
    Editor/Auto/InputRecorderTests.cpp
    Editor/Auto/JsonTests.cpp
    Editor/Auto/MaterialPreviewFiberTests.cpp
    Editor/Auto/PlaytestJsonTests.cpp
    Editor/Auto/PlaytestRunnerTests.cpp
        Editor/Auto/ManagedScriptInspectionTests.cpp
        Editor/Auto/ManagedScriptBuildTests.cpp
    Editor/Auto/ManagedSdkBindingTests.cpp
        Editor/Auto/ManagedCompletionTests.cpp
    Editor/Auto/SceneFindPaginationTests.cpp
        Editor/Auto/ManagedDataAssetAuthoringTests.cpp
    Editor/Auto/PixOperatorTests.cpp
    Editor/Auto/ProfilerBenchmarkTests.cpp
    Editor/Auto/PrefabOverrideTests.cpp
        Editor/Auto/PrefabReferenceOverrideTests.cpp
        Editor/Auto/SceneEditCapacityTests.cpp
    Editor/Auto/SceneEditReferenceCloneTests.cpp
    Editor/Auto/PrefabSaveTests.cpp
    Editor/Auto/SceneDirtyTrackerTests.cpp
    Editor/Auto/ManagedScriptMetadataTests.cpp
    Editor/Auto/SourceOpenTests.cpp
    Editor/Auto/SpriteFieldFilterTests.cpp
    Editor/Auto/SpriteSlicerTests.cpp
    Editor/Auto/TerrainBrushTests.cpp
        Editor/Auto/TerrainAuthoringTests.cpp
        Editor/Auto/TerrainGrassOperatorTests.cpp
        Editor/Auto/TerrainScatterTests.cpp
        Editor/Auto/TerrainPaintAssetTests.cpp
        Editor/Auto/TerrainToolAuthoringTests.cpp
    Editor/Auto/UndoStackTests.cpp
    Editor/Auto/WaterInspectorRuntimeTests.cpp
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
    TestKit/src/Console.cpp
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
target_link_libraries(FBZZAlgorithmOptimizationBench PRIVATE FBZZEngine psapi)
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
        Graphics/Auto/GraphicsStandaloneCpuTests.cpp
        Graphics/Auto/OcclusionCullerTests.cpp
        Graphics/Auto/SurfacePatchLodTests.cpp
        Graphics/Auto/RenderMeshSpatialIndexTests.cpp
        Graphics/Auto/DX12UploadArenaSizingTests.cpp
        Graphics/Auto/RenderSceneSpatialQueriesTests.cpp
        Graphics/Auto/MeshletGeometryBuilderTests.cpp
        Graphics/Auto/SurfacePatchMorphTests.cpp
        Graphics/Auto/SurfaceFarMeshTests.cpp
        Graphics/Auto/WaterFarRenderPassTests.cpp
        Graphics/Auto/ResolvedRenderPlanTests.cpp
        Graphics/Auto/GpuProfilerTests.cpp
        Graphics/Auto/RecordedGpuProfilerViewTests.cpp
    Graphics/Auto/RenderThreadTests.cpp
        Graphics/Auto/PixEventStackTests.cpp
        Graphics/Auto/ProbeCaptureBudgetTests.cpp
        Graphics/Auto/RayRenderPlanCpuTests.cpp
        Graphics/Auto/RayGameContractTests.cpp
        Graphics/Auto/RayReflectionMediumCacheTests.cpp
        Graphics/Auto/RayEnvironmentTests.cpp
        Graphics/Auto/RayPathSceneTests.cpp
        Graphics/Auto/RayPathHistoryTests.cpp
        Graphics/Auto/RaySceneBuilderTests.cpp
    LINKS FBZZGraphics ImGui
)

target_include_directories(FBZZTestsGraphicsAutoObj PRIVATE "${CMAKE_SOURCE_DIR}/Projects/Graphics/src")

# @note 実 DX12 デバイス・描画・GPU 読戻しを要するテストは通常 CTest / CI から外す。
fbzz_add_test_suite(GraphicsStandalone OPT_IN
    SOURCES Graphics/Auto/GraphicsStandaloneTests.cpp
        Graphics/Auto/ThreadedRenderingTests.cpp
        Graphics/Auto/DeferredEmissionTests.cpp
        Graphics/Auto/WaterShaderTests.cpp
    Graphics/Auto/WaterRenderSyncTests.cpp
        Graphics/Auto/GpuGeometrySelectionTests.cpp
        Graphics/Auto/TerrainInstanceShaderTests.cpp
        Graphics/Auto/InstancedMeshTests.cpp
        Graphics/Auto/FiberRenderPerfTests.cpp
        Graphics/Auto/MeshletGeometryShaderTests.cpp
        Graphics/Auto/WaterMediumShaderTests.cpp
        Graphics/Auto/ManyLightShaderTests.cpp
        Graphics/Auto/UnderwaterShaderTests.cpp
        Graphics/Auto/ReflectionProbeCaptureTests.cpp
        Graphics/Auto/GraphicsQualityShaderTests.cpp
        Graphics/Auto/GraphicsQualityLifetimeTests.cpp
        Graphics/Fixtures/GraphicsQualityFixture.cpp
        Graphics/Auto/BufferReadTests.cpp
        Editor/Auto/MaterialPreviewShaderTests.cpp
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
        Graphics/Auto/HybridHistoryTests.cpp
        Graphics/Auto/RayRenderPlanTests.cpp
        Graphics/Auto/RayTracingTests.cpp
        Graphics/Auto/RayReflectionTests.cpp
        Graphics/Auto/RayHybridGlassTraversalTests.cpp
        Graphics/Auto/RayReflectionReconstructionTests.cpp
        Graphics/Auto/RayHybridDiffuseGITests.cpp
        Graphics/Auto/RayGameTests.cpp
        Graphics/Auto/RayPathTracingTests.cpp
        Graphics/Auto/RayTextureTests.cpp
    LINKS FBZZGraphics
)
configure_file(Graphics/Auto/StandaloneTriangle.hlsl StandaloneTriangle.hlsl COPYONLY)
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
