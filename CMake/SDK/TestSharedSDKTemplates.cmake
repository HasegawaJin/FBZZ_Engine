# @file    TestSharedSDKTemplates.cmake
# @brief   Shared SDK templates, PIX runtime staging and distribution contracts.
# @author  Hasegawa Jin
# @date    2026-07-15

if(NOT DEFINED SOURCE_ROOT OR SOURCE_ROOT STREQUAL "")
    message(FATAL_ERROR "TestSharedSDKTemplates: SOURCE_ROOT is required")
endif()

foreach(TEMPLATE_KIND standard empty)
    set(TEMPLATE_ROOT "${SOURCE_ROOT}/Projects/GameHub/Templates/${TEMPLATE_KIND}")
    file(READ "${TEMPLATE_ROOT}/CMakeLists.txt" CMAKE_TEXT)
    file(READ "${TEMPLATE_ROOT}/CMakePresets.json" PRESET_TEXT)
    file(READ "${TEMPLATE_ROOT}/.fbzz_proj" PROJECT_TEXT)

    foreach(REQUIRED_TEXT "find_package(FBZZ {{ENGINE_VERSION}} EXACT CONFIG REQUIRED)"
                          "FBZZ::Engine" "fbzz_stage_runtime" "fbzz_enable_agility_sdk")
        string(FIND "${CMAKE_TEXT}" "${REQUIRED_TEXT}" REQUIRED_POS)
        if(REQUIRED_POS EQUAL -1)
            message(FATAL_ERROR "${TEMPLATE_KIND}: CMakeLists.txt is missing ${REQUIRED_TEXT}")
        endif()
    endforeach()
    foreach(FORBIDDEN_TEXT "FBZZ_ENGINE_ROOT" "add_subdirectory")
        string(FIND "${CMAKE_TEXT}" "${FORBIDDEN_TEXT}" FORBIDDEN_POS)
        if(NOT FORBIDDEN_POS EQUAL -1)
            message(FATAL_ERROR "${TEMPLATE_KIND}: CMakeLists.txt still contains ${FORBIDDEN_TEXT}")
        endif()
    endforeach()
    string(FIND "${PRESET_TEXT}" "FBZZ_SDK_ROOT" SDK_PRESET_POS)
    string(FIND "${PROJECT_TEXT}" "sdk_id = \"{{SDK_ID}}\"" SDK_PROJECT_POS)
    if(SDK_PRESET_POS EQUAL -1 OR SDK_PROJECT_POS EQUAL -1)
        message(FATAL_ERROR "${TEMPLATE_KIND}: SDK selection metadata is missing")
    endif()

    # @note Engine shaders come from the SDK; a template-owned copy would miss SDK updates.
    if(EXISTS "${TEMPLATE_ROOT}/Assets/Shaders")
        message(FATAL_ERROR "${TEMPLATE_KIND}: Assets/Shaders must not exist in a template (engine shaders come from the SDK)")
    endif()

    if(TEMPLATE_KIND STREQUAL "standard")
        # @note A later project-owned shader tree must still participate in the Standalone build.
        foreach(REQUIRED_SHADER_TEXT "add_custom_target(CompileShaders"
                                     "Assets/Shaders/*.hlsli"
                                     "add_dependencies({{TARGET_NAME}}Standalone CompileShaders)")
            string(FIND "${CMAKE_TEXT}" "${REQUIRED_SHADER_TEXT}" SHADER_TEXT_POS)
            if(SHADER_TEXT_POS EQUAL -1)
                message(FATAL_ERROR "standard: shader build integration is missing ${REQUIRED_SHADER_TEXT}")
            endif()
        endforeach()
    endif()
endforeach()

# @note A new binary-directory child leaves prior fixtures, source assets and real SDKs untouched.
get_filename_component(FIXTURE_BINARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}" ABSOLUTE)
set(FIXTURE_ROOT "${FIXTURE_BINARY_ROOT}/FBZZSDKTemplateFixture")
set(FIXTURE_SUFFIX 0)
while(EXISTS "${FIXTURE_ROOT}")
    math(EXPR FIXTURE_SUFFIX "${FIXTURE_SUFFIX} + 1")
    set(FIXTURE_ROOT "${FIXTURE_BINARY_ROOT}/FBZZSDKTemplateFixture-${FIXTURE_SUFFIX}")
endwhile()
get_filename_component(FIXTURE_PARENT "${FIXTURE_ROOT}" DIRECTORY)
if(NOT FIXTURE_PARENT STREQUAL FIXTURE_BINARY_ROOT OR FIXTURE_ROOT STREQUAL SOURCE_ROOT)
    message(FATAL_ERROR "TestSharedSDKTemplates: fixture must be the dedicated binary-directory child")
endif()
set(FIXTURE_SDK "${FIXTURE_ROOT}/SDK")
set(FIXTURE_INPUT "${FIXTURE_ROOT}/Input")
set(FIXTURE_EDITOR "${FIXTURE_ROOT}/InputEditor")
set(FIXTURE_CONFIG "Release")
set(FIXTURE_ID "pix-runtime-regression")
file(MAKE_DIRECTORY "${FIXTURE_INPUT}" "${FIXTURE_EDITOR}" "${FIXTURE_SDK}/share/fbzz/Assets/Shaders")

