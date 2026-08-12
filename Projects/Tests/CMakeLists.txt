# FBZZ Engine
# Projects/Tests/CMakeLists.txt
# GoogleTest による自動テストと、開発者が直接起動する手動テストの構成。

include(FetchContent)
include(GoogleTest)

option(FBZZ_FETCH_GOOGLETEST "Fetch GoogleTest when it is not installed" ON)

find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    if(NOT FBZZ_FETCH_GOOGLETEST)
        message(FATAL_ERROR
            "GoogleTest was not found. Install GTest or enable FBZZ_FETCH_GOOGLETEST.")
    endif()

    # WHY: MSVC の Engine と GoogleTest の CRT を揃え、DLL 境界で STL を共有しても
    #      Debug/Release のランタイム不一致を起こさない。
    set(gtest_force_shared_crt ON CACHE BOOL "Use the shared MSVC runtime" FORCE)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    )
    FetchContent_MakeAvailable(googletest)
endif()

set(FBZZ_TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/Tests")

# 共有 DLL は Engine 本体の出力先からテスト出力先へコピーする。
# WHY: Visual Studio からテスト exe だけを起動しても、DLL の検索先を手動設定しなくてよいようにする。
add_custom_target(fbzz_tests_stage_runtime
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FBZZ_TEST_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:fbzz_math> "${FBZZ_TEST_OUTPUT_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:fbzz_physics> "${FBZZ_TEST_OUTPUT_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:fbzz_engine> "${FBZZ_TEST_OUTPUT_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "${FBZZ_TEST_OUTPUT_DIR}/"
    DEPENDS fbzz_math fbzz_physics fbzz_engine
    COMMENT "Staging FBZZ runtime DLLs for GoogleTest"
)

add_executable(fbzz_tests_core_auto
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
)
set_target_properties(fbzz_tests_core_auto PROPERTIES
    OUTPUT_NAME "FBZZTestsCoreAuto"
    RUNTIME_OUTPUT_DIRECTORY "${FBZZ_TEST_OUTPUT_DIR}"
)
target_link_libraries(fbzz_tests_core_auto
    PRIVATE
        GTest::gtest_main
        fbzz_engine
        fbzz_physics
)
add_dependencies(fbzz_tests_core_auto fbzz_tests_stage_runtime)
gtest_discover_tests(fbzz_tests_core_auto
    DISCOVERY_TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/Tests"
)

# ManualTest は CTest に登録しない。Window/Cursor の OS 状態を変更するため、
# 開発者が Visual Studio から意図的に起動した場合だけ実行する。
add_executable(fbzz_tests_core_manual
    Core/ManualTest/WindowManualTests.cpp
    Core/ManualTest/CursorManualTests.cpp
)
set_target_properties(fbzz_tests_core_manual PROPERTIES
    OUTPUT_NAME "FBZZTestsCoreManual"
    RUNTIME_OUTPUT_DIRECTORY "${FBZZ_TEST_OUTPUT_DIR}"
)
target_link_libraries(fbzz_tests_core_manual
    PRIVATE
        GTest::gtest_main
        fbzz_engine
        fbzz_physics
)
add_dependencies(fbzz_tests_core_manual fbzz_tests_stage_runtime)
