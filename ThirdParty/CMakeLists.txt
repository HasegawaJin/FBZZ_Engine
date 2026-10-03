# @file    CMakeLists.txt
# @brief   Vendored library targets and shared dependency contracts.
# @author  Hasegawa Jin
# @date    2026-05-15
# @note Dear ImGui uses the vendored docking branch.
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

# @note Inherited definitions evaluate in the consumer context; ImNodes defines this macro before imgui.h itself.
# @see https://cmake.org/cmake/help/latest/manual/cmake-generator-expressions.7.html#target-properties TARGET_PROPERTY:prop consuming-target evaluation.
target_compile_definitions(ImGui
    PUBLIC
        "$<$<NOT:$<STREQUAL:$<TARGET_PROPERTY:NAME>,ImNodes>>:IMGUI_DEFINE_MATH_OPERATORS>"
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

# @note AnimationGraphPanel のノード編集は Dear ImGui と同じ ThirdParty レイヤーの ImNodes を使う。
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

# @note toml++ は例外を使わない header-only 依存として公開する。
add_library(TomlPlusPlus INTERFACE)
set_target_properties(TomlPlusPlus PROPERTIES EXPORT_NAME "TomlPlusPlus")
target_include_directories(TomlPlusPlus INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/TomlPlusPlus/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_compile_definitions(TomlPlusPlus INTERFACE TOML_EXCEPTIONS=0)

add_subdirectory(ImGuizmo)

# @note PIX headers and runtime remain a private DX12 dependency, not an installed public SDK interface.
# @see https://github.com/microsoft/PixEvents Official WinPixEventRuntime SDK distribution.
if(WIN32)
    add_library(WinPixEventRuntime SHARED IMPORTED GLOBAL)
    set_target_properties(WinPixEventRuntime PROPERTIES
        IMPORTED_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/WinPixEventRuntime/lib/x64/WinPixEventRuntime.lib"
        IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/WinPixEventRuntime/bin/x64/WinPixEventRuntime.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/WinPixEventRuntime/include"
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/WinPixEventRuntime/include"
    )
endif()

# @note GoogleTest/GoogleMock の集約ソースを直接ビルドし、上流の CRT 設定と install 規則を持ち込まずルートを継承する。
# @see ThirdParty/GoogleTest/VERSION Vendored test-library version.
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

    # @note 標準 GTest::* alias を保ち、gtest_discover_tests と一般的なリンク記法を利用可能にする。
    add_library(GTest::gtest      ALIAS GoogleTest)
    add_library(GTest::gtest_main ALIAS GoogleTestMain)
    add_library(GTest::gmock      ALIAS GoogleMock)
    add_library(GTest::gmock_main ALIAS GoogleMockMain)
endif()
