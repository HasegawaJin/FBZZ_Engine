# テストターゲットの生成規則。Projects/Tests/CMakeLists.txt から使う。
#
# 1 スイート = OBJECT ライブラリ 1 つ + コンソール exe 1 つ。
# ビジュアル検証ベンチと GUI 系は同じ OBJECT ライブラリを束ねて 1 プロセスに載せられるため、
# テスト本体のソースはどちらからも二重にコンパイルされない。

include(GoogleTest)

set(FBZZ_TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/Tests")

# 1 テストあたりの制限時間 [秒]。超えた時点で CTest がプロセスを殺し、次のテストへ進む。
#
# WHY 必ず付けるか: 無限ループや «押されるまで消えないダイアログ» を踏んだテストが 1 件でも
#     あると、CTest はそこで永久に待ち続け、残り全部が実行されないまま終わる。
#     実際 EPA の縮退した配置でポリトープが膨張し続け、そこから先が丸ごと走らなかった。
#     落ちるテストより «止まるテスト» の方が被害が大きいので、上限は必ず与える。
# 値の根拠: 自動テストは 1 件あたり数 ms で終わる設計 (sleep 禁止・固定刻み)。
#     Debug の CI で負荷が乗っても 2 桁 ms には収まるため、30 秒は «明らかに異常» の線。
set(FBZZ_TEST_TIMEOUT 30 CACHE STRING "1 テストあたりの制限時間 [秒]")

# テスト成果物の出力先を構成ごとに固定する。
# WHY 構成ごとに書くか: ルートの CMakeLists が CMAKE_RUNTIME_OUTPUT_DIRECTORY_<CONFIG> を
#     設定しているため、各ターゲットは RUNTIME_OUTPUT_DIRECTORY_<CONFIG> を初期値として
#     受け取る。CMake は構成つきプロパティを先に見るので、構成なしの
#     RUNTIME_OUTPUT_DIRECTORY だけを指定しても負けて Binaries/<config> 直下へ出てしまう。
#     Sandbox / Editor も同じ理由で構成ごとに書いている。
function(fbzz_set_test_output_dir TARGET)
    set_target_properties(${TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY             "${FBZZ_TEST_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Tests"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Tests"
        RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Tests"
    )
endfunction()

# 全テスト exe 共通の main。TestKit 側に 1 本だけ置く。
set(FBZZ_TESTKIT_MAIN_SOURCE "${CMAKE_SOURCE_DIR}/Projects/Tests/TestKit/src/Main.cpp")

# 生成済みスイートの一覧。ベンチや集約ターゲットがリンク対象を組み立てるのに使う。
define_property(GLOBAL PROPERTY FBZZ_TEST_SUITES
    BRIEF_DOCS "fbzz_add_test_suite で登録された自動テストスイート名"
    FULL_DOCS  "全スイートを 1 プロセスへ束ねたいターゲットのための登録簿")

# 依存 DLL を exe の隣へ並べる。
# WHY: Visual Studio からテスト exe 単体を起動したときに PATH を手で設定しなくて済ませる。
#      TARGET_RUNTIME_DLLS は CMake が知っている SHARED ターゲットだけを解決するため、
#      IMPORTED 化していない Assimp は別途コピーする。
function(fbzz_stage_test_runtime TARGET)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${TARGET}> $<TARGET_FILE_DIR:${TARGET}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
            $<TARGET_FILE_DIR:${TARGET}>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endfunction()

# fbzz_add_test_suite(<Suite> SOURCES <...> [LINKS <...>] [MANUAL])
#
#   Suite   スイート名。FBZZTests<Suite>Obj と FBZZTests<Suite> を生成する。
#   LINKS   FBZZTestKit に加えてリンクするターゲット。
#   MANUAL  CTest に登録しない (手動テスト)。
function(fbzz_add_test_suite SUITE)
    cmake_parse_arguments(ARG "MANUAL" "" "SOURCES;LINKS" ${ARGN})

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "fbzz_add_test_suite(${SUITE}): SOURCES が空です")
    endif()

    set(objTarget "FBZZTests${SUITE}Obj")
    set(exeTarget "FBZZTests${SUITE}")

    # WHY OBJECT か: STATIC にすると、TEST_F マクロが生やす静的初期化子を誰も参照しないため
    #      リンカが翻訳単位ごと捨て、テストが 1 件も登録されない exe が出来上がる。
    #      OBJECT ライブラリはオブジェクトファイルを必ず全部リンクする。
    add_library(${objTarget} OBJECT ${ARG_SOURCES})
    target_link_libraries(${objTarget} PUBLIC FBZZTestKit ${ARG_LINKS})
    set_target_properties(${objTarget} PROPERTIES FOLDER "Tests/Objects")

    # Main.cpp を exe 側のソースにすることで、OBJECT ライブラリからは
    # 「オブジェクト + 使用要件」の両方が target_link_libraries 経由で入る。
    add_executable(${exeTarget} "${FBZZ_TESTKIT_MAIN_SOURCE}")
    target_link_libraries(${exeTarget} PRIVATE ${objTarget})
    set_target_properties(${exeTarget} PROPERTIES FOLDER "Tests")
    fbzz_set_test_output_dir(${exeTarget})
    fbzz_stage_test_runtime(${exeTarget})

    if(ARG_MANUAL)
        return()
    endif()

    gtest_discover_tests(${exeTarget}
        DISCOVERY_TIMEOUT 60
        WORKING_DIRECTORY "${FBZZ_TEST_OUTPUT_DIR}"
        PROPERTIES TIMEOUT ${FBZZ_TEST_TIMEOUT}
    )
    set_property(GLOBAL APPEND PROPERTY FBZZ_TEST_SUITES ${SUITE})
endfunction()
