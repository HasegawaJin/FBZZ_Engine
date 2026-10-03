# @file    CMakeLists.txt
# @brief   Engine の内部レイヤー (OBJECT ライブラリ) と FBZZEngine.dll の構成。
# @author  Hasegawa Jin
# @date    2026-05-15
#
# @note 描画は FBZZGraphics、共通基盤は FBZZCore。Engine 内の自己登録モジュールだけを OBJECT として取り込む。
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
    target_link_libraries(${TARGET} PRIVATE FBZZGraphics FBZZPhysics FBZZFluid FBZZMath TomlPlusPlus ImGui)
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

# @name FBZZEnginePlatform
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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler/ScriptProfiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler/ProfilerFrame.cpp"
)
fbzz_engine_module(FBZZEnginePlatform SOURCES ${CORE_SOURCES})

# @name FBZZEngine
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    src/Scene/*.cpp src/Scene/*.hpp
    src/Asset/*.cpp src/Asset/*.hpp
    src/Audio/*.cpp src/Audio/*.hpp
    src/AI/*.cpp src/AI/*.hpp
    src/Renderer/*.cpp src/Renderer/*.hpp
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

list(APPEND ENGINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler/ScriptProfiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler/ProfilerFrame.cpp"
)

# @note 新ディレクトリをモジュールへ足し忘れると、リンクエラーか実行時の機能欠落としてしか現れないので configure で落とす。
file(GLOB_RECURSE FBZZ_ALL_ENGINE_CPP CONFIGURE_DEPENDS src/*.cpp)
set(FBZZ_CLAIMED_CPP ${CORE_SOURCES} ${ENGINE_SOURCES})
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
    $<TARGET_OBJECTS:FBZZEnginePlatform>
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
        FBZZGraphics
        FBZZCore
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
        # @note CrashHandler の MiniDumpWriteDump。落ちてから LoadLibrary しないよう起動時に読ませる。
        dbghelp
        "$<$<CONFIG:Debug>:${DIRECTXTEX_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${DIRECTXTEX_LIB_RELEASE}>"
        "$<$<CONFIG:Debug>:${ASSIMP_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${ASSIMP_LIB_RELEASE}>"
)
