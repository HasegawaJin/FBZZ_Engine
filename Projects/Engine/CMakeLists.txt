file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

add_library(FBZZEngine SHARED ${ENGINE_SOURCES})
set_target_properties(FBZZEngine PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    OUTPUT_NAME "FBZZEngine"
    EXPORT_NAME "Engine"
)

# ── EngineRebuildBootstrap 用のビルド情報埋め込み ────────────────────────────
# WHY: FBZZEngine.dll は各 exe が暗黙リンクするため、実行中はロックされ再ビルドできない。
#      起動時に「Engine ソースが DLL より新しいか」を判定し、古ければ cmake を叩いて
#      再ビルド→再起動する (EngineRebuildBootstrap.cpp)。そのために必要な
#      「このバイナリを生成した build ディレクトリ・engine ソース・cmake・構成名」を
#      コンパイル時に埋め込む。
# WHY (Release で埋め込まない): Release は配布構成。開発機の絶対パスを配布物へ残さないよう、
#      dev パス系マクロは Debug / Development のみに付与し、Release では自己リビルド機構を無効化する。
#      CMAKE_BINARY_DIR は add_subdirectory のネストに関係なく常にトップ build ディレクトリを指すため、
#      ルート・ParticleGame・GameHub 生成プロジェクトいずれのビルドでも build.config と同じ場所になる。
get_filename_component(FBZZ_ENGINE_ROOT_ABS "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
target_compile_definitions(FBZZEngine PRIVATE
    FBZZ_CMAKE_CONFIG="$<CONFIG>"
    "$<$<AND:$<BOOL:${FBZZ_ENABLE_SOURCE_REBUILD}>,$<NOT:$<CONFIG:Release>>>:FBZZ_DEV_BUILD_DIR=\"${CMAKE_BINARY_DIR}\">"
    "$<$<AND:$<BOOL:${FBZZ_ENABLE_SOURCE_REBUILD}>,$<NOT:$<CONFIG:Release>>>:FBZZ_DEV_ENGINE_ROOT=\"${FBZZ_ENGINE_ROOT_ABS}\">"
    "$<$<AND:$<BOOL:${FBZZ_ENABLE_SOURCE_REBUILD}>,$<NOT:$<CONFIG:Release>>>:FBZZ_DEV_CMAKE_COMMAND=\"${CMAKE_COMMAND}\">"
)

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
)
    if(NOT EXISTS "${REQUIRED_PATH}")
        message(FATAL_ERROR "Required third-party dependency was not found: ${REQUIRED_PATH}")
    endif()
endforeach()

target_include_directories(FBZZEngine
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE src
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
