# FBZZ Engine
# VFXEditorLauncher/CMakeLists.txt
# 独立VFXEditor実行ファイルのビルド定義
add_executable(FBZZVFXEditor WIN32
    src/main.cpp
)

# DPI 認識をプロセス起動時に確定させる (未適用だと高 DPI 環境で UI 全体が滲む)。
fbzz_apply_app_manifest(FBZZVFXEditor)

set_target_properties(FBZZVFXEditor PROPERTIES
    OUTPUT_NAME "FBZZVFXEditor"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Editor"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Editor"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Editor"
)

target_compile_definitions(FBZZVFXEditor PRIVATE UNICODE _UNICODE TOML_EXCEPTIONS=0)
target_link_libraries(FBZZVFXEditor PRIVATE
    FBZZEditor
    FBZZEngine
    FBZZPhysics
    TomlPlusPlus
    user32
    shell32
)

# 両exeは同じ出力フォルダーを共有する。EditorLauncherのbundle処理完了後にリンクすることで、
# 並列POST_BUILDが同じassetsを削除・コピーする競合を避けつつ、単体ターゲットビルドでもDLLを保証する。
add_dependencies(FBZZVFXEditor FBZZEditorLauncher)
