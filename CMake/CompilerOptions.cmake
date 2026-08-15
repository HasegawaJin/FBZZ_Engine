add_library(FBZZCompilerOptions INTERFACE)

target_compile_options(FBZZCompilerOptions INTERFACE
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

# Debug/Development/Release 設定
# Development は NDEBUG なし → アサート・デバッグツールが Release と同等の速度で動作する
target_compile_definitions(FBZZCompilerOptions INTERFACE
    $<$<CONFIG:Debug>:FBZZ_DEBUG>
    $<$<CONFIG:Development>:FBZZ_DEVELOPMENT>
    $<$<CONFIG:Release>:FBZZ_RELEASE NDEBUG>
)
