cmake_minimum_required(VERSION 3.25)
project(FBZZ_Engine VERSION 0.1.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

option(FBZZ_BUILD_TESTS "Build unit tests" ON)
if(FBZZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Projects/Tests)
endif()
