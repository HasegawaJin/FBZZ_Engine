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
set(CMAKE_CXX_FLAGS_DEVELOPMENT "/Zi /O2 /Ob2 /FS" CACHE STRING "Development CXX flags" FORCE)

# MSVC 19.51 (VS 18) で /DEBUG:FASTLINK は削除された。指定するとリンクのたびに
# LNK4315 が出て /DEBUG:FULL へ倒れる (link /? のヘルプには残っているが機能しない)。
# 使える PDB モードは FULL だけなので、構成ごとに明示して固定する。
# Debug 側も明示するのは、過去に FASTLINK をキャッシュへ焼いた build ツリーを
# 再構成だけで正しい値へ戻すため。
set(CMAKE_EXE_LINKER_FLAGS_DEBUG          "/DEBUG:FULL /INCREMENTAL"
    CACHE STRING "Debug EXE linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DEBUG       "/DEBUG:FULL /INCREMENTAL"
    CACHE STRING "Debug DLL linker flags" FORCE)

# /INCREMENTAL:NO を維持するのは、WINDOWS_EXPORT_ALL_SYMBOLS が毎ビルド .def を作り直し、
# 内容が変わった時点で増分リンクは破棄されるため。見かけ上の増分を諦めて挙動を安定させる。
set(CMAKE_EXE_LINKER_FLAGS_DEVELOPMENT    "/DEBUG:FULL /INCREMENTAL:NO"
    CACHE STRING "Development EXE linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DEVELOPMENT "/DEBUG:FULL /INCREMENTAL:NO"
    CACHE STRING "Development DLL linker flags" FORCE)

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

# 出力先を変えた／消したターゲットの残骸を掃除する。レイアウトの説明もこの中にある。
include("${CMAKE_SOURCE_DIR}/CMake/FBZZArtifacts.cmake")
fbzz_prune_stale_runtime_artifacts()

# 標準ヘッダーの PCH。fbzz_use_std_pch() を各ターゲットの CMakeLists から呼ぶ。
include("${CMAKE_SOURCE_DIR}/CMake/FBZZPch.cmake")

# 翻訳単位の統合。既定 OFF、-DFBZZ_UNITY_BUILD=ON で有効。
include("${CMAKE_SOURCE_DIR}/CMake/FBZZUnity.cmake")

# clang-cl + llvm-cov のカバレッジ計装。既定 OFF、coverage preset だけが ON にする。
# 計装対象の指定はターゲットが揃った後 (このファイル末尾) で行う。
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
    add_compile_options(/utf-8 $<$<COMPILE_LANGUAGE:CXX>:/bigobj>)

    # WHY 分けるか: MSVC は «MSVC 互換フロントエンド» を持つ clang-cl でも真になる。
    #     /MP (cl.exe の子プロセス並列) と /FS (PDB 書き込みの直列化) は cl.exe 固有で、
    #     clang-cl では unknown argument 警告が翻訳単位の数だけ出て、本物の警告が埋もれる。
    #     カバレッジ用の clang-cl ビルドは Ninja が並列を持つので /MP 自体が不要。
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(/MP${FBZZ_BUILD_JOBS} /FS)
    endif()

    # WHY ここで定義するか: <Windows.h> の min / max マクロは std::min / std::max を
    #     C2589 で壊す。以前は KeyCode.hpp が «Windows.h より先に NOMINMAX を定義する»
    #     ことで全域を偶然守っていたが、それは include 順に依存した防御で、
    #     ヘッダーの並べ替え 1 つで崩れる。ビルド全体の前提として一度だけ宣言する。
    add_compile_definitions(NOMINMAX)
endif()

# D3D の検証レイヤー。
# FBZZ_GPU_VALIDATION         … 検証レイヤーのコードを «積む» 構成 (Debug / Development)
# FBZZ_GPU_VALIDATION_DEFAULT … 何も指定しないときに «立てる» か (Debug だけ)
#
# WHY Development は既定で立てないか: 検証レイヤーの負荷は API 呼び出し 1 回ごとに掛かる。
#     溜めた検証メッセージを読まない工夫をしても、レイヤー本体の負荷は消せない。
#     Development は «Release 相当の速度で遊べる» ことが存在理由なので、既定は素通しにする。
# WHY コード自体は積むか: 環境変数 FBZZ_GPU_VALIDATION=1 で、ビルドし直さずに
#     «Release 相当の速度の構成に検証だけ足す» ことができる。漏れを追う日はこれを使う。
add_compile_definitions(
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:Development>>:FBZZ_GPU_VALIDATION>
    $<$<CONFIG:Debug>:FBZZ_GPU_VALIDATION_DEFAULT>
)

# 実行ファイル共通のアプリケーションマニフェスト (DPI 認識の宣言) を提供する。
# WHY サブディレクトリより前か: 各 Launcher の CMakeLists が fbzz_apply_app_manifest() を呼ぶため、
#      関数定義がその時点で見えている必要がある。
include(CMake/AppManifest.cmake)

# WHY サブディレクトリより前か: ThirdParty/CMakeLists.txt が GoogleTest / GoogleMock を
#      このオプションで囲っている。テストを切った構成では vendor 済み gtest を一切
#      ビルドしないため、判定がその時点で確定している必要がある。
option(FBZZ_BUILD_TESTS "Build unit tests" ON)
if(FBZZ_BUILD_TESTS)
    enable_testing()
endif()

# Add subdirectories in dependency order.
add_subdirectory(ThirdParty)
add_subdirectory(Projects/Math)
add_subdirectory(Projects/Physics)
# WHY Physics の直後か: Fluid は Physics の兄弟で、Math しか見ない (Docs/design/fluid-library.md)。
add_subdirectory(Projects/Fluid)
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

if(FBZZ_BUILD_TESTS)
    add_subdirectory(Projects/Tests)
endif()

# カバレッジの計装対象。OpenCppCoverage 側の --sources と同じ範囲に揃える。
# FBZZCore は Engine/src/Core の OBJECT ライブラリで、成果物は FBZZEngine.dll に入る。
# そのため «計装するのは FBZZCore、実行時ライブラリを要るのは FBZZEngine» とずれる。
if(FBZZ_COVERAGE)
    fbzz_instrument_for_coverage(FBZZMath FBZZPhysics FBZZFluid FBZZCore)
    fbzz_link_coverage_runtime(FBZZMath FBZZPhysics FBZZFluid FBZZEngine)
endif()
