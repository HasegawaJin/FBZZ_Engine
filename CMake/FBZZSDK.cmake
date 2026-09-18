# FBZZ Engine
# FBZZSDK.cmake | CMake
# 版別共有 SDK と CMake IMPORTED package の生成

include(CMakePackageConfigHelpers)

# SDKはEngine versionごとに1つだけ存在し、publishのたびに `SDK/<version>/` を上書きする。
# WHY: 以前はcommit revisionや未コミット差分のfingerprintをIDへ含め、入力ごとに別世代を
#      作るimmutableモデルだった。しかし1世代がlib/bin/include/Assetsを丸ごと抱えた
#      数百MB級で、編集して公開するたびにストアが増え続ける。ABI契約は「version完全一致」
#      であり (Docs/design/shared-engine-sdk.md)、版が同じSDKを複数持っても
#      選択の手間が増えるだけで区別の意味が無いため、版ごとに1つへ畳む。
set(FBZZ_SDK_ID "${PROJECT_VERSION}")

# 由来はIDから読めなくなるため、publish時点のrevisionをmanifestへ残す。
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE FBZZ_SDK_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ENCODING UTF-8
    ERROR_QUIET
)
if(FBZZ_SDK_REVISION STREQUAL "")
    set(FBZZ_SDK_REVISION "unknown")
endif()

set(FBZZ_SDK_STORE_ROOT "${CMAKE_SOURCE_DIR}/SDK" CACHE PATH
    "FBZZ SDK artifact store")
set(FBZZ_SDK_ROOT "${FBZZ_SDK_STORE_ROOT}/${FBZZ_SDK_ID}")
set(FBZZ_SDK_GENERATED_DIR "${CMAKE_BINARY_DIR}/Generated/FBZZSDK")
file(MAKE_DIRECTORY "${FBZZ_SDK_GENERATED_DIR}")

# WHY: Script DLL は Engine の C++ 型を共有するため、版数と構成をコンパイル単位へ伝播させる。
#      ScriptDllAbi.hpp の実行時署名と detect_mismatch の双方がこの値を利用する。
#
# WHY 内部モジュールにも配るか: ScriptDllAbi.hpp は Scene/Script.hpp 経由で広く届く。
#      これらのマクロが無い翻訳単位では «"0.0.0-unconfigured"» という既定値へ落ち、
#      detect_mismatch が同じ DLL の中で食い違って LNK2038 になる。現状は各モジュールが
#      FBZZMath をリンクしているおかげで PUBLIC 定義が «たまたま» 流れているだけで、
#      リンク先を 1 つ減らした瞬間に壊れる。宛先を明示して事故を断つ。
get_property(FBZZ_ENGINE_MODULE_TARGETS GLOBAL PROPERTY FBZZ_ENGINE_MODULES)
foreach(FBZZ_ABI_TARGET FBZZMath FBZZPhysics FBZZFluid FBZZEngine FBZZEditor
                        ${FBZZ_ENGINE_MODULE_TARGETS})
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
configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/CMake/SDK/FBZZConfig.cmake.in"
    "${FBZZ_SDK_GENERATED_DIR}/FBZZConfig.cmake"
    INSTALL_DESTINATION "cmake/FBZZ"
    NO_SET_AND_CHECK_MACRO
    NO_CHECK_REQUIRED_COMPONENTS_MACRO
)
write_basic_package_version_file(
    "${FBZZ_SDK_GENERATED_DIR}/FBZZConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY ExactVersion
)
configure_file(
    "${CMAKE_SOURCE_DIR}/CMake/SDK/fbzz-sdk.toml.in"
    "${FBZZ_SDK_GENERATED_DIR}/fbzz-sdk.toml"
    @ONLY
)

