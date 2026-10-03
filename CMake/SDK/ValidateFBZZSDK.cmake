# @file    ValidateFBZZSDK.cmake
# @brief   SDK staging 後の必須レイアウトと package 契約の検証。
# @author  Hasegawa Jin
# @date    2026-07-15

foreach(REQUIRED SDK_ROOT SDK_ID CONFIG)
    if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
        message(FATAL_ERROR "ValidateFBZZSDK: ${REQUIRED} is required")
    endif()
endforeach()

file(READ "${SDK_ROOT}/fbzz-sdk.toml" SDK_MANIFEST_TEXT)
string(REGEX MATCH "schema = ([0-9]+)" SCHEMA_MATCH "${SDK_MANIFEST_TEXT}")
if(NOT CMAKE_MATCH_1 STREQUAL "2")
    message(FATAL_ERROR "FBZZ SDK validation failed: unsupported manifest schema")
endif()
string(REGEX MATCH "dx12_enabled = (true|false)" DX12_MATCH "${SDK_MANIFEST_TEXT}")
set(DX12_ENABLED "${CMAKE_MATCH_1}")
if(DX12_ENABLED STREQUAL "")
    message(FATAL_ERROR "FBZZ SDK validation failed: manifest dx12_enabled is missing")
endif()
string(REGEX MATCH "fingerprint = \"([0-9a-f]+)\"" FINGERPRINT_MATCH "${SDK_MANIFEST_TEXT}")
set(RUNTIME_FINGERPRINT "${CMAKE_MATCH_1}")
string(LENGTH "${RUNTIME_FINGERPRINT}" RUNTIME_FINGERPRINT_LENGTH)
if(NOT RUNTIME_FINGERPRINT MATCHES "^[0-9a-f]+$" OR NOT RUNTIME_FINGERPRINT_LENGTH EQUAL 64
   OR NOT CONFIG MATCHES "^(Debug|Development|Release)$")
    message(FATAL_ERROR "FBZZ SDK validation failed: invalid runtime fingerprint or configuration")
endif()

set(REQUIRED_PATHS
    "fbzz-sdk.toml"
    "cmake/FBZZ/FBZZConfig.cmake"
    "cmake/FBZZ/FBZZConfigVersion.cmake"
    "cmake/FBZZ/FBZZTargets.cmake"
    "cmake/FBZZ/build.config.in"
    "cmake/FBZZ/FBZZAgilitySDK.cmake"
    "cmake/FBZZ/StageAgilitySDK.cmake"
    "cmake/FBZZ/AgilitySDKExports.cpp.in"
    "share/fbzz/Assets/Shaders"
    "include/Engine/Scene/ScriptDllAbi.hpp"
    "include/Physics/World.hpp"
    "include/Fluid/FluidSolver.hpp"
    "include/Math/Vector3.hpp"
    "include/Core/Logger.hpp"
    "include/Graphics/Renderer/RenderScene.hpp"
    "lib/${CONFIG}/FBZZMath.lib"
    "lib/${CONFIG}/FBZZPhysics.lib"
    "lib/${CONFIG}/FBZZFluid.lib"
    "lib/${CONFIG}/FBZZEngine.lib"
    "lib/${CONFIG}/FBZZCore.lib"
    "lib/${CONFIG}/FBZZGraphics.lib"
    "lib/${CONFIG}/imgui.lib"
    # @note 作業世代の最新性判定が公開時刻に依存するので、stamp も契約に含める。
    "bin/${CONFIG}/published.stamp"
    "tools/${CONFIG}/Editor/FBZZEditor.exe"
)
# @note Every Editor module must be the same installed configuration, including SDKs without DX12.
set(EDITOR_RUNTIME_DLLS FBZZMath.dll FBZZPhysics.dll FBZZFluid.dll FBZZEngine.dll FBZZCore.dll FBZZGraphics.dll imgui.dll)
if(CONFIG STREQUAL "Debug")
    list(APPEND EDITOR_RUNTIME_DLLS assimp-vc145-mtd.dll)
else()
    list(APPEND EDITOR_RUNTIME_DLLS assimp-vc145-mt.dll)