# @note Fixture files model install/export output without compiling or loading any DLL.
foreach(FIXTURE_PATH
        "cmake/FBZZ/FBZZConfig.cmake" "cmake/FBZZ/FBZZConfigVersion.cmake" "cmake/FBZZ/build.config.in"
        "cmake/FBZZ/FBZZAgilitySDK.cmake" "cmake/FBZZ/StageAgilitySDK.cmake" "cmake/FBZZ/AgilitySDKExports.cpp.in"
        "include/Engine/Scene/ScriptDllAbi.hpp" "include/Physics/World.hpp" "include/Fluid/FluidSolver.hpp"
        "include/Math/Vector3.hpp" "include/Core/Logger.hpp" "include/Graphics/Renderer/RenderScene.hpp")
    file(WRITE "${FIXTURE_SDK}/${FIXTURE_PATH}" "")
endforeach()
set(FIXTURE_TARGETS "")
foreach(FIXTURE_LIBRARY Math Physics Fluid Engine Core Graphics)
    file(WRITE "${FIXTURE_SDK}/lib/${FIXTURE_CONFIG}/FBZZ${FIXTURE_LIBRARY}.lib" "")
    file(WRITE "${FIXTURE_SDK}/bin/${FIXTURE_CONFIG}/FBZZ${FIXTURE_LIBRARY}.dll" "installed ${FIXTURE_LIBRARY}\n")
    file(WRITE "${FIXTURE_EDITOR}/FBZZ${FIXTURE_LIBRARY}.dll" "old source Editor ${FIXTURE_LIBRARY}\n")
    string(APPEND FIXTURE_TARGETS "FBZZ::${FIXTURE_LIBRARY}\n")
