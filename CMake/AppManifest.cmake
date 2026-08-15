# FBZZ Engine — アプリケーションマニフェストの適用
#
# WHAT: 実行ファイルへ CMake/FBZZApp.manifest を埋め込む。
# WHY:  DPI 認識をプロセス起動時に確定させるため (詳細は FBZZApp.manifest のコメント)。
#       実行時の SetProcessDpiAwarenessContext() はフォールバックとして残すが、
#       正規の宣言はこちら。

set(FBZZ_APP_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/FBZZApp.manifest"
    CACHE INTERNAL "FBZZ 実行ファイル共通のアプリケーションマニフェスト")

# 指定ターゲットの実行ファイルへ共通マニフェストを埋め込む。
#
# NOTE: MSVC では .manifest をソースとして渡すと、リンカーが自動生成するマニフェストと
#       マージした上で埋め込んでくれる (/MANIFESTINPUT 相当)。.rc 内で
#       CREATEPROCESS_MANIFEST_RESOURCE_ID を直接定義する方式は自動生成分と衝突して
#       LNK1327 になるため採らない。
function(fbzz_apply_app_manifest target)
    if(NOT MSVC)
        return()
    endif()

    if(NOT TARGET ${target})
        message(FATAL_ERROR "fbzz_apply_app_manifest: ターゲットが存在しません: ${target}")
    endif()

    # NOTE: HEADER_FILE_ONLY は付けないこと。CMake は .manifest 拡張子を認識して
    #       VS ジェネレーターでは <Manifest> 項目、Ninja/MSVC ではリンカーのマニフェスト
    #       入力として扱う。HEADER_FILE_ONLY を付けると <None> に落ちて埋め込まれなくなる。
    target_sources(${target} PRIVATE "${FBZZ_APP_MANIFEST}")
endfunction()
