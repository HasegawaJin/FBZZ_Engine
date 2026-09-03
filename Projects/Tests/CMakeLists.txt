# 自動テストと手動テストの構成。規約は Docs/conventions/test.md を参照。
#
# ソースは file(GLOB) で拾わない。新規ファイルが CMake 再実行まで拾われず、
# 「書いたのに一度も実行されていないテスト」が生まれるため。

add_subdirectory(TestKit)

include("${CMAKE_SOURCE_DIR}/CMake/FBZZTests.cmake")

# --- Math -------------------------------------------------------------------
fbzz_add_test_suite(MathAuto
    SOURCES
        Math/Auto/Vector3Tests.cpp
        Math/Auto/QuaternionTests.cpp
        Math/Auto/Matrix4Tests.cpp
)

# --- Core -------------------------------------------------------------------
fbzz_add_test_suite(CoreAuto
    SOURCES
        Core/Auto/Memory/LinearAllocatorTests.cpp
        Core/Auto/Memory/StackAllocatorTests.cpp
        Core/Auto/Memory/PoolAllocatorTests.cpp
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
    LINKS
        FBZZTestKitPhysics
)

# --- Engine -----------------------------------------------------------------
fbzz_add_test_suite(EngineAuto
    SOURCES
        Engine/Auto/RagdollRigTests.cpp
    LINKS
        FBZZTestKitEngine
        FBZZPhysics
)

# --- ビジュアル検証ベンチ ---------------------------------------------------
# CTest には載せない。合否をコードで判定しないため。
# WHY テスト実行機能を持たせないか: 自動テストの正は CTest / VS のテストエクスプローラー。
#      ここでも走らせられるようにすると «どちらの結果が正しいのか» が分からなくなる。
add_executable(FBZZTestBench
    Bench/main.cpp
    Bench/BenchApp.cpp
    Bench/Viewport2D.cpp
    Bench/Scenes/XPBDConvergenceScene.cpp
    Bench/Scenes/XPBDJointChainScene.cpp
    Bench/Scenes/ContactScene.cpp
    Bench/Scenes/BVHQueryScene.cpp
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
