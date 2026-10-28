# @file    FBZZSDK.cmake
# @brief   ビルドと実行時配置の構成。
# @author  Hasegawa Jin
# @date    2026-09-21
# @note FBZZ Engine
# @note FBZZSDK.cmake | CMake
# @note 版別共有 SDK と CMake IMPORTED package の生成

include(CMakePackageConfigHelpers)

# @note SDKはEngine versionごとに1つだけ存在し、publishのたびに `SDK/<version>/` を上書きする。
# @note 以前はcommit revisionや未コミット差分のfingerprintをIDへ含め、入力ごとに別世代を
# @note 作るimmutableモデルだった。しかし1世代がlib/bin/include/Assetsを丸ごと抱えた
# @note 数百MB級で、編集して公開するたびにストアが増え続ける。ABI契約は「version完全一致」
# @note であり (Docs/design/shared-engine-sdk.md)、版が同じSDKを複数持っても
# @note 選択の手間が増えるだけで区別の意味が無いため、版ごとに1つへ畳む。
set(FBZZ_SDK_ID "${PROJECT_VERSION}")

# @note 由来はIDから読めなくなるため、publish時点のrevisionをmanifestへ残す。
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

# @note Engine モジュールへ版数定義を明示し、クラッシュレポートへ正しい版を記録する。
get_property(FBZZ_ENGINE_MODULE_TARGETS GLOBAL PROPERTY FBZZ_ENGINE_MODULES)
foreach(FBZZ_VERSION_TARGET FBZZMath FBZZPhysics FBZZFluid FBZZCore FBZZGraphics FBZZEngine FBZZEditor
                        ${FBZZ_ENGINE_MODULE_TARGETS})
    target_compile_definitions(${FBZZ_VERSION_TARGET} PUBLIC
        FBZZ_ENGINE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        FBZZ_ENGINE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        FBZZ_ENGINE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        FBZZ_ENGINE_VERSION_STRING="${PROJECT_VERSION}"
    )
endforeach()

