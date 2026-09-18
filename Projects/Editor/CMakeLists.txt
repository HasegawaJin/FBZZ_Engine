# @file    CMakeLists.txt
# @brief   エディター本体 (FBZZEditor 静的ライブラリ) のビルド定義。
# @author  Hasegawa Jin
# @date    2026-05-19
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

# @note miniz.c (TinyEXR の zlib バックエンド) は C の tentative definition を使い、C++ TU から include すると C2086 になるので C としてビルドする。
set(EDITOR_MINIZ_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/TinyExr/miniz.c")
set_source_files_properties(${EDITOR_MINIZ_SRC} PROPERTIES
    LANGUAGE C
    # @note サードパーティの警告を抑制する。
    COMPILE_OPTIONS "/W0"
    # @note PCH は C++ 用で、C の TU へ強制 include すると壊れる。
    SKIP_PRECOMPILE_HEADERS ON
)

add_library(FBZZEditor STATIC ${EDITOR_SOURCES} ${EDITOR_MINIZ_SRC})
fbzz_use_std_pch(FBZZEditor)

# @note StbImage.cpp と HdriLoader.cpp はヘッダーオンリー実装の展開場所なので統合しない。miniz.c は念のため明示する。
fbzz_use_unity_build(FBZZEditor EXCLUDE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Import/StbImage.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Import/HdriLoader.cpp"
    "${EDITOR_MINIZ_SRC}"
)
set_target_properties(FBZZEditor PROPERTIES
    OUTPUT_NAME "FBZZEditor"
)

set(EDITOR_DIRECTXTEX_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/include")
set(EDITOR_DIRECTXTEX_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Debug/DirectXTex.lib")
set(EDITOR_DIRECTXTEX_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Release/DirectXTex.lib")

set(EDITOR_ASSIMP_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/include")
set(EDITOR_ASSIMP_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/lib/Debug/assimp-vc145-mtd.lib")
set(EDITOR_ASSIMP_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/lib/Release/assimp-vc145-mt.lib")

set(EDITOR_STB_INCLUDE     "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Stb")
set(EDITOR_TINYEXR_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/TinyExr")
# @note ImNodes の include パスはここに持たない。imnodes ターゲットが PUBLIC で伝播させ、リンクせずにヘッダーだけ見える状態を作らせない。

target_compile_definitions(FBZZEditor
    PRIVATE
        # @note Assimp が引き込む windows.h の min / max マクロから std::min / max を守る。
        NOMINMAX
        # @note NDEBUG では Development を Debug と区別できないので、構成名を焼き込んで cmake --config へ渡す。
        FBZZ_CMAKE_CONFIG="$<CONFIG>"
)

target_include_directories(FBZZEditor
    PUBLIC  include
    PRIVATE src
    PRIVATE ${EDITOR_DIRECTXTEX_INCLUDE}
    PRIVATE ${EDITOR_ASSIMP_INCLUDE}
    PRIVATE ${EDITOR_STB_INCLUDE}
    PRIVATE ${EDITOR_TINYEXR_INCLUDE}
)

target_link_libraries(FBZZEditor
    PUBLIC
        FBZZEngine
        ImGui
        # @note ノードエディターは GraphCanvas / VFX / Animation / BehaviorTree が使う。正式にリンクし、imnodes.cpp の二重取り込みをリンカに弾かせる。
        ImNodes
        ImGuizmo
        TomlPlusPlus
        # @note FBZZEditor は STATIC なので、Assimp / DirectXTex を PUBLIC で伝播させ FBZZEditorLauncher のリンクで解決させる。
        "$<$<CONFIG:Debug>:${EDITOR_DIRECTXTEX_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${EDITOR_DIRECTXTEX_LIB_RELEASE}>"
        "$<$<CONFIG:Debug>:${EDITOR_ASSIMP_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${EDITOR_ASSIMP_LIB_RELEASE}>"
)
