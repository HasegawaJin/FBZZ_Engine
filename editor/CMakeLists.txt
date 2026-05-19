file(GLOB_RECURSE EDITOR_SOURCES
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

add_library(fbzz_editor STATIC ${EDITOR_SOURCES})

target_include_directories(fbzz_editor
    PUBLIC  include
    PRIVATE src
)

target_link_libraries(fbzz_editor
    PUBLIC
        fbzz_engine
        imgui
        tomlplusplus
)
