file(GLOB_RECURSE SANDBOX_SOURCES
    src/*.cpp
    src/*.hpp
)

add_executable(sandbox ${SANDBOX_SOURCES})

set(SANDBOX_DEFAULT_PROJECT_DIR "$<TARGET_FILE_DIR:sandbox>/SandboxProject")
set(SANDBOX_PROJECT_TEMPLATE_DIR "${CMAKE_SOURCE_DIR}/Projects/GameHub/Templates/standard")
set(SANDBOX_ENSURE_PROJECT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/EnsureSandboxProject.cmake")

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

target_link_libraries(sandbox
    PRIVATE
        fbzz_editor
        fbzz_engine
        fbzz_physics
        tomlplusplus
        user32
        d3d11
        dxgi
        dwmapi
        shell32
)

add_custom_command(TARGET sandbox POST_BUILD
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
