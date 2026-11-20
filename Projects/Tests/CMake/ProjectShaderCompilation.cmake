# @file    ProjectShaderCompilation.cmake
# @brief   共有 SDK のスクリプトによるプロジェクト HLSL のコンパイルを検証する。
# @author  Hasegawa Jin
# @date    2026-11-17
cmake_minimum_required(VERSION 3.25)

set(FIXTURE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/ProjectShaderCompilation")
set(FIXTURE_SUFFIX 0)
while(EXISTS "${FIXTURE_ROOT}")
    math(EXPR FIXTURE_SUFFIX "${FIXTURE_SUFFIX} + 1")
    set(FIXTURE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/ProjectShaderCompilation-${FIXTURE_SUFFIX}")
endwhile()

# @note A template consumer has no project copy of the compiler script or the shared includes.
find_program(SHADER_POWERSHELL NAMES powershell.exe powershell REQUIRED)
set(SHADER_TOOL_ROOT "${FIXTURE_ROOT}/Shader SDK/Assets/Shaders")
set(SHADER_PROJECT_ROOT "${FIXTURE_ROOT}/Shader Project/Assets/Shaders")
set(SHADER_TOOL "${SHADER_TOOL_ROOT}/compile_shaders.ps1")
file(MAKE_DIRECTORY "${SHADER_TOOL_ROOT}")
file(COPY_FILE "${SOURCE_ROOT}/Assets/Shaders/compile_shaders.ps1" "${SHADER_TOOL}")
file(WRITE "${SHADER_TOOL_ROOT}/Common/Shared.hlsli" [=[
#include "Project.hlsli"
[numthreads(1, 1, 1)] void CSMain() { RequireProjectValue(); }
]=])
file(WRITE "${SHADER_TOOL_ROOT}/Project.hlsli" "#error Project include override is required\n")
file(WRITE "${SHADER_PROJECT_ROOT}/Project.hlsli" "void RequireProjectValue() {}\n")
file(WRITE "${SHADER_PROJECT_ROOT}/Custom.cs.hlsl" "#include \"Common/Shared.hlsli\"\n")

function(RunProjectShaderCompile EXPECTED_EXIT EXPECTED_LOG)
    execute_process(COMMAND "${SHADER_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${SHADER_TOOL}"
        -ShaderRoot "${SHADER_PROJECT_ROOT}"
        -CompilerPath "${SOURCE_ROOT}/ThirdParty/DXC/bin/x64/dxc.exe" ${ARGN}
        WORKING_DIRECTORY "${FIXTURE_ROOT}"
        RESULT_VARIABLE SHADER_EXIT OUTPUT_VARIABLE SHADER_STDOUT ERROR_VARIABLE SHADER_STDERR)
    if(NOT SHADER_EXIT STREQUAL "${EXPECTED_EXIT}" OR NOT SHADER_STDOUT MATCHES "${EXPECTED_LOG}")
        message(FATAL_ERROR "Project shader compile failed (exit=${SHADER_EXIT}): ${SHADER_STDOUT}\n${SHADER_STDERR}")
    endif()
endfunction()

RunProjectShaderCompile(0 "completed=1")
set(SHADER_OUTPUT "${SHADER_PROJECT_ROOT}/compiled_dx12/Custom.cs.cso")
if(NOT EXISTS "${SHADER_OUTPUT}.compiler" OR EXISTS "${SHADER_TOOL_ROOT}/compiled_dx12"
   OR EXISTS "${SHADER_TOOL_ROOT}/compile_log.txt")
    message(FATAL_ERROR "Shader compilation must write only to the project's shader tree")
endif()
RunProjectShaderCompile(0 "completed=0 skipped=1")
file(SHA256 "${SHADER_OUTPUT}" VALID_SHADER_HASH)
# @note Removing the override changes the chosen include without making its SDK replacement newer than the CSO.
file(REMOVE "${SHADER_PROJECT_ROOT}/Project.hlsli")
RunProjectShaderCompile(1 "Project include override is required")
file(SHA256 "${SHADER_OUTPUT}" FAILED_SHADER_HASH)
if(NOT FAILED_SHADER_HASH STREQUAL VALID_SHADER_HASH)
    message(FATAL_ERROR "A compile failure overwrote the previous shader binary")
endif()
file(REMOVE "${SHADER_PROJECT_ROOT}/Custom.cs.hlsl")
RunProjectShaderCompile(0 "orphans=1")
if(EXISTS "${SHADER_OUTPUT}" OR EXISTS "${SHADER_OUTPUT}.compiler")
    message(FATAL_ERROR "Removing the last project shader left generated files in the project")
endif()

# @note Sandbox runs the compiler from the repository root without a project shader root.
file(WRITE "${SHADER_TOOL_ROOT}/Default.cs.hlsl" "[numthreads(1, 1, 1)] void CSMain() {}\n")
file(WRITE "${FIXTURE_ROOT}/build/Stale.hlsl" "#include \"Missing.hlsli\"\n")
execute_process(COMMAND "${SHADER_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${SHADER_TOOL}"
    -CompilerPath "${SOURCE_ROOT}/ThirdParty/DXC/bin/x64/dxc.exe" -PlanOnly
    WORKING_DIRECTORY "${FIXTURE_ROOT}"
    RESULT_VARIABLE SHADER_EXIT OUTPUT_VARIABLE SHADER_STDOUT ERROR_VARIABLE SHADER_STDERR)
if(NOT SHADER_EXIT STREQUAL "0" OR NOT SHADER_STDOUT MATCHES "sources=1 jobs=1"
   OR NOT SHADER_STDOUT MATCHES "Default[.]cs[.]hlsl -> Default[.]cs[.]cso")
    message(FATAL_ERROR "Default shader root must be the script directory (exit=${SHADER_EXIT}): ${SHADER_STDOUT}\n${SHADER_STDERR}")
endif()