endif()
foreach(RUNTIME_DLL IN LISTS EDITOR_RUNTIME_DLLS)
    list(APPEND REQUIRED_PATHS "bin/${CONFIG}/${RUNTIME_DLL}" "tools/${CONFIG}/Editor/${RUNTIME_DLL}")
endforeach()
if(DX12_ENABLED STREQUAL "true")
    foreach(PREFIX "bin/${CONFIG}" "tools/${CONFIG}/Editor")
        list(APPEND REQUIRED_PATHS "${PREFIX}/D3D12/D3D12Core.dll" "${PREFIX}/dxcompiler.dll" "${PREFIX}/dxil.dll")
        if(NOT CONFIG STREQUAL "Release")
            list(APPEND REQUIRED_PATHS "${PREFIX}/D3D12/d3d12SDKLayers.dll")
        elseif(EXISTS "${SDK_ROOT}/${PREFIX}/D3D12/d3d12SDKLayers.dll")
            message(FATAL_ERROR "FBZZ SDK validation failed: ${PREFIX}/D3D12/d3d12SDKLayers.dll must be excluded from Release")
        endif()
    endforeach()
    list(APPEND REQUIRED_PATHS "tools/${CONFIG}/DXC/dxc.exe" "tools/${CONFIG}/DXC/dxcompiler.dll" "tools/${CONFIG}/DXC/dxil.dll")
    foreach(NOTICE LICENSE LICENSE.txt LICENSE-CODE.txt VERSION "distributable files.txt")
        list(APPEND REQUIRED_PATHS "share/fbzz/licenses/AgilitySDK/${NOTICE}")
        list(APPEND REQUIRED_PATHS "tools/${CONFIG}/Editor/EngineLicenses/AgilitySDK/${NOTICE}")
    endforeach()
    foreach(NOTICE LICENSE LICENCE-MIT.txt LICENSE-LLVM.txt LICENSE-MS.txt VERSION)
        list(APPEND REQUIRED_PATHS "share/fbzz/licenses/DXC/${NOTICE}")
        list(APPEND REQUIRED_PATHS "tools/${CONFIG}/Editor/EngineLicenses/DXC/${NOTICE}")
    endforeach()
    foreach(NOTICE LICENSE VERSION ThirdPartyNotices.txt)
        list(APPEND REQUIRED_PATHS "tools/${CONFIG}/Editor/EngineLicenses/WinPixEventRuntime/${NOTICE}")
    endforeach()
endif()
foreach(REQUIRED_PATH IN LISTS REQUIRED_PATHS)
    if(NOT EXISTS "${SDK_ROOT}/${REQUIRED_PATH}")
        message(FATAL_ERROR "FBZZ SDK validation failed: ${REQUIRED_PATH} is missing")
    endif()
endforeach()

