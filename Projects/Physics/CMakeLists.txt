file(GLOB_RECURSE PHYSICS_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

add_library(fbzz_physics STATIC ${PHYSICS_SOURCES})

target_include_directories(fbzz_physics
    PUBLIC  include
    PRIVATE src
)

target_link_libraries(fbzz_physics
    PUBLIC  fbzz_math
    PRIVATE tomlplusplus
)
