# FBZZ Engine
# FBZZSDK.cmake | CMake
# 版別共有 SDK と CMake IMPORTED package の生成

set(FBZZ_SDK_ROOT "${CMAKE_SOURCE_DIR}/SDK/${PROJECT_VERSION}" CACHE PATH
    "Versioned FBZZ SDK output directory")
set(FBZZ_SDK_GENERATED_DIR "${CMAKE_BINARY_DIR}/Generated/FBZZSDK")
file(MAKE_DIRECTORY "${FBZZ_SDK_GENERATED_DIR}")

# WHY: Script DLL は Engine の C++ 型を共有するため、版数と構成をコンパイル単位へ伝播させる。
#      ScriptDllAbi.hpp の実行時署名と detect_mismatch の双方がこの値を利用する。
foreach(FBZZ_ABI_TARGET FBZZMath FBZZPhysics FBZZEngine FBZZEditor)
    target_compile_definitions(${FBZZ_ABI_TARGET} PUBLIC
        FBZZ_ENGINE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        FBZZ_ENGINE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        FBZZ_ENGINE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        FBZZ_ENGINE_VERSION_STRING="${PROJECT_VERSION}"
        "FBZZ_BUILD_CONFIG_ID=$<IF:$<CONFIG:Debug>,1,$<IF:$<CONFIG:Development>,2,3>>"
        "FBZZ_BUILD_CONFIG_NAME=$<IF:$<CONFIG:Debug>,\"Debug\",$<IF:$<CONFIG:Development>,\"Development\",\"Release\">>"
    )
endforeach()

set(FBZZ_SDK_MSVC_VERSION "${MSVC_VERSION}")
set(FBZZ_SDK_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(FBZZ_SDK_MSVC_TOOLSET_VERSION "${CMAKE_VS_PLATFORM_TOOLSET_VERSION}")
configure_file(
    "${CMAKE_SOURCE_DIR}/CMake/SDK/FBZZConfig.cmake.in"
    "${FBZZ_SDK_GENERATED_DIR}/FBZZConfig.cmake"
    @ONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/CMake/SDK/FBZZTargets.cmake.in"
    "${FBZZ_SDK_GENERATED_DIR}/FBZZTargets.cmake"
    @ONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/CMake/SDK/FBZZConfigVersion.cmake.in"
    "${FBZZ_SDK_GENERATED_DIR}/FBZZConfigVersion.cmake"
    @ONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/CMake/SDK/fbzz-sdk.toml.in"
    "${FBZZ_SDK_GENERATED_DIR}/fbzz-sdk.toml"
    @ONLY
)

# WHAT: Visual Studio から FBZZSDK ターゲットをビルドすると、同じ構成の lib/bin と
#       構成非依存の headers/package/assets/tools を版別 SDK へ同期する。
add_custom_target(FBZZSDK
    COMMAND ${CMAKE_COMMAND}
        "-DSDK_ROOT=${FBZZ_SDK_ROOT}"
        "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}"
        "-DGENERATED_ROOT=${FBZZ_SDK_GENERATED_DIR}"
        "-DCONFIG=$<CONFIG>"
        "-DMATH_DLL=$<TARGET_FILE:FBZZMath>"
        "-DMATH_LIB=$<TARGET_LINKER_FILE:FBZZMath>"
        "-DPHYSICS_DLL=$<TARGET_FILE:FBZZPhysics>"
        "-DPHYSICS_LIB=$<TARGET_LINKER_FILE:FBZZPhysics>"
        "-DENGINE_DLL=$<TARGET_FILE:FBZZEngine>"
        "-DENGINE_LIB=$<TARGET_LINKER_FILE:FBZZEngine>"
        "-DIMGUI_DLL=$<TARGET_FILE:ImGui>"
        "-DASSIMP_DLL=$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "-DEDITOR_DIR=$<TARGET_FILE_DIR:FBZZEditorLauncher>"
        -P "${CMAKE_SOURCE_DIR}/CMake/SDK/StageFBZZSDK.cmake"
    COMMAND ${CMAKE_COMMAND}
        "-DSDK_ROOT=${FBZZ_SDK_ROOT}"
        "-DCONFIG=$<CONFIG>"
        -P "${CMAKE_SOURCE_DIR}/CMake/SDK/ValidateFBZZSDK.cmake"
    # WHY: GameHubからVFXを開く際もSDK内の実行ファイルを使うため、EditorとVFXEditorを同じ版へ揃える。
    DEPENDS FBZZMath FBZZPhysics FBZZEngine FBZZEditorLauncher FBZZVFXEditor
    COMMENT "Staging FBZZ SDK ${PROJECT_VERSION}"
    VERBATIM
)
set_target_properties(FBZZSDK PROPERTIES FOLDER "SDK")

# GameHubはTypeScript版へ分離したため、SDKは明示的にfbzz_sdkターゲットをビルドして生成する。
# WHY: Electron版GameHubはCMake依存グラフに参加しないため、SDK側からHubへの逆向き依存を持たせない。
