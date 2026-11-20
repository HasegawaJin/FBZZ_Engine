# @file    CMakeLists.txt
# @brief   物理ライブラリのビルド定義。
# @author  Hasegawa Jin
# @date    2026-05-15
file(GLOB_RECURSE PHYSICS_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.hpp
    include/*.hpp
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${PHYSICS_SOURCES})

add_library(FBZZPhysics SHARED ${PHYSICS_SOURCES})
set_target_properties(FBZZPhysics PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
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
