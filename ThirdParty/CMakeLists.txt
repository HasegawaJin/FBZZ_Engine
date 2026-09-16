# Dear ImGui (docking branch)
add_library(ImGui SHARED
    ImGui/imgui.cpp
    ImGui/imgui_draw.cpp
    ImGui/imgui_tables.cpp
    ImGui/imgui_widgets.cpp
    ImGui/backends/imgui_impl_win32.cpp
    ImGui/backends/imgui_impl_dx11.cpp
    ImGui/backends/imgui_impl_dx12.cpp
)

target_include_directories(ImGui
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ImGui>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ImGui/backends>"
        "$<INSTALL_INTERFACE:include/ThirdParty/ImGui>"
        "$<INSTALL_INTERFACE:include/ThirdParty/ImGui/backends>"
)

target_compile_definitions(ImGui
    PUBLIC
        IMGUI_DEFINE_MATH_OPERATORS
    PRIVATE
        FBZZ_IMGUI_EXPORTS
    INTERFACE
        FBZZ_IMGUI_IMPORTS
)

target_link_libraries(ImGui PRIVATE d3d12 dxgi dxguid)
set_target_properties(ImGui PROPERTIES
    OUTPUT_NAME "imgui"
    EXPORT_NAME "ImGui"
)

# imnodes
# WHY: AnimationGraphPanel は AnimatorComponent をノードグラフ編集するため、
#      Dear ImGui と同じ ThirdParty レイヤーでノードエディタ実装をビルドする。
add_library(ImNodes STATIC
    ImNodes/imnodes.cpp
)
target_include_directories(ImNodes
    PUBLIC
        ImNodes
)
target_link_libraries(ImNodes
    PUBLIC
        ImGui
)
set_target_properties(ImNodes PROPERTIES OUTPUT_NAME "imnodes")

# toml++ (header-only, exception-free モードで使用)
add_library(TomlPlusPlus INTERFACE)
set_target_properties(TomlPlusPlus PROPERTIES EXPORT_NAME "TomlPlusPlus")
target_include_directories(TomlPlusPlus INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/TomlPlusPlus/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_compile_definitions(TomlPlusPlus INTERFACE TOML_EXCEPTIONS=0)

# ImGuizmo
add_subdirectory(ImGuizmo)

# GoogleTest / GoogleMock (vendored: ThirdParty/GoogleTest/VERSION を参照)
# WHY: FetchContent はネット到達性に依存し、上流の CMake が CRT 設定と install 規則を
#      持ち込む。実体として要るのは gtest-all.cc / gmock-all.cc の 2 TU だけなので、
#      ImNodes と同じくソースを直接ビルドし、ルートの CRT 設定を暗黙に継承させる。
if(FBZZ_BUILD_TESTS)
    set(FBZZ_GTEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/GoogleTest")

    add_library(GoogleTest STATIC "${FBZZ_GTEST_ROOT}/googletest/src/gtest-all.cc")
    target_include_directories(GoogleTest
        PUBLIC  "${FBZZ_GTEST_ROOT}/googletest/include"
        PRIVATE "${FBZZ_GTEST_ROOT}/googletest"
    )
    set_target_properties(GoogleTest PROPERTIES OUTPUT_NAME "gtest" FOLDER "ThirdParty")

    add_library(GoogleTestMain STATIC "${FBZZ_GTEST_ROOT}/googletest/src/gtest_main.cc")
    target_link_libraries(GoogleTestMain PUBLIC GoogleTest)
    set_target_properties(GoogleTestMain PROPERTIES OUTPUT_NAME "gtest_main" FOLDER "ThirdParty")

    add_library(GoogleMock STATIC "${FBZZ_GTEST_ROOT}/googlemock/src/gmock-all.cc")
    target_include_directories(GoogleMock
        PUBLIC  "${FBZZ_GTEST_ROOT}/googlemock/include"
        PRIVATE "${FBZZ_GTEST_ROOT}/googlemock"
    )
    target_link_libraries(GoogleMock PUBLIC GoogleTest)
    set_target_properties(GoogleMock PROPERTIES OUTPUT_NAME "gmock" FOLDER "ThirdParty")

    add_library(GoogleMockMain STATIC "${FBZZ_GTEST_ROOT}/googlemock/src/gmock_main.cc")
    target_link_libraries(GoogleMockMain PUBLIC GoogleMock)
    set_target_properties(GoogleMockMain PROPERTIES OUTPUT_NAME "gmock_main" FOLDER "ThirdParty")

    # find_package(GTest) と同じ名前を張り、CMake 同梱の GoogleTest モジュール
    # (gtest_discover_tests) と一般的なリンク記法をそのまま使えるようにする。
    add_library(GTest::gtest      ALIAS GoogleTest)
    add_library(GTest::gtest_main ALIAS GoogleTestMain)
    add_library(GTest::gmock      ALIAS GoogleMock)
    add_library(GTest::gmock_main ALIAS GoogleMockMain)
endif()
