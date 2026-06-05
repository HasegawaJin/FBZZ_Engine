# Dear ImGui (docking branch)
add_library(imgui SHARED
    ImGui/imgui.cpp
    ImGui/imgui_draw.cpp
    ImGui/imgui_tables.cpp
    ImGui/imgui_widgets.cpp
    ImGui/backends/imgui_impl_win32.cpp
    ImGui/backends/imgui_impl_dx11.cpp
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

# toml++ (header-only, exception-free モードで使用)
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus INTERFACE TomlPlusPlus/include)
target_compile_definitions(tomlplusplus INTERFACE TOML_EXCEPTIONS=0)

# ImGuizmo
add_subdirectory(ImGuizmo)
