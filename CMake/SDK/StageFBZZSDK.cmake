# FBZZ Engine
# StageFBZZSDK.cmake | CMake
# 共有 SDK の構成別成果物と公開ファイルを安全に同期する

foreach(REQUIRED SDK_ROOT SOURCE_ROOT GENERATED_ROOT CONFIG MATH_DLL MATH_LIB
                 PHYSICS_DLL PHYSICS_LIB ENGINE_DLL ENGINE_LIB IMGUI_DLL ASSIMP_DLL EDITOR_DIR)
    if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
        message(FATAL_ERROR "StageFBZZSDK: ${REQUIRED} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${SDK_ROOT}/include" "${SDK_ROOT}/lib/${CONFIG}"
                    "${SDK_ROOT}/bin/${CONFIG}" "${SDK_ROOT}/cmake/FBZZ"
                    "${SDK_ROOT}/tools/${CONFIG}/Editor")

foreach(INCLUDE_DIR
        "${SOURCE_ROOT}/Projects/Math/include"
        "${SOURCE_ROOT}/Projects/Physics/include"
        "${SOURCE_ROOT}/Projects/Engine/include")
    file(COPY "${INCLUDE_DIR}/" DESTINATION "${SDK_ROOT}/include")
endforeach()
file(COPY "${SOURCE_ROOT}/ThirdParty/TomlPlusPlus/include/" DESTINATION "${SDK_ROOT}/include")

foreach(LIB_FILE "${MATH_LIB}" "${PHYSICS_LIB}" "${ENGINE_LIB}")
    file(COPY "${LIB_FILE}" DESTINATION "${SDK_ROOT}/lib/${CONFIG}")
endforeach()
foreach(DLL_FILE "${MATH_DLL}" "${PHYSICS_DLL}" "${ENGINE_DLL}" "${IMGUI_DLL}" "${ASSIMP_DLL}")
    file(COPY "${DLL_FILE}" DESTINATION "${SDK_ROOT}/bin/${CONFIG}")
endforeach()

# DX12 の遅延ロード DLL は Windows SDK に存在する場合だけ Engine 出力隣へ生成される。
get_filename_component(ENGINE_BINARY_DIR "${ENGINE_DLL}" DIRECTORY)
foreach(OPTIONAL_DLL dxcompiler.dll dxil.dll)
    if(EXISTS "${ENGINE_BINARY_DIR}/${OPTIONAL_DLL}")
        file(COPY "${ENGINE_BINARY_DIR}/${OPTIONAL_DLL}" DESTINATION "${SDK_ROOT}/bin/${CONFIG}")
    endif()
endforeach()

file(COPY "${EDITOR_DIR}/" DESTINATION "${SDK_ROOT}/tools/${CONFIG}/Editor")
# WHY: Editor出力側のPOST_BUILD状態に依存せず、SDK生成時にも起動必須のimgui.dllをexe隣へ保証する。
file(COPY "${IMGUI_DLL}" DESTINATION "${SDK_ROOT}/tools/${CONFIG}/Editor")
file(COPY "${SOURCE_ROOT}/Assets/" DESTINATION "${SDK_ROOT}/Assets")
file(COPY "${GENERATED_ROOT}/FBZZConfig.cmake" "${GENERATED_ROOT}/FBZZConfigVersion.cmake"
     "${GENERATED_ROOT}/FBZZTargets.cmake"
     DESTINATION "${SDK_ROOT}/cmake/FBZZ")
file(COPY "${GENERATED_ROOT}/fbzz-sdk.toml" DESTINATION "${SDK_ROOT}")
file(COPY "${SOURCE_ROOT}/CMake/build.config.in" DESTINATION "${SDK_ROOT}/cmake/FBZZ")
