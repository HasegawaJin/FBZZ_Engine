# @file    CMakeLists.txt
# @brief   Engine の内部レイヤー (OBJECT ライブラリ) と FBZZEngine.dll の構成。
# @author  Hasegawa Jin
# @date    2026-05-15
#
# @note 描画は FBZZGraphics、共通基盤は FBZZCore。Engine 内の自己登録モジュールだけを OBJECT として取り込む。
# @note STATIC にしないのは、参照されない静的初期化子で自己登録する TU をリンカが警告なしに捨てるため (CMake/FBZZTests.cmake と同じ理由)。
# @note $<TARGET_OBJECTS:> で取り込むのは、リンク依存にすると install(EXPORT) が内部レイヤーも export set に要求するため。

option(FBZZ_ENABLE_DX12 "Build the Direct3D 12 backend" ON)
include("${CMAKE_SOURCE_DIR}/CMake/EngineModuleBoundaries.cmake")

# @brief 下位ライブラリと Platform の直接 include の依存方向を configure 時に検証する。
function(fbzz_validate_layer_includes NAME FORBIDDEN)
    foreach(SOURCE IN LISTS ARGN)
        file(STRINGS "${SOURCE}" FBZZ_LAYER_INCLUDES REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
        foreach(INCLUDE IN LISTS FBZZ_LAYER_INCLUDES)
            if(INCLUDE MATCHES "[<\"](${FORBIDDEN})/")
                message(FATAL_ERROR "${NAME}: forbidden dependency in ${SOURCE}: ${INCLUDE}")
            endif()
        endforeach()
    endforeach()
endfunction()

foreach(LAYER IN ITEMS Core Graphics Physics Fluid Math)
    file(GLOB_RECURSE LAYER_FILES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/Projects/${LAYER}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/Projects/${LAYER}/src/*.hpp"
        "${CMAKE_SOURCE_DIR}/Projects/${LAYER}/include/*.hpp")
    if(LAYER STREQUAL "Core" OR LAYER STREQUAL "Math")
        set(FORBIDDEN "Engine|Editor|Graphics|Physics|Fluid")
        if(LAYER STREQUAL "Math")
            string(APPEND FORBIDDEN "|Core")
        endif()
    elseif(LAYER STREQUAL "Graphics")
        set(FORBIDDEN "Engine|Editor|Physics|Fluid")
    elseif(LAYER STREQUAL "Physics")
        set(FORBIDDEN "Engine|Editor|Graphics|Fluid|Core")
    else()
        set(FORBIDDEN "Engine|Editor|Graphics|Physics|Core")
    endif()
    fbzz_validate_layer_includes("${LAYER}" "${FORBIDDEN}" ${LAYER_FILES})
endforeach()

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
# @param PRIVATE_INCLUDES ホスト SDK など、そのモジュールだけが使う実装依存。
# @param PCH_EXTRA 標準 PCH に足すヘッダー。
# @param DEPENDENCIES そのモジュールが参照できるライブラリ。
function(fbzz_engine_module TARGET)
    cmake_parse_arguments(ARG "" "" "SOURCES;PRIVATE_INCLUDES;PCH_EXTRA;DEPENDENCIES" ${ARGN})
    string(REPLACE "FBZZEngine" "" MODULE "${TARGET}")
    set(PUBLIC_ROOTS_Platform Core Input Util Profiler)
    set(PUBLIC_ROOTS_Assets Asset Format)
    set(PUBLIC_ROOTS_Audio Audio)
    set(PUBLIC_ROOTS_AI AI)
    set(PUBLIC_ROOTS_Scene Scene)
    set(PUBLIC_ROOTS_Rendering Renderer Scene/Systems/RenderPasses)
    set(PUBLIC_ROOTS_Managed Scene/Managed)
    set(PUBLIC_HEADERS)
    foreach(PUBLIC_ROOT IN LISTS PUBLIC_ROOTS_${MODULE})
        file(GLOB_RECURSE MODULE_HEADERS CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/include/Engine/${PUBLIC_ROOT}/*.hpp")
        list(APPEND PUBLIC_HEADERS ${MODULE_HEADERS})
    endforeach()
    if(MODULE STREQUAL "Platform")
        list(REMOVE_ITEM PUBLIC_HEADERS
            "${CMAKE_CURRENT_SOURCE_DIR}/include/Engine/Core/Application.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/Engine/Profiler/ScriptProfiler.hpp")
    elseif(MODULE STREQUAL "Scene")
        list(FILTER PUBLIC_HEADERS EXCLUDE REGEX "/Scene/(Managed/|Systems/(Render|UISystem))")
    elseif(MODULE STREQUAL "Rendering")
        file(GLOB RENDER_HEADERS CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/include/Engine/Scene/Systems/Render*.hpp")
        list(APPEND PUBLIC_HEADERS ${RENDER_HEADERS}
            "${CMAKE_CURRENT_SOURCE_DIR}/include/Engine/Scene/Systems/UISystem.hpp")
    endif()
    fbzz_validate_engine_module("${CMAKE_SOURCE_DIR}" "${MODULE}" ${ARG_SOURCES} ${PUBLIC_HEADERS})
    fbzz_validate_engine_links("${MODULE}" ${ARG_DEPENDENCIES})

    add_library(${TARGET} OBJECT ${ARG_SOURCES})
    target_include_directories(${TARGET} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        ${ARG_PRIVATE_INCLUDES}
    )
    # @note OBJECT ライブラリはリンクしない。受け取るのは include パスとコンパイル定義だけで、リンクは FBZZEngine が行う。
    target_link_libraries(${TARGET} PRIVATE ${ARG_DEPENDENCIES})
    target_compile_definitions(${TARGET} PRIVATE ${FBZZ_ENGINE_DEFINES})
    fbzz_engine_remove_unused_optimized_comdats(${TARGET})

    # @note CMake が SHARED だけに定義する FBZZEngine_EXPORTS を補い、Engine/Api.hpp を DLL 本体と同じ向きにする。
    target_compile_definitions(${TARGET} PRIVATE FBZZEngine_EXPORTS)
    set_target_properties(${TARGET} PROPERTIES FOLDER "Engine/Modules")
    fbzz_use_std_pch(${TARGET} EXTRA ${ARG_PCH_EXTRA})
    fbzz_use_unity_build(${TARGET} EXCLUDE ${FBZZ_ENGINE_UNITY_EXCLUDE})

    # @note CMake/FBZZSDK.cmakeがクラッシュ記録用のEngine版数を内部モジュールへ配布する。
    set_property(GLOBAL APPEND PROPERTY FBZZ_ENGINE_MODULES ${TARGET})
endfunction()

define_property(GLOBAL PROPERTY FBZZ_ENGINE_MODULES
    BRIEF_DOCS "fbzz_engine_module で生成した Engine 内部モジュール"
    FULL_DOCS  "FBZZEngine.dll を構成する OBJECT ライブラリの一覧")

# @brief Engine DLL の最適化構成で未参照 inline COMDAT の出力を抑止する。
# @param TARGET Engine DLL を構成するコンパイル対象。
# @note 明示 export しない未参照 inline COMDAT の object 出力も抑止する。
# @see https://learn.microsoft.com/en-us/cpp/build/reference/zc-inline-remove-unreferenced-comdat?view=msvc-170 MSVC /Zc:inline removes unreferenced COMDAT functions and data.
function(fbzz_engine_remove_unused_optimized_comdats TARGET)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:/Zc:inline>")
    endif()
endfunction()

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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Platform/Cursor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Util/SaveStore.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler/ScriptProfiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler/Profiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler/ProfilerFrame.cpp"
)
fbzz_engine_module(FBZZEnginePlatform SOURCES ${CORE_SOURCES}
    DEPENDENCIES FBZZCore FBZZMath TomlPlusPlus
    PRIVATE_INCLUDES "${STB_INCLUDE}")
fbzz_validate_layer_includes("EnginePlatform" "Engine/(Scene|Asset|Renderer|Audio|AI)|Graphics|Physics|Fluid" ${CORE_SOURCES})

# @name 責務ごとの Engine 内部モジュール
file(GLOB_RECURSE ASSET_SOURCES CONFIGURE_DEPENDS src/Asset/*.cpp src/Asset/*.hpp)
file(GLOB_RECURSE AUDIO_SOURCES CONFIGURE_DEPENDS src/Audio/*.cpp src/Audio/*.hpp)
file(GLOB_RECURSE AI_SOURCES CONFIGURE_DEPENDS src/AI/*.cpp src/AI/*.hpp)
file(GLOB_RECURSE SCENE_SOURCES CONFIGURE_DEPENDS src/Scene/*.cpp src/Scene/*.hpp)
file(GLOB_RECURSE RENDERING_SOURCES CONFIGURE_DEPENDS
    src/Renderer/*.cpp src/Renderer/*.hpp
    src/Scene/Systems/RenderPasses/*.cpp src/Scene/Systems/RenderPasses/*.hpp)
file(GLOB RENDER_EXTRACTOR_SOURCES CONFIGURE_DEPENDS
    src/Scene/Systems/Render*.cpp src/Scene/Systems/Render*.hpp
    src/Scene/Systems/TerrainRenderCache.cpp src/Scene/Systems/TerrainRenderCache.hpp)
list(APPEND RENDERING_SOURCES ${RENDER_EXTRACTOR_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Scene/Systems/UISystem.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Scene/SceneRenderResources.cpp")
list(REMOVE_ITEM SCENE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/Scene/StandaloneProjectModule.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Scene/ProjectRuntime.cpp")
file(GLOB_RECURSE MANAGED_SOURCES CONFIGURE_DEPENDS src/Scene/Managed/*.cpp src/Scene/Managed/*.hpp)
list(REMOVE_ITEM SCENE_SOURCES ${MANAGED_SOURCES} ${RENDERING_SOURCES})

fbzz_engine_module(FBZZEngineAssets SOURCES ${ASSET_SOURCES}
    PRIVATE_INCLUDES "${DIRECTXTEX_INCLUDE}" "${ASSIMP_INCLUDE}" "${STB_INCLUDE}"
    DEPENDENCIES FBZZGraphics FBZZPhysics FBZZFluid FBZZMath TomlPlusPlus)
fbzz_engine_module(FBZZEngineAudio SOURCES ${AUDIO_SOURCES}
    DEPENDENCIES FBZZCore FBZZMath)
fbzz_engine_module(FBZZEngineAI SOURCES ${AI_SOURCES}
    DEPENDENCIES FBZZCore FBZZMath TomlPlusPlus)
fbzz_engine_module(FBZZEngineScene SOURCES ${SCENE_SOURCES}
    PRIVATE_INCLUDES "${DIRECTXTEX_INCLUDE}"
    DEPENDENCIES FBZZGraphics FBZZPhysics FBZZFluid FBZZMath TomlPlusPlus)
fbzz_engine_module(FBZZEngineRendering SOURCES ${RENDERING_SOURCES}
    DEPENDENCIES FBZZGraphics FBZZPhysics FBZZFluid FBZZMath TomlPlusPlus)

# @name FBZZEngine 合成ルート
file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS include/*.hpp)
set(ENGINE_SOURCES ${ENGINE_HEADERS})
# @note src 直下は非再帰 GLOB で別に拾う。GLOB_RECURSE に src/*.cpp を足すと全モジュールのソースを二重に拾う。
file(GLOB ENGINE_ROOT_SOURCES CONFIGURE_DEPENDS
    src/*.cpp src/*.hpp
)
list(APPEND ENGINE_SOURCES
    ${ENGINE_ROOT_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Scene/ProjectRuntime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Scene/StandaloneProjectModule.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Platform/Application.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Platform/Cursor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Util/SaveStore.cpp"
)

list(APPEND ENGINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler/ScriptProfiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler/Profiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler/ProfilerFrame.cpp"
)

# @note Runtime hosting headers are confined to this module; Scene adapters expose only FBZZ POD ABI types.
fbzz_engine_module(FBZZEngineManaged SOURCES ${MANAGED_SOURCES}
    PRIVATE_INCLUDES "${FBZZ_DOTNET_HOST_PACK}"
    DEPENDENCIES FBZZGraphics FBZZPhysics FBZZFluid FBZZMath TomlPlusPlus)

# @note 新ディレクトリをモジュールへ足し忘れると、リンクエラーか実行時の機能欠落としてしか現れないので configure で落とす。
file(GLOB_RECURSE FBZZ_ALL_ENGINE_CPP CONFIGURE_DEPENDS src/*.cpp)
set(FBZZ_CLAIMED_CPP ${CORE_SOURCES} ${ENGINE_SOURCES} ${MANAGED_SOURCES}
    ${ASSET_SOURCES} ${AUDIO_SOURCES} ${AI_SOURCES} ${SCENE_SOURCES} ${RENDERING_SOURCES})
list(FILTER FBZZ_CLAIMED_CPP INCLUDE REGEX "\\.cpp$")
set(FBZZ_UNIQUE_CPP ${FBZZ_CLAIMED_CPP})
list(REMOVE_DUPLICATES FBZZ_UNIQUE_CPP)
list(LENGTH FBZZ_CLAIMED_CPP FBZZ_CLAIMED_COUNT)
list(LENGTH FBZZ_UNIQUE_CPP FBZZ_UNIQUE_COUNT)
if(NOT FBZZ_CLAIMED_COUNT EQUAL FBZZ_UNIQUE_COUNT)
    message(FATAL_ERROR "Engine source is assigned to multiple OBJECT modules")
endif()
set(FBZZ_UNCLAIMED_CPP ${FBZZ_ALL_ENGINE_CPP})
list(REMOVE_ITEM FBZZ_UNCLAIMED_CPP ${FBZZ_CLAIMED_CPP})
if(FBZZ_UNCLAIMED_CPP)
    string(REPLACE ";" "\n  " FBZZ_UNCLAIMED_SHOWN "${FBZZ_UNCLAIMED_CPP}")
    message(FATAL_ERROR
        "どのモジュールにも割り当てられていない Engine のソースがあります。\n"
        "  ${FBZZ_UNCLAIMED_SHOWN}\n"
        "Projects/Engine/CMakeLists.txt のモジュール定義へ追加してください。")
endif()

fbzz_validate_engine_module("${CMAKE_SOURCE_DIR}" Host ${ENGINE_SOURCES})
add_library(FBZZEngine SHARED
    ${ENGINE_SOURCES}
    $<TARGET_OBJECTS:FBZZEnginePlatform>
    $<TARGET_OBJECTS:FBZZEngineManaged>
    $<TARGET_OBJECTS:FBZZEngineAssets>
    $<TARGET_OBJECTS:FBZZEngineAudio>
    $<TARGET_OBJECTS:FBZZEngineAI>
    $<TARGET_OBJECTS:FBZZEngineScene>
    $<TARGET_OBJECTS:FBZZEngineRendering>
)
fbzz_use_std_pch(FBZZEngine)
fbzz_use_unity_build(FBZZEngine EXCLUDE ${FBZZ_ENGINE_UNITY_EXCLUDE})
fbzz_engine_remove_unused_optimized_comdats(FBZZEngine)
set_target_properties(FBZZEngine PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    OUTPUT_NAME "FBZZEngine"
    EXPORT_NAME "Engine"
)
target_compile_definitions(FBZZEngine PRIVATE ${FBZZ_ENGINE_DEFINES})

# @note src を include パスへ入れない。入れると "Platform/DX12/DX12Renderer.hpp" が届き、バックエンドの隔離が破れる。
target_include_directories(FBZZEngine
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler"
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
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
        "${FBZZ_DOTNET_HOST_PACK}/nethost.lib"
        # @note Engine 実装専用だが install(EXPORT) がリンク依存先も要求するので、SDK では FBZZ::ImGui として出す。
        ImGui
        # @note CrashHandler の MiniDumpWriteDump。落ちてから LoadLibrary しないよう起動時に読ませる。
        dbghelp
        "$<$<CONFIG:Debug>:${DIRECTXTEX_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${DIRECTXTEX_LIB_RELEASE}>"
        "$<$<CONFIG:Debug>:${ASSIMP_LIB_DEBUG}>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${ASSIMP_LIB_RELEASE}>"
)
