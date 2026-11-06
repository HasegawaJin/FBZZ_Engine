# @file    TestFBZZRuntimeAssets.cmake
# @brief   Verify SDK asset changes reach an up-to-date Standalone build without relinking.
# @author  Hasegawa Jin
# @date    2026-11-06

set(ASSET_FIXTURE "${FIXTURE_ROOT}/Runtime Assets")
set(ASSET_SDK "${ASSET_FIXTURE}/SDK")
set(ASSET_OUTPUT "${ASSET_FIXTURE}/Binaries")
set(ASSET_SOURCE "${ASSET_FIXTURE}/Source")
set(ASSET_BUILD "${ASSET_FIXTURE}/Build")
file(MAKE_DIRECTORY "${ASSET_SDK}/bin/Release" "${ASSET_SDK}/share/fbzz/Assets/Shaders"
    "${ASSET_SDK}/share/fbzz/licenses" "${ASSET_SOURCE}")
file(WRITE "${ASSET_SDK}/bin/Release/runtime.txt" "runtime\n")
file(WRITE "${ASSET_SDK}/share/fbzz/Assets/Shaders/Shared.hlsl" "original shader\n")
file(WRITE "${ASSET_SDK}/share/fbzz/Assets/Removed.mat" "removed material\n")
file(WRITE "${ASSET_SDK}/share/fbzz/Assets/Removed.mat.meta" "removed metadata\n")
file(WRITE "${ASSET_SDK}/share/fbzz/licenses/Removed.txt" "removed notice\n")
file(WRITE "${ASSET_OUTPUT}/Assets/Keep.mat" "project material\n")
file(WRITE "${ASSET_SOURCE}/main.c" "int main(void) { return 0; }\n")
file(WRITE "${ASSET_SOURCE}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.25)\n"
    "project(RuntimeAssetFixture LANGUAGES C)\n"
    "set(FBZZ_SDK_ROOT \"${ASSET_SDK}\")\n"
    "set(FBZZ_VERSION fixture)\n"
    "include(\"${SOURCE_ROOT}/CMake/SDK/FBZZRuntime.cmake\")\n"
    "add_executable(Standalone main.c)\n"
    "set_target_properties(Standalone PROPERTIES RUNTIME_OUTPUT_DIRECTORY \"${ASSET_OUTPUT}\" RUNTIME_OUTPUT_DIRECTORY_RELEASE \"${ASSET_OUTPUT}\")\n"
    "fbzz_stage_runtime(Standalone)\n")
set(GENERATOR_ARGUMENTS)
if(DEFINED TEST_GENERATOR AND NOT TEST_GENERATOR STREQUAL "")
    list(APPEND GENERATOR_ARGUMENTS -G "${TEST_GENERATOR}")
endif()
if(DEFINED TEST_GENERATOR_PLATFORM AND NOT TEST_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND GENERATOR_ARGUMENTS -A "${TEST_GENERATOR_PLATFORM}")
endif()
if(DEFINED TEST_GENERATOR_TOOLSET AND NOT TEST_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND GENERATOR_ARGUMENTS -T "${TEST_GENERATOR_TOOLSET}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ASSET_SOURCE}" -B "${ASSET_BUILD}"
    ${GENERATOR_ARGUMENTS} -DCMAKE_BUILD_TYPE=Release
    RESULT_VARIABLE CONFIGURE_RESULT OUTPUT_VARIABLE CONFIGURE_OUTPUT ERROR_VARIABLE CONFIGURE_ERROR)