if(DX12_ENABLED STREQUAL "true")
    include("${SDK_ROOT}/share/fbzz/licenses/AgilitySDK/VERSION")
    include("${SDK_ROOT}/share/fbzz/licenses/DXC/VERSION")
    foreach(CONTRACT_LINE
            "agility_package = \"${FBZZ_AGILITY_PACKAGE_VERSION}\""
            "agility_path = '${FBZZ_AGILITY_SDK_PATH}'"
            "core_file_version = \"${FBZZ_AGILITY_CORE_FILE_VERSION}\""
            "dxc_version = \"${FBZZ_DXC_PACKAGE_VERSION}\"")
        string(FIND "${SDK_MANIFEST_TEXT}" "${CONTRACT_LINE}" CONTRACT_POS)
        if(CONTRACT_POS EQUAL -1)
            message(FATAL_ERROR "FBZZ SDK validation failed: VERSION disagrees with manifest: ${CONTRACT_LINE}")
        endif()
    endforeach()
    string(REGEX MATCH "agility_sdk_version = ([0-9]+)" AGILITY_VERSION_MATCH "${SDK_MANIFEST_TEXT}")
    if(NOT CMAKE_MATCH_1 STREQUAL FBZZ_AGILITY_SDK_VERSION)
        message(FATAL_ERROR "FBZZ SDK validation failed: SDK integer version disagrees with manifest")
    endif()
    foreach(FIELD core layers dxc_exe dxc_compiler dxc_validator)
        string(REGEX MATCH "${FIELD}_sha256 = \"([0-9a-f]+)\"" HASH_MATCH "${SDK_MANIFEST_TEXT}")
        set(EXPECTED_${FIELD} "${CMAKE_MATCH_1}")
        string(LENGTH "${EXPECTED_${FIELD}}" HASH_LENGTH)
        if(NOT HASH_LENGTH EQUAL 64)
            message(FATAL_ERROR "FBZZ SDK validation failed: manifest ${FIELD}_sha256 is invalid")
        endif()
    endforeach()
    foreach(PREFIX "bin/${CONFIG}" "tools/${CONFIG}/Editor" "tools/${CONFIG}/DXC")
        set(HASH_FILES "dxcompiler.dll|dxc_compiler" "dxil.dll|dxc_validator")
        if(PREFIX STREQUAL "tools/${CONFIG}/DXC")
            list(APPEND HASH_FILES "dxc.exe|dxc_exe")
        else()
            list(APPEND HASH_FILES "D3D12/D3D12Core.dll|core")
            if(NOT CONFIG STREQUAL "Release")
                list(APPEND HASH_FILES "D3D12/d3d12SDKLayers.dll|layers")
            endif()
        endif()
        foreach(HASH_FILE IN LISTS HASH_FILES)
            string(REPLACE "|" ";" HASH_PAIR "${HASH_FILE}")
            list(GET HASH_PAIR 0 RELATIVE_FILE)
            list(GET HASH_PAIR 1 HASH_FIELD)
            file(SHA256 "${SDK_ROOT}/${PREFIX}/${RELATIVE_FILE}" ACTUAL_HASH)
            if(NOT ACTUAL_HASH STREQUAL EXPECTED_${HASH_FIELD})
                message(FATAL_ERROR "FBZZ SDK validation failed: ${PREFIX}/${RELATIVE_FILE} SHA256 mismatch")
            endif()
        endforeach()
    endforeach()
    if(NOT FBZZ_AGILITY_CORE_SHA256 STREQUAL EXPECTED_core OR
       NOT FBZZ_AGILITY_LAYERS_SHA256 STREQUAL EXPECTED_layers OR
       NOT FBZZ_DXC_EXECUTABLE_SHA256 STREQUAL EXPECTED_dxc_exe OR
       NOT FBZZ_DXC_COMPILER_SHA256 STREQUAL EXPECTED_dxc_compiler OR
       NOT FBZZ_DXC_VALIDATOR_SHA256 STREQUAL EXPECTED_dxc_validator)
        message(FATAL_ERROR "FBZZ SDK validation failed: VERSION hashes disagree with manifest")
    endif()
    file(GLOB_RECURSE RUNTIME_NOTICES LIST_DIRECTORIES false "${SDK_ROOT}/share/fbzz/licenses/*")
    foreach(NOTICE IN LISTS RUNTIME_NOTICES)
        file(RELATIVE_PATH RELATIVE_NOTICE "${SDK_ROOT}" "${NOTICE}")
        list(APPEND REQUIRED_PATHS "${RELATIVE_NOTICE}")
    endforeach()
endif()

# @note PIX is private to Graphics, but every executable loading Graphics needs the event runtime.
if(PIX_REQUIRED OR EXISTS "${SDK_ROOT}/bin/${CONFIG}/WinPixEventRuntime.dll")
    list(APPEND EDITOR_RUNTIME_DLLS WinPixEventRuntime.dll)
    foreach(PIX_REQUIRED_PATH
        "bin/${CONFIG}/WinPixEventRuntime.dll"
        "tools/${CONFIG}/Editor/WinPixEventRuntime.dll"
        "share/fbzz/licenses/WinPixEventRuntime/LICENSE"
        "share/fbzz/licenses/WinPixEventRuntime/VERSION"
        "share/fbzz/licenses/WinPixEventRuntime/ThirdPartyNotices.txt")
        if(NOT EXISTS "${SDK_ROOT}/${PIX_REQUIRED_PATH}")
            message(FATAL_ERROR "FBZZ SDK validation failed: ${PIX_REQUIRED_PATH} is missing")
        endif()
        list(APPEND REQUIRED_PATHS "${PIX_REQUIRED_PATH}")
    endforeach()