# CMake targetの実際の公開interfaceをexportし、手書きpackageとの乖離を防ぐ。
install(TARGETS FBZZMath FBZZPhysics FBZZFluid FBZZEngine ImGui TomlPlusPlus
    EXPORT FBZZTargets
    RUNTIME DESTINATION "bin/$<CONFIG>"
    LIBRARY DESTINATION "bin/$<CONFIG>"
    ARCHIVE DESTINATION "lib/$<CONFIG>"
    INCLUDES DESTINATION "include"
)
install(EXPORT FBZZTargets
    FILE FBZZTargets.cmake
    NAMESPACE FBZZ::
    DESTINATION "cmake/FBZZ"
)
install(DIRECTORY
    "${CMAKE_SOURCE_DIR}/Projects/Math/include/"
    "${CMAKE_SOURCE_DIR}/Projects/Physics/include/"
    "${CMAKE_SOURCE_DIR}/Projects/Fluid/include/"
    "${CMAKE_SOURCE_DIR}/Projects/Engine/include/"
    "${CMAKE_SOURCE_DIR}/ThirdParty/TomlPlusPlus/include/"
    DESTINATION "include"
)
# ImGui は FBZZEngine の private 共有依存だが、CMake export の依存グラフを完結させるため
# FBZZ::ImGui として同梱する。公開 include path と同じ構造で必要な header のみ配置する。
install(DIRECTORY "${CMAKE_SOURCE_DIR}/ThirdParty/ImGui/"
    DESTINATION "include/ThirdParty/ImGui"
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/Assets/"
    DESTINATION "share/fbzz/Assets"
)
install(FILES
    "${FBZZ_SDK_GENERATED_DIR}/FBZZConfig.cmake"
    "${FBZZ_SDK_GENERATED_DIR}/FBZZConfigVersion.cmake"
    "${CMAKE_SOURCE_DIR}/CMake/build.config.in"
    DESTINATION "cmake/FBZZ"
)
install(FILES "${FBZZ_SDK_GENERATED_DIR}/fbzz-sdk.toml" DESTINATION ".")

# WHAT: Visual Studio から FBZZSDK ターゲットをビルドすると、同じ構成の lib/bin と
#       構成非依存の headers/package/assets/tools を版別 SDK へ同期する。
add_custom_target(FBZZSDK
    # 同じフォルダーを再利用するため、前回公開したheaderが残り続ける。
    # 削除したAPIをゲーム側がincludeできてしまう退行を避けたいので、公開前に捨てる。
    # WHY: lib/bin/shareは同名fileの上書きで最新化される。Assetsは350MB超あり毎回消して
    #      再コピーすると公開が遅くなりすぎるため、実害の大きいincludeだけに絞る。
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${FBZZ_SDK_ROOT}/include"
    COMMAND ${CMAKE_COMMAND}
        --install "${CMAKE_BINARY_DIR}"
        --config "$<CONFIG>"
        --prefix "${FBZZ_SDK_ROOT}"
    COMMAND ${CMAKE_COMMAND}
        "-DSDK_ROOT=${FBZZ_SDK_ROOT}"
        "-DCONFIG=$<CONFIG>"
        "-DENGINE_DLL=$<TARGET_FILE:FBZZEngine>"
        "-DIMGUI_DLL=$<TARGET_FILE:ImGui>"
        "-DASSIMP_DLL=$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "-DEDITOR_DIR=$<TARGET_FILE_DIR:FBZZEditorLauncher>"
        -P "${CMAKE_SOURCE_DIR}/CMake/SDK/StageFBZZSDK.cmake"
    COMMAND ${CMAKE_COMMAND}
        "-DSDK_ROOT=${FBZZ_SDK_ROOT}"
        "-DSDK_ID=${FBZZ_SDK_ID}"
        "-DCONFIG=$<CONFIG>"
        -P "${CMAKE_SOURCE_DIR}/CMake/SDK/ValidateFBZZSDK.cmake"
    # 検証を通った後にだけ、旧方式が残した同一versionの世代を回収する。
    # WHY: 先に消すと、公開に失敗した時点で「新しいSDKは不完全・古いSDKは無い」
    #      という戻り先の無い状態になる。
    COMMAND ${CMAKE_COMMAND}
        "-DSDK_STORE_ROOT=${FBZZ_SDK_STORE_ROOT}"
        "-DKEEP_SDK_ID=${FBZZ_SDK_ID}"
        -P "${CMAKE_SOURCE_DIR}/CMake/SDK/PruneFBZZSDKStore.cmake"
    DEPENDS FBZZMath FBZZPhysics FBZZFluid FBZZEngine FBZZEditorLauncher
    COMMENT "Publishing immutable FBZZ SDK ${FBZZ_SDK_ID}"
    VERBATIM
)
set_target_properties(FBZZSDK PROPERTIES FOLDER "SDK")

# GameHubはTypeScript版へ分離したため、SDKは明示的にFBZZSDKターゲットをビルドして生成する。
# WHY: Electron版GameHubはCMake依存グラフに参加しないため、SDK側からHubへの逆向き依存を持たせない。
