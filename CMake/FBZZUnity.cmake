# 翻訳単位の統合 (unity build)。
#
# ビルド時間は「TU 数 × 1 TU あたりの前処理行数」で決まる。PCH と include 整理で
# 後者を削ったので、残る大きな軸は TU 数そのもの。統合すると、同じヘッダーを何度も
# 通す分がまとめて消える。
#
# 既定は OFF。統合は次の理由で «書き方» を選ぶため、有効化には検証が要る。
#   - 無名 namespace の同名シンボルが衝突する (本エンジンは 144 の .cpp が無名 namespace を持つ)
#   - .cpp 側の #define が同じバッチの後続ファイルへ漏れる
#   - ファイルスコープの using namespace が後続ファイルの名前解決を変える
# 有効にして通らなくなったら、原因のファイルを EXCLUDE へ足すか、衝突名を直す。

option(FBZZ_UNITY_BUILD "Merge translation units per target" OFF)
set(FBZZ_UNITY_BATCH_SIZE 16 CACHE STRING "1 つへまとめる翻訳単位の数")

# fbzz_use_unity_build(<target> [EXCLUDE <source>...])
#
# EXCLUDE には «単独でしかコンパイルできない» ファイルを渡す。典型は
# STB_IMAGE_IMPLEMENTATION のようにヘッダーオンリー実装を展開する翻訳単位で、
# 他と混ざると同じ実体が二重に出るか、マクロが後続ファイルを壊す。
function(fbzz_use_unity_build TARGET)
    cmake_parse_arguments(ARG "" "" "EXCLUDE" ${ARGN})
    if(NOT FBZZ_UNITY_BUILD)
        return()
    endif()

    set_target_properties(${TARGET} PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_MODE BATCH
        UNITY_BUILD_BATCH_SIZE ${FBZZ_UNITY_BATCH_SIZE}
    )

    foreach(EXCLUDED IN LISTS ARG_EXCLUDE)
        if(EXISTS "${EXCLUDED}")
            set_source_files_properties("${EXCLUDED}"
                TARGET_DIRECTORY ${TARGET}
                PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
        endif()
    endforeach()
endfunction()