if(NOT CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Runtime asset fixture configure failed: ${CONFIGURE_OUTPUT}${CONFIGURE_ERROR}")
endif()

function(BuildRuntimeAssetFixture)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${ASSET_BUILD}" --config Release --target Standalone
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
    if(NOT RESULT EQUAL 0)
        message(FATAL_ERROR "Runtime asset fixture build failed: ${OUTPUT}${ERROR}")
    endif()
endfunction()

BuildRuntimeAssetFixture()
foreach(EXPECTED EngineAssets/Shaders/Shared.hlsl EngineAssets/Removed.mat EngineAssets/Removed.mat.meta EngineLicenses/Removed.txt runtime.txt)
    if(NOT EXISTS "${ASSET_OUTPUT}/${EXPECTED}")
        message(FATAL_ERROR "Initial runtime staging missed ${EXPECTED}")
    endif()
endforeach()
file(WRITE "${ASSET_SDK}/share/fbzz/Assets/Shaders/Shared.hlsl" "updated shader\n")
file(WRITE "${ASSET_SDK}/share/fbzz/Assets/Added.mat" "added material\n")
file(WRITE "${ASSET_SDK}/share/fbzz/Assets/Added.mat.meta" "added metadata\n")
file(WRITE "${ASSET_SDK}/share/fbzz/licenses/Added.txt" "added notice\n")
file(REMOVE "${ASSET_SDK}/share/fbzz/Assets/Removed.mat" "${ASSET_SDK}/share/fbzz/Assets/Removed.mat.meta"
    "${ASSET_SDK}/share/fbzz/licenses/Removed.txt")
if(WIN32)
    # @note Visual Studio can rerun POST_BUILD without linking; denying EXE writes proves the linker itself is not required.
    # @see https://learn.microsoft.com/en-us/dotnet/api/system.io.fileshare FileShare.Read excludes writers while allowing the build to inspect the EXE.
    file(WRITE "${ASSET_SOURCE}/BuildWithoutRelink.ps1" [=[
param([string] $ExecutablePath, [string] $CMakeCommand, [string] $BuildDirectory)
$handle = [System.IO.File]::Open($ExecutablePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
try {
    for ($iteration = 0; $iteration -lt 2; ++$iteration) {
        & $CMakeCommand --build $BuildDirectory --config Release --target Standalone
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
} finally { $handle.Dispose() }
]=])
    execute_process(COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${ASSET_SOURCE}/BuildWithoutRelink.ps1" -ExecutablePath "${ASSET_OUTPUT}/Standalone.exe"
        -CMakeCommand "${CMAKE_COMMAND}" -BuildDirectory "${ASSET_BUILD}"
        RESULT_VARIABLE LOCKED_RESULT OUTPUT_VARIABLE LOCKED_OUTPUT ERROR_VARIABLE LOCKED_ERROR)
    if(NOT LOCKED_RESULT EQUAL 0)
        message(FATAL_ERROR "Asset-only builds must succeed with the EXE locked against writes: ${LOCKED_OUTPUT}${LOCKED_ERROR}")
    endif()
else()
    BuildRuntimeAssetFixture()
    BuildRuntimeAssetFixture()
endif()
file(READ "${ASSET_OUTPUT}/EngineAssets/Shaders/Shared.hlsl" FINAL_SHADER)
file(READ "${ASSET_OUTPUT}/Assets/Keep.mat" PROJECT_MATERIAL)
if(NOT FINAL_SHADER STREQUAL "updated shader\n"
    OR NOT PROJECT_MATERIAL STREQUAL "project material\n")
    message(FATAL_ERROR "Asset-only builds must update shaders, preserve project assets and never relink")
endif()
foreach(EXPECTED EngineAssets/Added.mat EngineAssets/Added.mat.meta EngineLicenses/Added.txt)
    if(NOT EXISTS "${ASSET_OUTPUT}/${EXPECTED}")
        message(FATAL_ERROR "Asset-only build missed ${EXPECTED}")
    endif()
endforeach()
foreach(REMOVED EngineAssets/Removed.mat EngineAssets/Removed.mat.meta EngineLicenses/Removed.txt)
    if(EXISTS "${ASSET_OUTPUT}/${REMOVED}")
        message(FATAL_ERROR "Asset-only build retained deleted SDK file ${REMOVED}")
    endif()
endforeach()

# @note Existing junction fixture creation covers Windows directory redirection without requiring symlink privileges.
set(OUTSIDE_ASSETS "${ASSET_FIXTURE}/OutsideAssets")
file(MAKE_DIRECTORY "${OUTSIDE_ASSETS}")
file(WRITE "${OUTSIDE_ASSETS}/Keep.txt" "outside assets\n")
CreateSdkPreviewFixtureLink("${ASSET_OUTPUT}/EngineAssets/Redirected" "${OUTSIDE_ASSETS}")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DSDK_ROOT=${ASSET_SDK}" "-DRUNTIME_DIR=${ASSET_OUTPUT}"
    -P "${SOURCE_ROOT}/CMake/SDK/StageFBZZEngineAssets.cmake"
    RESULT_VARIABLE REDIRECT_RESULT OUTPUT_VARIABLE REDIRECT_OUTPUT ERROR_VARIABLE REDIRECT_ERROR)
file(READ "${OUTSIDE_ASSETS}/Keep.txt" OUTSIDE_CONTENT)
if(REDIRECT_RESULT EQUAL 0 OR NOT OUTSIDE_CONTENT STREQUAL "outside assets\n")
    message(FATAL_ERROR "Runtime asset staging must reject redirected output and preserve external files")
endif()
