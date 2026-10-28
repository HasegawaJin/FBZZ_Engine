# @file    EnsureSandboxProject.cmake
# @brief   SandboxのC#プロジェクトと実行用メタデータを配置する。
# @author  Hasegawa Jin
# @date    2026-09-21
foreach(REQUIRED SOURCE_DIR DEST_DIR BUILD_DIR MANAGED_STARTER_DIR)
    if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
        message(FATAL_ERROR "EnsureSandboxProject: ${REQUIRED} is required")
    endif()
endforeach()

# @note 既存のプロジェクト設定は初回配置だけで生成する。
if(NOT EXISTS "${DEST_DIR}/ProjectSettings/ProjectSettings.toml")
    file(COPY "${SOURCE_DIR}/" DESTINATION "${DEST_DIR}")
endif()

if(NOT EXISTS "${DEST_DIR}/Sandbox.Scripts.csproj")
    configure_file("${MANAGED_STARTER_DIR}/Starter.Scripts.csproj" "${DEST_DIR}/Sandbox.Scripts.csproj" COPYONLY)
    file(COPY "${MANAGED_STARTER_DIR}/Assets/" DESTINATION "${DEST_DIR}/Assets")
    configure_file("${MANAGED_STARTER_DIR}/AGENTS.md" "${DEST_DIR}/AGENTS.md" COPYONLY)
endif()

# @note 正確なbuild_rootを記録し、配置先の起動はrootの実行ファイルを使う。
file(TO_CMAKE_PATH "${BUILD_DIR}" BUILD_DIR_SLASH)
file(WRITE "${DEST_DIR}/.fbzz_proj"
"[project]\nsettings_path = \"ProjectSettings/ProjectSettings.toml\"\ndefault_scene = \"Assets/Scenes/Main.scene\"\nstandalone_target_name = \"SandboxStandalone\"\nbuild_root = \"${BUILD_DIR_SLASH}\"\n")
