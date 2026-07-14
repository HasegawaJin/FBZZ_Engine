# FBZZ Engine
# ValidateFBZZSDK.cmake | CMake
# SDK staging 後の必須レイアウトと package 契約を検証する

foreach(REQUIRED SDK_ROOT CONFIG)
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
    "include/Engine/Scene/ScriptDllAbi.hpp"
    "include/Physics/World.hpp"
    "include/Math/Vector3.hpp"
    "lib/${CONFIG}/FBZZMath.lib"
    "lib/${CONFIG}/FBZZPhysics.lib"
    "lib/${CONFIG}/FBZZEngine.lib"
    "bin/${CONFIG}/FBZZMath.dll"
    "bin/${CONFIG}/FBZZPhysics.dll"
    "bin/${CONFIG}/FBZZEngine.dll"
    "tools/${CONFIG}/Editor/FBZZEditor.exe"
)
foreach(REQUIRED_PATH IN LISTS REQUIRED_PATHS)
    if(NOT EXISTS "${SDK_ROOT}/${REQUIRED_PATH}")
        message(FATAL_ERROR "FBZZ SDK validation failed: ${REQUIRED_PATH} is missing")
    endif()
endforeach()

file(READ "${SDK_ROOT}/cmake/FBZZ/FBZZTargets.cmake" TARGETS_TEXT)
foreach(IMPORTED_TARGET "FBZZ::Math" "FBZZ::Physics" "FBZZ::Engine")
    string(FIND "${TARGETS_TEXT}" "${IMPORTED_TARGET}" TARGET_POS)
    if(TARGET_POS EQUAL -1)
        message(FATAL_ERROR "FBZZ SDK validation failed: ${IMPORTED_TARGET} is not exported")
    endif()
endforeach()
