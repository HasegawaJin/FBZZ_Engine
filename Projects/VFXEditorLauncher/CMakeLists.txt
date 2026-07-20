# FBZZ Engine
# VFXEditorLauncher/CMakeLists.txt
# 独立VFXEditor実行ファイルのビルド定義
add_executable(fbzz_vfx_editor WIN32
    src/main.cpp
)

set_target_properties(fbzz_vfx_editor PROPERTIES
    OUTPUT_NAME "FBZZVFXEditor"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Editor"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Editor"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Editor"
)

target_compile_definitions(fbzz_vfx_editor PRIVATE UNICODE _UNICODE TOML_EXCEPTIONS=0)
target_link_libraries(fbzz_vfx_editor PRIVATE
    fbzz_editor
    fbzz_engine
    fbzz_physics
    tomlplusplus
    user32
    shell32
)

# 両exeは同じ出力フォルダーを共有する。EditorLauncherのbundle処理完了後にリンクすることで、
# 並列POST_BUILDが同じassetsを削除・コピーする競合を避けつつ、単体ターゲットビルドでもDLLを保証する。
add_dependencies(fbzz_vfx_editor fbzz_editor_launcher)