set(FBZZ_SDK_MSVC_VERSION "${MSVC_VERSION}")
set(FBZZ_SDK_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(FBZZ_SDK_MSVC_TOOLSET_VERSION "${CMAKE_VS_PLATFORM_TOOLSET_VERSION}")
set(FBZZ_SDK_DX12_ENABLED false)
set(FBZZ_SDK_AGILITY_VERSION 0)
set(FBZZ_AGILITY_SDK_PATH ".\\D3D12\\")
set(FBZZ_SDK_CORE_SHA256 "")
set(FBZZ_SDK_LAYERS_SHA256 "")
set(FBZZ_SDK_DXC_EXE_SHA256 "")
set(FBZZ_SDK_DXC_COMPILER_SHA256 "")
set(FBZZ_SDK_DXC_VALIDATOR_SHA256 "")
set(FBZZ_SDK_RUNTIME_CONTRACT "schema=2;dx12=${FBZZ_ENABLE_DX12}")
if(FBZZ_ENABLE_DX12)
    include("${CMAKE_SOURCE_DIR}/ThirdParty/AgilitySDK/VERSION")
    include("${CMAKE_SOURCE_DIR}/ThirdParty/DXC/VERSION")
    set(FBZZ_SDK_DX12_ENABLED true)
    set(FBZZ_SDK_AGILITY_VERSION "${FBZZ_AGILITY_SDK_VERSION}")
    set(FBZZ_SDK_CORE_SHA256 "${FBZZ_AGILITY_CORE_SHA256}")
    set(FBZZ_SDK_LAYERS_SHA256 "${FBZZ_AGILITY_LAYERS_SHA256}")
    set(FBZZ_SDK_DXC_EXE_SHA256 "${FBZZ_DXC_EXECUTABLE_SHA256}")
    set(FBZZ_SDK_DXC_COMPILER_SHA256 "${FBZZ_DXC_COMPILER_SHA256}")
    set(FBZZ_SDK_DXC_VALIDATOR_SHA256 "${FBZZ_DXC_VALIDATOR_SHA256}")
    foreach(FBZZ_CONTRACT_FILE ThirdParty/AgilitySDK/VERSION ThirdParty/DXC/VERSION)
        file(SHA256 "${CMAKE_SOURCE_DIR}/${FBZZ_CONTRACT_FILE}" FBZZ_CONTRACT_HASH)
        string(APPEND FBZZ_SDK_RUNTIME_CONTRACT ";${FBZZ_CONTRACT_FILE}=${FBZZ_CONTRACT_HASH}")
    endforeach()
endif()
# @note The canonical Editor layout and package contract apply equally to SDKs without DX12.
foreach(FBZZ_CONTRACT_FILE CMake/FBZZAgilitySDK.cmake CMake/StageAgilitySDK.cmake CMake/AgilitySDKExports.cpp.in
        CMake/SDK/fbzz-sdk.toml.in CMake/SDK/FBZZConfig.cmake.in CMake/SDK/StageFBZZSDK.cmake CMake/SDK/ValidateFBZZSDK.cmake)
    file(SHA256 "${CMAKE_SOURCE_DIR}/${FBZZ_CONTRACT_FILE}" FBZZ_CONTRACT_HASH)
    string(APPEND FBZZ_SDK_RUNTIME_CONTRACT ";${FBZZ_CONTRACT_FILE}=${FBZZ_CONTRACT_HASH}")
endforeach()
string(SHA256 FBZZ_SDK_RUNTIME_FINGERPRINT "${FBZZ_SDK_RUNTIME_CONTRACT}")
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

# @note CMake targetの実際の公開interfaceをexportし、手書きpackageとの乖離を防ぐ。
install(TARGETS FBZZMath FBZZPhysics FBZZFluid FBZZCore FBZZGraphics FBZZEngine ImGui TomlPlusPlus
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
    "${CMAKE_SOURCE_DIR}/Projects/Core/include/"
    "${CMAKE_SOURCE_DIR}/Projects/Graphics/include/"
    "${CMAKE_SOURCE_DIR}/Projects/Engine/include/"
    "${CMAKE_SOURCE_DIR}/ThirdParty/TomlPlusPlus/include/"
    DESTINATION "include"
)
# @note ImGui は FBZZEngine の private 共有依存だが、CMake export の依存グラフを完結させるため
# @note FBZZ::ImGui として同梱する。公開 include path と同じ構造で必要な header のみ配置する。
install(DIRECTORY "${CMAKE_SOURCE_DIR}/ThirdParty/ImGui/"
    DESTINATION "include/ThirdParty/ImGui"
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/Assets/"
    DESTINATION "share/fbzz/Assets"
)
# @note Framework-dependent managed hosting uses the user's .NET 10 runtime; nethost and SDK assemblies accompany each Engine configuration.
install(FILES "${FBZZ_DOTNET_HOST_PACK}/nethost.dll" DESTINATION "bin/$<CONFIG>")
install(DIRECTORY "${FBZZ_MANAGED_OUTPUT_DIRECTORY}/" DESTINATION "bin/$<CONFIG>/Managed")
# @note ソース checkout の無い GameHub project も既存の直列ビルド入口と同じ固定 Roslyn SDK でコンパイルする。
install(FILES "${CMAKE_SOURCE_DIR}/Tools/AgentBuild.ps1" DESTINATION "Tools")
install(FILES "${CMAKE_SOURCE_DIR}/Projects/Managed/global.json" DESTINATION "Projects/Managed")
# @note GameHub and SDK-only authoring use the same starter and canonical documentation.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/Projects/Managed/Authoring/ManagedStarter/"
    DESTINATION "share/fbzz/managed/ManagedStarter"
    PATTERN "bin" EXCLUDE PATTERN "obj" EXCLUDE PATTERN "Library" EXCLUDE)
install(FILES "${CMAKE_SOURCE_DIR}/Projects/Managed/global.json"
    DESTINATION "share/fbzz/managed/ManagedStarter")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/Docs/" DESTINATION "share/fbzz/Docs"
    FILES_MATCHING PATTERN "*.md" PATTERN "*.svg" PATTERN "*.png")
install(FILES "${CMAKE_SOURCE_DIR}/Projects/Managed/README.md"
    DESTINATION "share/fbzz/Projects/Managed")
install(FILES "${FBZZ_DOTNET_ROOT}/LICENSE.txt" "${FBZZ_DOTNET_ROOT}/ThirdPartyNotices.txt"
    DESTINATION "share/fbzz/licenses/DotNet")
install(FILES
    "${FBZZ_SDK_GENERATED_DIR}/FBZZConfig.cmake"
    "${FBZZ_SDK_GENERATED_DIR}/FBZZConfigVersion.cmake"
    "${CMAKE_SOURCE_DIR}/CMake/build.config.in"
    DESTINATION "cmake/FBZZ"
)
install(FILES
    "${CMAKE_SOURCE_DIR}/CMake/FBZZAgilitySDK.cmake"
    "${CMAKE_SOURCE_DIR}/CMake/StageAgilitySDK.cmake"
    "${CMAKE_SOURCE_DIR}/CMake/AgilitySDKExports.cpp.in"
    DESTINATION "cmake/FBZZ")
