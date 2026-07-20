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

add_executable(fbzz_editor_launcher WIN32
    ${EDITOR_LAUNCHER_SOURCES}
    "${EDITOR_LAUNCHER_RESOURCE}"
)

set_target_properties(fbzz_editor_launcher PROPERTIES
    OUTPUT_NAME "FBZZEditor"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Editor"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Editor"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Editor"
)

target_compile_definitions(fbzz_editor_launcher
    PRIVATE
        UNICODE
        _UNICODE
        TOML_EXCEPTIONS=0
)

target_link_libraries(fbzz_editor_launcher
    PRIVATE
        fbzz_editor
        fbzz_engine
        fbzz_physics
        tomlplusplus
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

add_custom_target(compile_editor_launcher_shaders
    COMMAND "${FBZZ_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass -File "${SHADER_SCRIPT}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Compiling HLSL shaders for FBZZEditor..."
    SOURCES ${EDITOR_LAUNCHER_HLSL_FILES} "${SHADER_SCRIPT}"
    VERBATIM
)
add_dependencies(fbzz_editor_launcher compile_editor_launcher_shaders)

add_custom_command(TARGET fbzz_editor_launcher POST_BUILD
    # コア DLL と ImGui DLL を Editor/ に配置する。
    # WHY: fbzz_editor は imgui を共有 DLL として参照するため、exe 隣にないと起動前に 0xC0000135 で終了する。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_math>    "$<TARGET_FILE_DIR:fbzz_editor_launcher>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_physics> "$<TARGET_FILE_DIR:fbzz_editor_launcher>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_engine>  "$<TARGET_FILE_DIR:fbzz_editor_launcher>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:imgui>        "$<TARGET_FILE_DIR:fbzz_editor_launcher>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:fbzz_editor_launcher>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_FILE_DIR:fbzz_editor_launcher>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:fbzz_editor_launcher>/assets"
)
