# Dear ImGui (docking branch)
add_library(imgui SHARED
    ImGui/imgui.cpp
    ImGui/imgui_draw.cpp
    ImGui/imgui_tables.cpp
    ImGui/imgui_widgets.cpp
    ImGui/backends/imgui_impl_win32.cpp
    ImGui/backends/imgui_impl_dx11.cpp
    ImGui/backends/imgui_impl_dx12.cpp
)

target_include_directories(imgui
    PUBLIC
        ImGui
        ImGui/backends
)

target_compile_definitions(imgui
    PUBLIC
        IMGUI_DEFINE_MATH_OPERATORS
    PRIVATE
        FBZZ_IMGUI_EXPORTS
    INTERFACE
        FBZZ_IMGUI_IMPORTS
)

target_link_libraries(imgui PRIVATE d3d12 dxgi dxguid)

# imnodes
# WHY: AnimationGraphPanel は AnimatorComponent をノードグラフ編集するため、
#      Dear ImGui と同じ ThirdParty レイヤーでノードエディタ実装をビルドする。
add_library(imnodes STATIC
    ImNodes/imnodes.cpp
)
target_include_directories(imnodes
    PUBLIC
        ImNodes
)
target_link_libraries(imnodes
    PUBLIC
        imgui
)

# toml++ (header-only, exception-free モードで使用)
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus INTERFACE TomlPlusPlus/include)
target_compile_definitions(tomlplusplus INTERFACE TOML_EXCEPTIONS=0)

# ImGuizmo
add_subdirectory(ImGuizmo)
