cmake_minimum_required(VERSION 3.25)
project(ParticleGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
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
find_package(FBZZ 0.1.0 EXACT CONFIG REQUIRED)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY             "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY             "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY             "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_SOURCE_DIR}/Binaries/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_SOURCE_DIR}/Binaries/Release")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_SOURCE_DIR}/Binaries/Development")

set(FBZZ_RUNTIME_BUILD_EXE_DEBUG               "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/ParticleGameStandalone.exe")
set(FBZZ_RUNTIME_BUILD_EXE_RELEASE             "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/ParticleGameStandalone.exe")
set(FBZZ_RUNTIME_BUILD_EXE_DEVELOPMENT         "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT}/ParticleGameStandalone.exe")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_DEBUG       "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/Scripts.dll")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_RELEASE     "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/Scripts.dll")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_DEVELOPMENT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT}/Scripts.dll")

file(GLOB_RECURSE SCRIPTS_DLL_SOURCES CONFIGURE_DEPENDS
    Assets/*.cpp Assets/*.hpp Src/Scripts/*.cpp Src/Scripts/*.hpp Src/ParticleGameScriptsDll.cpp
)
add_library(Scripts SHARED ${SCRIPTS_DLL_SOURCES})
set_target_properties(Scripts PROPERTIES OUTPUT_NAME "Scripts")
target_compile_definitions(Scripts PRIVATE GAMESCRIPTS_EXPORTS)
target_include_directories(Scripts PRIVATE Assets Src)
target_link_libraries(Scripts PRIVATE FBZZ::Engine)

add_executable(ParticleGameStandalone Src/AppMain.cpp Src/GameMain.cpp Include/ParticleGame/ProjectAPI.hpp)
add_dependencies(ParticleGameStandalone Scripts)
target_compile_definitions(ParticleGameStandalone PRIVATE FBZZ_STANDALONE_TARGET)
target_include_directories(ParticleGameStandalone PRIVATE Assets Include Src)
target_link_libraries(ParticleGameStandalone PRIVATE FBZZ::Engine shell32 user32)
target_link_options(ParticleGameStandalone PRIVATE
    "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>"
    "$<$<CONFIG:Release>:/ENTRY:mainCRTStartup>"
)
configure_file("${FBZZ_SDK_ROOT}/cmake/FBZZ/build.config.in" "${CMAKE_BINARY_DIR}/build.config" @ONLY)
fbzz_stage_runtime(ParticleGameStandalone)
