add_library(fbzz_compiler_options INTERFACE)

target_compile_options(fbzz_compiler_options INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4 /WX         # 警告レベル4, 警告をエラー扱い
        /permissive-    # 準拠モード
        /utf-8
    >
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
        -Wall -Wextra -Wpedantic
        -Werror
    >
)

# Debug/Release 設定
target_compile_definitions(fbzz_compiler_options INTERFACE
    $<$<CONFIG:Debug>:FBZZ_DEBUG>
    $<$<CONFIG:Release>:FBZZ_RELEASE NDEBUG>
)
