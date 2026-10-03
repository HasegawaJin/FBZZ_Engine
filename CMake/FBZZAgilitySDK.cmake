# @file    FBZZAgilitySDK.cmake
# @brief   描画ホストの Agility 選択と固定 DXC 一式の配置。
# @author  Hasegawa Jin
# @date    2026-10-03
include_guard(GLOBAL)

if(NOT DEFINED FBZZ_AGILITY_ROOT)
    set(FBZZ_AGILITY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/AgilitySDK")
endif()
if(NOT DEFINED FBZZ_DXC_ROOT)
    set(FBZZ_DXC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/DXC")
endif()
if(FBZZ_ENABLE_DX12)
    include("${FBZZ_AGILITY_ROOT}/VERSION")
    include("${FBZZ_DXC_ROOT}/VERSION")
    string(REPLACE "\\" "\\\\" FBZZ_AGILITY_SDK_PATH_CPP "${FBZZ_AGILITY_SDK_PATH}")
    if(EXISTS "${FBZZ_AGILITY_ROOT}/build/native/bin/x64/D3D12Core.dll")
        set(FBZZ_AGILITY_SOURCE_TREE TRUE)
        set(FBZZ_AGILITY_BINARY_DIRECTORY "${FBZZ_AGILITY_ROOT}/build/native/bin/x64")
        set(FBZZ_DXC_RUNTIME_DIRECTORY "${FBZZ_DXC_ROOT}/bin/x64" CACHE INTERNAL "Selected DXC suite" FORCE)
        if(DEFINED ENV{FBZZ_DXC} AND NOT "$ENV{FBZZ_DXC}" STREQUAL "")
            # @note 既存端末の Windows SDK 指定で固定一式を置き換えない。配置変更は FBZZ_DXC_ROOT で明示する。
            message(STATUS "Using verified DXC from ${FBZZ_DXC_RUNTIME_DIRECTORY}; ambient FBZZ_DXC does not override the package")
        endif()
        foreach(name dxc.exe dxcompiler.dll dxil.dll)
            if(NOT EXISTS "${FBZZ_DXC_RUNTIME_DIRECTORY}/${name}")
                message(FATAL_ERROR "DXC suite is incomplete: ${FBZZ_DXC_RUNTIME_DIRECTORY}/${name}")
            endif()
        endforeach()
        foreach(pair "dxc.exe|${FBZZ_DXC_EXECUTABLE_SHA256}" "dxcompiler.dll|${FBZZ_DXC_COMPILER_SHA256}" "dxil.dll|${FBZZ_DXC_VALIDATOR_SHA256}")
            string(REPLACE "|" ";" parts "${pair}")
            list(GET parts 0 name)
            list(GET parts 1 expected)
            file(SHA256 "${FBZZ_DXC_RUNTIME_DIRECTORY}/${name}" actual)
            if(NOT actual STREQUAL expected)
                message(FATAL_ERROR "DXC suite is not the verified package: ${FBZZ_DXC_RUNTIME_DIRECTORY}/${name}")
            endif()
        endforeach()
        set(FBZZ_DXC_EXECUTABLE "${FBZZ_DXC_RUNTIME_DIRECTORY}/dxc.exe" CACHE FILEPATH "Selected DXC executable" FORCE)
        foreach(name D3D12Core.dll d3d12SDKLayers.dll)
            if(NOT EXISTS "${FBZZ_AGILITY_BINARY_DIRECTORY}/${name}")
                message(FATAL_ERROR "Agility package is incomplete: ${name}")
            endif()
        endforeach()
    endif()
endif()

# @note EXE の直接ソースにすることで未参照 STATIC 翻訳単位の除去を避ける。
# @see https://microsoft.github.io/DirectX-Specs/d3d/D3D12Redistributable.html#application-and-games
function(fbzz_enable_agility_sdk target)
    if(NOT FBZZ_ENABLE_DX12)
        fbzz_stage_agility_sdk(${target})
        return()
    endif()
    get_target_property(targetType ${target} TYPE)
    if(NOT targetType STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "fbzz_enable_agility_sdk requires an EXECUTABLE: ${target}")
    endif()
    get_target_property(alreadyEnabled ${target} FBZZ_AGILITY_ENABLED)
    if(alreadyEnabled)
        return()
    endif()
    set(exportSource "${CMAKE_CURRENT_BINARY_DIR}/${target}AgilityExports.cpp")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AgilitySDKExports.cpp.in" "${exportSource}" @ONLY)
    target_sources(${target} PRIVATE "${exportSource}")
    set_property(TARGET ${target} PROPERTY FBZZ_AGILITY_ENABLED TRUE)
    fbzz_stage_agility_sdk(${target})
endfunction()

# @note SDK consumer は公開済み構成の runtime を使用し、ThirdParty へ依存しない。
function(fbzz_stage_agility_sdk target)
    get_target_property(alreadyStaged ${target} FBZZ_AGILITY_STAGED)
    if(alreadyStaged)
        return()
    endif()
    if(NOT FBZZ_ENABLE_DX12)
        # @note 描画依存を持ち込まず source-tree 配布にも無効構成の契約を記録する。
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} "-DFBZZ_OUTPUT=$<TARGET_FILE_DIR:${target}>" -DFBZZ_DX12_ENABLED=OFF
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageAgilitySDK.cmake" VERBATIM)
        set_property(TARGET ${target} PROPERTY FBZZ_AGILITY_STAGED TRUE)
        return()
    endif()
    if(NOT FBZZ_AGILITY_SOURCE_TREE)
        set(coreDirectory "${FBZZ_SDK_ROOT}/bin/$<CONFIG>/D3D12")
        set(dxcDirectory "${FBZZ_SDK_ROOT}/bin/$<CONFIG>")
        set(pixLicenses "${FBZZ_SDK_ROOT}/share/fbzz/licenses/WinPixEventRuntime")
    else()
        set(coreDirectory "${FBZZ_AGILITY_BINARY_DIRECTORY}")
        set(dxcDirectory "${FBZZ_DXC_RUNTIME_DIRECTORY}")
        set(pixLicenses "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../ThirdParty/WinPixEventRuntime")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DFBZZ_OUTPUT=$<TARGET_FILE_DIR:${target}>"
            "-DFBZZ_CONFIG=$<CONFIG>"
            "-DFBZZ_CORE_SOURCE=${coreDirectory}"
            "-DFBZZ_DXC_INPUT_DIRECTORY=${dxcDirectory}"
            "-DFBZZ_AGILITY_LICENSES=${FBZZ_AGILITY_ROOT}"
            "-DFBZZ_DXC_LICENSES=${FBZZ_DXC_ROOT}"
            "-DFBZZ_PIX_LICENSES=${pixLicenses}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageAgilitySDK.cmake"
        VERBATIM)
    set_property(TARGET ${target} PROPERTY FBZZ_AGILITY_STAGED TRUE)
endfunction()