# @note SDK の Editor は ImPlot を静的リンクし、既存の staging がこの告知を Editor/EngineLicenses へ配置する。
install(FILES
    "${CMAKE_SOURCE_DIR}/ThirdParty/ImPlot/LICENSE"
    "${CMAKE_SOURCE_DIR}/ThirdParty/ImPlot/VERSION"
    DESTINATION "share/fbzz/licenses/ImPlot")
# @note ImPlot のソースに記載された全著作権者を Editor の配布告知へ含める。
install(FILES
    "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md"
    DESTINATION "share/fbzz/licenses")
if(FBZZ_ENABLE_DX12)
    foreach(FBZZ_RUNTIME_PACKAGE AgilitySDK DXC)
        if(FBZZ_RUNTIME_PACKAGE STREQUAL "AgilitySDK")
            set(FBZZ_PACKAGE_NOTICES LICENSE LICENSE.txt LICENSE-CODE.txt VERSION "distributable files.txt")
        else()
            set(FBZZ_PACKAGE_NOTICES LICENSE LICENCE-MIT.txt LICENSE-LLVM.txt LICENSE-MS.txt VERSION)
        endif()
        foreach(FBZZ_NOTICE IN LISTS FBZZ_PACKAGE_NOTICES)
            install(FILES "${CMAKE_SOURCE_DIR}/ThirdParty/${FBZZ_RUNTIME_PACKAGE}/${FBZZ_NOTICE}"
                DESTINATION "share/fbzz/licenses/${FBZZ_RUNTIME_PACKAGE}")
        endforeach()
    endforeach()
endif()

set(FBZZ_SDK_PIX_RUNTIME_DLL "")
set(FBZZ_SDK_PIX_REQUIRED OFF)
if(FBZZ_ENABLE_DX12 AND TARGET WinPixEventRuntime)
    set(FBZZ_SDK_PIX_RUNTIME_DLL "$<TARGET_FILE:WinPixEventRuntime>")
    set(FBZZ_SDK_PIX_REQUIRED ON)
    # @note The graphics DLL imports this private runtime even when GPU capture is disabled.
    install(FILES "$<TARGET_FILE:WinPixEventRuntime>" DESTINATION "bin/$<CONFIG>")
    install(FILES
        "${CMAKE_SOURCE_DIR}/ThirdParty/WinPixEventRuntime/LICENSE"
        "${CMAKE_SOURCE_DIR}/ThirdParty/WinPixEventRuntime/VERSION"
        "${CMAKE_SOURCE_DIR}/ThirdParty/WinPixEventRuntime/ThirdPartyNotices.txt"
        DESTINATION "share/fbzz/licenses/WinPixEventRuntime")
endif()

# @note Visual Studio から FBZZSDK ターゲットをビルドすると、同じ構成の lib/bin と
# @note 構成非依存の headers/package/assets/tools を版別 SDK へ同期する。
add_custom_target(FBZZManagedAuthoringSample
    COMMAND ${CMAKE_COMMAND} -E env MSBuildEnableWorkloadResolver=false DOTNET_CLI_TELEMETRY_OPTOUT=1
        "${FBZZ_DOTNET_EXECUTABLE}" build
        "${CMAKE_SOURCE_DIR}/Projects/Managed/Authoring/ManagedStarter/Starter.Scripts.csproj"
        --configuration "${FBZZ_MANAGED_CONFIGURATION}" --nologo --verbosity minimal
        --output "${CMAKE_BINARY_DIR}/ManagedAuthoring/$<CONFIG>/bin"
        -m:1 -nodeReuse:false -p:UseSharedCompilation=false -p:Platform=AnyCPU
        "-p:FbzzScriptApiPath=${FBZZ_MANAGED_OUTPUT_DIRECTORY}/Fbzz.ScriptApi.dll"
        "-p:FbzzScriptGeneratorPath=${FBZZ_MANAGED_OUTPUT_DIRECTORY}/Fbzz.ScriptGenerator.dll"
        "-p:BaseIntermediateOutputPath=${CMAKE_BINARY_DIR}/ManagedAuthoring/$<CONFIG>/obj/"
        "-p:RestoreSources=${CMAKE_SOURCE_DIR}/Projects/Managed"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/Projects/Managed"
    DEPENDS FBZZManagedHost
    COMMENT "Compiling the canonical SDK-only C# authoring example"
    VERBATIM)
set_target_properties(FBZZManagedAuthoringSample PROPERTIES FOLDER "Managed")

