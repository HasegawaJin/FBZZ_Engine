# FBZZ Engine
# EnsureSandboxProject.cmake | Sandbox
# Sandbox 実行先のユーザープロジェクトを初回だけ作成する
#
# SandboxProject は実行中に Main.fbzz などが保存される作業領域である。
# post-build のたびにテンプレートを上書きするとユーザー編集が消えるため、
# プロジェクト設定が存在しない初回のみ標準テンプレートをコピーする。

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED DEST_DIR)
    message(FATAL_ERROR "SOURCE_DIR and DEST_DIR are required")
endif()

if(NOT EXISTS "${DEST_DIR}/ProjectSettings/ProjectSettings.toml")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SOURCE_DIR}" "${DEST_DIR}"
        RESULT_VARIABLE COPY_RESULT
    )
    if(NOT COPY_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to create SandboxProject from ${SOURCE_DIR}")
    endif()
endif()
