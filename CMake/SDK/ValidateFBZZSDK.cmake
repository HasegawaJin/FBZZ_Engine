# @file    ValidateFBZZSDK.cmake
# @brief   SDK staging 後の必須レイアウトと package 契約の検証。
# @author  Hasegawa Jin
# @date    2026-07-15

foreach(REQUIRED SDK_ROOT SDK_ID CONFIG)
    if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
        message(FATAL_ERROR "ValidateFBZZSDK: ${REQUIRED} is required")
    endif()
endforeach()

set(REQUIRED_PATHS
    "fbzz-sdk.toml"
    "cmake/FBZZ/FBZZConfig.cmake"
    "cmake/FBZZ/FBZZConfigVersion.cmake"
    "cmake/FBZZ/FBZZTargets.cmake"
    "cmake/FBZZ/build.config.in"
    "share/fbzz/Assets/Shaders"
    "include/Engine/Scene/ScriptDllAbi.hpp"
    "include/Physics/World.hpp"
    "include/Fluid/FluidSolver.hpp"
    "include/Math/Vector3.hpp"
    "include/Core/Logger.hpp"
    "include/Graphics/Renderer/RenderScene.hpp"
    "lib/${CONFIG}/FBZZMath.lib"
    "lib/${CONFIG}/FBZZPhysics.lib"
    "lib/${CONFIG}/FBZZFluid.lib"
    "lib/${CONFIG}/FBZZEngine.lib"
    "lib/${CONFIG}/FBZZCore.lib"
    "lib/${CONFIG}/FBZZGraphics.lib"
    "lib/${CONFIG}/imgui.lib"
    "bin/${CONFIG}/FBZZMath.dll"
    "bin/${CONFIG}/FBZZPhysics.dll"
    "bin/${CONFIG}/FBZZFluid.dll"
    "bin/${CONFIG}/FBZZEngine.dll"
    "bin/${CONFIG}/FBZZCore.dll"
    "bin/${CONFIG}/FBZZGraphics.dll"
    "tools/${CONFIG}/Editor/FBZZCore.dll"
    "tools/${CONFIG}/Editor/FBZZGraphics.dll"
    "bin/${CONFIG}/imgui.dll"
    # @note 作業世代の最新性判定が公開時刻に依存するので、stamp も契約に含める。
    "bin/${CONFIG}/published.stamp"
    "tools/${CONFIG}/Editor/FBZZEditor.exe"
    # @note Editor は起動時に imgui.dll を exe 隣から読む。
    "tools/${CONFIG}/Editor/imgui.dll"
)
foreach(REQUIRED_PATH IN LISTS REQUIRED_PATHS)
    if(NOT EXISTS "${SDK_ROOT}/${REQUIRED_PATH}")
        message(FATAL_ERROR "FBZZ SDK validation failed: ${REQUIRED_PATH} is missing")
    endif()
endforeach()

# @note DXIL reflection は dxcompiler.dll を exe 隣から LoadLibraryW で解決する。Editor 隣に無いとシェーダーが全滅する。
# @note 契約は同梱の有無ではなく、bin に在れば Editor 隣にも在るという整合。
foreach(DXC_RUNTIME_DLL dxcompiler.dll dxil.dll)
    if(EXISTS "${SDK_ROOT}/bin/${CONFIG}/${DXC_RUNTIME_DLL}"
       AND NOT EXISTS "${SDK_ROOT}/tools/${CONFIG}/Editor/${DXC_RUNTIME_DLL}")
        message(FATAL_ERROR
            "FBZZ SDK validation failed: tools/${CONFIG}/Editor/${DXC_RUNTIME_DLL} is missing")
    endif()
endforeach()

# @note engine-rebuild.config が紛れ込むと、SDK から起動した Editor が開発機のソース build を再ビルドしに行く。
# @note share/ (Assets) は 350MB 超で走査が重いので、exe / DLL が並ぶ範囲だけを見る。
file(GLOB_RECURSE LEAKED_DEV_CONFIGS
    "${SDK_ROOT}/bin/engine-rebuild.config"
    "${SDK_ROOT}/tools/engine-rebuild.config"
)
if(EXISTS "${SDK_ROOT}/engine-rebuild.config")
    list(APPEND LEAKED_DEV_CONFIGS "${SDK_ROOT}/engine-rebuild.config")
endif()
if(LEAKED_DEV_CONFIGS)
    message(FATAL_ERROR "FBZZ SDK validation failed: source-tree only file was published: ${LEAKED_DEV_CONFIGS}")
endif()

file(READ "${SDK_ROOT}/fbzz-sdk.toml" SDK_MANIFEST_TEXT)
string(FIND "${SDK_MANIFEST_TEXT}" "id = \"${SDK_ID}\"" SDK_ID_POSITION)
if(SDK_ID_POSITION EQUAL -1)
    message(FATAL_ERROR "FBZZ SDK validation failed: manifest SDK ID does not match ${SDK_ID}")
endif()

file(READ "${SDK_ROOT}/cmake/FBZZ/FBZZTargets.cmake" TARGETS_TEXT)
foreach(IMPORTED_TARGET "FBZZ::Math" "FBZZ::Physics" "FBZZ::Fluid" "FBZZ::Engine" "FBZZ::Core" "FBZZ::Graphics" "FBZZ::ImGui" "FBZZ::TomlPlusPlus")
    string(FIND "${TARGETS_TEXT}" "${IMPORTED_TARGET}" TARGET_POS)
    if(TARGET_POS EQUAL -1)
        message(FATAL_ERROR "FBZZ SDK validation failed: ${IMPORTED_TARGET} is not exported")
    endif()
endforeach()
