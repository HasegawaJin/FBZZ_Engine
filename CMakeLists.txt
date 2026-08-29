cmake_minimum_required(VERSION 3.25)
project(FBZZEngine VERSION 0.1.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ソースツリー専用EditorだけがEngineの自動再ビルドを利用する。
# WHY: SDKへ公開するDLLに開発機の絶対パスを埋め込むと、共有SDKから元のソースbuildが起動してしまう。
option(FBZZ_ENABLE_SOURCE_REBUILD "Enable source-tree Engine rebuild bootstrap" OFF)

# WHY: 共有 SDK とゲーム側で CRT の選択が暗黙値に依存すると、同じ構成名でも
#      STL オブジェクトを DLL 境界へ渡した時点で ABI が壊れる。全ターゲットを
#      Debug=/MDd、Development/Release=/MD に固定し、SDK package 側でも同値を検証する。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Development: Release相当の最適化 + デバッグシンボルあり + NDEBUG なし（アサート有効）。
# WHY: /Ob1 は明示的に inline 指定した関数しか展開しないため、DX12 のコマンド記録のように
#      小さな関数を大量に呼ぶ経路で Release と大きな差が出る。/Ob2 で通常の最適化インラインを許可する。
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Development" CACHE STRING "Build configurations" FORCE)
endif()
set(CMAKE_CXX_FLAGS_DEVELOPMENT           "/Zi /O2 /Ob2 /FS"            CACHE STRING "Development CXX flags"        FORCE)
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

# ---------------------------------------------------------------------------
# 並列コンパイル数の上限
# ---------------------------------------------------------------------------
# WHY: /MP を数値なしで指定すると cl.exe は「論理コア数」ぶんの子プロセスを起動する。
#      本エンジンは Inspector / schema など /bigobj が要るほどテンプレート展開の重い
#      翻訳単位を多数抱えており、cl.exe 1 つで 1〜2GB を使う。多コア機ではこれだけで
#      物理メモリを使い切り、並行して走らせている別ビルドや起動中のエディタまで
#      スワップに巻き込まれて全体が遅くなる。
#      そこで「コア数」ではなく「メモリで同時に養える cl.exe の数」を上限にする。
#      -DFBZZ_BUILD_JOBS=<n> または環境変数 FBZZ_BUILD_JOBS で明示上書きできる。
if(DEFINED FBZZ_BUILD_JOBS)
    set(FBZZ_BUILD_JOBS_DEFAULT "${FBZZ_BUILD_JOBS}")
    set(FBZZ_BUILD_JOBS_ORIGIN "-DFBZZ_BUILD_JOBS で明示指定")
elseif(DEFINED ENV{FBZZ_BUILD_JOBS})
    set(FBZZ_BUILD_JOBS_DEFAULT "$ENV{FBZZ_BUILD_JOBS}")
    set(FBZZ_BUILD_JOBS_ORIGIN "環境変数 FBZZ_BUILD_JOBS で指定")
else()
    cmake_host_system_information(RESULT FBZZ_HOST_CORES     QUERY NUMBER_OF_LOGICAL_CORES)
    cmake_host_system_information(RESULT FBZZ_HOST_MEMORY_MB QUERY TOTAL_PHYSICAL_MEMORY)
    # cl.exe 1 プロセス = 2GB 見積り (重い TU に合わせた保守値)。
    math(EXPR FBZZ_JOBS_BY_MEMORY "${FBZZ_HOST_MEMORY_MB} / 2048")
    # OS / エディタ / デバッガ用に 2 コア残す。
    math(EXPR FBZZ_JOBS_BY_CORES "${FBZZ_HOST_CORES} - 2")
    set(FBZZ_BUILD_JOBS_DEFAULT ${FBZZ_JOBS_BY_CORES})
    if(FBZZ_JOBS_BY_MEMORY LESS FBZZ_BUILD_JOBS_DEFAULT)
        set(FBZZ_BUILD_JOBS_DEFAULT ${FBZZ_JOBS_BY_MEMORY})
    endif()
    # 下限 2 / 上限 12。上限を切るのは、メモリに余裕があっても
    # ディスク I/O とリンクが律速になり並列度を上げても縮まなくなるため。
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

# MSVC source files are UTF-8.
if(MSVC)
    # WHY: Inspector / schema の大きな翻訳単位はテンプレート展開によって通常 COFF の
    #      セクション数上限を超えることがある。/bigobj は形式上限だけを拡張し、
    #      実行コードや ABI を変えず C1128 を防ぐ。
    add_compile_options(/utf-8 /MP${FBZZ_BUILD_JOBS} /FS $<$<COMPILE_LANGUAGE:CXX>:/bigobj>)
endif()

# 実行ファイル共通のアプリケーションマニフェスト (DPI 認識の宣言) を提供する。
# WHY サブディレクトリより前か: 各 Launcher の CMakeLists が fbzz_apply_app_manifest() を呼ぶため、
#      関数定義がその時点で見えている必要がある。
include(CMake/AppManifest.cmake)

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

# VS ソリューションを開いたときのデフォルトスタートアッププロジェクトを Sandbox に設定する
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT Sandbox)

option(FBZZ_BUILD_TESTS "Build unit tests" ON)
if(FBZZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Projects/Tests)
endif()
