cmake_minimum_required(VERSION 3.25)
project(FBZZ_Engine VERSION 0.1.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# RuntimeBuild は build.config に記録した成果物パスから SandboxStandalone.exe をコピーする。
# WHY: Visual Studio は Debug / Release で出力先が分かれるため、全ターゲットの exe 出力を明示して
#      エディタから見た成果物パスを安定させる。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Binaries")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Binaries/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Binaries/Release")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Binaries")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Binaries/Debug")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Binaries/Release")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Lib/Debug")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Lib/Release")

set(FBZZ_RUNTIME_BUILD_EXE_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/SandboxStandalone.exe")
set(FBZZ_RUNTIME_BUILD_EXE_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/SandboxStandalone.exe")

configure_file(
    "${CMAKE_SOURCE_DIR}/CMake/build.config.in"
    "${CMAKE_BINARY_DIR}/build.config"
    @ONLY
)

# MSVC source files are UTF-8.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Add subdirectories in dependency order.
add_subdirectory(ThirdParty)
add_subdirectory(Projects/Math)
add_subdirectory(Projects/Physics)
add_subdirectory(Projects/Engine)
add_subdirectory(Projects/Editor)
add_subdirectory(Projects/EditorLauncher)
add_subdirectory(Projects/GameHub)
add_subdirectory(Projects/Sandbox)

# VS ソリューションを開いたときのデフォルトスタートアッププロジェクトを sandbox に設定する
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT sandbox)

option(FBZZ_BUILD_TESTS "Build unit tests" ON)
if(FBZZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Projects/Tests)
endif()
