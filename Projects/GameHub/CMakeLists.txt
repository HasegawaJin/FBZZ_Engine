file(GLOB_RECURSE HUB_SOURCES
    Src/*.cpp
    Src/*.hpp
)

add_executable(fbzz_hub WIN32 ${HUB_SOURCES})

set_target_properties(fbzz_hub PROPERTIES
    OUTPUT_NAME "FBZZHub"
)

target_include_directories(fbzz_hub
    PRIVATE Src
)

target_compile_definitions(fbzz_hub
    PRIVATE
        UNICODE
        _UNICODE
)

target_link_libraries(fbzz_hub
    PRIVATE
        imgui
        tomlplusplus
        d3d11
        dxgi
        dwmapi
)
