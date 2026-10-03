# @file    CMakeLists.txt
# @brief   エンジン全体の共通ビルド設定とサブプロジェクトの登録。
# @author  Hasegawa Jin
# @date    2026-05-15
cmake_minimum_required(VERSION 3.25)
# @note 版はリリースのタグ (vX.Y.Z) と揃える。SDK の置き場所 SDK/<版>/ と Script DLL の ABI 署名もこの値から決まる (Docs/conventions/git.md)。
# @note Editor の miniz.c は C としてコンパイルするため、Ninja でも C のビルドルールを生成する。
# @see https://cmake.org/cmake/help/latest/command/project.html LANGUAGES
project(FBZZEngine VERSION 0.9.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# @note ON のとき build 直下へ engine-rebuild.config を書き、ソースツリーから起動した exe だけが Engine を自動再ビルドする。
# @note コンパイル結果には影響しないので、同じツリーの成果物をそのまま共有 SDK へ公開できる。
option(FBZZ_ENABLE_SOURCE_REBUILD "Enable source-tree Engine rebuild bootstrap" OFF)

# @note CRT を暗黙値に任せると SDK とゲームで食い違い、DLL 境界で STL の ABI が壊れる。
# @note Debug=/MDd、Development/Release=/MD に固定し、SDK package 側でも同値を検証する。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# @note Development は Release 相当の最適化 + デバッグシンボル + NDEBUG なし (assert 有効)。
# @note /Ob1 は inline 指定の関数しか展開せず、DX12 のコマンド記録のような小関数の多い経路が遅くなるので /Ob2 にする。
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Development" CACHE STRING "Build configurations" FORCE)
endif()
set(CMAKE_CXX_FLAGS_DEVELOPMENT "/Zi /O2 /Ob2 /FS" CACHE STRING "Development CXX flags" FORCE)

# @note MSVC 19.51 (VS 18) で /DEBUG:FASTLINK は削除され、指定すると LNK4315 で FULL へ倒れる。使えるのは FULL だけ。
# @note キャッシュに FASTLINK が残った build ツリーを再構成だけで直すため、Debug も FORCE で明示する。
set(CMAKE_EXE_LINKER_FLAGS_DEBUG          "/DEBUG:FULL /INCREMENTAL"
    CACHE STRING "Debug EXE linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DEBUG       "/DEBUG:FULL /INCREMENTAL"
    CACHE STRING "Debug DLL linker flags" FORCE)

# @note WINDOWS_EXPORT_ALL_SYMBOLS が毎ビルド .def を作り直し増分リンクが破棄されるので、Development は /INCREMENTAL:NO。
set(CMAKE_EXE_LINKER_FLAGS_DEVELOPMENT    "/DEBUG:FULL /INCREMENTAL:NO"
    CACHE STRING "Development EXE linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DEVELOPMENT "/DEBUG:FULL /INCREMENTAL:NO"
    CACHE STRING "Development DLL linker flags" FORCE)

# @note RuntimeBuild は build.config に記録したパスから SandboxStandalone.exe をコピーするので、構成ごとの出力先を固定する。
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

# @note 出力レイアウトの説明は FBZZArtifacts.cmake にある。
include("${CMAKE_SOURCE_DIR}/CMake/FBZZArtifacts.cmake")
fbzz_prune_stale_runtime_artifacts()

# @note fbzz_use_std_pch() は各ターゲットの CMakeLists から呼ぶ。
include("${CMAKE_SOURCE_DIR}/CMake/FBZZPch.cmake")

# @note Unity ビルドは既定 OFF。-DFBZZ_UNITY_BUILD=ON で有効。
include("${CMAKE_SOURCE_DIR}/CMake/FBZZUnity.cmake")

# @note clang-cl + llvm-cov の計装は coverage preset だけが ON。対象の指定はターゲットが揃うファイル末尾で行う。
include("${CMAKE_SOURCE_DIR}/CMake/FBZZCoverage.cmake")

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

# @name 並列コンパイル数の上限
# @note 数値なしの /MP は論理コア数ぶん cl.exe を起動し、重い TU は 1 つ 1〜2GB 使うため物理メモリを使い切る。
# @note 上限はコア数ではなくメモリで養える cl.exe の数。-DFBZZ_BUILD_JOBS=<n> か環境変数 FBZZ_BUILD_JOBS で上書きできる。
if(DEFINED FBZZ_BUILD_JOBS)
    set(FBZZ_BUILD_JOBS_DEFAULT "${FBZZ_BUILD_JOBS}")
    set(FBZZ_BUILD_JOBS_ORIGIN "-DFBZZ_BUILD_JOBS で明示指定")
elseif(DEFINED ENV{FBZZ_BUILD_JOBS})
    set(FBZZ_BUILD_JOBS_DEFAULT "$ENV{FBZZ_BUILD_JOBS}")
    set(FBZZ_BUILD_JOBS_ORIGIN "環境変数 FBZZ_BUILD_JOBS で指定")
else()
    cmake_host_system_information(RESULT FBZZ_HOST_CORES     QUERY NUMBER_OF_LOGICAL_CORES)
    cmake_host_system_information(RESULT FBZZ_HOST_MEMORY_MB QUERY TOTAL_PHYSICAL_MEMORY)
    # @note cl.exe 1 プロセス = 2GB 見積り (重い TU に合わせた保守値)。
    math(EXPR FBZZ_JOBS_BY_MEMORY "${FBZZ_HOST_MEMORY_MB} / 2048")
    # @note OS / エディタ / デバッガ用に 2 コア残す。
    math(EXPR FBZZ_JOBS_BY_CORES "${FBZZ_HOST_CORES} - 2")
    set(FBZZ_BUILD_JOBS_DEFAULT ${FBZZ_JOBS_BY_CORES})
    if(FBZZ_JOBS_BY_MEMORY LESS FBZZ_BUILD_JOBS_DEFAULT)
        set(FBZZ_BUILD_JOBS_DEFAULT ${FBZZ_JOBS_BY_MEMORY})
    endif()
    # @note 上限 12 は、それ以上はディスク I/O とリンクが律速になり縮まないため。
    if(FBZZ_BUILD_JOBS_DEFAULT LESS 2)
        set(FBZZ_BUILD_JOBS_DEFAULT 2)
    elseif(FBZZ_BUILD_JOBS_DEFAULT GREATER 12)
        set(FBZZ_BUILD_JOBS_DEFAULT 12)
    endif()
    set(FBZZ_BUILD_JOBS_ORIGIN "cores=${FBZZ_HOST_CORES} memory=${FBZZ_HOST_MEMORY_MB}MB より自動決定")
endif()
set(FBZZ_BUILD_JOBS "${FBZZ_BUILD_JOBS_DEFAULT}" CACHE STRING
    "1 プロジェクトあたり同時に走らせる cl.exe の上限 (/MP<n>)")
message(STATUS "FBZZ: 並列コンパイル上限 = /MP${FBZZ_BUILD_JOBS} (${FBZZ_BUILD_JOBS_ORIGIN})")

if(MSVC)
    # @note Inspector / schema の大きな TU は COFF のセクション数上限を超え C1128 になる。/bigobj は ABI を変えない。
    add_compile_options(/utf-8 $<$<COMPILE_LANGUAGE:CXX>:/bigobj>)

    # @note MSVC は clang-cl でも真。/MP と /FS は cl.exe 固有で、clang-cl では TU ごとに unknown argument 警告が出る。
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(/MP${FBZZ_BUILD_JOBS} /FS)
    endif()

    # @note <Windows.h> の min / max は std::min / max を C2589 で壊す。include 順に依存させずビルド全体で定義する。
    add_compile_definitions(NOMINMAX)

    # @note ThirdParty/DirectXTex はビルド済み .lib で PDB を同梱しないため、リンクのたびに obj の数だけ LNK4099 が出る。
    # @note 自前のターゲットは /DEBUG:FULL で必ず PDB を出すので、この警告を落としても失う情報はない。
    # @see https://learn.microsoft.com/cpp/build/reference/ignore-ignore-warnings "/IGNORE (Ignore Specific Warnings)"
    add_link_options(/IGNORE:4099)
endif()

# @note FBZZ_GPU_VALIDATION は D3D 検証レイヤーのコードを積む構成、FBZZ_GPU_VALIDATION_DEFAULT は既定で有効にする構成。
# @note 検証レイヤーは API 呼び出しごとに負荷が掛かるので、Release 相当の速度が目的の Development は既定で無効。
# @note Development でも環境変数 FBZZ_GPU_VALIDATION=1 で再ビルドなしに有効化できる。
add_compile_definitions(
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:Development>>:FBZZ_GPU_VALIDATION>
    $<$<CONFIG:Debug>:FBZZ_GPU_VALIDATION_DEFAULT>
)

# @note 各 Launcher が fbzz_apply_app_manifest() (DPI 認識の宣言) を呼ぶので、add_subdirectory より前に include する。
include(CMake/AppManifest.cmake)

# @note ThirdParty/CMakeLists.txt が gtest / gmock をこのオプションで囲うので、add_subdirectory より前に確定させる。
option(FBZZ_BUILD_TESTS "Build unit tests" ON)
option(FBZZ_ENABLE_DX12 "Build the Direct3D 12 backend" ON)
include(CMake/FBZZAgilitySDK.cmake)
if(FBZZ_BUILD_TESTS)
    enable_testing()
endif()

# @note 依存順に並べる。
add_subdirectory(ThirdParty)
add_subdirectory(Projects/Math)
add_subdirectory(Projects/Physics)
# @note Fluid は Physics の兄弟で Math しか見ない。
# @see Docs/design/fluid-library.md
add_subdirectory(Projects/Fluid)
add_subdirectory(Projects/Core)
add_subdirectory(Projects/Graphics)
add_subdirectory(Projects/Engine)
add_subdirectory(Projects/Editor)
add_subdirectory(Projects/EditorLauncher)
# @note GameHub は Electron 版 (Projects/GameHubNext) で完結するので CMake のビルド対象に含めない。
add_subdirectory(Projects/Sandbox)

# @note 共有 Engine SDK を SDK/<engine-version>/ に生成する。外部ゲームはソースツリーでなく IMPORTED target を参照する。
include(CMake/FBZZSDK.cmake)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT Sandbox)

if(FBZZ_BUILD_TESTS)
    add_subdirectory(Projects/Tests)
endif()

# @note 計装範囲は OpenCppCoverage の --sources と揃える (Projects/Core = FBZZCore、Projects/Engine/src/Core = FBZZEnginePlatform)。
# @note FBZZEnginePlatform は FBZZEngine.dll に入る OBJECT ライブラリなので、計装はそちら・ランタイムのリンクは FBZZEngine とずれる。
if(FBZZ_COVERAGE)
    fbzz_instrument_for_coverage(FBZZMath FBZZPhysics FBZZFluid FBZZCore FBZZEnginePlatform)
    fbzz_link_coverage_runtime(FBZZMath FBZZPhysics FBZZFluid FBZZCore FBZZEngine)
endif()
