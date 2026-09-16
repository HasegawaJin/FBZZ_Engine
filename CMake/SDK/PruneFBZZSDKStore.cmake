# FBZZ Engine
# PruneFBZZSDKStore.cmake | CMake/SDK
# 旧方式が残した同一 version の SDK 世代を削除する
#
# WHY: 旧方式では SDK ID に commit revision や worktree の差分 fingerprint が入っていたため、
#      ソースを編集して SDK を公開するたびに新しい世代が 1 つ増え、1 世代あたり
#      lib/bin/include/Assets を丸ごと含んだ数百 MB 級のフォルダーになっていた。
#      現在は `SDK/<version>/` を毎回上書きするので世代は増えないが、既存ストアには
#      `0.1.0-dev.<rev>` や `0.1.0-dev.dirty` が残っているため publish のたびに回収する。
#
# 削除するのは「今公開した SDK と同じ version から派生した名前」だけに限る。
# WHY: 別 version の SDK は今も参照されうる正規の成果物で、ここで消す理由が無い。
#      ストア直下を無条件に掃除すると、共有ストアへ複数 version を置いた運用を壊す。

if(NOT DEFINED SDK_STORE_ROOT OR SDK_STORE_ROOT STREQUAL "")
    message(FATAL_ERROR "PruneFBZZSDKStore: SDK_STORE_ROOT が指定されていません")
endif()
if(NOT DEFINED KEEP_SDK_ID OR KEEP_SDK_ID STREQUAL "")
    message(FATAL_ERROR "PruneFBZZSDKStore: KEEP_SDK_ID が指定されていません")
endif()

if(NOT IS_DIRECTORY "${SDK_STORE_ROOT}")
    return()
endif()

# `0.1.0` に対して `0.1.0-dev.abc123` は消し、`0.1.10` は残す。区切り文字を要求することで
# version の前方一致による誤爆を防ぎ、同時に今公開した `0.1.0` 自身も対象外になる。
string(REPLACE "." "\\." SDK_LEGACY_VERSION_PREFIX "${KEEP_SDK_ID}")
set(SDK_LEGACY_PATTERN "^${SDK_LEGACY_VERSION_PREFIX}[-+.]")

file(GLOB SDK_STORE_ENTRIES LIST_DIRECTORIES true "${SDK_STORE_ROOT}/*")

foreach(SDK_STORE_ENTRY IN LISTS SDK_STORE_ENTRIES)
    if(NOT IS_DIRECTORY "${SDK_STORE_ENTRY}")
        continue()
    endif()

    get_filename_component(SDK_ENTRY_ID "${SDK_STORE_ENTRY}" NAME)
    if(NOT SDK_ENTRY_ID MATCHES "${SDK_LEGACY_PATTERN}")
        continue()
    endif()

    message(STATUS "Pruning legacy FBZZ SDK generation: ${SDK_ENTRY_ID}")

    # 削除に失敗しても SDK の公開自体は成功しているので、警告に留めてビルドは通す。
    # WHY: エディターが古い世代の DLL を掴んだままだと削除できない場合があり、
    #      そこでビルドを失敗させると本質的でない理由で開発が止まる。
    file(REMOVE_RECURSE "${SDK_STORE_ENTRY}")
    if(IS_DIRECTORY "${SDK_STORE_ENTRY}")
        message(WARNING
            "FBZZ SDK 世代 ${SDK_ENTRY_ID} を削除できませんでした。"
            "使用中のプロセスを閉じてから手動で削除してください: ${SDK_STORE_ENTRY}")
    endif()
endforeach()
