# @file    CMakeLists.txt
# @brief   ビルドと実行時配置の構成。
# @author  Hasegawa Jin
# @date    2026-09-21
file(GLOB_RECURSE SANDBOX_SOURCES
    src/*.cpp
    src/*.hpp
)
add_executable(Sandbox ${SANDBOX_SOURCES})


# @note DPI 認識をプロセス起動時に確定させる (未適用だと高 DPI 環境で UI 全体が滲む)。
fbzz_apply_app_manifest(Sandbox)
fbzz_enable_agility_sdk(Sandbox)

set_target_properties(Sandbox PROPERTIES
    # @note CMake の内部ターゲット名は既存依存との互換性を保ち、公開成果物だけ PascalCase に統一する。
    OUTPUT_NAME "Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Sandbox"
)

# @note post-build コマンドや SyncSandboxAssets ターゲットで参照するため、
# @note add_executable(Sandbox) の直後に定義する。
set(SANDBOX_DEFAULT_PROJECT_DIR "$<TARGET_FILE_DIR:Sandbox>/SandboxProject")
set(SANDBOX_PROJECT_TEMPLATE_DIR "${CMAKE_SOURCE_DIR}/Projects/GameHub/Templates/standard")
set(SANDBOX_MANAGED_STARTER_DIR "${CMAKE_SOURCE_DIR}/Projects/Managed/Authoring/ManagedStarter")
set(SANDBOX_ENSURE_PROJECT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/EnsureSandboxProject.cmake")

# @note 更新するのは実行用の共通 Assets だけ。編集する SandboxProject は初回配置後に上書きしない。
add_custom_target(SyncSandboxAssets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:Sandbox>/Assets"
    COMMENT "Syncing engine runtime Assets..."
    VERBATIM
)
# @note Sandbox の実行ファイルと共通 Assets の配置を同じ依存先で完了させる。
add_dependencies(SyncSandboxAssets Sandbox)

set(SANDBOX_STANDALONE_SOURCES ${SANDBOX_SOURCES})

# @note RuntimeBuild 用の配布 exe。FBZZ_STANDALONE_TARGET ではEditorAppをコンパイルしないため、
# @note Build Settings からゲーム本体だけを再コンパイルできる。
add_executable(SandboxStandalone ${SANDBOX_STANDALONE_SOURCES})
target_compile_definitions(SandboxStandalone PRIVATE FBZZ_STANDALONE_TARGET)

# @note DPI 認識をプロセス起動時に確定させる (配布 exe も Editor と同じ挙動にする)。
fbzz_apply_app_manifest(SandboxStandalone)
fbzz_enable_agility_sdk(SandboxStandalone)

set_target_properties(SandboxStandalone PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Sandbox"
)

# @note シェーダーターゲットは毎回軽量な差分走査を行い、影響を受けたCSOだけを更新する。
# @note ファイル削除・改名はタイムスタンプ型stampでは検知できないため、収集も差分判定も統合スクリプトへ委ねる。
set(SHADER_SCRIPT "${CMAKE_SOURCE_DIR}/Assets/shaders/compile_shaders.ps1")

file(GLOB_RECURSE HLSL_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Assets/shaders/*.hlsl"
    "${CMAKE_SOURCE_DIR}/Assets/shaders/*.hlsli"
)

# @note Visual Studio の FXCompile による main 固定の二重コンパイルを防ぎ、
# @note VSMain / PSMain / CSMain の処理を compile_shaders.ps1 だけに集約する。
set_source_files_properties(${HLSL_FILES} PROPERTIES
    HEADER_FILE_ONLY TRUE
    VS_TOOL_OVERRIDE "None"
)

find_program(FBZZ_POWERSHELL_EXECUTABLE NAMES powershell.exe powershell REQUIRED)

add_custom_target(CompileShaders
    COMMAND "${FBZZ_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass -File "${SHADER_SCRIPT}" -CompilerPath "${FBZZ_DXC_EXECUTABLE}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Compiling HLSL shaders..."
    SOURCES ${HLSL_FILES} "${SHADER_SCRIPT}"
    VERBATIM
)
if(FBZZ_ENABLE_DX12)
    add_dependencies(Sandbox CompileShaders)
    add_dependencies(SandboxStandalone CompileShaders)
endif()

target_include_directories(Sandbox PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/Assets
)
target_link_libraries(Sandbox
    PRIVATE
        FBZZEditor
        FBZZEngine
        FBZZPhysics
        FBZZFluid
        TomlPlusPlus
        user32
        dwmapi
        shell32
)

target_include_directories(SandboxStandalone PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/Assets
)
target_link_libraries(SandboxStandalone
    PRIVATE
        FBZZEngine
        FBZZPhysics
        FBZZFluid
        TomlPlusPlus
        user32
        dwmapi
        shell32
)

add_custom_command(TARGET Sandbox POST_BUILD
    # @note コア DLL を Sandbox/ に配置
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZMath>    "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZPhysics> "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZFluid>   "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZEngine>  "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZCore>  "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZGraphics>  "$<TARGET_FILE_DIR:Sandbox>"
# @note FBZZEngine は共有ライブラリ版 ImGui を参照するため、Sandbox 単体起動にも DLL が必要。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ImGui> "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:Sandbox>/Assets"
    COMMAND ${CMAKE_COMMAND}
        -D "SOURCE_DIR=${SANDBOX_PROJECT_TEMPLATE_DIR}"
        -D "DEST_DIR=${SANDBOX_DEFAULT_PROJECT_DIR}"
        -D "BUILD_DIR=${CMAKE_BINARY_DIR}"
        -D "MANAGED_STARTER_DIR=${SANDBOX_MANAGED_STARTER_DIR}"
        -D "ENGINE_ASSET_DIR=${CMAKE_SOURCE_DIR}/Assets"
        -P "${SANDBOX_ENSURE_PROJECT_SCRIPT}"
)

add_custom_command(TARGET SandboxStandalone POST_BUILD
    # @note コア DLL を Sandbox/ に配置 (Sandbox.exe と共有ディレクトリなので 1 コピーで済む)
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZMath>    "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZPhysics> "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZFluid>   "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZEngine>  "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZCore>  "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZGraphics>  "$<TARGET_FILE_DIR:SandboxStandalone>"
# @note Standalone のみをビルドした場合も、FBZZEngine が参照する ImGui DLL を同じ階層へ配置する。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ImGui> "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:SandboxStandalone>/Assets"
)

# @note dxcompiler.dll は LoadLibraryW で exe 隣から解決される。Engine の POST_BUILD が置くのは
# @note Binaries/<Config>/ 直下で、Sandbox は 1 段下の Sandbox/ へ出るため届かない。
# @note 既定 renderer の DX12 は DXIL reflection にこれを要求し、無いと全シェーダーのロードが
# @note 失敗して DebugDraw の初期化 assert で落ちる。Editor と同じ規則で exe 隣へ配置する。
if(FBZZ_DXC_RUNTIME_DIRECTORY)
    foreach(FBZZ_SANDBOX_TARGET Sandbox SandboxStandalone)
        foreach(FBZZ_DXC_RUNTIME_DLL dxcompiler.dll dxil.dll)
            if(EXISTS "${FBZZ_DXC_RUNTIME_DIRECTORY}/${FBZZ_DXC_RUNTIME_DLL}")
                add_custom_command(TARGET ${FBZZ_SANDBOX_TARGET} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${FBZZ_DXC_RUNTIME_DIRECTORY}/${FBZZ_DXC_RUNTIME_DLL}"
                        "$<TARGET_FILE_DIR:${FBZZ_SANDBOX_TARGET}>/${FBZZ_DXC_RUNTIME_DLL}"
                    COMMENT "Copying ${FBZZ_DXC_RUNTIME_DLL} for ${FBZZ_SANDBOX_TARGET}"
                )
            endif()
        endforeach()
    endforeach()
endif()

# @note Both local Sandbox executables import Graphics and need its private PIX runtime beside them.
if(FBZZ_ENABLE_DX12)
    foreach(FBZZ_SANDBOX_TARGET Sandbox SandboxStandalone)
        add_custom_command(TARGET ${FBZZ_SANDBOX_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:WinPixEventRuntime>"
                "$<TARGET_FILE_DIR:${FBZZ_SANDBOX_TARGET}>/WinPixEventRuntime.dll"
            VERBATIM
        )
    endforeach()
endif()
