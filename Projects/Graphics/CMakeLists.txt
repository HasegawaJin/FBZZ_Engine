# @file    CMakeLists.txt
# @brief   Scene に依存しない描画ライブラリと非公開 DX12 バックエンド。
# @author  Hasegawa Jin
# @date    2026-09-21
option(FBZZ_ENABLE_DX12 "Build the Direct3D 12 backend" ON)
set(DIRECTXTEX_INCLUDE "${CMAKE_SOURCE_DIR}/ThirdParty/DirectXTex/include")
set(DIRECTXTEX_LIB_DEBUG "${CMAKE_SOURCE_DIR}/ThirdParty/DirectXTex/lib/x64/Debug/DirectXTex.lib")
set(DIRECTXTEX_LIB_RELEASE "${CMAKE_SOURCE_DIR}/ThirdParty/DirectXTex/lib/x64/Release/DirectXTex.lib")
file(GLOB_RECURSE GRAPHICS_SOURCES CONFIGURE_DEPENDS src/*.cpp include/*.hpp)
if(NOT FBZZ_ENABLE_DX12)
    list(FILTER GRAPHICS_SOURCES EXCLUDE REGEX "/Platform/DX12/")
endif()
add_library(FBZZGraphics SHARED ${GRAPHICS_SOURCES})
set_target_properties(FBZZGraphics PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON EXPORT_NAME Graphics)
target_include_directories(FBZZGraphics
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE "${DIRECTXTEX_INCLUDE}" "${CMAKE_SOURCE_DIR}/ThirdParty/Stb")
target_compile_definitions(FBZZGraphics PRIVATE FBZZ_ENABLE_DX12=$<BOOL:${FBZZ_ENABLE_DX12}>)
target_link_libraries(FBZZGraphics PUBLIC FBZZCore FBZZMath
    PRIVATE ImGui d3d12 dxgi dxguid d3dcompiler
    "$<$<CONFIG:Debug>:${DIRECTXTEX_LIB_DEBUG}>"
    "$<$<OR:$<CONFIG:Release>,$<CONFIG:Development>>:${DIRECTXTEX_LIB_RELEASE}>")
fbzz_use_std_pch(FBZZGraphics)
fbzz_use_unity_build(FBZZGraphics EXCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/StbFontImpl.cpp")

# @note DX12 のホットリロードと DXIL reflection は dxcompiler.dll を実行時に遅延ロードする。FBZZ_ENABLE_DX12=OFF には DXC を強制しない。
get_filename_component(FBZZ_WINDOWS_SDK_BIN_DIRECTORY "${CMAKE_RC_COMPILER}" DIRECTORY)
if(FBZZ_ENABLE_DX12)
    # @note USE_PIX enables official named events in Release as well as Debug; SDK types never escape backend sources.
    # @see https://github.com/microsoft/PixEvents Using the prebuilt runtime and pix3.h.
    target_compile_definitions(FBZZGraphics PRIVATE USE_PIX)
    target_link_libraries(FBZZGraphics PRIVATE WinPixEventRuntime)
    add_custom_command(TARGET FBZZGraphics POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:WinPixEventRuntime>"
            "$<TARGET_FILE_DIR:FBZZGraphics>/WinPixEventRuntime.dll"
        VERBATIM
    )
    if(DEFINED ENV{FBZZ_DXC} AND EXISTS "$ENV{FBZZ_DXC}")
        set(FBZZ_DXC_EXECUTABLE "$ENV{FBZZ_DXC}")
    else()
        # @note VS 開発者環境の外 (CI の素の cmake) では WindowsSdkDir が無く、VS ジェネレーターの CMAKE_RC_COMPILER も "rc" だけでディレクトリを持たない。
        # @note dxc.exe を同梱するのは新しい Windows SDK だけなので、bin を新しい順に並べて当てる (古い SDK の bin には無い)。
        file(GLOB FBZZ_WINDOWS_KITS_BIN_DIRECTORIES
            "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64"
            "$ENV{ProgramFiles}/Windows Kits/10/bin/*/x64")
        list(SORT FBZZ_WINDOWS_KITS_BIN_DIRECTORIES COMPARE NATURAL ORDER DESCENDING)
        find_program(FBZZ_DXC_EXECUTABLE
            NAMES dxc.exe
            HINTS
                "${FBZZ_WINDOWS_SDK_BIN_DIRECTORY}"
                "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
                ${FBZZ_WINDOWS_KITS_BIN_DIRECTORIES}
        )
    endif()
    if(FBZZ_DXC_EXECUTABLE)
        get_filename_component(FBZZ_DXC_DIRECTORY "${FBZZ_DXC_EXECUTABLE}" DIRECTORY)
        # @note dxcompiler.dll は exe 隣から LoadLibraryW で解決されるので、サブディレクトリに出る Editor 等の配置用に場所を公開する。
        set(FBZZ_DXC_RUNTIME_DIRECTORY "${FBZZ_DXC_DIRECTORY}" CACHE INTERNAL "DXC runtime DLL directory")
        foreach(FBZZ_DXC_RUNTIME_DLL dxcompiler.dll dxil.dll)
            if(EXISTS "${FBZZ_DXC_DIRECTORY}/${FBZZ_DXC_RUNTIME_DLL}")
                add_custom_command(TARGET FBZZGraphics POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${FBZZ_DXC_DIRECTORY}/${FBZZ_DXC_RUNTIME_DLL}"
                        "$<TARGET_FILE_DIR:FBZZGraphics>/${FBZZ_DXC_RUNTIME_DLL}"
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
