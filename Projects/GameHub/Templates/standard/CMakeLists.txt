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

add_subdirectory("${FBZZ_ENGINE_ROOT}/ThirdParty" "${CMAKE_BINARY_DIR}/FBZZ/ThirdParty")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Math" "${CMAKE_BINARY_DIR}/FBZZ/Math")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Physics" "${CMAKE_BINARY_DIR}/FBZZ/Physics")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Engine" "${CMAKE_BINARY_DIR}/FBZZ/Engine")

add_executable({{TARGET_NAME}}
    Src/GameMain.cpp
    Include/{{TARGET_NAME}}/ProjectAPI.hpp
)

target_include_directories({{TARGET_NAME}}
    PRIVATE
        Include
        Src
)

target_link_libraries({{TARGET_NAME}}
    PRIVATE
        fbzz_engine
        fbzz_physics
        fbzz_math
)
