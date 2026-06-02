cmake_minimum_required(VERSION 3.25)
project({{TARGET_NAME}} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED FBZZ_ENGINE_ROOT OR FBZZ_ENGINE_ROOT STREQUAL "")
    message(FATAL_ERROR "FBZZ_ENGINE_ROOT is required")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/Binaries/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/Binaries/Release")
set(FBZZ_RUNTIME_BUILD_EXE_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/{{TARGET_NAME}}Standalone.exe")
set(FBZZ_RUNTIME_BUILD_EXE_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/{{TARGET_NAME}}Standalone.exe")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/{{TARGET_NAME}}Scripts.dll")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/{{TARGET_NAME}}Scripts.dll")

add_subdirectory("${FBZZ_ENGINE_ROOT}/ThirdParty" "${CMAKE_BINARY_DIR}/FBZZ/ThirdParty")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Math" "${CMAKE_BINARY_DIR}/FBZZ/Math")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Physics" "${CMAKE_BINARY_DIR}/FBZZ/Physics")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Engine" "${CMAKE_BINARY_DIR}/FBZZ/Engine")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Editor" "${CMAKE_BINARY_DIR}/FBZZ/Editor")

# スクリプト DLL (ホットリロード用)
# WHY: Editor が SandboxScripts.dll と同じ仕組みでスクリプトをホットリロードできるよう、
#      プロジェクト固有のスクリプトを独立した DLL としてビルドする。
#      ScriptFactory の dual registry 問題は EXE から関数ポインタを渡す方式で回避する。
file(GLOB_RECURSE {{TARGET_NAME}}_SCRIPT_DLL_SOURCES
    Src/Scripts/*.cpp
    Src/Scripts/*.hpp
    Src/{{TARGET_NAME}}ScriptsDll.cpp
)
add_library({{TARGET_NAME}}Scripts SHARED ${{{TARGET_NAME}}_SCRIPT_DLL_SOURCES})
target_compile_definitions({{TARGET_NAME}}Scripts PRIVATE GAMESCRIPTS_EXPORTS)
target_include_directories({{TARGET_NAME}}Scripts PRIVATE Src)
target_link_libraries({{TARGET_NAME}}Scripts
    PRIVATE
        fbzz_engine
        fbzz_physics
)
set_target_properties({{TARGET_NAME}}Scripts PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>"
)

add_executable({{TARGET_NAME}}
    Src/AppMain.cpp
    Src/GameMain.cpp
    Include/{{TARGET_NAME}}/ProjectAPI.hpp
)
add_dependencies({{TARGET_NAME}} {{TARGET_NAME}}Scripts)

add_executable({{TARGET_NAME}}Standalone
    Src/AppMain.cpp
    Src/GameMain.cpp
    Include/{{TARGET_NAME}}/ProjectAPI.hpp
)
add_dependencies({{TARGET_NAME}}Standalone {{TARGET_NAME}}Scripts)
target_compile_definitions({{TARGET_NAME}}Standalone PRIVATE FBZZ_STANDALONE_TARGET)

target_include_directories({{TARGET_NAME}}
    PRIVATE
        Include
        Src
)

target_include_directories({{TARGET_NAME}}Standalone
    PRIVATE
        Include
        Src
)

target_link_libraries({{TARGET_NAME}}
    PRIVATE
        fbzz_engine
        fbzz_physics
        fbzz_math
        fbzz_editor
)

target_link_libraries({{TARGET_NAME}}Standalone
    PRIVATE
        fbzz_engine
        fbzz_physics
        fbzz_math
)

configure_file(
    "${FBZZ_ENGINE_ROOT}/CMake/build.config.in"
    "${CMAKE_BINARY_DIR}/build.config"
    @ONLY
)
