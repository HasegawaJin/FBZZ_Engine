# FBZZ Engine
# EnsureSandboxProject.cmake | Sandbox
# Sandbox 実行先のユーザープロジェクトを初回だけ作成し、.fbzz_proj を常に更新する
#
# WHY (.fbzz_proj を毎回上書き):
#   テンプレートをそのまま copy_directory すると {{...}} プレースホルダが残り、
#   LoadRuntimeBuildMetadata が build_root を解決できず RuntimeBuild が失敗する。
#   BUILD_DIR (= CMAKE_BINARY_DIR) はビルド構成が変わるたびに変わるため、
#   毎 cmake --build 実行時に正確な値で上書きすることで常に最新を維持する。

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED DEST_DIR OR NOT DEFINED BUILD_DIR)
    message(FATAL_ERROR "SOURCE_DIR, DEST_DIR, and BUILD_DIR are required")
endif()

# 初回のみ: テンプレートからプロジェクト全体をコピー
if(NOT EXISTS "${DEST_DIR}/ProjectSettings/ProjectSettings.toml")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SOURCE_DIR}" "${DEST_DIR}"
        RESULT_VARIABLE COPY_RESULT
    )
    if(NOT COPY_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to create SandboxProject from ${SOURCE_DIR}")
    endif()
endif()

# 常に: build_root を含む正確な .fbzz_proj を書き込む
# WHY: copy_directory で入るテンプレートの .fbzz_proj は {{...}} プレースホルダのままなので
#      ToolchainLocator が build.config を見つけられない。
#      BUILD_DIR (CMAKE_BINARY_DIR) を直接書き込むことでフォールバック不要になる。
file(TO_CMAKE_PATH "${BUILD_DIR}" BUILD_DIR_SLASH)
file(WRITE "${DEST_DIR}/.fbzz_proj"
"[project]
settings_path          = \"ProjectSettings/ProjectSettings.toml\"
default_scene          = \"Assets/Scenes/Main.fbzz\"
standalone_target_name = \"SandboxStandalone\"
build_root             = \"${BUILD_DIR_SLASH}\"
")
