# ─── Tests: モジュール別実行ファイル ────────────────────────────────────────────
# 各モジュールを独立した実行ファイルとしてビルドする。
# GLOB を避け、追加したファイルが意図せずテストに混入しないよう明示列挙する。

# SDK テンプレートは実行ファイルを必要としないため、CMake script で参照契約を検証する。
add_test(NAME fbzz_tests_sdk_templates
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/CMake/SDK/TestSharedSDKTemplates.cmake"
)

# テスト exe をゲーム本体の exe と分離するため Binaries/<Config>/Tests/ に出力する。
# CMakeLists.txt スコープ内の上書きはサブディレクトリにのみ効き、上位に影響しない。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY             "${CMAKE_BINARY_DIR}/Binaries/Tests")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Tests")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Tests")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Tests")

# コア DLL を Tests/ へ一括コピーするカスタムターゲット。
# 各テスト exe が個別にコピーすると多重書き込みになるため、依存ターゲットで 1 回にまとめる。
add_custom_target(copy_dlls_to_tests ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/Tests"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:fbzz_math>    "${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/Tests/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:fbzz_physics> "${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/Tests/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:fbzz_engine>  "${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/Tests/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:imgui>        "${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/Tests/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/Tests/"
    DEPENDS fbzz_math fbzz_physics fbzz_engine imgui
    COMMENT "Copying runtime DLLs to Tests/"
)

# ── Physics テスト ──────────────────────────────────────────────────────────────
add_executable(fbzz_tests_physics
    Physics/main.cpp
)
set_target_properties(fbzz_tests_physics PROPERTIES OUTPUT_NAME "FBZZTestsPhysics")
target_link_libraries(fbzz_tests_physics
    PRIVATE
        fbzz_physics
)
add_test(NAME fbzz_tests_physics COMMAND fbzz_tests_physics)
add_dependencies(fbzz_tests_physics copy_dlls_to_tests)

# ── Math テスト ─────────────────────────────────────────────────────────────────
add_executable(fbzz_tests_math
    Math/main.cpp
)
set_target_properties(fbzz_tests_math PROPERTIES OUTPUT_NAME "FBZZTestsMath")
target_link_libraries(fbzz_tests_math
    PRIVATE
        fbzz_math
)
add_test(NAME fbzz_tests_math COMMAND fbzz_tests_math)
add_dependencies(fbzz_tests_math copy_dlls_to_tests)

# ── Memory テスト ───────────────────────────────────────────────────────────────
# fbzz_engine は PUBLIC include を持つため Engine/include 以下のヘッダーが使える。
add_executable(fbzz_tests_memory
    Memory/main.cpp
)
set_target_properties(fbzz_tests_memory PROPERTIES OUTPUT_NAME "FBZZTestsMemory")
target_link_libraries(fbzz_tests_memory
    PRIVATE
        fbzz_engine
)
add_test(NAME fbzz_tests_memory COMMAND fbzz_tests_memory)
add_dependencies(fbzz_tests_memory copy_dlls_to_tests)

# ── EventBus テスト ─────────────────────────────────────────────────────────────
add_executable(fbzz_tests_eventbus
    EventBus/main.cpp
)
set_target_properties(fbzz_tests_eventbus PROPERTIES OUTPUT_NAME "FBZZTestsEventBus")
target_link_libraries(fbzz_tests_eventbus
    PRIVATE
        fbzz_engine
)
add_test(NAME fbzz_tests_eventbus COMMAND fbzz_tests_eventbus)
add_dependencies(fbzz_tests_eventbus copy_dlls_to_tests)

# ── Transform テスト ────────────────────────────────────────────────────────────
add_executable(fbzz_tests_transform
    Transform/main.cpp
)
set_target_properties(fbzz_tests_transform PROPERTIES OUTPUT_NAME "FBZZTestsTransform")
target_link_libraries(fbzz_tests_transform
    PRIVATE
        fbzz_engine
)
add_test(NAME fbzz_tests_transform COMMAND fbzz_tests_transform)
add_dependencies(fbzz_tests_transform copy_dlls_to_tests)

# ── Time テスト ─────────────────────────────────────────────────────────────────
add_executable(fbzz_tests_time
    Time/main.cpp
)
set_target_properties(fbzz_tests_time PROPERTIES OUTPUT_NAME "FBZZTestsTime")
target_link_libraries(fbzz_tests_time
    PRIVATE
        fbzz_engine
)
add_test(NAME fbzz_tests_time COMMAND fbzz_tests_time)
add_dependencies(fbzz_tests_time copy_dlls_to_tests)

# ── Logger テスト ───────────────────────────────────────────────────────────────
add_executable(fbzz_tests_logger
    Logger/main.cpp
)
set_target_properties(fbzz_tests_logger PROPERTIES OUTPUT_NAME "FBZZTestsLogger")
target_link_libraries(fbzz_tests_logger
    PRIVATE
        fbzz_engine
)
add_test(NAME fbzz_tests_logger COMMAND fbzz_tests_logger)
add_dependencies(fbzz_tests_logger copy_dlls_to_tests)

# ── TaskSystem テスト ───────────────────────────────────────────────────────────
add_executable(fbzz_tests_tasksystem
    TaskSystem/main.cpp
)
set_target_properties(fbzz_tests_tasksystem PROPERTIES OUTPUT_NAME "FBZZTestsTaskSystem")
target_link_libraries(fbzz_tests_tasksystem
    PRIVATE
        fbzz_engine
)
add_test(NAME fbzz_tests_tasksystem COMMAND fbzz_tests_tasksystem)
add_dependencies(fbzz_tests_tasksystem copy_dlls_to_tests)

# ── Scene テスト ────────────────────────────────────────────────────────────────
# Scene.hpp は ComponentRegistry.hpp 経由で RigidBodyComponent → fbzz_physics を参照するため
# fbzz_physics も明示リンクする。
add_executable(fbzz_tests_scene
    Scene/main.cpp
)
set_target_properties(fbzz_tests_scene PROPERTIES OUTPUT_NAME "FBZZTestsScene")
target_link_libraries(fbzz_tests_scene
    PRIVATE
        fbzz_engine
        fbzz_physics
)
add_test(NAME fbzz_tests_scene COMMAND fbzz_tests_scene)
add_dependencies(fbzz_tests_scene copy_dlls_to_tests)