endforeach()
file(WRITE "${FIXTURE_SDK}/lib/${FIXTURE_CONFIG}/imgui.lib" "")
file(WRITE "${FIXTURE_SDK}/bin/${FIXTURE_CONFIG}/imgui.dll" "installed imgui\n")
file(WRITE "${FIXTURE_EDITOR}/imgui.dll" "old source Editor imgui\n")
string(APPEND FIXTURE_TARGETS "FBZZ::ImGui\nFBZZ::TomlPlusPlus\n")
file(WRITE "${FIXTURE_SDK}/cmake/FBZZ/FBZZTargets.cmake" "${FIXTURE_TARGETS}")
include("${SOURCE_ROOT}/ThirdParty/AgilitySDK/VERSION")
include("${SOURCE_ROOT}/ThirdParty/DXC/VERSION")
set(FBZZ_SDK_ID "${FIXTURE_ID}")
set(PROJECT_VERSION "0.0.0")
set(FBZZ_SDK_DX12_ENABLED true)
set(FBZZ_SDK_RUNTIME_FINGERPRINT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
set(FBZZ_SDK_AGILITY_VERSION "${FBZZ_AGILITY_SDK_VERSION}")
set(FBZZ_SDK_CORE_SHA256 "${FBZZ_AGILITY_CORE_SHA256}")
set(FBZZ_SDK_LAYERS_SHA256 "${FBZZ_AGILITY_LAYERS_SHA256}")
set(FBZZ_SDK_DXC_EXE_SHA256 "${FBZZ_DXC_EXECUTABLE_SHA256}")
set(FBZZ_SDK_DXC_COMPILER_SHA256 "${FBZZ_DXC_COMPILER_SHA256}")
set(FBZZ_SDK_DXC_VALIDATOR_SHA256 "${FBZZ_DXC_VALIDATOR_SHA256}")
configure_file("${SOURCE_ROOT}/CMake/SDK/fbzz-sdk.toml.in" "${FIXTURE_ROOT}/manifest.toml" @ONLY)
execute_process(COMMAND "${CMAKE_COMMAND}" "-DSDK_ROOT=${FIXTURE_SDK}" "-DCONFIG=${FIXTURE_CONFIG}"
    "-DPUBLISH_BEGIN=ON" "-DMANIFEST_TEMPLATE=${FIXTURE_ROOT}/manifest.toml"
    -P "${SOURCE_ROOT}/CMake/SDK/StageFBZZSDK.cmake" RESULT_VARIABLE BEGIN_RESULT)
if(NOT BEGIN_RESULT EQUAL 0)
    message(FATAL_ERROR "Fixture publication invalidation failed")
endif()
foreach(PACKAGE AgilitySDK DXC)
    file(MAKE_DIRECTORY "${FIXTURE_SDK}/share/fbzz/licenses/${PACKAGE}")
    if(PACKAGE STREQUAL "AgilitySDK")
        set(NOTICES LICENSE LICENSE.txt LICENSE-CODE.txt VERSION "distributable files.txt")
    else()
        set(NOTICES LICENSE LICENCE-MIT.txt LICENSE-LLVM.txt LICENSE-MS.txt VERSION)
    endif()
    foreach(NOTICE IN LISTS NOTICES)
        file(COPY "${SOURCE_ROOT}/ThirdParty/${PACKAGE}/${NOTICE}" DESTINATION "${FIXTURE_SDK}/share/fbzz/licenses/${PACKAGE}")
    endforeach()
endforeach()
foreach(FIXTURE_DLL assimp-vc145-mtd.dll assimp-vc145-mt.dll WinPixEventRuntime.dll)
    file(WRITE "${FIXTURE_INPUT}/${FIXTURE_DLL}" "installed ${FIXTURE_DLL}\n")
endforeach()
file(WRITE "${FIXTURE_EDITOR}/FBZZEditor.exe" "")
set(FIXTURE_LICENSE_SOURCE "${SOURCE_ROOT}/ThirdParty/WinPixEventRuntime")
file(MAKE_DIRECTORY "${FIXTURE_SDK}/share/fbzz/licenses/WinPixEventRuntime")
foreach(FIXTURE_NOTICE LICENSE VERSION ThirdPartyNotices.txt)
    file(COPY "${FIXTURE_LICENSE_SOURCE}/${FIXTURE_NOTICE}"
        DESTINATION "${FIXTURE_SDK}/share/fbzz/licenses/WinPixEventRuntime")
endforeach()

set(FIXTURE_STAGE_DX12 ON)
function(RunFixtureStage SDK_DIRECTORY PIX_RUNTIME OUT_RESULT OUT_LOG)
    if(FIXTURE_CONFIG STREQUAL "Debug")
        set(FIXTURE_ASSIMP assimp-vc145-mtd.dll)
    else()
        set(FIXTURE_ASSIMP assimp-vc145-mt.dll)
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DSDK_ROOT=${SDK_DIRECTORY}" "-DCONFIG=${FIXTURE_CONFIG}"
        "-DASSIMP_DLL=${FIXTURE_INPUT}/${FIXTURE_ASSIMP}" "-DEDITOR_DIR=${FIXTURE_EDITOR}"
        "-DPIX_RUNTIME_DLL=${PIX_RUNTIME}" "-DDX12_ENABLED=${FIXTURE_STAGE_DX12}"
        "-DAGILITY_ROOT=${SOURCE_ROOT}/ThirdParty/AgilitySDK" "-DDXC_ROOT=${SOURCE_ROOT}/ThirdParty/DXC"
        -P "${SOURCE_ROOT}/CMake/SDK/StageFBZZSDK.cmake"
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
    set(${OUT_RESULT} "${RESULT}" PARENT_SCOPE)
    set(${OUT_LOG} "${STDOUT}${STDERR}" PARENT_SCOPE)
endfunction()

function(RunFixtureBegin MANIFEST_PATH)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSDK_ROOT=${FIXTURE_SDK}" "-DCONFIG=${FIXTURE_CONFIG}"
        "-DPUBLISH_BEGIN=ON" "-DMANIFEST_TEMPLATE=${MANIFEST_PATH}"
        -P "${SOURCE_ROOT}/CMake/SDK/StageFBZZSDK.cmake"
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
    if(NOT "${RESULT}" STREQUAL "0")
        message(FATAL_ERROR "Fixture publication begin failed: ${STDOUT}${STDERR}")
    endif()
endfunction()

function(VerifyCanonicalEditorFixture)
    file(READ "${FIXTURE_SDK}/cmake/FBZZ/configurations/${FIXTURE_CONFIG}.toml" CONFIG_RECORD)
    set(FIXTURE_RUNTIME_DLLS FBZZMath.dll FBZZPhysics.dll FBZZFluid.dll FBZZEngine.dll FBZZCore.dll FBZZGraphics.dll imgui.dll)
    if(FIXTURE_CONFIG STREQUAL "Debug")
        list(APPEND FIXTURE_RUNTIME_DLLS assimp-vc145-mtd.dll)
    else()
        list(APPEND FIXTURE_RUNTIME_DLLS assimp-vc145-mt.dll)
    endif()
    foreach(FIXTURE_DLL IN LISTS FIXTURE_RUNTIME_DLLS)
        set(EDITOR_PATH "tools/${FIXTURE_CONFIG}/Editor/${FIXTURE_DLL}")
        file(SHA256 "${FIXTURE_SDK}/bin/${FIXTURE_CONFIG}/${FIXTURE_DLL}" INSTALLED_HASH)
        file(SHA256 "${FIXTURE_SDK}/${EDITOR_PATH}" EDITOR_HASH)
        if(NOT INSTALLED_HASH STREQUAL EDITOR_HASH)
            message(FATAL_ERROR "SDK Editor inherited stale source output: ${FIXTURE_DLL}")
        endif()
        foreach(FIXTURE_PATH "bin/${FIXTURE_CONFIG}/${FIXTURE_DLL}" "${EDITOR_PATH}")
            string(FIND "${CONFIG_RECORD}" "\"${FIXTURE_PATH}\" = \"${INSTALLED_HASH}\"" HASH_ROW_POS)
            if(HASH_ROW_POS EQUAL -1)
                message(FATAL_ERROR "Canonical runtime hash was not recorded: ${FIXTURE_PATH}")
            endif()
        endforeach()
        file(RENAME "${FIXTURE_SDK}/${EDITOR_PATH}" "${FIXTURE_SDK}/${EDITOR_PATH}.fixture-hidden")
        RunFixtureValidation(RESULT LOG)
        file(RENAME "${FIXTURE_SDK}/${EDITOR_PATH}.fixture-hidden" "${FIXTURE_SDK}/${EDITOR_PATH}")
        string(REGEX REPLACE "[ \t\r\n]+" " " NORMALIZED "${LOG}")
        string(FIND "${NORMALIZED}" "${EDITOR_PATH} is missing" MISSING_POS)
        if("${RESULT}" STREQUAL "0" OR MISSING_POS EQUAL -1)
            message(FATAL_ERROR "Missing Editor DLL was not rejected precisely: ${FIXTURE_DLL}: ${LOG}")
        endif()
        file(APPEND "${FIXTURE_SDK}/${EDITOR_PATH}" "tampered Editor runtime\n")
        RunFixtureValidation(RESULT LOG)
        file(COPY_FILE "${FIXTURE_SDK}/bin/${FIXTURE_CONFIG}/${FIXTURE_DLL}" "${FIXTURE_SDK}/${EDITOR_PATH}")
        string(REGEX REPLACE "[ \t\r\n]+" " " NORMALIZED "${LOG}")
        string(FIND "${NORMALIZED}" "${EDITOR_PATH} SHA256 mismatch" MISMATCH_POS)
        if("${RESULT}" STREQUAL "0" OR MISMATCH_POS EQUAL -1)
            message(FATAL_ERROR "Tampered Editor DLL was not rejected precisely: ${FIXTURE_DLL}: ${LOG}")
        endif()
    endforeach()
endfunction()

function(RunFixtureValidation OUT_RESULT OUT_LOG)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DSDK_ROOT=${FIXTURE_SDK}" "-DSDK_ID=${FIXTURE_ID}" "-DCONFIG=${FIXTURE_CONFIG}"
        "-DPIX_REQUIRED=ON" -P "${SOURCE_ROOT}/CMake/SDK/ValidateFBZZSDK.cmake"
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
    set(${OUT_RESULT} "${RESULT}" PARENT_SCOPE)
    set(${OUT_LOG} "${STDOUT}${STDERR}" PARENT_SCOPE)
endfunction()

# @note InputEditor intentionally lacks PIX; Stage must independently place the explicitly supplied runtime beside both consumers.
RunFixtureStage("${FIXTURE_SDK}" "${FIXTURE_INPUT}/WinPixEventRuntime.dll" STAGE_RESULT STAGE_LOG)
if(NOT "${STAGE_RESULT}" STREQUAL "0")
    message(FATAL_ERROR "PIX fixture staging failed: ${STAGE_LOG}")
endif()
RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
if(NOT "${VALIDATE_RESULT}" STREQUAL "0")
    message(FATAL_ERROR "Complete PIX fixture was rejected: ${VALIDATE_LOG}")
endif()
VerifyCanonicalEditorFixture()
foreach(FIXTURE_NOTICE LICENSE VERSION ThirdPartyNotices.txt)
    file(SHA256 "${FIXTURE_LICENSE_SOURCE}/${FIXTURE_NOTICE}" SOURCE_HASH)
    file(SHA256 "${FIXTURE_SDK}/share/fbzz/licenses/WinPixEventRuntime/${FIXTURE_NOTICE}" STAGED_HASH)
    if(NOT SOURCE_HASH STREQUAL STAGED_HASH)
        message(FATAL_ERROR "PIX fixture notice content changed: ${FIXTURE_NOTICE}")
    endif()
endforeach()

# @note Each mandatory PIX artifact is removed independently so an unrelated missing file cannot satisfy a negative case.
foreach(MISSING_PATH
        "bin/${FIXTURE_CONFIG}/WinPixEventRuntime.dll"
        "tools/${FIXTURE_CONFIG}/Editor/WinPixEventRuntime.dll"
        "share/fbzz/licenses/WinPixEventRuntime/LICENSE"
        "share/fbzz/licenses/WinPixEventRuntime/VERSION"
        "share/fbzz/licenses/WinPixEventRuntime/ThirdPartyNotices.txt"
        "bin/${FIXTURE_CONFIG}/D3D12/D3D12Core.dll"
        "tools/${FIXTURE_CONFIG}/Editor/D3D12/D3D12Core.dll"
        "bin/${FIXTURE_CONFIG}/dxcompiler.dll"
        "tools/${FIXTURE_CONFIG}/Editor/dxil.dll"
        "tools/${FIXTURE_CONFIG}/DXC/dxc.exe"
        "share/fbzz/licenses/AgilitySDK/LICENSE-CODE.txt"
        "share/fbzz/licenses/AgilitySDK/distributable files.txt"
        "share/fbzz/licenses/DXC/LICENSE-MS.txt")
    file(RENAME "${FIXTURE_SDK}/${MISSING_PATH}" "${FIXTURE_SDK}/${MISSING_PATH}.fixture-hidden")
    RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
    file(RENAME "${FIXTURE_SDK}/${MISSING_PATH}.fixture-hidden" "${FIXTURE_SDK}/${MISSING_PATH}")
    string(REGEX REPLACE "[ \t\r\n]+" " " VALIDATE_NORMALIZED "${VALIDATE_LOG}")
    string(FIND "${VALIDATE_NORMALIZED}" "${MISSING_PATH} is missing" MISSING_POS)
    if("${VALIDATE_RESULT}" STREQUAL "0" OR MISSING_POS EQUAL -1)
        message(FATAL_ERROR "Missing PIX artifact was not rejected precisely: ${MISSING_PATH}: ${VALIDATE_LOG}")
    endif()
endforeach()

# @note ハッシュ変更は配置が揃っていても旧パッケージとの混在として拒否する。
file(APPEND "${FIXTURE_SDK}/bin/${FIXTURE_CONFIG}/D3D12/D3D12Core.dll" "fixture corruption")
RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
string(REGEX REPLACE "[ \t\r\n]+" " " VALIDATE_NORMALIZED "${VALIDATE_LOG}")
string(FIND "${VALIDATE_NORMALIZED}" "bin/${FIXTURE_CONFIG}/D3D12/D3D12Core.dll SHA256 mismatch" MISMATCH_POS)
if("${VALIDATE_RESULT}" STREQUAL "0" OR MISMATCH_POS EQUAL -1)
    message(FATAL_ERROR "Corrupt Agility runtime was not rejected: ${VALIDATE_LOG}")
endif()
RunFixtureStage("${FIXTURE_SDK}" "${FIXTURE_INPUT}/WinPixEventRuntime.dll" STAGE_RESULT STAGE_LOG)
RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)

