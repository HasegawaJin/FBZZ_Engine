file(GLOB_RECURSE HUB_SOURCES
    src/*.cpp
    src/*.hpp
)

set(HUB_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/hub.rc")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/hub.rc.in"
    "${HUB_RESOURCE}"
    @ONLY
)

add_executable(fbzz_hub WIN32
    ${HUB_SOURCES}
    "${HUB_RESOURCE}"
)

set_target_properties(fbzz_hub PROPERTIES
    OUTPUT_NAME "FBZZHub"
)

target_include_directories(fbzz_hub
    PRIVATE
        src
        "${CMAKE_SOURCE_DIR}/ThirdParty/Stb"
)

target_compile_definitions(fbzz_hub
    PRIVATE
        UNICODE
        _UNICODE
        FBZZ_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(fbzz_hub
    PRIVATE
        imgui
        tomlplusplus
        d3d11
        dxgi
        dwmapi
        shell32
)

add_custom_command(TARGET fbzz_hub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Templates"
        "$<TARGET_FILE_DIR:fbzz_hub>/Templates"
)