endif()

# @note DXIL reflection は dxcompiler.dll を exe 隣から LoadLibraryW で解決する。Editor 隣に無いとシェーダーが全滅する。
# @note 契約は同梱の有無ではなく、bin に在れば Editor 隣にも在るという整合。
foreach(DXC_RUNTIME_DLL dxcompiler.dll dxil.dll)
    if(EXISTS "${SDK_ROOT}/bin/${CONFIG}/${DXC_RUNTIME_DLL}")
        if(NOT EXISTS "${SDK_ROOT}/tools/${CONFIG}/Editor/${DXC_RUNTIME_DLL}")
            message(FATAL_ERROR
                "FBZZ SDK validation failed: tools/${CONFIG}/Editor/${DXC_RUNTIME_DLL} is missing")
        endif()
        list(APPEND EDITOR_RUNTIME_DLLS "${DXC_RUNTIME_DLL}")
        list(APPEND REQUIRED_PATHS "bin/${CONFIG}/${DXC_RUNTIME_DLL}" "tools/${CONFIG}/Editor/${DXC_RUNTIME_DLL}")
    endif()
endforeach()
foreach(RUNTIME_DLL IN LISTS EDITOR_RUNTIME_DLLS)
    file(SHA256 "${SDK_ROOT}/bin/${CONFIG}/${RUNTIME_DLL}" INSTALLED_RUNTIME_HASH)
    file(SHA256 "${SDK_ROOT}/tools/${CONFIG}/Editor/${RUNTIME_DLL}" EDITOR_RUNTIME_HASH)
    if(NOT EDITOR_RUNTIME_HASH STREQUAL INSTALLED_RUNTIME_HASH)
        message(FATAL_ERROR "FBZZ SDK validation failed: tools/${CONFIG}/Editor/${RUNTIME_DLL} SHA256 mismatch with bin/${CONFIG}/${RUNTIME_DLL}")
    endif()
endforeach()

# @note engine-rebuild.config が紛れ込むと、SDK から起動した Editor が開発機のソース build を再ビルドしに行く。
# @note share/ (Assets) は 350MB 超で走査が重いので、exe / DLL が並ぶ範囲だけを見る。
file(GLOB_RECURSE LEAKED_DEV_CONFIGS
    "${SDK_ROOT}/bin/engine-rebuild.config"
    "${SDK_ROOT}/tools/engine-rebuild.config"
)
if(EXISTS "${SDK_ROOT}/engine-rebuild.config")
    list(APPEND LEAKED_DEV_CONFIGS "${SDK_ROOT}/engine-rebuild.config")
endif()
if(LEAKED_DEV_CONFIGS)
    message(FATAL_ERROR "FBZZ SDK validation failed: source-tree only file was published: ${LEAKED_DEV_CONFIGS}")
endif()

string(FIND "${SDK_MANIFEST_TEXT}" "id = \"${SDK_ID}\"" SDK_ID_POSITION)
if(SDK_ID_POSITION EQUAL -1)
    message(FATAL_ERROR "FBZZ SDK validation failed: manifest SDK ID does not match ${SDK_ID}")
endif()

file(READ "${SDK_ROOT}/cmake/FBZZ/FBZZTargets.cmake" TARGETS_TEXT)
foreach(IMPORTED_TARGET "FBZZ::Math" "FBZZ::Physics" "FBZZ::Fluid" "FBZZ::Engine" "FBZZ::Core" "FBZZ::Graphics" "FBZZ::ImGui" "FBZZ::TomlPlusPlus")
    string(FIND "${TARGETS_TEXT}" "${IMPORTED_TARGET}" TARGET_POS)
    if(TARGET_POS EQUAL -1)
        message(FATAL_ERROR "FBZZ SDK validation failed: ${IMPORTED_TARGET} is not exported")
    endif()
