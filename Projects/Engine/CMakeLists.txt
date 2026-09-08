# Engine の内部レイヤー構成。
#
#   FBZZCore           Logger / Time / Memory / Scheduler / Util / Profiler / Input
#   FBZZRHI            IRenderer 等の抽象とその上の共通実装 (Mesh / Material / ResourceManager)
#   FBZZRenderPlatform D3D 共通の下支え (GpuValidation / RenderTargetCapture)
#   FBZZRenderDX11     DirectX 11 バックエンド
#   FBZZRenderDX12     DirectX 12 バックエンド (FBZZ_ENABLE_DX12)
#   FBZZEngine         Scene / Asset / Audio / AI と合成ルート。上記を全て飲み込む DLL
#
# WHY OBJECT ライブラリか (STATIC ではなく):
#   本エンジンは FBZZ_REGISTER_VOLUME_OVERRIDE のような «誰も参照しない静的初期化子で
#   自分を登録する» 実装を持つ。STATIC にするとリンカが「参照されていない」と判断して
#   翻訳単位ごと捨て、機能が *エラーも警告も出さずに* 消える。テスト基盤が同じ理由で
#   OBJECT を強制しているのと同じ話 (CMake/FBZZTests.cmake)。
#
# WHY target_link_libraries ではなく $<TARGET_OBJECTS:> で取り込むか:
#   install(EXPORT) はリンク依存先も同じ export set に居ることを要求する (ImGui が
#   FBZZTargets に入っているのはそのため)。オブジェクトとして取り込めばリンク依存に
#   ならないので、内部レイヤーを SDK の公開ターゲットへ漏らさずに済む。

option(FBZZ_ENABLE_DX12 "Build the Direct3D 12 backend" ON)

