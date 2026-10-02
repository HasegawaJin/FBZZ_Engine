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
                          "FBZZ::Engine" "fbzz_stage_runtime")
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

# @note Empty files model install/export output without compiling or loading any DLL.
foreach(FIXTURE_PATH
        "cmake/FBZZ/FBZZConfig.cmake" "cmake/FBZZ/FBZZConfigVersion.cmake" "cmake/FBZZ/build.config.in"
        "include/Engine/Scene/ScriptDllAbi.hpp" "include/Physics/World.hpp" "include/Fluid/FluidSolver.hpp"
        "include/Math/Vector3.hpp" "include/Core/Logger.hpp" "include/Graphics/Renderer/RenderScene.hpp")
    file(WRITE "${FIXTURE_SDK}/${FIXTURE_PATH}" "")
endforeach()
set(FIXTURE_TARGETS "")
foreach(FIXTURE_LIBRARY Math Physics Fluid Engine Core Graphics)
    file(WRITE "${FIXTURE_SDK}/lib/${FIXTURE_CONFIG}/FBZZ${FIXTURE_LIBRARY}.lib" "")
    file(WRITE "${FIXTURE_SDK}/bin/${FIXTURE_CONFIG}/FBZZ${FIXTURE_LIBRARY}.dll" "")
    string(APPEND FIXTURE_TARGETS "FBZZ::${FIXTURE_LIBRARY}\n")
endforeach()
file(WRITE "${FIXTURE_SDK}/lib/${FIXTURE_CONFIG}/imgui.lib" "")
file(WRITE "${FIXTURE_SDK}/bin/${FIXTURE_CONFIG}/imgui.dll" "")
string(APPEND FIXTURE_TARGETS "FBZZ::ImGui\nFBZZ::TomlPlusPlus\n")
file(WRITE "${FIXTURE_SDK}/cmake/FBZZ/FBZZTargets.cmake" "${FIXTURE_TARGETS}")
file(WRITE "${FIXTURE_SDK}/fbzz-sdk.toml" "id = \"${FIXTURE_ID}\"\n")
foreach(FIXTURE_DLL FBZZEngine.dll FBZZCore.dll FBZZGraphics.dll imgui.dll assimp.dll WinPixEventRuntime.dll)
    file(WRITE "${FIXTURE_INPUT}/${FIXTURE_DLL}" "")
endforeach()
file(WRITE "${FIXTURE_EDITOR}/FBZZEditor.exe" "")
set(FIXTURE_LICENSE_SOURCE "${SOURCE_ROOT}/ThirdParty/WinPixEventRuntime")
file(MAKE_DIRECTORY "${FIXTURE_SDK}/share/fbzz/licenses/WinPixEventRuntime")
foreach(FIXTURE_NOTICE LICENSE VERSION ThirdPartyNotices.txt)
    file(COPY "${FIXTURE_LICENSE_SOURCE}/${FIXTURE_NOTICE}"
        DESTINATION "${FIXTURE_SDK}/share/fbzz/licenses/WinPixEventRuntime")
endforeach()

function(RunFixtureStage SDK_DIRECTORY PIX_RUNTIME OUT_RESULT OUT_LOG)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DSDK_ROOT=${SDK_DIRECTORY}" "-DCONFIG=${FIXTURE_CONFIG}"
        "-DENGINE_DLL=${FIXTURE_INPUT}/FBZZEngine.dll" "-DCORE_DLL=${FIXTURE_INPUT}/FBZZCore.dll"
        "-DGRAPHICS_DLL=${FIXTURE_INPUT}/FBZZGraphics.dll" "-DIMGUI_DLL=${FIXTURE_INPUT}/imgui.dll"
        "-DASSIMP_DLL=${FIXTURE_INPUT}/assimp.dll" "-DEDITOR_DIR=${FIXTURE_EDITOR}"
        "-DPIX_RUNTIME_DLL=${PIX_RUNTIME}" -P "${SOURCE_ROOT}/CMake/SDK/StageFBZZSDK.cmake"
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
    set(${OUT_RESULT} "${RESULT}" PARENT_SCOPE)
    set(${OUT_LOG} "${STDOUT}${STDERR}" PARENT_SCOPE)
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
        "share/fbzz/licenses/WinPixEventRuntime/ThirdPartyNotices.txt")
    file(RENAME "${FIXTURE_SDK}/${MISSING_PATH}" "${FIXTURE_SDK}/${MISSING_PATH}.fixture-hidden")
    RunFixtureValidation(VALIDATE_RESULT VALIDATE_LOG)
    file(RENAME "${FIXTURE_SDK}/${MISSING_PATH}.fixture-hidden" "${FIXTURE_SDK}/${MISSING_PATH}")
    string(REGEX REPLACE "[ \t\r\n]+" " " VALIDATE_NORMALIZED "${VALIDATE_LOG}")
    string(FIND "${VALIDATE_NORMALIZED}" "${MISSING_PATH} is missing" MISSING_POS)
    if("${VALIDATE_RESULT}" STREQUAL "0" OR MISSING_POS EQUAL -1)
        message(FATAL_ERROR "Missing PIX artifact was not rejected precisely: ${MISSING_PATH}: ${VALIDATE_LOG}")
    endif()
endforeach()
RunFixtureStage("${FIXTURE_ROOT}/MissingRuntimeSDK" "${FIXTURE_INPUT}/missing/WinPixEventRuntime.dll" STAGE_RESULT STAGE_LOG)
string(FIND "${STAGE_LOG}" "PIX runtime is missing" MISSING_POS)
if("${STAGE_RESULT}" STREQUAL "0" OR MISSING_POS EQUAL -1)
    message(FATAL_ERROR "Missing PIX staging input was not rejected: ${STAGE_LOG}")
endif()

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
