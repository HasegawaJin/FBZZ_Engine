# @file    StageFBZZSDK.cmake
# @brief   ビルドと実行時配置の構成。
# @author  Hasegawa Jin
# @date    2026-09-21
# @note FBZZ Engine
# @note StageFBZZSDK.cmake | CMake
# @note install export外の構成別runtimeとEditor toolを共有SDKへ同期する

foreach(REQUIRED SDK_ROOT CONFIG)
    if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
        message(FATAL_ERROR "StageFBZZSDK: ${REQUIRED} is required")
    endif()
endforeach()

if(NOT CONFIG MATCHES "^(Debug|Development|Release)$")
    message(FATAL_ERROR "StageFBZZSDK: unsupported configuration ${CONFIG}")
endif()

if(PUBLISH_BEGIN)
    if(NOT EXISTS "${MANIFEST_TEMPLATE}")
        message(FATAL_ERROR "StageFBZZSDK: manifest template is missing: ${MANIFEST_TEMPLATE}")
    endif()
    file(READ "${MANIFEST_TEMPLATE}" MANIFEST_TEXT)
    string(REGEX MATCH "fingerprint = \"([0-9a-f]+)\"" FINGERPRINT_MATCH "${MANIFEST_TEXT}")
    set(RUNTIME_FINGERPRINT "${CMAKE_MATCH_1}")
    if(RUNTIME_FINGERPRINT STREQUAL "")
        message(FATAL_ERROR "StageFBZZSDK: runtime fingerprint is missing")
    endif()
    file(MAKE_DIRECTORY "${SDK_ROOT}/cmake/FBZZ/configurations")
    file(WRITE "${SDK_ROOT}/cmake/FBZZ/runtime-manifest.toml" "${MANIFEST_TEXT}")
    foreach(PUBLISHED_CONFIG Debug Development Release)
        set(RECORD "${SDK_ROOT}/cmake/FBZZ/configurations/${PUBLISHED_CONFIG}.toml")
        set(RECORD_TEXT "")
        if(EXISTS "${RECORD}")
            file(READ "${RECORD}" RECORD_TEXT)
        endif()
        string(FIND "${RECORD_TEXT}" "fingerprint = \"${RUNTIME_FINGERPRINT}\"" CONTRACT_POS)
        # @note Shared package files may change during install; successful old records are only restored after their hashes are rechecked.
        file(REMOVE "${RECORD}.previous")
        if(NOT PUBLISHED_CONFIG STREQUAL CONFIG AND NOT CONTRACT_POS EQUAL -1 AND RECORD_TEXT MATCHES "validated = true")
            file(WRITE "${RECORD}.previous" "${RECORD_TEXT}")
        endif()
        set(RECORD_TEXT "\n[configurations.${PUBLISHED_CONFIG}]\nfingerprint = \"${RUNTIME_FINGERPRINT}\"\nvalidated = false\n")
        file(WRITE "${RECORD}" "${RECORD_TEXT}")
        string(APPEND MANIFEST_TEXT "${RECORD_TEXT}")
    endforeach()
    file(WRITE "${SDK_ROOT}/fbzz-sdk.toml" "${MANIFEST_TEXT}")
    return()
endif()

foreach(REQUIRED ASSIMP_DLL EDITOR_DIR)
    if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
        message(FATAL_ERROR "StageFBZZSDK: ${REQUIRED} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${SDK_ROOT}/bin/${CONFIG}" "${SDK_ROOT}/tools/${CONFIG}/Editor")

# @note Assimp は CMake target ではなく install export に含まれないため、runtime を明示配置する。
# @note ImGui は FBZZ::ImGui の install 処理が bin へ配置するため、ここでは重複コピーしない。
file(COPY "${ASSIMP_DLL}" DESTINATION "${SDK_ROOT}/bin/${CONFIG}")

file(COPY "${EDITOR_DIR}/" DESTINATION "${SDK_ROOT}/tools/${CONFIG}/Editor")

if(DEFINED PIX_RUNTIME_DLL AND NOT "${PIX_RUNTIME_DLL}" STREQUAL "")
    if(NOT EXISTS "${PIX_RUNTIME_DLL}")
        message(FATAL_ERROR "StageFBZZSDK: PIX runtime is missing: ${PIX_RUNTIME_DLL}")
    endif()
    # @note Do not depend on a previous Editor POST_BUILD copy for the imported event runtime.
    file(COPY "${PIX_RUNTIME_DLL}" DESTINATION "${SDK_ROOT}/bin/${CONFIG}")
endif()

