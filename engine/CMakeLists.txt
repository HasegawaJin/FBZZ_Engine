file(GLOB_RECURSE ENGINE_SOURCES
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

add_library(fbzz_engine STATIC ${ENGINE_SOURCES})

target_include_directories(fbzz_engine
    PUBLIC  include
    PRIVATE src
)

target_link_libraries(fbzz_engine
    PUBLIC
        fbzz_physics
)
