# @file    CMakeLists.txt
# @brief   テスト補助ライブラリを依存する層ごとに定義する。
# @author  Hasegawa Jin
# @date    2026-09-04

# @note Math と Physics のテストを Engine の静的状態から切り離す。
add_library(FBZZTestKit STATIC
    src/Assertions/Approx.cpp
    src/Output/Console.cpp
    src/Support/Deterministic.cpp
    src/Fixture.cpp
    src/Output/Print.cpp
    src/Support/TempDir.cpp
)
target_include_directories(FBZZTestKit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(FBZZTestKit PUBLIC GTest::gmock FBZZMath)
set_target_properties(FBZZTestKit PROPERTIES OUTPUT_NAME "FBZZTestKit" FOLDER "Tests")

add_library(FBZZTestKitPhysics STATIC
    src/Physics/SupportShapes.cpp
)
target_link_libraries(FBZZTestKitPhysics PUBLIC FBZZTestKit FBZZPhysics)
set_target_properties(FBZZTestKitPhysics PROPERTIES OUTPUT_NAME "FBZZTestKitPhysics" FOLDER "Tests")

add_library(FBZZTestKitEngine STATIC
    src/Engine/EngineFixture.cpp
    src/Engine/LogSinks.cpp
)
target_link_libraries(FBZZTestKitEngine PUBLIC FBZZTestKit FBZZEngine)
set_target_properties(FBZZTestKitEngine PROPERTIES OUTPUT_NAME "FBZZTestKitEngine" FOLDER "Tests")

# @note EditorContext を使うフィクスチャは Editor を検証するスイートだけにリンクする。
add_library(FBZZTestKitEditor STATIC
    src/Editor/EditorFixture.cpp
)
target_link_libraries(FBZZTestKitEditor PUBLIC FBZZTestKitEngine FBZZEditor)
set_target_properties(FBZZTestKitEditor PROPERTIES OUTPUT_NAME "FBZZTestKitEditor" FOLDER "Tests")
