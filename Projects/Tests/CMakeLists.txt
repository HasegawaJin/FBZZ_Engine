# 自動テストと手動テストの構成。規約は Docs/conventions/test.md を参照。
#
# ソースは file(GLOB) で拾わない。新規ファイルが CMake 再実行まで拾われず、
# 「書いたのに一度も実行されていないテスト」が生まれるため。

add_subdirectory(TestKit)

include("${CMAKE_SOURCE_DIR}/CMake/FBZZTests.cmake")

# --- Math -------------------------------------------------------------------
fbzz_add_test_suite(MathAuto
    SOURCES
        Math/Auto/Vector2Tests.cpp
        Math/Auto/Vector3Tests.cpp
        Math/Auto/Vector4Tests.cpp
        Math/Auto/QuaternionTests.cpp
        Math/Auto/Matrix3Tests.cpp
        Math/Auto/Matrix4Tests.cpp
        Math/Auto/MathUtilsTests.cpp
        Math/Auto/PlaneTests.cpp
        Math/Auto/RayTests.cpp
        Math/Auto/FrustumTests.cpp
)

# --- Core -------------------------------------------------------------------
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
        Core/Auto/Time/TimeConfigurationTests.cpp
        Core/Auto/Time/TimeTickTests.cpp
        Core/Auto/Concurrency/TaskSystemSubmitTests.cpp
        Core/Auto/Concurrency/TaskSystemConcurrencyTests.cpp
        Core/Auto/Scheduler/SchedulerDeclarationTests.cpp
        Core/Auto/Scheduler/SchedulerExecutionTests.cpp
        Core/Auto/Scheduler/SchedulerBatchingTests.cpp
    LINKS
        FBZZTestKitEngine
        FBZZPhysics   # SchedulerExecutionTests がコンポーネント型として参照する
)

# --- Physics ----------------------------------------------------------------
fbzz_add_test_suite(PhysicsAuto
    SOURCES
        Physics/Auto/AABBTests.cpp
        Physics/Auto/ColliderTests.cpp
        Physics/Auto/GJKTests.cpp
        Physics/Auto/EPATests.cpp
        Physics/Auto/BVHTests.cpp
        Physics/Auto/XPBDSolverTests.cpp
        Physics/Auto/XPBDJointTests.cpp
        Physics/Auto/RigidBodyTests.cpp
        Physics/Auto/BroadPhaseTests.cpp
        Physics/Auto/CollisionMatrixTests.cpp
        Physics/Auto/CCDTests.cpp
        Physics/Auto/ConvexHullTests.cpp
        Physics/Auto/WorldQueryTests.cpp
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
        Physics/Auto/XPBDPlaneContactTests.cpp
        Physics/Auto/XPBDPoseAnchorTests.cpp
        Physics/Auto/ConstraintTests.cpp
        Physics/Auto/ContactCacheTests.cpp
        Physics/Auto/PhysicsMaterialTests.cpp
        Physics/Auto/LayerTests.cpp
        Physics/Auto/ColliderVolumeTests.cpp
        Physics/Auto/ColliderDebugGeometryTests.cpp
        Physics/Auto/ConstraintDebugGeometryTests.cpp
    LINKS
        FBZZTestKitPhysics
)

# --- Engine -----------------------------------------------------------------
fbzz_add_test_suite(EngineAuto
    SOURCES
        Engine/Auto/RagdollRigTests.cpp
        Engine/Auto/AssetDatabaseTests.cpp
        Engine/Auto/EventBusTests.cpp
        Engine/Auto/FileSystemPathTests.cpp
        Engine/Auto/ScriptTransformProxyTests.cpp
        Engine/Auto/ColorTests.cpp
        Engine/Auto/EasingTests.cpp
        Engine/Auto/GuidRefCodecTests.cpp
        Engine/Auto/SpriteReferenceTests.cpp
        Engine/Auto/StringUtilsTests.cpp
        Engine/Auto/TransformTests.cpp
        Engine/Auto/Utf8Tests.cpp
        Engine/Auto/UuidTests.cpp
        Engine/Auto/AnimationSamplingTests.cpp
        Engine/Auto/AudioBusTests.cpp
        Engine/Auto/BlackboardTests.cpp
        Engine/Auto/ComponentArrayTests.cpp
        Engine/Auto/FzAssetFormatTests.cpp
        Engine/Auto/InputActionMapTests.cpp
        Engine/Auto/ParticleCodecTests.cpp
        Engine/Auto/ParticleCurveTests.cpp
        Engine/Auto/VectorFieldTests.cpp
        Engine/Auto/FlipbookMotionVectorEncodingTests.cpp
        Engine/Auto/FlipbookMotionVectorsTests.cpp
        Engine/Auto/VolumeFlipbookAnalyticTests.cpp
        Engine/Auto/VolumeFlipbookFramingTests.cpp
        Engine/Auto/ComponentReflectionParityTests.cpp
        Engine/Auto/VFXEnvelopeTests.cpp
        Engine/Auto/ParticleMaterialPathTests.cpp
        Engine/Auto/RenderGraphTests.cpp
        Engine/Auto/RenderGraphLifetimeTests.cpp
        Engine/Auto/SceneSerializerTests.cpp
        Engine/Auto/UIRectTests.cpp
    LINKS
        FBZZTestKitEngine
        FBZZPhysics
)

