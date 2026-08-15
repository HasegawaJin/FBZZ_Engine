# FBZZ Engine
# ValidateFBZZSDK.cmake | CMake
# SDK staging 後の必須レイアウトと package 契約を検証する

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
    "include/Math/Vector3.hpp"
    "lib/${CONFIG}/FBZZMath.lib"
    "lib/${CONFIG}/FBZZPhysics.lib"
    "lib/${CONFIG}/FBZZEngine.lib"
    "lib/${CONFIG}/imgui.lib"
    "bin/${CONFIG}/FBZZMath.dll"
    "bin/${CONFIG}/FBZZPhysics.dll"
    "bin/${CONFIG}/FBZZEngine.dll"
    "bin/${CONFIG}/imgui.dll"
    # WHY: 作業世代の最新性判定は公開時刻に依存するため、stampの存在もSDK契約に含める。
    "bin/${CONFIG}/published.stamp"
    "tools/${CONFIG}/Editor/FBZZEditor.exe"
    "tools/${CONFIG}/Editor/FBZZVFXEditor.exe"
    # WHY: Editor は imgui 共有 DLL を起動時に読み込むため、exe と同じ階層への配置をSDK契約として検証する。
    "tools/${CONFIG}/Editor/imgui.dll"
)
foreach(REQUIRED_PATH IN LISTS REQUIRED_PATHS)
    if(NOT EXISTS "${SDK_ROOT}/${REQUIRED_PATH}")
        message(FATAL_ERROR "FBZZ SDK validation failed: ${REQUIRED_PATH} is missing")
    endif()
endforeach()

file(READ "${SDK_ROOT}/fbzz-sdk.toml" SDK_MANIFEST_TEXT)
string(FIND "${SDK_MANIFEST_TEXT}" "id = \"${SDK_ID}\"" SDK_ID_POSITION)
if(SDK_ID_POSITION EQUAL -1)
    message(FATAL_ERROR "FBZZ SDK validation failed: manifest SDK ID does not match ${SDK_ID}")
endif()

file(READ "${SDK_ROOT}/cmake/FBZZ/FBZZTargets.cmake" TARGETS_TEXT)
foreach(IMPORTED_TARGET "FBZZ::Math" "FBZZ::Physics" "FBZZ::Engine" "FBZZ::ImGui" "FBZZ::TomlPlusPlus")
    string(FIND "${TARGETS_TEXT}" "${IMPORTED_TARGET}" TARGET_POS)
    if(TARGET_POS EQUAL -1)
        message(FATAL_ERROR "FBZZ SDK validation failed: ${IMPORTED_TARGET} is not exported")
    endif()
endforeach()