# @note Canonical Editor modules remain mandatory and byte-identical when Agility and DXC are disabled.
set(DX12_FIXTURE_SDK "${FIXTURE_SDK}")
set(FIXTURE_SDK "${FIXTURE_ROOT}/GPUlessSDK")
file(COPY "${DX12_FIXTURE_SDK}/" DESTINATION "${FIXTURE_SDK}")
set(FIXTURE_STAGE_DX12 OFF)
set(FBZZ_SDK_DX12_ENABLED false)
set(FBZZ_SDK_RUNTIME_FINGERPRINT "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc")
configure_file("${SOURCE_ROOT}/CMake/SDK/fbzz-sdk.toml.in" "${FIXTURE_ROOT}/gpuless-manifest.toml" @ONLY)
RunFixtureBegin("${FIXTURE_ROOT}/gpuless-manifest.toml")
foreach(PREFIX "bin/${FIXTURE_CONFIG}" "tools/${FIXTURE_CONFIG}/Editor")
    file(REMOVE_RECURSE "${FIXTURE_SDK}/${PREFIX}/D3D12")
    file(REMOVE "${FIXTURE_SDK}/${PREFIX}/dxcompiler.dll" "${FIXTURE_SDK}/${PREFIX}/dxil.dll")
endforeach()
file(REMOVE_RECURSE "${FIXTURE_SDK}/tools/${FIXTURE_CONFIG}/DXC")
RunFixtureStage("${FIXTURE_SDK}" "${FIXTURE_INPUT}/WinPixEventRuntime.dll" STAGE_RESULT STAGE_LOG)
RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
if(NOT "${STAGE_RESULT}" STREQUAL "0" OR NOT "${VALIDATE_RESULT}" STREQUAL "0")
    message(FATAL_ERROR "GPUless canonical Editor fixture failed: ${STAGE_LOG}${VALIDATE_LOG}")