endforeach()

# @brief Restore another configuration only when its recorded files and canonical Editor pairs still match after install.
function(ValidatePreviousConfiguration PUBLISHED_CONFIG PREVIOUS_RECORD OUT_VALID)
    set(${OUT_VALID} false PARENT_SCOPE)
    string(FIND "${PREVIOUS_RECORD}" "fingerprint = \"${RUNTIME_FINGERPRINT}\"" CONTRACT_POS)
    if(CONTRACT_POS EQUAL -1 OR NOT PREVIOUS_RECORD MATCHES "validated = true")
        return()
    endif()
    string(REGEX MATCHALL "\"[^\"]+\" = \"[^\"]*\"" PREVIOUS_HASH_ROWS "${PREVIOUS_RECORD}")
    set(RECORDED_PATHS "")
    foreach(HASH_ROW IN LISTS PREVIOUS_HASH_ROWS)
        string(REGEX MATCH "^\"([^\"]+)\" = \"([^\"]*)\"$" HASH_MATCH "${HASH_ROW}")
        set(RELATIVE_FILE "${CMAKE_MATCH_1}")
        set(EXPECTED_HASH "${CMAKE_MATCH_2}")
        string(LENGTH "${EXPECTED_HASH}" HASH_LENGTH)
        if(IS_ABSOLUTE "${RELATIVE_FILE}" OR RELATIVE_FILE MATCHES "(^|/)\\.\\.(/|$)"
           OR NOT EXPECTED_HASH MATCHES "^[0-9a-f]+$" OR NOT HASH_LENGTH EQUAL 64
           OR NOT EXISTS "${SDK_ROOT}/${RELATIVE_FILE}" OR IS_DIRECTORY "${SDK_ROOT}/${RELATIVE_FILE}")
            return()
        endif()
        file(SHA256 "${SDK_ROOT}/${RELATIVE_FILE}" ACTUAL_HASH)
        if(NOT ACTUAL_HASH STREQUAL EXPECTED_HASH)
            return()
        endif()
        list(APPEND RECORDED_PATHS "${RELATIVE_FILE}")
    endforeach()
    # @note A previous record must contain every current common-file contract, not just the rows it happened to retain.
    set(PREVIOUS_REQUIRED_PATHS "")
    foreach(REQUIRED_FILE IN LISTS REQUIRED_PATHS)
        string(REPLACE "/${CONFIG}/" "/${PUBLISHED_CONFIG}/" REQUIRED_FILE "${REQUIRED_FILE}")
        if(REQUIRED_FILE STREQUAL "fbzz-sdk.toml" OR REQUIRED_FILE STREQUAL "share/fbzz/Assets/Shaders"
           OR REQUIRED_FILE MATCHES "/assimp-vc145-mtd?\\.dll$"
           OR (PUBLISHED_CONFIG STREQUAL "Release" AND REQUIRED_FILE MATCHES "/D3D12/d3d12SDKLayers\\.dll$"))
            continue()
        endif()
        list(APPEND PREVIOUS_REQUIRED_PATHS "${REQUIRED_FILE}")
    endforeach()
    if(DX12_ENABLED STREQUAL "true" AND NOT PUBLISHED_CONFIG STREQUAL "Release")
        list(APPEND PREVIOUS_REQUIRED_PATHS "bin/${PUBLISHED_CONFIG}/D3D12/d3d12SDKLayers.dll"
            "tools/${PUBLISHED_CONFIG}/Editor/D3D12/d3d12SDKLayers.dll")
    endif()
    foreach(REQUIRED_FILE IN LISTS PREVIOUS_REQUIRED_PATHS)
        list(FIND RECORDED_PATHS "${REQUIRED_FILE}" RECORDED_PATH_POS)
        if(RECORDED_PATH_POS EQUAL -1)
            return()
        endif()
    endforeach()
    set(PREVIOUS_RUNTIME_DLLS FBZZMath.dll FBZZPhysics.dll FBZZFluid.dll FBZZEngine.dll FBZZCore.dll FBZZGraphics.dll imgui.dll)
    if(PUBLISHED_CONFIG STREQUAL "Debug")
        list(APPEND PREVIOUS_RUNTIME_DLLS assimp-vc145-mtd.dll)
    else()
        list(APPEND PREVIOUS_RUNTIME_DLLS assimp-vc145-mt.dll)
    endif()
    foreach(OPTIONAL_RUNTIME WinPixEventRuntime.dll dxcompiler.dll dxil.dll)
        if(EXISTS "${SDK_ROOT}/bin/${PUBLISHED_CONFIG}/${OPTIONAL_RUNTIME}")
            list(APPEND PREVIOUS_RUNTIME_DLLS "${OPTIONAL_RUNTIME}")
        endif()
    endforeach()
    foreach(RUNTIME_DLL IN LISTS PREVIOUS_RUNTIME_DLLS)
        foreach(PREFIX "bin/${PUBLISHED_CONFIG}" "tools/${PUBLISHED_CONFIG}/Editor")
            list(FIND RECORDED_PATHS "${PREFIX}/${RUNTIME_DLL}" RECORDED_PATH_POS)
            if(RECORDED_PATH_POS EQUAL -1)
                return()
            endif()
        endforeach()
        file(SHA256 "${SDK_ROOT}/bin/${PUBLISHED_CONFIG}/${RUNTIME_DLL}" INSTALLED_HASH)
        file(SHA256 "${SDK_ROOT}/tools/${PUBLISHED_CONFIG}/Editor/${RUNTIME_DLL}" EDITOR_HASH)
        if(NOT INSTALLED_HASH STREQUAL EDITOR_HASH)
            return()
        endif()
    endforeach()
    set(${OUT_VALID} true PARENT_SCOPE)