# --- Editor -----------------------------------------------------------------
# WHY Editor をリンクするか: import 判定まわりは «起動のたびに全アセットを焼き直す»
#     という形でしか表に出ず、しかも直ったかどうかがローカルの mtime に左右される。
#     再現条件をテストへ固定できないと、同じ回帰を何度も踏む。
#     FBZZEditor は STATIC なので、テストが参照するオブジェクトだけが引き込まれる。
fbzz_add_test_suite(EditorAuto
  SOURCES
    Editor/Auto/AnimatorMaskAuditTests.cpp
    Editor/Auto/AssetDirtyRegistryTests.cpp
    Editor/Auto/AssetPathTests.cpp
    Editor/Auto/EditorBusDispatcherTests.cpp
    Editor/Auto/EditorBusProtocolTests.cpp
    Editor/Auto/EditorSettingsTests.cpp
    Editor/Auto/GraphLayoutTests.cpp
    Editor/Auto/ImportCacheStoreTests.cpp
    Editor/Auto/ImportFingerprintTests.cpp
    Editor/Auto/JsonTests.cpp
    Editor/Auto/PrefabOverrideTests.cpp
    Editor/Auto/PrefabSaveTests.cpp
    Editor/Auto/SceneDirtyTrackerTests.cpp
    Editor/Auto/ScriptCodeGenTests.cpp
    Editor/Auto/SpriteSlicerTests.cpp
    Editor/Auto/TerrainBrushTests.cpp
    Editor/Auto/UndoStackTests.cpp
  LINKS
    FBZZTestKitEditor
)

# Editor の一部の道具はヘッダごと src/ に置かれている (公開 API ではないため)。
# WHY テストからだけ覗くか: TerrainBrush の減衰カーブや座標変換は «なんとなく変» という
#     形でしか壊れず、目視では追えない。公開範囲を広げる代わりに、テスト側だけが
#     内部ヘッダを見られるようにする。
target_include_directories(FBZZTestsEditorAutoObj
    PRIVATE "${CMAKE_SOURCE_DIR}/Projects/Editor/src")

# --- ビジュアル検証ベンチ ---------------------------------------------------
# CTest には載せない。合否をコードで判定しないため。
# WHY テスト実行機能を持たせないか: 自動テストの正は CTest / VS のテストエクスプローラー。
#      ここでも走らせられるようにすると «どちらの結果が正しいのか» が分からなくなる。
add_executable(FBZZTestBench
    Bench/main.cpp
    Bench/AnomalyLog.cpp
    Bench/BenchApp.cpp
    Bench/Viewport2D.cpp
    Bench/Scenes/XPBDConvergenceScene.cpp
    Bench/Scenes/XPBDJointChainScene.cpp
    Bench/Scenes/ContactScene.cpp
    Bench/Scenes/BVHQueryScene.cpp
    Bench/Scenes/CCDScene.cpp
    Bench/Scenes/ConvexHullScene.cpp
    Bench/Scenes/RagdollScene.cpp
    # コンソール制御だけ TestKit と共有する。
    # WHY リンクせずソースを直接足すか: FBZZTestKit は gtest / gmock を引き連れてくる。
    #      ベンチはテストフレームワークに依存させたくないので、この 1 ファイルだけ借りる。
    TestKit/src/Console.cpp
)
target_include_directories(FBZZTestBench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/TestKit/include")
target_link_libraries(FBZZTestBench PRIVATE FBZZEngine FBZZPhysics FBZZMath ImGui)
set_target_properties(FBZZTestBench PROPERTIES FOLDER "Tests")
fbzz_set_test_output_dir(FBZZTestBench)
fbzz_apply_app_manifest(FBZZTestBench)
fbzz_stage_test_runtime(FBZZTestBench)

# --- 手動テスト -------------------------------------------------------------
# Window / Cursor は OS の状態を書き換えるため CTest に登録しない。
# 開発者が Visual Studio から意図的に起動したときだけ実行する。
fbzz_add_test_suite(CoreManual MANUAL
    SOURCES
        Core/ManualTest/WindowManualTests.cpp
        Core/ManualTest/CursorManualTests.cpp
    LINKS
        FBZZTestKitEngine
)
