# @file    CMakeLists.txt
# @brief   Engine の内部レイヤー (OBJECT ライブラリ) と FBZZEngine.dll の構成。
# @author  Hasegawa Jin
# @date    2026-05-15
#
# @note レイヤーは FBZZCore / FBZZRHI / FBZZRenderPlatform / FBZZRenderDX12 で、FBZZEngine.dll (Scene / Asset / Audio / AI) が全部を飲み込む。
# @note STATIC にしないのは、参照されない静的初期化子で自己登録する TU をリンカが警告なしに捨てるため (CMake/FBZZTests.cmake と同じ理由)。
# @note $<TARGET_OBJECTS:> で取り込むのは、リンク依存にすると install(EXPORT) が内部レイヤーも export set に要求するため。

option(FBZZ_ENABLE_DX12 "Build the Direct3D 12 backend" ON)

# @name サードパーティの実体確認
set(DIRECTXTEX_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/include")
set(DIRECTXTEX_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Debug/DirectXTex.lib")
set(DIRECTXTEX_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Release/DirectXTex.lib")

# @note stb_truetype / stb_rect_pack の実装展開は src/Renderer/StbFontImpl.cpp だけが行う。
# @see Docs/design/font-system.md
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

# @name EngineRebuildBootstrap 用のビルド情報
# @note 実行中の FBZZEngine.dll はロックされるので、起動時にソースと比べ古ければ再ビルドして再起動する (EngineRebuildBootstrap.cpp)。
# @note パスはコンパイル定義に焼かず実行時に読む。DLL がツリーに依存せず、そのまま SDK へ出せる。
# @note SDK の staging はこのファイルを複製しないので、SDK から起動すると機構は働かない。
set(FBZZ_ENGINE_REBUILD_CONFIG "${CMAKE_BINARY_DIR}/engine-rebuild.config")
if(FBZZ_ENABLE_SOURCE_REBUILD)
    get_filename_component(FBZZ_ENGINE_ROOT_ABS "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    file(CONFIGURE OUTPUT "${FBZZ_ENGINE_REBUILD_CONFIG}"
        CONTENT "cmake_exe=${CMAKE_COMMAND}\nbuild_dir=${CMAKE_BINARY_DIR}\nengine_root=${FBZZ_ENGINE_ROOT_ABS}\n"
    )
else()
    file(REMOVE "${FBZZ_ENGINE_REBUILD_CONFIG}")
endif()

set(FBZZ_ENGINE_DEFINES
    FBZZ_CMAKE_CONFIG="$<CONFIG>"
    FBZZ_ENABLE_DX12=$<IF:$<BOOL:${FBZZ_ENABLE_DX12}>,1,0>
)

# @brief Unity ビルドで統合してはいけない翻訳単位。
set(FBZZ_ENGINE_UNITY_EXCLUDE
    # @note STB_TRUETYPE_IMPLEMENTATION / STB_RECT_PACK_IMPLEMENTATION の展開場所。
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/StbFontImpl.cpp"
    # @note STB_IMAGE_IMPLEMENTATION の展開場所 (カーソル画像のデコード)。
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Platform/Cursor.cpp"
    # @note STB_IMAGE_IMPLEMENTATION の展開場所。STB_IMAGE_STATIC で Cursor.cpp とは別の実体。
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Asset/FluidSourceMaskLoader.cpp"
    # @note DPI_AWARENESS_CONTEXT_* と WM_DPICHANGED を \#undef せずに定義する。
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Platform/Window.cpp"
    # @note WIN32_LEAN_AND_MEAN を定義し、同じバッチの後続 TU の Windows.h の中身を変えてしまう。
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Input/Input.cpp"
)

# @brief 内部レイヤー 1 つ分の OBJECT ライブラリを作る。
# @param PRIVATE_INCLUDES そのモジュールだけが見える include パス。d3d12.h をここに閉じ、上位層の具象参照をコンパイルエラーにする。
# @param PCH_EXTRA 標準 PCH に足すヘッダー。
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
    # @note OBJECT ライブラリはリンクしない。受け取るのは include パスとコンパイル定義だけで、リンクは FBZZEngine が行う。
    target_link_libraries(${TARGET} PRIVATE FBZZPhysics FBZZFluid FBZZMath TomlPlusPlus ImGui)
    target_compile_definitions(${TARGET} PRIVATE ${FBZZ_ENGINE_DEFINES})

    # @note CMake は FBZZEngine_EXPORTS を SHARED にしか定義しない。無いと Time.hpp / Cursor.hpp が dllimport に落ちて C2491 になる。
    # @todo WINDOWS_EXPORT_ALL_SYMBOLS を FBZZ_API へ置き換えるとき、この判定ごと生成ヘッダーへ統合して消す。
    target_compile_definitions(${TARGET} PRIVATE FBZZEngine_EXPORTS)
    set_target_properties(${TARGET} PROPERTIES FOLDER "Engine/Modules")
    fbzz_use_std_pch(${TARGET} EXTRA ${ARG_PCH_EXTRA})
    fbzz_use_unity_build(${TARGET} EXCLUDE ${FBZZ_ENGINE_UNITY_EXCLUDE})

    # @note CMake/FBZZSDK.cmake がこの登録簿を ABI 定義 (FBZZ_ENGINE_VERSION_* / FBZZ_BUILD_CONFIG_*) の配布先として読む。
    set_property(GLOBAL APPEND PROPERTY FBZZ_ENGINE_MODULES ${TARGET})
endfunction()

define_property(GLOBAL PROPERTY FBZZ_ENGINE_MODULES
    BRIEF_DOCS "fbzz_engine_module で生成した Engine 内部モジュール"
    FULL_DOCS  "FBZZEngine.dll を構成する OBJECT ライブラリの一覧")

# @name FBZZCore
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    src/Core/*.cpp src/Core/*.hpp
    src/Util/*.cpp src/Util/*.hpp
    src/Profiler/*.cpp src/Profiler/*.hpp
    src/Input/*.cpp src/Input/*.hpp
)
# @note ヘッダーは低層だが実装が Scene を要求する TU は FBZZEngine へ移す (Application は合成ルート、SaveStore は scene::IReflector を使う)。
list(REMOVE_ITEM CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Platform/Application.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Util/SaveStore.cpp"
)
fbzz_engine_module(FBZZCore SOURCES ${CORE_SOURCES})

# @name FBZZRHI
# @note 非再帰の GLOB で Platform/ (バックエンド) を含めない。
file(GLOB RHI_SOURCES CONFIGURE_DEPENDS
    src/Renderer/*.cpp src/Renderer/*.hpp
)
fbzz_engine_module(FBZZRHI SOURCES ${RHI_SOURCES})

# @name FBZZRenderPlatform
# @note バックエンドから使う D3D 共通処理。特定バックエンドの型は持たない。
file(GLOB RENDER_PLATFORM_SOURCES CONFIGURE_DEPENDS
    src/Renderer/Platform/*.cpp src/Renderer/Platform/*.hpp
)
fbzz_engine_module(FBZZRenderPlatform
    SOURCES ${RENDER_PLATFORM_SOURCES}
)

# @name FBZZRenderDX12
# @note バックエンドが 1 つでもモジュールを分けたままにする。DX ヘッダーを PRIVATE_INCLUDES に閉じ、上位層からのダウンキャストを禁じる (AGENTS.md)。
set(FBZZ_BACKEND_OBJECTS
    $<TARGET_OBJECTS:FBZZRenderPlatform>
)

if(FBZZ_ENABLE_DX12)
    file(GLOB_RECURSE DX12_SOURCES CONFIGURE_DEPENDS
        src/Renderer/Platform/DX12/*.cpp src/Renderer/Platform/DX12/*.hpp
    )
    fbzz_engine_module(FBZZRenderDX12
        SOURCES ${DX12_SOURCES}
        PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/Platform/DX12"
        # @note d3d12.h は 15 TU 中 12、wrl/client.h は 10 が include している。
        PCH_EXTRA <Windows.h> <d3d12.h> <wrl/client.h>
    )
    list(APPEND FBZZ_BACKEND_OBJECTS $<TARGET_OBJECTS:FBZZRenderDX12>)
else()
    message(STATUS "FBZZ: 描画バックエンドを含めずにビルドします (dxcompiler.dll / dxil.dll も配置しない)")
    # @warning この構成は描画バックエンドを 1 つも持たず、ここから公開した SDK のゲームは起動時に必ず落ちる。ValidateFBZZSDK.cmake は検出しない。
    # @note GPU を使わないビルド (カバレッジ計測など) 専用。
    message(WARNING
        "FBZZ_ENABLE_DX12=OFF の構成から FBZZSDK を公開しないこと。"
        "DirectX 11 撤去後、この構成には描画バックエンドが 1 つも無いため、"
        "配布した SDK から作ったゲームが起動できなくなる。")
endif()

# @name FBZZEngine
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    src/Scene/*.cpp src/Scene/*.hpp
    src/Asset/*.cpp src/Asset/*.hpp
    src/Audio/*.cpp src/Audio/*.hpp
    src/AI/*.cpp src/AI/*.hpp
    include/*.hpp
)
# @note src 直下は非再帰 GLOB で別に拾う。GLOB_RECURSE に src/*.cpp を足すと全モジュールのソースを二重に拾う。
file(GLOB ENGINE_ROOT_SOURCES CONFIGURE_DEPENDS
    src/*.cpp src/*.hpp
)
list(APPEND ENGINE_SOURCES
    ${ENGINE_ROOT_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Platform/Application.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Util/SaveStore.cpp"
)

# @note 新ディレクトリをモジュールへ足し忘れると、リンクエラーか実行時の機能欠落としてしか現れないので configure で落とす。
file(GLOB_RECURSE FBZZ_ALL_ENGINE_CPP CONFIGURE_DEPENDS src/*.cpp)
set(FBZZ_CLAIMED_CPP ${CORE_SOURCES} ${RHI_SOURCES} ${RENDER_PLATFORM_SOURCES}
                     ${DX12_SOURCES} ${ENGINE_SOURCES})
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

# @note src を include パスへ入れない。入れると "Platform/DX12/DX12Renderer.hpp" が届き、バックエンドの隔離が破れる。
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
        # @note 公開ヘッダー (FluidRecipeCodec / FluidBaker …) が Fluid の型を引数に取るので PUBLIC。
        FBZZFluid
        FBZZMath
        TomlPlusPlus
        xaudio2
        # @note xinput.lib は XInput 1.4 (Windows 8 以降)。旧 xinput9_1_0 は使わない。
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
        # @note Engine 実装専用だが install(EXPORT) がリンク依存先も要求するので、SDK では FBZZ::ImGui として出す。
        ImGui
        "$<$<CONFIG:Debug>:${DIRECTXTEX_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${DIRECTXTEX_LIB_RELEASE}>"
        "$<$<CONFIG:Debug>:${ASSIMP_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${ASSIMP_LIB_RELEASE}>"
)

# @note DX12 のホットリロードと DXIL reflection は dxcompiler.dll を実行時に遅延ロードする。FBZZ_ENABLE_DX12=OFF には DXC を強制しない。
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
        # @note dxcompiler.dll は exe 隣から LoadLibraryW で解決されるので、サブディレクトリに出る Editor 等の配置用に場所を公開する。
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
    # @note Editor / Launcher 側の DXC 配置判定もこの変数を見る。
    unset(FBZZ_DXC_RUNTIME_DIRECTORY CACHE)
endif()
