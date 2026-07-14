file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

add_library(fbzz_engine SHARED ${ENGINE_SOURCES})
set_target_properties(fbzz_engine PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    OUTPUT_NAME "FBZZEngine"
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
target_compile_definitions(fbzz_engine PRIVATE
    FBZZ_CMAKE_CONFIG="$<CONFIG>"
    "$<$<NOT:$<CONFIG:Release>>:FBZZ_DEV_BUILD_DIR=\"${CMAKE_BINARY_DIR}\">"
    "$<$<NOT:$<CONFIG:Release>>:FBZZ_DEV_ENGINE_ROOT=\"${FBZZ_ENGINE_ROOT_ABS}\">"
    "$<$<NOT:$<CONFIG:Release>>:FBZZ_DEV_CMAKE_COMMAND=\"${CMAKE_COMMAND}\">"
)

set(DIRECTXTEX_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/include")
set(DIRECTXTEX_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Debug/DirectXTex.lib")
set(DIRECTXTEX_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Release/DirectXTex.lib")

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
)
    if(NOT EXISTS "${REQUIRED_PATH}")
        message(FATAL_ERROR "Required third-party dependency was not found: ${REQUIRED_PATH}")
    endif()
endforeach()

target_include_directories(fbzz_engine
    PUBLIC  include
    PRIVATE src
    PRIVATE ${DIRECTXTEX_INCLUDE}
    PRIVATE ${ASSIMP_INCLUDE}
)

target_link_libraries(fbzz_engine
    PUBLIC
        fbzz_physics
        fbzz_math
        tomlplusplus
        xaudio2
        mfplat
        mfreadwrite
        mfuuid
        winmm
        d3d12
        dxgi
        dxguid
        d3dcompiler
    PRIVATE
        imgui
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
    foreach(FBZZ_DXC_RUNTIME_DLL dxcompiler.dll dxil.dll)
        if(EXISTS "${FBZZ_DXC_DIRECTORY}/${FBZZ_DXC_RUNTIME_DLL}")
            add_custom_command(TARGET fbzz_engine POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FBZZ_DXC_DIRECTORY}/${FBZZ_DXC_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:fbzz_engine>/${FBZZ_DXC_RUNTIME_DLL}"
                COMMENT "Copying ${FBZZ_DXC_RUNTIME_DLL} for DXIL runtime support"
            )
        endif()
    endforeach()
else()
    message(WARNING "dxc.exe was not found; DX12 shader hot reload will require FBZZ_DXC at runtime")
endif()
