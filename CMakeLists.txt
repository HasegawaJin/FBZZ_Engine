cmake_minimum_required(VERSION 3.25)
project(FBZZ_Engine VERSION 0.1.0 LANGUAGES CXX)

# C++ バージョン指定
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 必須にする
set(CMAKE_CXX_EXTENSIONS OFF) # 拡張機能を無効にする

# サブフォルダを追加 (依存順)
add_subdirectory(third_party)
add_subdirectory(physics)
add_subdirectory(engine)
add_subdirectory(sandbox)

option(FBZZ_BUILD_TESTS "Build unit tests" OFF)
if(FBZZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

