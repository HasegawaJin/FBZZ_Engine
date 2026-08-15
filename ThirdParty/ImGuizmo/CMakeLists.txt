add_library(ImGuizmo STATIC
    src/ImGuizmo.cpp
)

target_include_directories(ImGuizmo PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_link_libraries(ImGuizmo PUBLIC ImGui)
set_target_properties(ImGuizmo PROPERTIES OUTPUT_NAME "imguizmo")
