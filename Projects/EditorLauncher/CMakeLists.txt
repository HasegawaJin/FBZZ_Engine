file(GLOB_RECURSE EDITOR_LAUNCHER_SOURCES
    src/*.cpp
    src/*.hpp
)

set(EDITOR_LAUNCHER_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/editor_launcher.rc")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/editor_launcher.rc.in"
    "${EDITOR_LAUNCHER_RESOURCE}"
    @ONLY
)

add_executable(FBZZEditorLauncher WIN32
    ${EDITOR_LAUNCHER_SOURCES}
    "${EDITOR_LAUNCHER_RESOURCE}"
)

# DPI 認識をプロセス起動時に確定させる (未適用だと高 DPI 環境で UI 全体が滲む)。
fbzz_apply_app_manifest(FBZZEditorLauncher)

set_target_properties(FBZZEditorLauncher PROPERTIES
    OUTPUT_NAME "FBZZEditor"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Editor"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Editor"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Editor"
)

target_compile_definitions(FBZZEditorLauncher
    PRIVATE
        UNICODE
        _UNICODE
        TOML_EXCEPTIONS=0
)

target_link_libraries(FBZZEditorLauncher
    PRIVATE
        FBZZEditor
        FBZZEngine
        FBZZPhysics
        TomlPlusPlus
        user32
        dwmapi
        shell32
)

set(SHADER_SCRIPT "${CMAKE_SOURCE_DIR}/Assets/shaders/compile_shaders.ps1")

file(GLOB_RECURSE EDITOR_LAUNCHER_HLSL_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Assets/shaders/*.hlsl"
    "${CMAKE_SOURCE_DIR}/Assets/shaders/*.hlsli"
)

# WHY: Visual Studio は .hlsl を既定で FXCompile に渡して main を探すが、
#      エントリーポイントとバックエンドの選択は compile_shaders.ps1 が一元管理する。
set_source_files_properties(${EDITOR_LAUNCHER_HLSL_FILES} PROPERTIES
    HEADER_FILE_ONLY TRUE
    VS_TOOL_OVERRIDE "None"
)

find_program(FBZZ_POWERSHELL_EXECUTABLE NAMES powershell.exe powershell REQUIRED)

add_custom_target(CompileEditorLauncherShaders
    COMMAND "${FBZZ_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass -File "${SHADER_SCRIPT}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Compiling HLSL shaders for FBZZEditor..."
    SOURCES ${EDITOR_LAUNCHER_HLSL_FILES} "${SHADER_SCRIPT}"
    VERBATIM
)
add_dependencies(FBZZEditorLauncher CompileEditorLauncherShaders)

add_custom_command(TARGET FBZZEditorLauncher POST_BUILD
    # コア DLL と ImGui DLL を Editor/ に配置する。
# WHY: FBZZEditor は ImGui を共有 DLL として参照するため、exe 隣にないと起動前に 0xC0000135 で終了する。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZMath>    "$<TARGET_FILE_DIR:FBZZEditorLauncher>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZPhysics> "$<TARGET_FILE_DIR:FBZZEditorLauncher>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:FBZZEngine>  "$<TARGET_FILE_DIR:FBZZEditorLauncher>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ImGui>        "$<TARGET_FILE_DIR:FBZZEditorLauncher>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:FBZZEditorLauncher>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_FILE_DIR:FBZZEditorLauncher>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:FBZZEditorLauncher>/assets"
)
