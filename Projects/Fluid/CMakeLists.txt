# @file    CMakeLists.txt
# @brief   流体ライブラリのビルド定義。
# @author  Hasegawa Jin
# @date    2026-09-17
# @note 流体のファイル形式は Engine が扱う。Fluid の依存は Math に限定する。
# @see Docs/design/fluid-library.md
file(GLOB_RECURSE FLUID_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.hpp
    include/*.hpp
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${FLUID_SOURCES})

add_library(FBZZFluid SHARED ${FLUID_SOURCES})
set_target_properties(FBZZFluid PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    OUTPUT_NAME "FBZZFluid"
    EXPORT_NAME "Fluid"
)

target_include_directories(FBZZFluid
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE src
)

target_link_libraries(FBZZFluid
    PUBLIC FBZZMath
)