endif()
VerifyCanonicalEditorFixture()
set(FIXTURE_SDK "${DX12_FIXTURE_SDK}")
set(FIXTURE_STAGE_DX12 ON)
set(FBZZ_SDK_DX12_ENABLED true)

# @note 共通契約の変更と公開途中の失敗は、既存の成功状態を引き継がない。
file(READ "${FIXTURE_ROOT}/manifest.toml" UPDATED_MANIFEST)
string(REPLACE "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" UPDATED_MANIFEST "${UPDATED_MANIFEST}")
file(WRITE "${FIXTURE_ROOT}/updated-manifest.toml" "${UPDATED_MANIFEST}")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DSDK_ROOT=${FIXTURE_SDK}" "-DCONFIG=Release"
    "-DPUBLISH_BEGIN=ON" "-DMANIFEST_TEMPLATE=${FIXTURE_ROOT}/updated-manifest.toml"
    -P "${SOURCE_ROOT}/CMake/SDK/StageFBZZSDK.cmake" RESULT_VARIABLE BEGIN_RESULT)
foreach(PUBLISHED_CONFIG Debug Development Release)
    file(READ "${FIXTURE_SDK}/cmake/FBZZ/configurations/${PUBLISHED_CONFIG}.toml" INVALID_RECORD)
    if(NOT INVALID_RECORD MATCHES "validated = false")
        message(FATAL_ERROR "Runtime contract update kept ${PUBLISHED_CONFIG} valid")
    endif()
endforeach()
RunFixtureStage("${FIXTURE_ROOT}/MissingRuntimeSDK" "${FIXTURE_INPUT}/missing/WinPixEventRuntime.dll" STAGE_RESULT STAGE_LOG)
string(FIND "${STAGE_LOG}" "PIX runtime is missing" MISSING_POS)
if("${STAGE_RESULT}" STREQUAL "0" OR MISSING_POS EQUAL -1)
    message(FATAL_ERROR "Missing PIX staging input was not rejected: ${STAGE_LOG}")
endif()