# ── サードパーティの実体確認 ────────────────────────────────────────────────
set(DIRECTXTEX_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/include")
set(DIRECTXTEX_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Debug/DirectXTex.lib")
set(DIRECTXTEX_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Release/DirectXTex.lib")

# stb_truetype / stb_rect_pack (public domain, ヘッダオンリー)。
# フォントの実行時ラスタライズとアトラス配置に使う (Docs/design/font-system.md)。
# 実装の展開は src/Renderer/StbFontImpl.cpp のみが行う。
set(STB_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Stb")

set(ASSIMP_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/include")
set(ASSIMP_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/lib/Debug/assimp-vc145-mtd.lib")
set(ASSIMP_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/lib/Release/assimp-vc145-mt.lib")

foreach(REQUIRED_PATH
    "${DIRECTXTEX_INCLUDE}"
    "${DIRECTXTEX_LIB_DEBUG}"
    "${DIRECTXTEX_LIB_RELEASE}"
    "${ASSIMP_INCLUDE}"
    "${ASSIMP_LIB_DEBUG}"
    "${ASSIMP_LIB_RELEASE}"
    "${STB_INCLUDE}/stb_truetype.h"
    "${STB_INCLUDE}/stb_rect_pack.h"
    "${STB_INCLUDE}/stb_image.h"
)
    if(NOT EXISTS "${REQUIRED_PATH}")
        message(FATAL_ERROR "Required third-party dependency was not found: ${REQUIRED_PATH}")
    endif()
endforeach()

# ── EngineRebuildBootstrap 用のビルド情報埋め込み ────────────────────────────
# WHY: FBZZEngine.dll は各 exe が暗黙リンクするため、実行中はロックされ再ビルドできない。
#      起動時に「Engine ソースが DLL より新しいか」を判定し、古ければ cmake を叩いて
#      再ビルド→再起動する (EngineRebuildBootstrap.cpp)。そのために必要な
#      「このバイナリを生成した build ディレクトリ・engine ソース・cmake・構成名」を
#      コンパイル時に埋め込む。
# WHY (Release で埋め込まない): Release は配布構成。開発機の絶対パスを配布物へ残さないよう、
#      dev パス系マクロは Debug / Development のみに付与し、Release では自己リビルド機構を無効化する。
get_filename_component(FBZZ_ENGINE_ROOT_ABS "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(FBZZ_ENGINE_DEFINES
    FBZZ_CMAKE_CONFIG="$<CONFIG>"
    FBZZ_ENABLE_DX12=$<IF:$<BOOL:${FBZZ_ENABLE_DX12}>,1,0>
    "$<$<AND:$<BOOL:${FBZZ_ENABLE_SOURCE_REBUILD}>,$<NOT:$<CONFIG:Release>>>:FBZZ_DEV_BUILD_DIR=\"${CMAKE_BINARY_DIR}\">"
    "$<$<AND:$<BOOL:${FBZZ_ENABLE_SOURCE_REBUILD}>,$<NOT:$<CONFIG:Release>>>:FBZZ_DEV_ENGINE_ROOT=\"${FBZZ_ENGINE_ROOT_ABS}\">"
    "$<$<AND:$<BOOL:${FBZZ_ENABLE_SOURCE_REBUILD}>,$<NOT:$<CONFIG:Release>>>:FBZZ_DEV_CMAKE_COMMAND=\"${CMAKE_COMMAND}\">"
)

# 統合してはいけない翻訳単位。
#   StbFontImpl.cpp   STB_TRUETYPE_IMPLEMENTATION / STB_RECT_PACK_IMPLEMENTATION の展開場所
#   Cursor.cpp        STB_IMAGE_IMPLEMENTATION の展開場所 (カーソル画像のデコード)
#   Window.cpp        DPI_AWARENESS_CONTEXT_* と WM_DPICHANGED を #undef せずに定義する
#   Input.cpp         WIN32_LEAN_AND_MEAN を定義するため、同じバッチの後続ファイルの
#                     Windows.h の中身が変わってしまう
set(FBZZ_ENGINE_UNITY_EXCLUDE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/StbFontImpl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Cursor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Window.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Input/Input.cpp"
)

# 内部レイヤー 1 つ分の OBJECT ライブラリを作る。
#
#   PRIVATE_INCLUDES に渡したディレクトリだけが、そのモジュールの «自分専用の窓» になる。
#   例えば DX11 のヘッダーは FBZZRenderDX11 の PRIVATE_INCLUDES にしか現れないため、
#   他のモジュールからは #include できない。«上位レイヤーは IRenderer& のみ» という
#   規約が、レビューではなくコンパイルエラーで守られる。
function(fbzz_engine_module TARGET)
    cmake_parse_arguments(ARG "" "" "SOURCES;PRIVATE_INCLUDES;PCH_EXTRA" ${ARGN})

    add_library(${TARGET} OBJECT ${ARG_SOURCES})
    target_include_directories(${TARGET} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${DIRECTXTEX_INCLUDE}"
        "${ASSIMP_INCLUDE}"
        "${STB_INCLUDE}"
        ${ARG_PRIVATE_INCLUDES}
    )
    # OBJECT ライブラリはリンクしない。ここで受け取るのは include パスと
    # コンパイル定義だけで、実際のリンクは FBZZEngine が行う。
    target_link_libraries(${TARGET} PRIVATE FBZZPhysics FBZZMath TomlPlusPlus ImGui)
    target_compile_definitions(${TARGET} PRIVATE ${FBZZ_ENGINE_DEFINES})

    # WHY FBZZEngine_EXPORTS を手で定義するか:
    #     CMake がこのマクロを自動定義するのは SHARED ターゲット (FBZZEngine) だけ。
    #     Time.hpp / Cursor.hpp は «定義されていれば dllexport、無ければ dllimport» を
    #     ヘッダー内で自前判定しており、モジュール側で未定義だと dllimport に落ちて
    #     「dllimport な静的データメンバーは定義できない」(C2491) になる。
    #     これらのオブジェクトは最終的に FBZZEngine.dll の一部になるので、
    #     «自分は Engine を作っている側» と宣言するのが意味的に正しい。
    #     WINDOWS_EXPORT_ALL_SYMBOLS を FBZZ_API へ置き換える際に、この判定ごと
    #     1 本の生成ヘッダーへ統合して消す。
    target_compile_definitions(${TARGET} PRIVATE FBZZEngine_EXPORTS)
    set_target_properties(${TARGET} PROPERTIES FOLDER "Engine/Modules")
    fbzz_use_std_pch(${TARGET} EXTRA ${ARG_PCH_EXTRA})
    fbzz_use_unity_build(${TARGET} EXCLUDE ${FBZZ_ENGINE_UNITY_EXCLUDE})

    # 生成したモジュールを登録簿へ載せる。CMake/FBZZSDK.cmake が ABI 定義
    # (FBZZ_ENGINE_VERSION_* / FBZZ_BUILD_CONFIG_*) を配る宛先として読む。
    set_property(GLOBAL APPEND PROPERTY FBZZ_ENGINE_MODULES ${TARGET})
endfunction()

define_property(GLOBAL PROPERTY FBZZ_ENGINE_MODULES
    BRIEF_DOCS "fbzz_engine_module で生成した Engine 内部モジュール"
    FULL_DOCS  "FBZZEngine.dll を構成する OBJECT ライブラリの一覧")

# ── FBZZCore ────────────────────────────────────────────────────────────────
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    src/Core/*.cpp src/Core/*.hpp
    src/Util/*.cpp src/Util/*.hpp
    src/Profiler/*.cpp src/Profiler/*.hpp
    src/Input/*.cpp src/Input/*.hpp
)
# WHY 除外するか: どちらもヘッダーは低層の API だが、実装は Scene / Renderer を要求する。
#      «インターフェースは下、実装は上» の配置なので翻訳単位だけ FBZZEngine へ移す。
#      Application は合成ルートそのもの。SaveStore は scene::IReflector を使う。
list(REMOVE_ITEM CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Application.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Util/SaveStore.cpp"
)
fbzz_engine_module(FBZZCore SOURCES ${CORE_SOURCES})

# ── FBZZRHI ─────────────────────────────────────────────────────────────────
# Platform/ は含めない (バックエンドは別モジュール)。
file(GLOB RHI_SOURCES CONFIGURE_DEPENDS
    src/Renderer/*.cpp src/Renderer/*.hpp
)
fbzz_engine_module(FBZZRHI SOURCES ${RHI_SOURCES})

# ── FBZZRenderPlatform ──────────────────────────────────────────────────────
# DX11 / DX12 のどちらからも使う D3D 共通処理。特定バックエンドの型は持たない。
file(GLOB RENDER_PLATFORM_SOURCES CONFIGURE_DEPENDS
    src/Renderer/Platform/*.cpp src/Renderer/Platform/*.hpp
)
fbzz_engine_module(FBZZRenderPlatform
    SOURCES ${RENDER_PLATFORM_SOURCES}
)

# ── FBZZRenderDX11 / FBZZRenderDX12 ─────────────────────────────────────────
file(GLOB_RECURSE DX11_SOURCES CONFIGURE_DEPENDS
    src/Renderer/Platform/DX11/*.cpp src/Renderer/Platform/DX11/*.hpp
)
fbzz_engine_module(FBZZRenderDX11
    SOURCES ${DX11_SOURCES}
    PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/Platform/DX11"
    # d3d11.h は 11 TU 中 10、wrl/client.h は 9 が既に include している。
    PCH_EXTRA <Windows.h> <d3d11.h> <wrl/client.h>
)

set(FBZZ_BACKEND_OBJECTS
    $<TARGET_OBJECTS:FBZZRenderPlatform>
    $<TARGET_OBJECTS:FBZZRenderDX11>
)

if(FBZZ_ENABLE_DX12)
    file(GLOB_RECURSE DX12_SOURCES CONFIGURE_DEPENDS
        src/Renderer/Platform/DX12/*.cpp src/Renderer/Platform/DX12/*.hpp
    )
    fbzz_engine_module(FBZZRenderDX12
        SOURCES ${DX12_SOURCES}
        PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/Platform/DX12"
        # d3d12.h は 15 TU 中 12、wrl/client.h は 10 が既に include している。
        PCH_EXTRA <Windows.h> <d3d12.h> <wrl/client.h>
    )
    list(APPEND FBZZ_BACKEND_OBJECTS $<TARGET_OBJECTS:FBZZRenderDX12>)
else()
    message(STATUS "FBZZ: DX12 バックエンドを含めずにビルドします (dxcompiler.dll / dxil.dll も配置しない)")
    # WHY 警告するか: GameHub のプロジェクトテンプレートは既定 renderer が DX12。
    #      この構成で FBZZSDK を公開すると、そこから作られたゲームは起動時に
    #      «DX12 を含まずにビルドされています» で描画できない。SDK 検証
    #      (ValidateFBZZSDK.cmake) は bin と Editor の整合しか見ないため素通りする。
    message(WARNING
        "FBZZ_ENABLE_DX12=OFF の構成から FBZZSDK を公開しないこと。"
        "プロジェクトテンプレートの既定 renderer は DX12 のため、"
        "配布した SDK から作ったゲームが起動できなくなる。")
endif()

# ── FBZZEngine ──────────────────────────────────────────────────────────────
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    src/Scene/*.cpp src/Scene/*.hpp
    src/Asset/*.cpp src/Asset/*.hpp
    src/Audio/*.cpp src/Audio/*.hpp
    src/AI/*.cpp src/AI/*.hpp
    include/*.hpp
)
# src 直下の 3 ファイル (engine.cpp / ProjectResolver.cpp / ProjectSettings.cpp)。
# 非再帰の GLOB を分けているのは、上の GLOB_RECURSE に src/*.cpp を足すと
# 全モジュールのソースを二重に拾ってしまうため。
file(GLOB ENGINE_ROOT_SOURCES CONFIGURE_DEPENDS
    src/*.cpp src/*.hpp
)
list(APPEND ENGINE_SOURCES
    ${ENGINE_ROOT_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Application.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Util/SaveStore.cpp"
)

# どのモジュールにも属さない .cpp が出ていないか検査する。
# WHY: 分割前は src/**.cpp を丸ごと拾う 1 本の GLOB だった。ディレクトリ単位へ
#      分けた結果「新しいディレクトリを作ったが CMake に足し忘れる」が起こりうる。
#      その抜けはコンパイルではなく «リンク時の未解決シンボル» か、最悪
#      «実行時に機能が無い» として現れるため、configure の時点で落とす。
file(GLOB_RECURSE FBZZ_ALL_ENGINE_CPP CONFIGURE_DEPENDS src/*.cpp)
set(FBZZ_CLAIMED_CPP ${CORE_SOURCES} ${RHI_SOURCES} ${RENDER_PLATFORM_SOURCES}
                     ${DX11_SOURCES} ${DX12_SOURCES} ${ENGINE_SOURCES})
list(FILTER FBZZ_CLAIMED_CPP INCLUDE REGEX "\\.cpp$")
set(FBZZ_UNCLAIMED_CPP ${FBZZ_ALL_ENGINE_CPP})
list(REMOVE_ITEM FBZZ_UNCLAIMED_CPP ${FBZZ_CLAIMED_CPP})
if(FBZZ_UNCLAIMED_CPP AND FBZZ_ENABLE_DX12)
    string(REPLACE ";" "\n  " FBZZ_UNCLAIMED_SHOWN "${FBZZ_UNCLAIMED_CPP}")
    message(FATAL_ERROR
        "どのモジュールにも割り当てられていない Engine のソースがあります。\n"
        "  ${FBZZ_UNCLAIMED_SHOWN}\n"
        "Projects/Engine/CMakeLists.txt のモジュール定義へ追加してください。")
endif()

add_library(FBZZEngine SHARED
    ${ENGINE_SOURCES}
    $<TARGET_OBJECTS:FBZZCore>
    $<TARGET_OBJECTS:FBZZRHI>
    ${FBZZ_BACKEND_OBJECTS}
)
fbzz_use_std_pch(FBZZEngine)
fbzz_use_unity_build(FBZZEngine EXCLUDE ${FBZZ_ENGINE_UNITY_EXCLUDE})
set_target_properties(FBZZEngine PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    OUTPUT_NAME "FBZZEngine"
    EXPORT_NAME "Engine"
)
target_compile_definitions(FBZZEngine PRIVATE ${FBZZ_ENGINE_DEFINES})

# WHY src を include パスへ入れないか: src/Renderer/Platform/DX11 等が
#      "Platform/DX11/DX11Renderer.hpp" として届いてしまい、バックエンドの隔離が
#      名ばかりになる。src ルート起点の include は 1 つも無いことを確認済み。
target_include_directories(FBZZEngine
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE ${DIRECTXTEX_INCLUDE}
    PRIVATE ${ASSIMP_INCLUDE}
    PRIVATE ${STB_INCLUDE}
)

target_link_libraries(FBZZEngine
    PUBLIC
        FBZZPhysics
        FBZZMath
        TomlPlusPlus
        xaudio2
        # ゲームパッド入力 (Engine/Input/Gamepad.cpp)。
        # xinput.lib は XInput 1.4 (Windows 8 以降) を指す。旧 xinput9_1_0 は使わない。
        xinput
        mfplat
        mfreadwrite
        mfuuid
        winmm
        d3d12
        dxgi
        dxguid
        d3dcompiler
    PRIVATE
        # WHY: ImGui は Engine 実装専用だが、CMake の install export はリンク依存先も
        #      同じ export set に要求する。SDK 側では FBZZ::ImGui として整合性を保つ。
        ImGui
        "$<$<CONFIG:Debug>:${DIRECTXTEX_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${DIRECTXTEX_LIB_RELEASE}>"
        "$<$<CONFIG:Debug>:${ASSIMP_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${ASSIMP_LIB_RELEASE}>"
)

# DX12 のホットリロードと DXIL reflection は dxcompiler.dll を実行時に遅延ロードする。
# WHY: DX11 専用起動には DXC を強制せず、DX12 を使う構成だけ同梱 SDK の DLL を配置する。
get_filename_component(FBZZ_WINDOWS_SDK_BIN_DIRECTORY "${CMAKE_RC_COMPILER}" DIRECTORY)
if(FBZZ_ENABLE_DX12)
    if(DEFINED ENV{FBZZ_DXC} AND EXISTS "$ENV{FBZZ_DXC}")
        set(FBZZ_DXC_EXECUTABLE "$ENV{FBZZ_DXC}")
    else()
        find_program(FBZZ_DXC_EXECUTABLE
            NAMES dxc.exe
            HINTS
                "${FBZZ_WINDOWS_SDK_BIN_DIRECTORY}"
                "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
        )
    endif()
    if(FBZZ_DXC_EXECUTABLE)
        get_filename_component(FBZZ_DXC_DIRECTORY "${FBZZ_DXC_EXECUTABLE}" DIRECTORY)
        # WHY: dxcompiler.dll は LoadLibraryW で exe 隣から解決されるため、Engine 出力に置くだけでは
        #      サブディレクトリへ出力される Editor から見つからない。配置先を各 exe 側でも使えるよう公開する。
        set(FBZZ_DXC_RUNTIME_DIRECTORY "${FBZZ_DXC_DIRECTORY}" CACHE INTERNAL "DXC runtime DLL directory")
        foreach(FBZZ_DXC_RUNTIME_DLL dxcompiler.dll dxil.dll)
            if(EXISTS "${FBZZ_DXC_DIRECTORY}/${FBZZ_DXC_RUNTIME_DLL}")
                add_custom_command(TARGET FBZZEngine POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${FBZZ_DXC_DIRECTORY}/${FBZZ_DXC_RUNTIME_DLL}"
                        "$<TARGET_FILE_DIR:FBZZEngine>/${FBZZ_DXC_RUNTIME_DLL}"
                    COMMENT "Copying ${FBZZ_DXC_RUNTIME_DLL} for DXIL runtime support"
                )
            endif()
        endforeach()
    else()
        message(WARNING "dxc.exe was not found; DX12 shader hot reload will require FBZZ_DXC at runtime")
    endif()
else()
    # DX12 を積まない構成では DXC は要らない。Editor / Launcher 側の配置判定もこれを見る。
    unset(FBZZ_DXC_RUNTIME_DIRECTORY CACHE)
endif()
