cmake_minimum_required(VERSION 3.25)
project(FBZZ_Engine VERSION 0.1.0 LANGUAGES CXX)

# C++ バージョン指定
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 必須にする
set(CMAKE_CXX_EXTENSIONS OFF) # 拡張機能を無効にする
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # コンパイルコマンドを出力

# MSVC: ソースを UTF-8 として読む (BOM なし環境でも日本語コメントが壊れない)
if(MSVC)
    add_compile_options(/utf-8)
endif()

# サブフォルダを追加 (依存順: math → physics → engine → editor → sandbox)
add_subdirectory(ThirdParty)
add_subdirectory(Projects/Math)
add_subdirectory(Projects/Physics)
add_subdirectory(Projects/Engine)
add_subdirectory(Projects/Editor)
add_subdirectory(Projects/Sandbox)

option(FBZZ_BUILD_TESTS "Build unit tests" OFF)
if(FBZZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Projects/Tests)
endif()

