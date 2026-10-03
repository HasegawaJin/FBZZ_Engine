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

if(FBZZ_ENABLE_DX12)
    # @note DX 型と Agility / DXC ヘッダーは Graphics の非公開実装だけが使う。
    # @see https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/ Include path ordering.
    target_include_directories(FBZZGraphics BEFORE PRIVATE
        "${FBZZ_AGILITY_ROOT}/build/native/include" "${FBZZ_DXC_ROOT}/include"
        "${CMAKE_CURRENT_BINARY_DIR}")
    configure_file("${CMAKE_SOURCE_DIR}/CMake/GraphicsRuntimeContract.hpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/GraphicsRuntimeContract.hpp" @ONLY)
    target_link_libraries(FBZZGraphics PRIVATE version)
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
    fbzz_stage_agility_sdk(FBZZGraphics)
else()
    # @note Editor / Launcher 側の DXC 配置判定もこの変数を見る。
    unset(FBZZ_DXC_RUNTIME_DIRECTORY CACHE)
endif()
