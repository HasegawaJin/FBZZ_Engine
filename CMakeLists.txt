cmake_minimum_required(VERSION 3.25)
project(FBZZ_Engine VERSION 0.1.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# WHY: 共有 SDK とゲーム側で CRT の選択が暗黙値に依存すると、同じ構成名でも
#      STL オブジェクトを DLL 境界へ渡した時点で ABI が壊れる。全ターゲットを
#      Debug=/MDd、Development/Release=/MD に固定し、SDK package 側でも同値を検証する。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Development: 最適化ON + デバッグシンボルあり + NDEBUG なし（アサート有効）
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Development" CACHE STRING "Build configurations" FORCE)
endif()
set(CMAKE_CXX_FLAGS_DEVELOPMENT           "/Zi /O2 /Ob1 /FS"            CACHE STRING "Development CXX flags"        FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DEVELOPMENT    "/DEBUG:FULL /INCREMENTAL:NO" CACHE STRING "Development EXE linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DEVELOPMENT "/DEBUG:FULL /INCREMENTAL:NO" CACHE STRING "Development DLL linker flags" FORCE)

# RuntimeBuild は build.config に記録した成果物パスから SandboxStandalone.exe をコピーする。
# WHY: Visual Studio は Debug / Release で出力先が分かれるため、全ターゲットの exe 出力を明示して
#      エディタから見た成果物パスを安定させる。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Binaries")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Binaries/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Binaries/Release")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Binaries")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Binaries/Debug")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Binaries/Release")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Lib/Debug")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Lib/Release")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Lib/Development")

set(FBZZ_RUNTIME_BUILD_EXE_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/Sandbox/SandboxStandalone.exe")
set(FBZZ_RUNTIME_BUILD_EXE_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/Sandbox/SandboxStandalone.exe")
set(FBZZ_RUNTIME_BUILD_EXE_DEVELOPMENT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT}/Sandbox/SandboxStandalone.exe")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/Sandbox/SandboxScripts.dll")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/Sandbox/SandboxScripts.dll")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_DEVELOPMENT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT}/Sandbox/SandboxScripts.dll")

configure_file(
    "${CMAKE_SOURCE_DIR}/CMake/build.config.in"
    "${CMAKE_BINARY_DIR}/build.config"
    @ONLY
)

# MSVC source files are UTF-8.
if(MSVC)
    add_compile_options(/utf-8 /MP /FS)
endif()

# Add subdirectories in dependency order.
add_subdirectory(ThirdParty)
add_subdirectory(Projects/Math)
add_subdirectory(Projects/Physics)
add_subdirectory(Projects/Engine)
add_subdirectory(Projects/Editor)
add_subdirectory(Projects/EditorLauncher)
# GameHubはElectron / TypeScript版へ移行したため、CMakeのビルド対象には含めない。
# WHY: HubのUIとOS連携はProjects/GameHubNext側で完結し、EngineのC++依存グラフから分離する。
add_subdirectory(Projects/Sandbox)

# 共有 Engine SDK を SDK/<engine-version>/ に生成する。
# 通常ビルド成果物と分離し、外部ゲームはソースツリーを add_subdirectory せず IMPORTED target を参照する。
include(CMake/FBZZSDK.cmake)

# VS ソリューションを開いたときのデフォルトスタートアッププロジェクトを sandbox に設定する
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT sandbox)

option(FBZZ_BUILD_TESTS "Build unit tests" ON)
if(FBZZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Projects/Tests)
endif()