function(FBZZAddSdkTarget target sdkRoot preview)
    set(previewGuardCommands)
    set(pruneCommands)
    if(preview)
        # @note Resolve existing links before any staging write or removal; preview owns only a binary-directory child.
        list(APPEND previewGuardCommands COMMAND ${CMAKE_COMMAND}
            "-DBINARY_ROOT=${CMAKE_BINARY_DIR}" "-DSDK_ROOT=${sdkRoot}" "-DSDK_ID=${FBZZ_SDK_ID}"
            -P "${CMAKE_SOURCE_DIR}/CMake/SDK/GuardFBZZSDKPreview.cmake")
        set(targetComment "Validating isolated FBZZ SDK preview ${FBZZ_SDK_ID}")
    else()
        # @note The official publisher retains its existing version-store cleanup after validation.
        list(APPEND pruneCommands COMMAND ${CMAKE_COMMAND}
            "-DSDK_STORE_ROOT=${FBZZ_SDK_STORE_ROOT}"
            "-DKEEP_SDK_ID=${FBZZ_SDK_ID}"
            -P "${CMAKE_SOURCE_DIR}/CMake/SDK/PruneFBZZSDKStore.cmake")
        set(targetComment "Publishing immutable FBZZ SDK ${FBZZ_SDK_ID}")
    endif()
    add_custom_target(${target}
        ${previewGuardCommands}
        # @note 公開を始める前に成功状態を落とし、途中失敗した構成を選択不能にする。
        COMMAND ${CMAKE_COMMAND}
            "-DSDK_ROOT=${sdkRoot}" "-DCONFIG=$<CONFIG>"
            "-DPUBLISH_BEGIN=ON" "-DMANIFEST_TEMPLATE=${FBZZ_SDK_GENERATED_DIR}/fbzz-sdk.toml"
            -P "${CMAKE_SOURCE_DIR}/CMake/SDK/StageFBZZSDK.cmake"
        # @note 同じフォルダーを再利用するため、前回公開したheaderが残り続ける。
        # @note 削除したAPIをゲーム側がincludeできてしまう退行を避けたいので、公開前に捨てる。
        # @note lib/bin/shareは同名fileの上書きで最新化される。Assetsは350MB超あり毎回消して
        # @note 再コピーすると公開が遅くなりすぎるため、実害の大きいincludeだけに絞る。
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${sdkRoot}/include"
        COMMAND ${CMAKE_COMMAND}
            --install "${CMAKE_BINARY_DIR}"
            --config "$<CONFIG>"
            --prefix "${sdkRoot}"
        COMMAND ${CMAKE_COMMAND}
            "-DSDK_ROOT=${sdkRoot}"
            "-DCONFIG=$<CONFIG>"
            "-DASSIMP_DLL=$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
            "-DEDITOR_DIR=$<TARGET_FILE_DIR:FBZZEditorLauncher>"
            "-DPIX_RUNTIME_DLL=${FBZZ_SDK_PIX_RUNTIME_DLL}"
            "-DDX12_ENABLED=${FBZZ_ENABLE_DX12}"
            "-DAGILITY_ROOT=${CMAKE_SOURCE_DIR}/ThirdParty/AgilitySDK"
            "-DDXC_ROOT=${CMAKE_SOURCE_DIR}/ThirdParty/DXC"
            -P "${CMAKE_SOURCE_DIR}/CMake/SDK/StageFBZZSDK.cmake"
        COMMAND ${CMAKE_COMMAND}
            "-DSDK_ROOT=${sdkRoot}"
            "-DSDK_ID=${FBZZ_SDK_ID}"
            "-DCONFIG=$<CONFIG>"
            "-DPIX_REQUIRED=${FBZZ_SDK_PIX_REQUIRED}"
            -P "${CMAKE_SOURCE_DIR}/CMake/SDK/ValidateFBZZSDK.cmake"
        # @note 検証を通った後にだけ、旧方式が残した同一versionの世代を回収する。
        # @note 先に消すと、公開に失敗した時点で「新しいSDKは不完全・古いSDKは無い」
        # @note という戻り先の無い状態になる。
        ${pruneCommands}
        DEPENDS FBZZMath FBZZPhysics FBZZFluid FBZZCore FBZZGraphics FBZZEngine FBZZEditorLauncher FBZZManagedHost FBZZManagedAuthoringSample
        COMMENT "${targetComment}"
        VERBATIM
    )
    set_target_properties(${target} PROPERTIES FOLDER "SDK")
endfunction()

FBZZAddSdkTarget(FBZZSDK "${FBZZ_SDK_ROOT}" OFF)
# @note Preview uses fresh native/managed outputs and the exact official install/stage/validator path without publishing or pruning the SDK store.
FBZZAddSdkTarget(FBZZSDKPreview "${CMAKE_BINARY_DIR}/SDKPreview/${FBZZ_SDK_ID}" ON)

# @note GameHubはTypeScript版へ分離したため、SDKは明示的にFBZZSDKターゲットをビルドして生成する。
# @note Electron版GameHubはCMake依存グラフに参加しないため、SDK側からHubへの逆向き依存を持たせない。
