file(GLOB_RECURSE SANDBOX_SOURCES
    src/*.cpp
    src/*.hpp
)

add_executable(sandbox ${SANDBOX_SOURCES})

target_link_libraries(sandbox
    PRIVATE
        fbzz_engine
        fbzz_physics
)