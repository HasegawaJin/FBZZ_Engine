# 標準ヘッダーのプリコンパイル。
#
# 前処理量の実測 (2026-09-03、#if を評価しない静的見積り):
#   Engine 237 TU = 8,600 万行 / Editor 146 TU = 6,200 万行。
#   このうち自作ヘッダーは 1 TU あたり平均 1.3 万行しかなく、残りは標準ヘッダー。
#   <vector> だけで 7.6 万行、<functional> 10 万行、<chrono> 13 万行ある。
#   下のリストを PCH にすると Engine -23% / Editor -16%。
#
# WHY <Windows.h> を入れないか: PCH は全 TU への強制 include なので、入れると
#     min/max/near/far や GetMessage 等のマクロが全翻訳単位に配られる。
#     ビルドは速くなるが、どの TU がその汚染に依存しているか分からなくなる。
#     Win32 が要る TU は今までどおり自分で include する。

option(FBZZ_USE_PCH "Precompile the standard headers" ON)

# WHY オプションにするか: PCH は「include を書き忘れた TU」も通してしまう。
#     -DFBZZ_USE_PCH=OFF でビルドすれば include 漏れがコンパイルエラーとして出る。
#     CI か、ヘッダーを整理した後の確認で使う。

set(FBZZ_STD_PCH_HEADERS
    <algorithm>
    <array>
    <cassert>
    <cctype>
    <chrono>
    <cmath>
    <cstddef>
    <cstdint>
    <cstdio>
    <cstring>
    <functional>
    <initializer_list>
    <limits>
    <memory>
    <optional>
    <queue>
    <source_location>
    <span>
    <string>
    <string_view>
    <type_traits>
    <unordered_map>
    <unordered_set>
    <utility>
    <vector>
)

# fbzz_use_std_pch(<target> [EXTRA <header>...])
#
# EXTRA には「そのターゲットのほぼ全 TU が既に include しているヘッダー」だけを渡す。
# 一部の TU しか使わないものを入れると、使わない TU にマクロと宣言を配るだけになる。
#
# 実測 (cl /P、2026-09-03):
#   <Windows.h>                                    91,809 行
#   <Windows.h> + WIN32_LEAN_AND_MEAN              43,448 行
#   上記 lean + d3d12 + dxgi1_6 + wrl/client       81,969 行
# DX バックエンドはこれを TU ごとに払っていた。PCH にすれば 1 回で済む。
function(fbzz_use_std_pch TARGET)
    cmake_parse_arguments(ARG "" "" "EXTRA" ${ARGN})
    if(NOT FBZZ_USE_PCH)
        return()
    endif()
    # PRIVATE: PCH をリンク先へ伝播させない。SDK 利用側のビルド設定を縛らないため。
    target_precompile_headers(${TARGET} PRIVATE ${FBZZ_STD_PCH_HEADERS} ${ARG_EXTRA})

    # WHY /Yl- が要るか: /Yc は既定で「__@@_PchSym_@00@<符号化パス>」という参照シンボルを
    #     cmake_pch.obj へ埋める。WINDOWS_EXPORT_ALL_SYMBOLS はリンク前に全オブジェクトを
    #     bindexplib で走査して exports.def を作るが、この '@' 混じりの装飾名を解釈できず
    #     «__» という壊れたエントリを吐く。ThirdParty の DirectXTex.lib も同種のシンボルを
    #     持っているため «unique match が見つからない» (LNK4022) となり、最終的に
    #     「外部シンボル __ は未解決」(LNK2001) でリンクが落ちる。
    #     PCH 参照の注入はデバッグライブラリ向けの機能で本エンジンには不要なので切る。
    #     この一行が無いと FBZZ_USE_PCH=ON と WINDOWS_EXPORT_ALL_SYMBOLS ON は共存できない。
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /Yl-)
    endif()
endfunction()
