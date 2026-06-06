file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

add_library(fbzz_editor STATIC ${EDITOR_SOURCES})
set_target_properties(fbzz_editor PROPERTIES
    OUTPUT_NAME "FBZZEditor"
)

set(EDITOR_DIRECTXTEX_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/include")
set(EDITOR_DIRECTXTEX_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Debug/DirectXTex.lib")
set(EDITOR_DIRECTXTEX_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Release/DirectXTex.lib")

set(EDITOR_ASSIMP_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/include")
set(EDITOR_ASSIMP_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/lib/Debug/assimp-vc145-mtd.lib")
set(EDITOR_ASSIMP_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/lib/Release/assimp-vc145-mt.lib")

set(EDITOR_STB_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Stb")

target_compile_definitions(fbzz_editor
    PRIVATE
        NOMINMAX   # Assimp が windows.h を引き込む際に min/max マクロで std::min/max が壊れるのを防ぐ
)

target_include_directories(fbzz_editor
    PUBLIC  include
    PRIVATE src
    PRIVATE ${EDITOR_DIRECTXTEX_INCLUDE}
    PRIVATE ${EDITOR_ASSIMP_INCLUDE}
    PRIVATE ${EDITOR_STB_INCLUDE}
)

target_link_libraries(fbzz_editor
    PUBLIC
        fbzz_engine
        imgui
        imguizmo
        tomlplusplus
        # fbzz_editor は STATIC lib なので直接使う Assimp/DirectXTex シンボルを
        # PUBLIC で伝播させ、fbzz_editor_launcher がリンク時に解決できるようにする
        "$<$<CONFIG:Debug>:${EDITOR_DIRECTXTEX_LIB_DEBUG}>"
        "$<$<CONFIG:Release>:${EDITOR_DIRECTXTEX_LIB_RELEASE}>"
        "$<$<CONFIG:Debug>:${EDITOR_ASSIMP_LIB_DEBUG}>"
        "$<$<CONFIG:Release>:${EDITOR_ASSIMP_LIB_RELEASE}>"
)
