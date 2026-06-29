cmake_minimum_required(VERSION 3.25)
project(DemoGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# WHY: ゲームプロジェクトはエンジン本体と独立した CMake ルートを持つため、
#      エンジン側で設定している Development 構成をここでも明示的に宣言する。
#      これがないと VS プロジェクトに Development が生成されず、
#      エディタが --config Development でビルドしようとしたときに MSB8013 が発生する。
set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Development" CACHE STRING "Build configurations" FORCE)
set(CMAKE_CXX_FLAGS_DEVELOPMENT           "/Zi /O2 /Ob1 /FS"            CACHE STRING "Development CXX flags"        FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DEVELOPMENT    "/DEBUG:FULL /INCREMENTAL:NO" CACHE STRING "Development EXE linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DEVELOPMENT "/DEBUG:FULL /INCREMENTAL:NO" CACHE STRING "Development DLL linker flags" FORCE)

# WHY: GameHub 生成プロジェクトはエンジン本体とは独立した CMake ルートを持つ。
#      ここでも MSVC に UTF-8 ソースとして読ませないと、日本語コメントや assert 文字列が
#      CP932 として解釈され、プリプロセッサや文字列リテラルが壊れる。
if(MSVC)
    add_compile_options(/utf-8 /FS)
endif()

if(NOT DEFINED FBZZ_ENGINE_ROOT OR FBZZ_ENGINE_ROOT STREQUAL "")
    message(FATAL_ERROR "FBZZ_ENGINE_ROOT is required")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY             "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY             "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY             "${CMAKE_SOURCE_DIR}/Binaries/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_SOURCE_DIR}/Binaries/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_SOURCE_DIR}/Binaries/Release")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_SOURCE_DIR}/Binaries/Development")

set(FBZZ_RUNTIME_BUILD_EXE_DEBUG         "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/DemoGameStandalone.exe")
set(FBZZ_RUNTIME_BUILD_EXE_RELEASE       "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/DemoGameStandalone.exe")
set(FBZZ_RUNTIME_BUILD_EXE_DEVELOPMENT   "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT}/DemoGameStandalone.exe")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_DEBUG       "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/Scripts.dll")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_RELEASE     "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/Scripts.dll")
set(FBZZ_RUNTIME_BUILD_SCRIPTS_DLL_DEVELOPMENT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT}/Scripts.dll")

add_subdirectory("${FBZZ_ENGINE_ROOT}/ThirdParty" "${CMAKE_BINARY_DIR}/FBZZ/ThirdParty")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Math" "${CMAKE_BINARY_DIR}/FBZZ/Math")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Physics" "${CMAKE_BINARY_DIR}/FBZZ/Physics")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Engine" "${CMAKE_BINARY_DIR}/FBZZ/Engine")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Editor" "${CMAKE_BINARY_DIR}/FBZZ/Editor")

# スクリプト DLL (ホットリロード用)
# WHY: Editor が SandboxScripts.dll と同じ仕組みでスクリプトをホットリロードできるよう、
#      プロジェクト固有のスクリプトを独立した DLL としてビルドする。
#      ScriptFactory の dual registry 問題は EXE から関数ポインタを渡す方式で回避する。
# WHY (OUTPUT_NAME "Scripts"): プロジェクト名に依存しない固定名にすることで
#      config.target = m_scriptDllPath.stem() が常に "Scripts" と解決し、
#      EditorApp がプロジェクト名を知らなくてもビルドターゲットを特定できる。
# WHY (CONFIGURE_DEPENDS): スクリプトは Editor から動的に追加/削除される。
#   新方式では 1 スクリプト = 1 ヘッダ (.generated.hpp / 専用 .cpp を廃止) のため、
#   ファイルの増減を CMake が自動検知できるよう CONFIGURE_DEPENDS を付ける。
#   これがないと .cpp 削除後に手動再構成しないと VS プロジェクトが古い参照を残す。
file(GLOB_RECURSE SCRIPTS_DLL_SOURCES CONFIGURE_DEPENDS
    Assets/Scripts/*.cpp
    Assets/Scripts/*.hpp
    Src/Scripts/*.cpp
    Src/Scripts/*.hpp
    Src/DemoGameScriptsDll.cpp
)
add_library(Scripts SHARED ${SCRIPTS_DLL_SOURCES})
set_target_properties(Scripts PROPERTIES OUTPUT_NAME "Scripts")
target_compile_definitions(Scripts PRIVATE GAMESCRIPTS_EXPORTS)
target_include_directories(Scripts PRIVATE Assets Src)
target_link_libraries(Scripts
    PRIVATE
        fbzz_engine
        fbzz_physics
)

add_executable(DemoGame
    Src/AppMain.cpp
    Src/GameMain.cpp
    Include/DemoGame/ProjectAPI.hpp
)
add_dependencies(DemoGame Scripts)

add_executable(DemoGameStandalone
    Src/AppMain.cpp
    Src/GameMain.cpp
    Include/DemoGame/ProjectAPI.hpp
)
add_dependencies(DemoGameStandalone Scripts)
target_compile_definitions(DemoGameStandalone PRIVATE FBZZ_STANDALONE_TARGET)
# WHY: 配布用 Release ではコンソールウィンドウを出さない。
#      AppMain.cpp は main() を持つため、Windows subsystem でも CRT の main 起動点を明示する。
target_link_options(DemoGameStandalone PRIVATE
    "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>"
    "$<$<CONFIG:Release>:/ENTRY:mainCRTStartup>"
)

target_include_directories(DemoGame
    PRIVATE
        Assets
        Include
        Src
)

target_include_directories(DemoGameStandalone
    PRIVATE
        Assets
        Include
        Src
)

target_link_libraries(DemoGame
    PRIVATE
        fbzz_engine
        fbzz_physics
        fbzz_math
        fbzz_editor
)

target_link_libraries(DemoGameStandalone
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

# WHY: fbzz_engine が assimp を動的リンクするため、exe と同じディレクトリに DLL が必要。
#      DemoGameStandalone のビルド後に Config に合わせた DLL をコピーする。
add_custom_command(TARGET DemoGameStandalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${FBZZ_ENGINE_ROOT}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${FBZZ_ENGINE_ROOT}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:DemoGameStandalone>"
)
