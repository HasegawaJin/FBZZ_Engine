# clang-cl + llvm-cov による分岐 (C1) / 条件・MC/DC (C2 以上) カバレッジの計装。
#
# 通常ビルド (MSVC) では何もしない。coverage preset が -DFBZZ_COVERAGE=ON を渡したときだけ、
# 計測対象のターゲットへ計装フラグを足す。
#
# WHY MSVC ではなく clang-cl か: MSVC には分岐を数える機構が無く、OpenCppCoverage が
#     出せるのは行カバレッジ (C0) だけ。分岐と条件まで測るには LLVM の
#     source-based code coverage が要り、それは clang でしかコンパイルできない。
#     通常ビルドは MSVC のまま残し、カバレッジ計測のときだけ 2 本目のツールチェーンを使う。
#
# WHY 全体ではなく対象ターゲットだけへ足すか: 計装はコンパイル時間とバイナリサイズを
#     大きく増やす。分母に入れると決めているのは Math / Physics / Engine の Core だけなので、
#     Renderer や ThirdParty まで計装しても «レポートで捨てるデータ» を作るために
#     ビルド時間を払うだけになる。OpenCppCoverage 側の --sources と同じ範囲に揃えてある。

option(FBZZ_COVERAGE "clang-cl + llvm-cov で分岐 / MC/DC カバレッジを計測する" OFF)

if(NOT FBZZ_COVERAGE)
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "FBZZ_COVERAGE=ON には clang-cl が必要です (現在: ${CMAKE_CXX_COMPILER_ID})。\n"
        "  cmake --preset coverage で configure してください。\n"
        "  clang-cl が無い場合は Visual Studio インストーラーの\n"
        "  «C++ Clang compiler for Windows» を追加してください。")
endif()

set(FBZZ_COVERAGE_COMPILE_OPTIONS -fprofile-instr-generate -fcoverage-mapping)

# MC/DC (改良条件判定網羅) は clang 18 から。条件が 6 個を超える判定は
# clang が警告を出して MC/DC の記録だけを飛ばす (分岐カバレッジは残る)。
if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 18)
    list(APPEND FBZZ_COVERAGE_COMPILE_OPTIONS -fcoverage-mcdc)
    set(FBZZ_COVERAGE_HAS_MCDC ON)
else()
    set(FBZZ_COVERAGE_HAS_MCDC OFF)
    message(WARNING "clang ${CMAKE_CXX_COMPILER_VERSION} は -fcoverage-mcdc 非対応です。分岐 (C1) までの計測になります。")
endif()

# プロファイル実行時ライブラリの置き場を探す。
#
# WHY リンクオプションではなく /LIBPATH か: CMake は clang-cl を «MSVC 互換フロントエンド» と
#     見なし、リンクをドライバー (clang-cl) ではなくリンカー (lld-link / link.exe) へ直接投げる。
#     そのため -fprofile-instr-generate をリンク時に渡しても、リンカーは解釈できない。
#     一方 clang は計装したオブジェクトへ /DEFAULTLIB:clang_rt.profile-x86_64.lib の
#     リンカーディレクティブを埋め込むので、«探せる場所» さえ与えればリンクは通る。
get_filename_component(FBZZ_CLANG_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(FBZZ_CLANG_ROOT_DIR "${FBZZ_CLANG_BIN_DIR}" DIRECTORY)

file(GLOB FBZZ_CLANG_RT_CANDIDATES
    "${FBZZ_CLANG_ROOT_DIR}/lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib"
    "${FBZZ_CLANG_ROOT_DIR}/lib/clang/*/lib/x86_64-pc-windows-msvc/clang_rt.profile.lib"
)
if(NOT FBZZ_CLANG_RT_CANDIDATES)
    message(FATAL_ERROR
        "clang_rt.profile-x86_64.lib が見つかりません。\n"
        "  探索した場所: ${FBZZ_CLANG_ROOT_DIR}/lib/clang/*/lib/\n"
        "  clang のインストールが不完全か、compiler-rt が同梱されていません。")
endif()
list(GET FBZZ_CLANG_RT_CANDIDATES 0 FBZZ_CLANG_RT_LIB)
get_filename_component(FBZZ_CLANG_RT_LIB_DIR "${FBZZ_CLANG_RT_LIB}" DIRECTORY)

message(STATUS "FBZZ: カバレッジ計装 ON (clang ${CMAKE_CXX_COMPILER_VERSION}, MC/DC=${FBZZ_COVERAGE_HAS_MCDC})")
message(STATUS "FBZZ: プロファイル実行時ライブラリ = ${FBZZ_CLANG_RT_LIB_DIR}")

# fbzz_instrument_for_coverage(<target>...)
#   指定したターゲットのソースを計装する。OBJECT ライブラリにも使える。
function(fbzz_instrument_for_coverage)
    foreach(target IN LISTS ARGN)
        if(NOT TARGET ${target})
            message(FATAL_ERROR "fbzz_instrument_for_coverage: 未知のターゲット ${target}")
        endif()
        target_compile_options(${target} PRIVATE ${FBZZ_COVERAGE_COMPILE_OPTIONS})
    endforeach()
endfunction()

# fbzz_link_coverage_runtime(<target>...)
#   計装したオブジェクトを実際にリンクするターゲット (DLL / EXE) へ、
#   プロファイル実行時ライブラリの探索パスを与える。
#   OBJECT ライブラリ経由で計装コードを取り込む FBZZEngine のように、
#   自分自身は計装していないターゲットにも必要になる。
function(fbzz_link_coverage_runtime)
    foreach(target IN LISTS ARGN)
        if(NOT TARGET ${target})
            message(FATAL_ERROR "fbzz_link_coverage_runtime: 未知のターゲット ${target}")
        endif()
        target_link_options(${target} PRIVATE "/LIBPATH:${FBZZ_CLANG_RT_LIB_DIR}")
    endforeach()
endfunction()
