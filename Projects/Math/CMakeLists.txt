file(GLOB_RECURSE MATH_SOURCES
    src/*.cpp
    include/*.hpp
)

add_library(FBZZMath SHARED ${MATH_SOURCES})
set_target_properties(FBZZMath PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    OUTPUT_NAME "FBZZMath"
    EXPORT_NAME "Math"
)

target_include_directories(FBZZMath
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
