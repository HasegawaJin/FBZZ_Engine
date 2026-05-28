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
        d3d11
        dxgi
        dwmapi
        shell32
)

set(SHADER_SCRIPT    "${CMAKE_SOURCE_DIR}/Assets/shaders/compile_shaders.bat")
set(SHADER_UI_SCRIPT "${CMAKE_SOURCE_DIR}/Assets/shaders/compile_ui_shaders.bat")
set(SHADER_STAMP     "${CMAKE_BINARY_DIR}/editor_launcher_shaders_compiled.stamp")

file(GLOB_RECURSE EDITOR_LAUNCHER_HLSL_FILES "${CMAKE_SOURCE_DIR}/Assets/shaders/*.hlsl")

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
    DEPENDS ${EDITOR_LAUNCHER_HLSL_FILES} "${SHADER_SCRIPT}" "${SHADER_UI_SCRIPT}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Compiling HLSL shaders for FBZZEditor..."
)

add_custom_target(compile_editor_launcher_shaders DEPENDS ${SHADER_STAMP})
add_dependencies(fbzz_editor_launcher compile_editor_launcher_shaders)

add_custom_command(TARGET fbzz_editor_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:fbzz_editor_launcher>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_FILE_DIR:fbzz_editor_launcher>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:fbzz_editor_launcher>/assets"
)