# @note Development の各 Layers は Core と同じパッケージから独立に必須配置する。
set(FIXTURE_CONFIG "Development")
file(COPY "${FIXTURE_SDK}/lib/Release/" DESTINATION "${FIXTURE_SDK}/lib/${FIXTURE_CONFIG}")
file(COPY "${FIXTURE_SDK}/bin/Release/" DESTINATION "${FIXTURE_SDK}/bin/${FIXTURE_CONFIG}")
RunFixtureStage("${FIXTURE_SDK}" "${FIXTURE_INPUT}/WinPixEventRuntime.dll" STAGE_RESULT STAGE_LOG)
RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
if(NOT VALIDATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Development Agility fixture was rejected: ${VALIDATE_LOG}")
endif()
foreach(PREFIX "bin/${FIXTURE_CONFIG}" "tools/${FIXTURE_CONFIG}/Editor")
    set(MISSING_PATH "${PREFIX}/D3D12/d3d12SDKLayers.dll")
    file(RENAME "${FIXTURE_SDK}/${MISSING_PATH}" "${FIXTURE_SDK}/${MISSING_PATH}.fixture-hidden")
    RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
    file(RENAME "${FIXTURE_SDK}/${MISSING_PATH}.fixture-hidden" "${FIXTURE_SDK}/${MISSING_PATH}")
    string(REGEX REPLACE "[ \t\r\n]+" " " VALIDATE_NORMALIZED "${VALIDATE_LOG}")
    string(FIND "${VALIDATE_NORMALIZED}" "${MISSING_PATH} is missing" MISSING_POS)
    if("${VALIDATE_RESULT}" STREQUAL "0" OR MISSING_POS EQUAL -1)
        message(FATAL_ERROR "Missing Development Layers was not rejected: ${VALIDATE_LOG}")
    endif()
endforeach()

# @note Publication suspends all configurations, then restores another unchanged configuration after its full file audit.
set(FIXTURE_CONFIG Release)
RunFixtureBegin("${FIXTURE_ROOT}/updated-manifest.toml")
foreach(PUBLISHED_CONFIG Debug Development Release)
    file(READ "${FIXTURE_SDK}/cmake/FBZZ/configurations/${PUBLISHED_CONFIG}.toml" CONFIG_RECORD)
    if(NOT CONFIG_RECORD MATCHES "validated = false")
        message(FATAL_ERROR "Publication left ${PUBLISHED_CONFIG} usable during shared-file install")
    endif()
endforeach()
RunFixtureStage("${FIXTURE_SDK}" "${FIXTURE_INPUT}/WinPixEventRuntime.dll" STAGE_RESULT STAGE_LOG)
RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
if(NOT "${STAGE_RESULT}" STREQUAL "0" OR NOT "${VALIDATE_RESULT}" STREQUAL "0")
    message(FATAL_ERROR "Unchanged configuration publication failed: ${STAGE_LOG}${VALIDATE_LOG}")
endif()
foreach(PUBLISHED_CONFIG Development Release)
    file(READ "${FIXTURE_SDK}/cmake/FBZZ/configurations/${PUBLISHED_CONFIG}.toml" CONFIG_RECORD)
    if(NOT CONFIG_RECORD MATCHES "validated = true")
        message(FATAL_ERROR "Unchanged ${PUBLISHED_CONFIG} configuration was not preserved")
    endif()
endforeach()

# @note A changed shared package module invalidates another configuration even when the runtime fingerprint is unchanged.
RunFixtureBegin("${FIXTURE_ROOT}/updated-manifest.toml")
file(APPEND "${FIXTURE_SDK}/cmake/FBZZ/FBZZConfig.cmake" "updated shared package\n")
RunFixtureStage("${FIXTURE_SDK}" "${FIXTURE_INPUT}/WinPixEventRuntime.dll" STAGE_RESULT STAGE_LOG)
RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
file(READ "${FIXTURE_SDK}/cmake/FBZZ/configurations/Development.toml" CONFIG_RECORD)
if(NOT "${VALIDATE_RESULT}" STREQUAL "0" OR NOT CONFIG_RECORD MATCHES "validated = false")
    message(FATAL_ERROR "Changed common module kept Development validated: ${VALIDATE_LOG}")
endif()

# @note Old successful records without a mandatory Editor hash row cannot be restored by another configuration's publication.
file(READ "${FIXTURE_SDK}/cmake/FBZZ/configurations/Release.toml" CONFIG_RECORD)
string(REGEX REPLACE "\"tools/Release/Editor/FBZZEngine\\.dll\" = \"[0-9a-f]+\"\n" "" CONFIG_RECORD "${CONFIG_RECORD}")
file(WRITE "${FIXTURE_SDK}/cmake/FBZZ/configurations/Release.toml" "${CONFIG_RECORD}")
set(FIXTURE_CONFIG Development)
RunFixtureBegin("${FIXTURE_ROOT}/updated-manifest.toml")
RunFixtureStage("${FIXTURE_SDK}" "${FIXTURE_INPUT}/WinPixEventRuntime.dll" STAGE_RESULT STAGE_LOG)
RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
file(READ "${FIXTURE_SDK}/cmake/FBZZ/configurations/Release.toml" CONFIG_RECORD)
if(NOT "${VALIDATE_RESULT}" STREQUAL "0" OR NOT CONFIG_RECORD MATCHES "validated = false")
    message(FATAL_ERROR "Incomplete canonical Editor record kept Release validated: ${VALIDATE_LOG}")
endif()

# @note A missing common-module hash row must independently prevent restoration even when all binaries and files still exist.
set(FIXTURE_CONFIG Release)
RunFixtureBegin("${FIXTURE_ROOT}/updated-manifest.toml")
RunFixtureStage("${FIXTURE_SDK}" "${FIXTURE_INPUT}/WinPixEventRuntime.dll" STAGE_RESULT STAGE_LOG)
RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
if(NOT "${STAGE_RESULT}" STREQUAL "0" OR NOT "${VALIDATE_RESULT}" STREQUAL "0")
    message(FATAL_ERROR "Complete record restoration precondition failed: ${STAGE_LOG}${VALIDATE_LOG}")
endif()
file(READ "${FIXTURE_SDK}/cmake/FBZZ/configurations/Release.toml" CONFIG_RECORD)
string(REGEX REPLACE "\"cmake/FBZZ/FBZZAgilitySDK\\.cmake\" = \"[0-9a-f]+\"\n" "" CONFIG_RECORD "${CONFIG_RECORD}")
file(WRITE "${FIXTURE_SDK}/cmake/FBZZ/configurations/Release.toml" "${CONFIG_RECORD}")
set(FIXTURE_CONFIG Development)
RunFixtureBegin("${FIXTURE_ROOT}/updated-manifest.toml")
RunFixtureStage("${FIXTURE_SDK}" "${FIXTURE_INPUT}/WinPixEventRuntime.dll" STAGE_RESULT STAGE_LOG)
RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
file(READ "${FIXTURE_SDK}/cmake/FBZZ/configurations/Release.toml" CONFIG_RECORD)
if(NOT "${VALIDATE_RESULT}" STREQUAL "0" OR NOT CONFIG_RECORD MATCHES "validated = false")
    message(FATAL_ERROR "Missing common module hash row kept Release validated: ${VALIDATE_LOG}")
endif()

# @note VERSION の取得元 URL と配置引数の衝突を、実際の vendor / SDK DLL の配置で検出する。
foreach(LOCAL_STAGE_KIND SourceDebug SourceRelease SdkDevelopment GPUless)
    set(LOCAL_STAGE_DX12 ON)
    set(LOCAL_STAGE_CORE "${SOURCE_ROOT}/ThirdParty/AgilitySDK/build/native/bin/x64")
    set(LOCAL_STAGE_DXC "${SOURCE_ROOT}/ThirdParty/DXC/bin/x64")
    set(LOCAL_STAGE_LICENSES "${SOURCE_ROOT}/ThirdParty")
    set(LOCAL_STAGE_OUTPUT "${FIXTURE_ROOT}/LocalRuntime/Source")
    if(LOCAL_STAGE_KIND STREQUAL "SourceDebug")
        set(LOCAL_STAGE_CONFIG Debug)
    elseif(LOCAL_STAGE_KIND STREQUAL "SourceRelease")
        set(LOCAL_STAGE_CONFIG Release)
    elseif(LOCAL_STAGE_KIND STREQUAL "SdkDevelopment")
        set(LOCAL_STAGE_CONFIG Development)
        set(LOCAL_STAGE_CORE "${FIXTURE_SDK}/bin/Development/D3D12")
        set(LOCAL_STAGE_DXC "${FIXTURE_SDK}/bin/Development")
        set(LOCAL_STAGE_LICENSES "${FIXTURE_SDK}/share/fbzz/licenses")
        set(LOCAL_STAGE_OUTPUT "${FIXTURE_ROOT}/LocalRuntime/SdkConsumer")
    else()
        set(LOCAL_STAGE_CONFIG Debug)
        set(LOCAL_STAGE_DX12 OFF)
        set(LOCAL_STAGE_CORE "${FIXTURE_ROOT}/AbsentAgility")
        set(LOCAL_STAGE_DXC "${FIXTURE_ROOT}/AbsentDXC")
        set(LOCAL_STAGE_LICENSES "${FIXTURE_ROOT}/AbsentLicenses")
        set(LOCAL_STAGE_OUTPUT "${FIXTURE_ROOT}/LocalRuntime/GPUless")
    endif()
    file(MAKE_DIRECTORY "${LOCAL_STAGE_OUTPUT}")
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DFBZZ_OUTPUT=${LOCAL_STAGE_OUTPUT}" "-DFBZZ_CONFIG=${LOCAL_STAGE_CONFIG}"
        "-DFBZZ_DX12_ENABLED=${LOCAL_STAGE_DX12}"
        "-DFBZZ_CORE_SOURCE=${LOCAL_STAGE_CORE}" "-DFBZZ_DXC_INPUT_DIRECTORY=${LOCAL_STAGE_DXC}"
        "-DFBZZ_AGILITY_LICENSES=${LOCAL_STAGE_LICENSES}/AgilitySDK"
        "-DFBZZ_DXC_LICENSES=${LOCAL_STAGE_LICENSES}/DXC"
        "-DFBZZ_PIX_LICENSES=${LOCAL_STAGE_LICENSES}/WinPixEventRuntime"
        -P "${SOURCE_ROOT}/CMake/StageAgilitySDK.cmake"
        RESULT_VARIABLE LOCAL_STAGE_RESULT OUTPUT_VARIABLE LOCAL_STAGE_STDOUT ERROR_VARIABLE LOCAL_STAGE_STDERR)
    if(NOT "${LOCAL_STAGE_RESULT}" STREQUAL "0")
        message(FATAL_ERROR "${LOCAL_STAGE_KIND} runtime staging failed: ${LOCAL_STAGE_STDOUT}${LOCAL_STAGE_STDERR}")
    endif()
    file(READ "${LOCAL_STAGE_OUTPUT}/fbzz-runtime.toml" LOCAL_STAGE_MANIFEST)
    if(NOT LOCAL_STAGE_DX12)
        if(NOT LOCAL_STAGE_MANIFEST MATCHES "dx12_enabled = false" OR EXISTS "${LOCAL_STAGE_OUTPUT}/D3D12"
            OR EXISTS "${LOCAL_STAGE_OUTPUT}/dxcompiler.dll" OR EXISTS "${LOCAL_STAGE_OUTPUT}/dxil.dll")
            message(FATAL_ERROR "GPUless runtime staging introduced graphics dependencies")
        endif()
        continue()
    endif()
    foreach(LOCAL_STAGE_HASH_PAIR
            "D3D12/D3D12Core.dll|${FBZZ_AGILITY_CORE_SHA256}"
            "dxcompiler.dll|${FBZZ_DXC_COMPILER_SHA256}" "dxil.dll|${FBZZ_DXC_VALIDATOR_SHA256}")
        string(REPLACE "|" ";" LOCAL_STAGE_HASH_PARTS "${LOCAL_STAGE_HASH_PAIR}")
        list(GET LOCAL_STAGE_HASH_PARTS 0 LOCAL_STAGE_FILE)
        list(GET LOCAL_STAGE_HASH_PARTS 1 LOCAL_STAGE_EXPECTED_HASH)
        file(SHA256 "${LOCAL_STAGE_OUTPUT}/${LOCAL_STAGE_FILE}" LOCAL_STAGE_ACTUAL_HASH)
        if(NOT LOCAL_STAGE_ACTUAL_HASH STREQUAL LOCAL_STAGE_EXPECTED_HASH)
            message(FATAL_ERROR "${LOCAL_STAGE_KIND} runtime bytes disagree: ${LOCAL_STAGE_FILE}")
        endif()
    endforeach()
    if(LOCAL_STAGE_CONFIG STREQUAL "Release")
        if(EXISTS "${LOCAL_STAGE_OUTPUT}/D3D12/d3d12SDKLayers.dll")
            message(FATAL_ERROR "Release runtime staging retained previous Debug Layers")
        endif()
    else()
        file(SHA256 "${LOCAL_STAGE_OUTPUT}/D3D12/d3d12SDKLayers.dll" LOCAL_STAGE_LAYERS_HASH)
        if(NOT LOCAL_STAGE_LAYERS_HASH STREQUAL FBZZ_AGILITY_LAYERS_SHA256)
            message(FATAL_ERROR "${LOCAL_STAGE_KIND} runtime Layers bytes disagree")
        endif()
    endif()
    foreach(LOCAL_STAGE_PACKAGE AgilitySDK DXC WinPixEventRuntime)
        file(GLOB LOCAL_STAGE_NOTICES LIST_DIRECTORIES FALSE "${LOCAL_STAGE_LICENSES}/${LOCAL_STAGE_PACKAGE}/*")
        foreach(LOCAL_STAGE_NOTICE IN LISTS LOCAL_STAGE_NOTICES)
            get_filename_component(LOCAL_STAGE_NOTICE_NAME "${LOCAL_STAGE_NOTICE}" NAME)
            file(SHA256 "${LOCAL_STAGE_NOTICE}" LOCAL_STAGE_NOTICE_HASH)
            file(SHA256 "${LOCAL_STAGE_OUTPUT}/EngineLicenses/${LOCAL_STAGE_PACKAGE}/${LOCAL_STAGE_NOTICE_NAME}" LOCAL_STAGE_STAGED_NOTICE_HASH)
            if(NOT LOCAL_STAGE_NOTICE_HASH STREQUAL LOCAL_STAGE_STAGED_NOTICE_HASH)
                message(FATAL_ERROR "${LOCAL_STAGE_KIND} runtime notice bytes disagree: ${LOCAL_STAGE_PACKAGE}/${LOCAL_STAGE_NOTICE_NAME}")
            endif()
        endforeach()
    endforeach()
