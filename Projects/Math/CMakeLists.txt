file(GLOB_RECURSE MATH_SOURCES
    src/*.cpp
    include/*.hpp
)

add_library(fbzz_math SHARED ${MATH_SOURCES})
set_target_properties(fbzz_math PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    OUTPUT_NAME "FBZZMath"
)

target_include_directories(fbzz_math
    PUBLIC  include
)
