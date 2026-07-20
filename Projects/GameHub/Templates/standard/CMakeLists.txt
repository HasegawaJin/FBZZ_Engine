cmake_minimum_required(VERSION 3.25)
project({{TARGET_NAME}} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# WHY: 共有 SDK と同じ CRT を使用し、Script DLL が STL/C++ オブジェクトを渡す境界の ABI を固定する。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Development" CACHE STRING "Build configurations" FORCE)
set(CMAKE_CXX_FLAGS_DEVELOPMENT           "/Zi /O2 /Ob1 /FS"            CACHE STRING "Development CXX flags"        FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DEVELOPMENT    "/DEBUG:FULL /INCREMENTAL:NO" CACHE STRING "Development EXE linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DEVELOPMENT "/DEBUG:FULL /INCREMENTAL:NO" CACHE STRING "Development DLL linker flags" FORCE)

if(MSVC)
    add_compile_options(/utf-8 /FS)
endif()

if((NOT DEFINED FBZZ_SDK_ROOT OR FBZZ_SDK_ROOT STREQUAL "") AND DEFINED ENV{FBZZ_SDK_ROOT})
    set(FBZZ_SDK_ROOT "$ENV{FBZZ_SDK_ROOT}" CACHE PATH "Versioned FBZZ SDK root" FORCE)
endif()
if(NOT DEFINED FBZZ_SDK_ROOT OR FBZZ_SDK_ROOT STREQUAL "")
    message(FATAL_ERROR "FBZZ_SDK_ROOT is required. Select the versioned SDK in FBZZ Hub.")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${FBZZ_SDK_ROOT}/cmake")
find_package(FBZZ {{ENGINE_VERSION}} EXACT CONFIG REQUIRED)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY             "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY             "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY             "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_SOURCE_DIR}/Binaries/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_SOURCE_DIR}/Binaries/Release")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_SOURCE_DIR}/Binaries/Development")

set(FBZZ_RUNTIME_BUILD_EXE_DEBUG               "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/{{TARGET_NAME}}Standalone.exe")
set(FBZZ_RUNTIME_BUILD_EXE_RELEASE             "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/{{TARGET_NAME}}Standalone.exe")
set(FBZZ_RUNTIME_BUILD_EXE_DEVELOPMENT         "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT}/{{TARGET_NAME}}Standalone.exe")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_DEBUG       "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/Scripts.dll")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_RELEASE     "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/Scripts.dll")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_DEVELOPMENT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT}/Scripts.dll")

# 共有 Editor がホットリロードするゲーム固有 Script DLL。
file(GLOB_RECURSE SCRIPTS_DLL_SOURCES CONFIGURE_DEPENDS
    Assets/*.cpp
    Assets/*.hpp
    Src/Scripts/*.cpp
    Src/Scripts/*.hpp
    Src/{{TARGET_NAME}}ScriptsDll.cpp
)
add_library(Scripts SHARED ${SCRIPTS_DLL_SOURCES})
set_target_properties(Scripts PROPERTIES OUTPUT_NAME "Scripts")
target_compile_definitions(Scripts PRIVATE GAMESCRIPTS_EXPORTS)
target_include_directories(Scripts PRIVATE Assets Src)
target_link_libraries(Scripts PRIVATE FBZZ::Engine)

# ゲーム側は共有 Editor を生成せず、配布・Play 用 Standalone だけを所有する。
add_executable({{TARGET_NAME}}Standalone
    Src/AppMain.cpp
    Src/GameMain.cpp
    Include/{{TARGET_NAME}}/ProjectAPI.hpp
)
add_dependencies({{TARGET_NAME}}Standalone Scripts)
target_compile_definitions({{TARGET_NAME}}Standalone PRIVATE FBZZ_STANDALONE_TARGET)
target_include_directories({{TARGET_NAME}}Standalone PRIVATE Assets Include Src)
target_link_libraries({{TARGET_NAME}}Standalone PRIVATE FBZZ::Engine shell32 user32)
target_link_options({{TARGET_NAME}}Standalone PRIVATE
    "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>"
    "$<$<CONFIG:Release>:/ENTRY:mainCRTStartup>"
)

# Standaloneの正式ビルドでは毎回差分走査し、追加・変更・削除を統合スクリプトへ自動反映する。
set(PROJECT_SHADER_SCRIPT "${CMAKE_SOURCE_DIR}/Assets/Shaders/compile_shaders.ps1")
file(GLOB_RECURSE PROJECT_SHADER_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Assets/Shaders/*.hlsl"
    "${CMAKE_SOURCE_DIR}/Assets/Shaders/*.hlsli"
)
# WHY: Visual Studio の FXCompile による main 固定の二重コンパイルを防ぎ、
#      VSMain / PSMain / CSMain の処理を compile_shaders.ps1 だけに集約する。
set_source_files_properties(${PROJECT_SHADER_FILES} PROPERTIES
    HEADER_FILE_ONLY TRUE
    VS_TOOL_OVERRIDE "None"
)
find_program(FBZZ_POWERSHELL_EXECUTABLE NAMES powershell.exe powershell REQUIRED)
add_custom_target(compile_shaders
    COMMAND "${FBZZ_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass -File "${PROJECT_SHADER_SCRIPT}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Compiling project HLSL shaders..."
    SOURCES ${PROJECT_SHADER_FILES} "${PROJECT_SHADER_SCRIPT}"
    VERBATIM
)
add_dependencies({{TARGET_NAME}}Standalone compile_shaders)

configure_file(
    "${FBZZ_SDK_ROOT}/cmake/FBZZ/build.config.in"
    "${CMAKE_BINARY_DIR}/build.config"
    @ONLY
)
fbzz_stage_runtime({{TARGET_NAME}}Standalone)
