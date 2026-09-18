file(GLOB_RECURSE SANDBOX_SOURCES
    src/*.cpp
    src/*.hpp
)
# SandboxScriptsDll.cpp は DLL 専用エントリポイント。EXE に含めると dllimport/dllexport が衝突する。
list(REMOVE_ITEM SANDBOX_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SandboxScriptsDll.cpp"
)

# スクリプト DLL (ホットリロード用)
# WHY: SandboxScripts.dll だけを再ビルドすることで、エディタを再起動せずに
#      スクリプトの変更を反映できる。DLL は EXE のコールバック経由で
#      ScriptFactory に登録する (dual registry 問題を回避するため)。
# WHY (スクリプトを Assets/ に配置):
#      AssetBrowser からスクリプトを Unity 同様に Assets/ 配下で確認・作成できるよう、
#      ソース置き場を Projects/Sandbox/src/Scripts/ から Assets/ 配下に移動した。
#      CMake の include_directories に Assets/ を追加することで
#      #include "Scripts/Foo.hpp" → Assets/Scripts/Foo.hpp のように Assets 相対で解決する。
file(GLOB_RECURSE SANDBOX_SCRIPT_DLL_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/Assets/*.cpp
    ${CMAKE_SOURCE_DIR}/Assets/*.hpp
    src/SandboxScriptsDll.cpp
)
add_library(SandboxScripts SHARED ${SANDBOX_SCRIPT_DLL_SOURCES})
target_compile_definitions(SandboxScripts PRIVATE SANDBOXSCRIPTS_EXPORTS)
target_include_directories(SandboxScripts PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/Assets
)
target_link_libraries(SandboxScripts PRIVATE FBZZEngine FBZZPhysics FBZZFluid)
# WHY: Script.hpp は全スクリプトが #include する重いヘッダ群を束ねる。
#      ホットリロードのたびに再パースしないよう PCH に焼き込む。
target_precompile_headers(SandboxScripts PRIVATE
    <Engine/Scene/Script.hpp>
    <Engine/Scene/Scene.hpp>
    <functional>
    <memory>
    <string>
    <vector>
)
# Sandbox.exe と同じ Sandbox/ に出力 (ホットリロードで exe 隣接の DLL を読む)
set_target_properties(SandboxScripts PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Sandbox"
)
if(MSVC)
    # WHY: ホットリロード時はエディタを起動したまま SandboxScripts だけを短い間隔で再リンクする。
    #      Visual Studio / mspdbsrv / デバッガが直前の SandboxScripts.pdb を参照していると、
    #      LINK が同じ PDB を上書きできず LNK1201 で失敗する。
    #      スクリプト DLL は _hot コピーをロードして差し替える運用が主目的なので、
    #      Debug 構成ではリンク PDB を生成せず、DLL 本体の再リンク成功を優先する。
    target_link_options(SandboxScripts PRIVATE
        "$<$<CONFIG:Debug>:/DEBUG:NONE>"
    )
    # WHY (/MP をここで付けない): 数値なしの /MP はコア数ぶんの cl.exe を起こすうえ、
    #      target_compile_options はディレクトリ既定より後ろに並ぶためルートの
    #      /MP${FBZZ_BUILD_JOBS} を上書きしてしまう。並列度はルート CMakeLists.txt の
    #      FBZZ_BUILD_JOBS に一元化し、このターゲットもそこから継承する。
endif()

add_executable(Sandbox ${SANDBOX_SOURCES})
add_dependencies(Sandbox SandboxScripts)

# DPI 認識をプロセス起動時に確定させる (未適用だと高 DPI 環境で UI 全体が滲む)。
fbzz_apply_app_manifest(Sandbox)

set_target_properties(Sandbox PROPERTIES
    # WHY: CMake の内部ターゲット名は既存依存との互換性を保ち、公開成果物だけ PascalCase に統一する。
    OUTPUT_NAME "Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Sandbox"
)

# WHY: post-build コマンドや SyncSandboxAssets ターゲットで参照するため、
#      add_executable(Sandbox) の直後に定義する。
set(SANDBOX_DEFAULT_PROJECT_DIR "$<TARGET_FILE_DIR:Sandbox>/SandboxProject")
set(SANDBOX_PROJECT_TEMPLATE_DIR "${CMAKE_SOURCE_DIR}/Projects/GameHub/Templates/standard")
set(SANDBOX_ENSURE_PROJECT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/EnsureSandboxProject.cmake")

# エンジン全アセットを Sandbox 実行ディレクトリと SandboxProject に常時同期するターゲット。
# WHY: post-build は Sandbox.exe が再ビルドされたときのみ実行される。
#      Scripts/ などのアセットを追加・変更したときも確実に反映されるよう、
#      ALL キーワードで毎 cmake --build 実行時に走るターゲットを分離する。
#      rm -rf を行わず copy_directory だけで上書き同期するため、既存ファイルは保持される。
add_custom_target(SyncSandboxAssets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:Sandbox>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "${SANDBOX_DEFAULT_PROJECT_DIR}/Assets"
    COMMENT "Syncing engine Assets to SandboxProject..."
    VERBATIM
)
# WHY: SandboxProject ディレクトリは sandbox の post-build で初回作成されるため、
#      SyncSandboxAssets はその後に実行する必要がある。
add_dependencies(SyncSandboxAssets Sandbox)

set(SANDBOX_STANDALONE_SOURCES ${SANDBOX_SOURCES})

# RuntimeBuild 用の配布 exe。FBZZ_STANDALONE_TARGET ではEditorAppをコンパイルしないため、
# Build Settings からゲーム本体だけを再コンパイルできる。
add_executable(SandboxStandalone ${SANDBOX_STANDALONE_SOURCES})
target_compile_definitions(SandboxStandalone PRIVATE FBZZ_STANDALONE_TARGET)

# DPI 認識をプロセス起動時に確定させる (配布 exe も Editor と同じ挙動にする)。
fbzz_apply_app_manifest(SandboxStandalone)

set_target_properties(SandboxStandalone PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Sandbox"
)

# シェーダーターゲットは毎回軽量な差分走査を行い、影響を受けたCSOだけを更新する。
# WHY: ファイル削除・改名はタイムスタンプ型stampでは検知できないため、収集も差分判定も統合スクリプトへ委ねる。
set(SHADER_SCRIPT "${CMAKE_SOURCE_DIR}/Assets/shaders/compile_shaders.ps1")

file(GLOB_RECURSE HLSL_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Assets/shaders/*.hlsl"
    "${CMAKE_SOURCE_DIR}/Assets/shaders/*.hlsli"
)

# WHY: Visual Studio の FXCompile による main 固定の二重コンパイルを防ぎ、
#      VSMain / PSMain / CSMain の処理を compile_shaders.ps1 だけに集約する。
set_source_files_properties(${HLSL_FILES} PROPERTIES
    HEADER_FILE_ONLY TRUE
    VS_TOOL_OVERRIDE "None"
)

find_program(FBZZ_POWERSHELL_EXECUTABLE NAMES powershell.exe powershell REQUIRED)

add_custom_target(CompileShaders
    COMMAND "${FBZZ_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass -File "${SHADER_SCRIPT}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Compiling HLSL shaders..."
    SOURCES ${HLSL_FILES} "${SHADER_SCRIPT}"
    VERBATIM
)
add_dependencies(Sandbox CompileShaders)
add_dependencies(SandboxStandalone CompileShaders)

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
    # コア DLL を Sandbox/ に配置
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZMath>    "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZPhysics> "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZFluid>   "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZEngine>  "$<TARGET_FILE_DIR:Sandbox>"
# WHY: FBZZEngine は共有ライブラリ版 ImGui を参照するため、Sandbox 単体起動にも DLL が必要。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ImGui> "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:Sandbox>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_FILE_DIR:Sandbox>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:Sandbox>/Assets"
    COMMAND ${CMAKE_COMMAND}
        -D "SOURCE_DIR=${SANDBOX_PROJECT_TEMPLATE_DIR}"
        -D "DEST_DIR=${SANDBOX_DEFAULT_PROJECT_DIR}"
        -D "BUILD_DIR=${CMAKE_BINARY_DIR}"
        -P "${SANDBOX_ENSURE_PROJECT_SCRIPT}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "${SANDBOX_DEFAULT_PROJECT_DIR}/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${SANDBOX_PROJECT_TEMPLATE_DIR}/Assets"
        "${SANDBOX_DEFAULT_PROJECT_DIR}/Assets"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "${SANDBOX_DEFAULT_PROJECT_DIR}/ProjectSettings"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${SANDBOX_PROJECT_TEMPLATE_DIR}/ProjectSettings"
        "${SANDBOX_DEFAULT_PROJECT_DIR}/ProjectSettings"
    # WHY: Editor の AssetBrowser は実行時プロジェクトの Assets を表示する。
    #      Shaders / Models などを個別列挙すると Fonts / Icons のような新カテゴリが漏れるため、
    #      テンプレート固有 Assets にエンジン共通 Assets 全体をマージして同期する。
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "${SANDBOX_DEFAULT_PROJECT_DIR}/Assets"
)

add_custom_command(TARGET SandboxStandalone POST_BUILD
    # コア DLL を Sandbox/ に配置 (Sandbox.exe と共有ディレクトリなので 1 コピーで済む)
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZMath>    "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZPhysics> "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZFluid>   "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZEngine>  "$<TARGET_FILE_DIR:SandboxStandalone>"
# WHY: Standalone のみをビルドした場合も、FBZZEngine が参照する ImGui DLL を同じ階層へ配置する。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ImGui> "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_FILE_DIR:SandboxStandalone>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:SandboxStandalone>/Assets"
)

# dxcompiler.dll は LoadLibraryW で exe 隣から解決される。Engine の POST_BUILD が置くのは
# Binaries/<Config>/ 直下で、Sandbox は 1 段下の Sandbox/ へ出るため届かない。
# 既定 renderer の DX12 は DXIL reflection にこれを要求し、無いと全シェーダーのロードが
# 失敗して DebugDraw の初期化 assert で落ちる。Editor と同じ規則で exe 隣へ配置する。
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
