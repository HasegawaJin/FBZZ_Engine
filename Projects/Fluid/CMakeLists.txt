# 流体の «数式» 層。Physics の兄弟で、互いを知らない (Docs/design/fluid-library.md)。
# WHY TomlPlusPlus を持たないか: ファイル形式は Engine の担当。リンカーがこの境界を守る。
file(GLOB_RECURSE FLUID_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

add_library(FBZZFluid SHARED ${FLUID_SOURCES})
set_target_properties(FBZZFluid PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
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