endforeach()

# @note Packaging itself is asynchronous; fixed source contracts verify that its existing file queue preserves notices without asset stripping.
file(READ "${SOURCE_ROOT}/CMake/SDK/FBZZConfig.cmake.in" PACKAGE_TEXT)
foreach(REQUIRED_TEXT
        "set(FBZZ_ENGINE_LICENSE_ROOT \"\${FBZZ_SDK_ROOT}/share/fbzz/licenses\")"
        "\"\${FBZZ_ENGINE_LICENSE_ROOT}\""
        "\"$<TARGET_FILE_DIR:\${TARGET_NAME}>/EngineLicenses\"")
    string(FIND "${PACKAGE_TEXT}" "${REQUIRED_TEXT}" REQUIRED_POS)
    if(REQUIRED_POS EQUAL -1)
        message(FATAL_ERROR "SDK consumer notice staging contract is missing: ${REQUIRED_TEXT}")
    endif()
endforeach()
file(READ "${SOURCE_ROOT}/Projects/Editor/src/BuildPipeline.cpp" PIPELINE_TEXT)
foreach(REQUIRED_TEXT
        "required.push_back(L\"WinPixEventRuntime.dll\");"
        "exeDir / L\"EngineLicenses\" / L\"WinPixEventRuntime\" / name"
        "enqueueTree(engineLicensesDir, m_tmpDir / L\"EngineLicenses\", false);")
    string(FIND "${PIPELINE_TEXT}" "${REQUIRED_TEXT}" REQUIRED_POS)
    if(REQUIRED_POS EQUAL -1)
        message(FATAL_ERROR "Game runtime distribution contract is missing: ${REQUIRED_TEXT}")
    endif()
endforeach()