if(DX12_ENABLED)
    foreach(REQUIRED AGILITY_ROOT DXC_ROOT)
        if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
            message(FATAL_ERROR "StageFBZZSDK: ${REQUIRED} is required for DX12")
        endif()
    endforeach()
    # @note SDK が所有する D3D12 子ディレクトリだけ同期し、旧 Layers を Release へ残さない。
    # @see https://microsoft.github.io/DirectX-Specs/d3d/D3D12Redistributable.html#d3d12-debug-layer
    foreach(DESTINATION "${SDK_ROOT}/bin/${CONFIG}")
        file(REMOVE_RECURSE "${DESTINATION}/D3D12")
        file(MAKE_DIRECTORY "${DESTINATION}/D3D12")
        file(COPY "${AGILITY_ROOT}/build/native/bin/x64/D3D12Core.dll" DESTINATION "${DESTINATION}/D3D12")
        if(NOT CONFIG STREQUAL "Release")
            file(COPY "${AGILITY_ROOT}/build/native/bin/x64/d3d12SDKLayers.dll" DESTINATION "${DESTINATION}/D3D12")
        endif()
        file(COPY "${DXC_ROOT}/bin/x64/dxcompiler.dll" "${DXC_ROOT}/bin/x64/dxil.dll" DESTINATION "${DESTINATION}")
    endforeach()
    file(MAKE_DIRECTORY "${SDK_ROOT}/tools/${CONFIG}/DXC")
    file(COPY "${DXC_ROOT}/bin/x64/dxc.exe" "${DXC_ROOT}/bin/x64/dxcompiler.dll" "${DXC_ROOT}/bin/x64/dxil.dll"
        DESTINATION "${SDK_ROOT}/tools/${CONFIG}/DXC")
endif()

# @note SDK/bin is authoritative even when an unchanged Editor executable skipped its POST_BUILD runtime copy.
set(EDITOR_RUNTIME_DLLS FBZZMath.dll FBZZPhysics.dll FBZZFluid.dll FBZZEngine.dll FBZZCore.dll FBZZGraphics.dll imgui.dll)
if(CONFIG STREQUAL "Debug")
    list(APPEND EDITOR_RUNTIME_DLLS assimp-vc145-mtd.dll)
else()
    list(APPEND EDITOR_RUNTIME_DLLS assimp-vc145-mt.dll)
endif()
foreach(RUNTIME_DLL IN LISTS EDITOR_RUNTIME_DLLS)
    if(NOT EXISTS "${SDK_ROOT}/bin/${CONFIG}/${RUNTIME_DLL}")
        message(FATAL_ERROR "StageFBZZSDK: installed runtime is missing: bin/${CONFIG}/${RUNTIME_DLL}")
    endif()
endforeach()
file(GLOB INSTALLED_RUNTIME_DLLS LIST_DIRECTORIES false "${SDK_ROOT}/bin/${CONFIG}/*.dll")
# @see https://cmake.org/cmake/help/latest/command/file.html#copy-file ONLY_IF_DIFFERENT compares contents rather than source timestamps.
foreach(INSTALLED_RUNTIME_DLL IN LISTS INSTALLED_RUNTIME_DLLS)
    get_filename_component(RUNTIME_DLL "${INSTALLED_RUNTIME_DLL}" NAME)
    file(COPY_FILE "${INSTALLED_RUNTIME_DLL}" "${SDK_ROOT}/tools/${CONFIG}/Editor/${RUNTIME_DLL}" ONLY_IF_DIFFERENT)
endforeach()
file(REMOVE_RECURSE "${SDK_ROOT}/tools/${CONFIG}/Editor/D3D12")
if(EXISTS "${SDK_ROOT}/bin/${CONFIG}/D3D12")
    file(COPY "${SDK_ROOT}/bin/${CONFIG}/D3D12" DESTINATION "${SDK_ROOT}/tools/${CONFIG}/Editor")
endif()

# @note SDK 版 Editor の告知は開発出力の POST_BUILD 履歴によらず公開済み原本から配置する。
if(EXISTS "${SDK_ROOT}/share/fbzz/licenses")
    file(COPY "${SDK_ROOT}/share/fbzz/licenses/" DESTINATION "${SDK_ROOT}/tools/${CONFIG}/Editor/EngineLicenses")
endif()

# @note 構成別の公開時刻。作業世代は毎回同じIDへ上書きされるため、IDだけでは最新性を判定できない。
# @note install/file(COPY)は内容が同じfileをcopyせずmtimeも更新しないので、manifestや
# @note binaryの時刻は公開時刻として信用できない。公開のたびに必ず書き換わるstampを置く。
string(TIMESTAMP SDK_PUBLISH_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
file(WRITE "${SDK_ROOT}/bin/${CONFIG}/published.stamp" "${SDK_PUBLISH_TIME}\n")
