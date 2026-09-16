file(GLOB_RECURSE PHYSICS_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

add_library(FBZZPhysics SHARED ${PHYSICS_SOURCES})
set_target_properties(FBZZPhysics PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    OUTPUT_NAME "FBZZPhysics"
    EXPORT_NAME "Physics"
)

target_include_directories(FBZZPhysics
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE src
)

target_link_libraries(FBZZPhysics
    PUBLIC  FBZZMath
    PRIVATE TomlPlusPlus
)
