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
target_link_libraries(SandboxScripts PRIVATE fbzz_engine fbzz_physics)
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
    # WHY: Ninja はプロセスレベルで並列化するが、MSBuild (Visual Studio の手動ビルド) では
    #      /MP で cl.exe 内部の並列コンパイルを有効化する。Ninja 使用時は /MP 不要だが無害。
    target_compile_options(SandboxScripts PRIVATE /MP)
endif()

add_executable(sandbox ${SANDBOX_SOURCES})
add_dependencies(sandbox SandboxScripts)

set_target_properties(sandbox PROPERTIES
    # WHY: CMake の内部ターゲット名は既存依存との互換性を保ち、公開成果物だけ PascalCase に統一する。
    OUTPUT_NAME "Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Sandbox"
)

# WHY: post-build コマンドや sync_sandbox_assets ターゲットで参照するため、
#      add_executable(sandbox) の直後に定義する。
set(SANDBOX_DEFAULT_PROJECT_DIR "$<TARGET_FILE_DIR:sandbox>/SandboxProject")
set(SANDBOX_PROJECT_TEMPLATE_DIR "${CMAKE_SOURCE_DIR}/Projects/GameHub/Templates/standard")
set(SANDBOX_ENSURE_PROJECT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/EnsureSandboxProject.cmake")

# エンジン全アセットを sandbox 実行ディレクトリと SandboxProject に常時同期するターゲット。
# WHY: post-build は Sandbox.exe が再ビルドされたときのみ実行される。
#      Scripts/ などのアセットを追加・変更したときも確実に反映されるよう、
#      ALL キーワードで毎 cmake --build 実行時に走るターゲットを分離する。
#      rm -rf を行わず copy_directory だけで上書き同期するため、既存ファイルは保持される。
add_custom_target(sync_sandbox_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:sandbox>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "${SANDBOX_DEFAULT_PROJECT_DIR}/Assets"
    COMMENT "Syncing engine Assets to SandboxProject..."
    VERBATIM
)
# WHY: SandboxProject ディレクトリは sandbox の post-build で初回作成されるため、
#      sync_sandbox_assets はその後に実行する必要がある。
add_dependencies(sync_sandbox_assets sandbox)

set(SANDBOX_STANDALONE_SOURCES ${SANDBOX_SOURCES})

# RuntimeBuild 用の配布 exe。FBZZ_STANDALONE_TARGET ではEditorAppをコンパイルしないため、
# Build Settings からゲーム本体だけを再コンパイルできる。
add_executable(SandboxStandalone ${SANDBOX_STANDALONE_SOURCES})
target_compile_definitions(SandboxStandalone PRIVATE FBZZ_STANDALONE_TARGET)

set_target_properties(SandboxStandalone PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Sandbox"
)

# シェーダーを HLSL ファイルが更新されるたびに自動コンパイルする
set(SHADER_SCRIPT    "${CMAKE_SOURCE_DIR}/Assets/shaders/compile_shaders.bat")
set(SHADER_UI_SCRIPT "${CMAKE_SOURCE_DIR}/Assets/shaders/compile_ui_shaders.bat")
set(SHADER_STAMP     "${CMAKE_BINARY_DIR}/shaders_compiled.stamp")

file(GLOB_RECURSE HLSL_FILES "${CMAKE_SOURCE_DIR}/Assets/shaders/*.hlsl")

find_program(FBZZ_FXC_EXECUTABLE
    NAMES fxc.exe
    PATHS
        "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
    DOC "Path to fxc.exe from the Windows SDK"
)

if(FBZZ_FXC_EXECUTABLE)
    set(FBZZ_FXC_ENV "FBZZ_FXC=${FBZZ_FXC_EXECUTABLE}")
else()
    set(FBZZ_FXC_ENV "FBZZ_FXC=")
endif()

add_custom_command(
    OUTPUT  ${SHADER_STAMP}
    COMMAND ${CMAKE_COMMAND} -E env "${FBZZ_FXC_ENV}" cmd /c "${SHADER_SCRIPT}"
    COMMAND ${CMAKE_COMMAND} -E env "${FBZZ_FXC_ENV}" cmd /c "${SHADER_UI_SCRIPT}"
    COMMAND ${CMAKE_COMMAND} -E touch ${SHADER_STAMP}
    DEPENDS ${HLSL_FILES} "${SHADER_SCRIPT}" "${SHADER_UI_SCRIPT}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Compiling HLSL shaders..."
)

add_custom_target(compile_shaders DEPENDS ${SHADER_STAMP})
add_dependencies(sandbox compile_shaders)
add_dependencies(SandboxStandalone compile_shaders)

target_include_directories(sandbox PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/Assets
)
target_link_libraries(sandbox
    PRIVATE
        fbzz_editor
        fbzz_engine
        fbzz_physics
        tomlplusplus
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
        fbzz_engine
        fbzz_physics
        tomlplusplus
        user32
        dwmapi
        shell32
)

add_custom_command(TARGET sandbox POST_BUILD
    # コア DLL を Sandbox/ に配置
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_math>    "$<TARGET_FILE_DIR:sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_physics> "$<TARGET_FILE_DIR:sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_engine>  "$<TARGET_FILE_DIR:sandbox>"
    # WHY: fbzz_engine は共有ライブラリ版 ImGui を参照するため、Sandbox 単体起動にも DLL が必要。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:imgui> "$<TARGET_FILE_DIR:sandbox>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:sandbox>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_FILE_DIR:sandbox>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:sandbox>/Assets"
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
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_math>    "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_physics> "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_engine>  "$<TARGET_FILE_DIR:SandboxStandalone>"
    # WHY: Standalone のみをビルドした場合も、fbzz_engine が参照する ImGui DLL を同じ階層へ配置する。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:imgui> "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:SandboxStandalone>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_FILE_DIR:SandboxStandalone>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:SandboxStandalone>/Assets"
)
