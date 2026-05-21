file(GLOB_RECURSE MATH_SOURCES
    src/*.cpp
    include/*.hpp
)

add_library(fbzz_math STATIC ${MATH_SOURCES})

target_include_directories(fbzz_math
    PUBLIC  include
)
