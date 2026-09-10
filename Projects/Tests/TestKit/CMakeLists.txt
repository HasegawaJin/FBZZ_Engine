# FBZZTestKit — テストの土台。
#
# Math 層と Engine 層で 2 つに分けている。
# WHY: Math / Physics のテストに Engine をリンクさせると、依存方向
#      (Engine -> Physics -> Math) がテスト側から逆流する。Engine の静的状態を
#      触る道具は FBZZTestKitEngine に隔離し、必要なスイートだけが引く。

add_library(FBZZTestKit STATIC
    src/Approx.cpp
    src/Console.cpp
    src/Deterministic.cpp
    src/Fixture.cpp
    src/Print.cpp
    src/TempDir.cpp
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

# Editor 層。EditorContext を «必要な分だけ» 組み立てる土台を置く。
# WHY 分けるか: Engine のテストへ Editor をリンクさせると、依存方向
#      (Editor -> Engine) がテスト側から逆流する。Editor を触るスイートだけが引く。
add_library(FBZZTestKitEditor STATIC
    src/Editor/EditorFixture.cpp
)
target_link_libraries(FBZZTestKitEditor PUBLIC FBZZTestKitEngine FBZZEditor)
set_target_properties(FBZZTestKitEditor PROPERTIES OUTPUT_NAME "FBZZTestKitEditor" FOLDER "Tests")
