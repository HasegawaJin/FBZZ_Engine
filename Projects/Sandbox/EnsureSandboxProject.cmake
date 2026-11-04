# @file    EnsureSandboxProject.cmake
# @brief   SandboxのC#プロジェクトと実行用メタデータを配置する。
# @author  Hasegawa Jin
# @date    2026-09-21
cmake_minimum_required(VERSION 3.25)
foreach(REQUIRED SOURCE_DIR DEST_DIR BUILD_DIR MANAGED_STARTER_DIR)
    if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
        message(FATAL_ERROR "EnsureSandboxProject: ${REQUIRED} is required")
    endif()
endforeach()

# @note 初回配置でも既存の原本を保持する。原本が存在する場合は雛形の meta だけを移さない。
function(CopyMissingTree source destination)
    file(GLOB_RECURSE seedFiles LIST_DIRECTORIES FALSE RELATIVE "${source}" "${source}/*")
    file(GLOB_RECURSE existingFiles LIST_DIRECTORIES FALSE RELATIVE "${destination}" "${destination}/*")
    foreach(relative IN LISTS seedFiles)
        if(EXISTS "${destination}/${relative}")
            continue()
        endif()
        if(relative MATCHES "\\.meta$")
            string(REGEX REPLACE "\\.meta$" "" assetRelative "${relative}")
            if(assetRelative IN_LIST existingFiles)
                continue()
            endif()
        endif()
        configure_file("${source}/${relative}" "${destination}/${relative}" COPYONLY)
    endforeach()
endfunction()

if(NOT EXISTS "${DEST_DIR}/.fbzz_proj")
    CopyMissingTree("${SOURCE_DIR}/Assets" "${DEST_DIR}/Assets")
    CopyMissingTree("${SOURCE_DIR}/ProjectSettings" "${DEST_DIR}/ProjectSettings")
    if(DEFINED ENGINE_ASSET_DIR)
        CopyMissingTree("${ENGINE_ASSET_DIR}" "${DEST_DIR}/Assets")
    endif()
endif()

if(NOT EXISTS "${DEST_DIR}/Sandbox.Scripts.csproj")
    configure_file("${MANAGED_STARTER_DIR}/Starter.Scripts.csproj" "${DEST_DIR}/Sandbox.Scripts.csproj" COPYONLY)
    CopyMissingTree("${MANAGED_STARTER_DIR}/Assets" "${DEST_DIR}/Assets")
    CopyMissingTree("${MANAGED_STARTER_DIR}/.vscode" "${DEST_DIR}/.vscode")
    foreach(starterFile AGENTS.md global.json Fbzz.ScriptBuild.props)
        if(EXISTS "${MANAGED_STARTER_DIR}/${starterFile}" AND NOT EXISTS "${DEST_DIR}/${starterFile}")
            configure_file("${MANAGED_STARTER_DIR}/${starterFile}" "${DEST_DIR}/${starterFile}" COPYONLY)
        endif()
    endforeach()
endif()

# @note 正確なbuild_rootを記録し、配置先の起動はrootの実行ファイルを使う。
if(NOT EXISTS "${DEST_DIR}/.fbzz_proj")
    file(TO_CMAKE_PATH "${BUILD_DIR}" BUILD_DIR_SLASH)
    file(WRITE "${DEST_DIR}/.fbzz_proj"
"[project]\nsettings_path = \"ProjectSettings/ProjectSettings.toml\"\ndefault_scene = \"Assets/Scenes/Main.scene\"\nstandalone_target_name = \"SandboxStandalone\"\nbuild_root = \"${BUILD_DIR_SLASH}\"\n")
endif()
