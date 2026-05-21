file(GLOB_RECURSE SANDBOX_SOURCES
    src/*.cpp
    src/*.hpp
)

add_executable(sandbox ${SANDBOX_SOURCES})

# シェーダーを HLSL ファイルが更新されるたびに自動コンパイルする
set(SHADER_SCRIPT "${CMAKE_SOURCE_DIR}/Assets/shaders/compile_shaders.bat")
set(SHADER_STAMP  "${CMAKE_BINARY_DIR}/shaders_compiled.stamp")

file(GLOB HLSL_FILES "${CMAKE_SOURCE_DIR}/Assets/shaders/*.hlsl")

add_custom_command(
    OUTPUT  ${SHADER_STAMP}
    COMMAND cmd /c "${SHADER_SCRIPT}"
    COMMAND ${CMAKE_COMMAND} -E touch ${SHADER_STAMP}
    DEPENDS ${HLSL_FILES}
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
)

add_custom_command(TARGET sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:sandbox>"
)