endfunction()

# @note 検証完了後だけ公開状態を戻し、全必須ファイルのハッシュを構成ごとに残す。
set(RECORD_TEXT "\n[configurations.${CONFIG}]\nfingerprint = \"${RUNTIME_FINGERPRINT}\"\nvalidated = true\n\n[files.${CONFIG}]\n")
list(REMOVE_DUPLICATES REQUIRED_PATHS)
foreach(REQUIRED_PATH IN LISTS REQUIRED_PATHS)
    if(NOT IS_DIRECTORY "${SDK_ROOT}/${REQUIRED_PATH}" AND NOT REQUIRED_PATH STREQUAL "fbzz-sdk.toml")
        file(SHA256 "${SDK_ROOT}/${REQUIRED_PATH}" FILE_HASH)
        string(APPEND RECORD_TEXT "\"${REQUIRED_PATH}\" = \"${FILE_HASH}\"\n")
    endif()
endforeach()
file(WRITE "${SDK_ROOT}/cmake/FBZZ/configurations/${CONFIG}.toml" "${RECORD_TEXT}")
file(READ "${SDK_ROOT}/cmake/FBZZ/runtime-manifest.toml" MANIFEST_TEXT)
foreach(PUBLISHED_CONFIG Debug Development Release)
    set(RECORD "${SDK_ROOT}/cmake/FBZZ/configurations/${PUBLISHED_CONFIG}.toml")
    file(READ "${RECORD}" CONFIG_TEXT)
    if(NOT PUBLISHED_CONFIG STREQUAL CONFIG)
        if(EXISTS "${RECORD}.previous")
            file(READ "${RECORD}.previous" CONFIG_TEXT)
        endif()
        ValidatePreviousConfiguration("${PUBLISHED_CONFIG}" "${CONFIG_TEXT}" PREVIOUS_VALID)
        if(NOT PREVIOUS_VALID)
            set(CONFIG_TEXT "\n[configurations.${PUBLISHED_CONFIG}]\nfingerprint = \"${RUNTIME_FINGERPRINT}\"\nvalidated = false\n")
        endif()
        file(WRITE "${RECORD}" "${CONFIG_TEXT}")
    endif()
    file(REMOVE "${RECORD}.previous")
    string(APPEND MANIFEST_TEXT "${CONFIG_TEXT}")
endforeach()
file(WRITE "${SDK_ROOT}/fbzz-sdk.toml" "${MANIFEST_TEXT}")
