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
        ImGui
        ImGui/backends
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
set_target_properties(ImGui PROPERTIES OUTPUT_NAME "imgui")

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
target_include_directories(TomlPlusPlus INTERFACE TomlPlusPlus/include)
target_compile_definitions(TomlPlusPlus INTERFACE TOML_EXCEPTIONS=0)

# ImGuizmo
add_subdirectory(ImGuizmo)
