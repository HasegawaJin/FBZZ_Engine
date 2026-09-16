file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

# miniz (TinyEXR の zlib バックエンド) を C ソースとして追加する。
# WHY: miniz.c は C の tentative definition を使っているため、
#      C++ 翻訳単位から #include すると MSVC C2086 (再定義) になる。
#      明示的に C ファイルとしてビルドすることで問題を回避する。
set(EDITOR_MINIZ_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/TinyExr/miniz.c")
set_source_files_properties(${EDITOR_MINIZ_SRC} PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "/W0"       # サードパーティ警告を抑制
    SKIP_PRECOMPILE_HEADERS ON  # PCH は C++ 用。C 翻訳単位へ強制 include すると壊れる
)

add_library(FBZZEditor STATIC ${EDITOR_SOURCES} ${EDITOR_MINIZ_SRC})
fbzz_use_std_pch(FBZZEditor)

# StbImage.cpp と HdriLoader.cpp はヘッダーオンリー実装の展開場所なので統合しない。
# miniz.c は C なので CMake が別バッチにするが、明示しておく。
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
# NOTE: ImNodes のヘッダ検索パスは imnodes ターゲットが PUBLIC で伝播させる。
#       ここで手動の include パスを持つと「.lib はリンクしていないがヘッダは見える」
#       という状態を作れてしまい、#include <imnodes.cpp> で実装を各 .obj へ抱え込む
#       回避策が成立してしまう (実際そうなっていた)。パスは持たない。

target_compile_definitions(FBZZEditor
    PRIVATE
        NOMINMAX   # Assimp が windows.h を引き込む際に min/max マクロで std::min/max が壊れるのを防ぐ
        # WHY: #ifdef NDEBUG は Development (NDEBUG なし・最適化 ON) を Debug と誤判定する。
        #      $<CONFIG> をビルド時に文字列として焼き込み、cmake --config に正確に渡す。
        FBZZ_CMAKE_CONFIG="$<CONFIG>"
        # SDK内Editorは共有assetを読み取り専用で利用し、source treeだけがHLSLを再コンパイルする。
        "FBZZ_SOURCE_TREE_BUILD=$<BOOL:${FBZZ_ENABLE_SOURCE_REBUILD}>"
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
        # WHY PUBLIC か: ノードエディタは GraphCanvas / VFX / Animation / BehaviorTree の
        #      4 か所が使う。以前は imnodes.lib をリンクせず AnimationGraphPanel.cpp が
        #      #include <imnodes.cpp> で実装を自分の .obj へ抱えていたため、
        #      「他のどの .cpp も同じことをしてはならない」という規約をヘッダの
        #      コメントで守るしかなかった。正式リンクにすれば二重定義はリンカが弾く。
        ImNodes
        ImGuizmo
        TomlPlusPlus
# FBZZEditor は STATIC lib なので直接使う Assimp/DirectXTex シンボルを
# PUBLIC で伝播させ、FBZZEditorLauncher がリンク時に解決できるようにする
        "$<$<CONFIG:Debug>:${EDITOR_DIRECTXTEX_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${EDITOR_DIRECTXTEX_LIB_RELEASE}>"
        "$<$<CONFIG:Debug>:${EDITOR_ASSIMP_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${EDITOR_ASSIMP_LIB_RELEASE}>"
)
