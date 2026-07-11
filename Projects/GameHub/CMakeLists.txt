file(GLOB_RECURSE HUB_SOURCES
    src/*.cpp
    src/*.hpp
)

set(HUB_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/hub.rc")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/hub.rc.in"
    "${HUB_RESOURCE}"
    @ONLY
)

add_executable(fbzz_hub WIN32
    ${HUB_SOURCES}
    "${HUB_RESOURCE}"
)

set_target_properties(fbzz_hub PROPERTIES
    OUTPUT_NAME "FBZZHub"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG       "${CMAKE_BINARY_DIR}/Binaries/Debug/Hub"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_BINARY_DIR}/Binaries/Release/Hub"
    RUNTIME_OUTPUT_DIRECTORY_DEVELOPMENT "${CMAKE_BINARY_DIR}/Binaries/Development/Hub"
)

target_include_directories(fbzz_hub
    PRIVATE
        src
        "${CMAKE_SOURCE_DIR}/ThirdParty/Stb"
)

target_compile_definitions(fbzz_hub
    PRIVATE
        UNICODE
        _UNICODE
        FBZZ_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(fbzz_hub
    PRIVATE
        fbzz_engine
        imgui
        tomlplusplus
        # WHY: GPU バックエンドは fbzz_engine 内部に閉じたため、GameHub 自身は d3d11 / dxgi を
        #      直接リンクしない (DX11 具象への依存を排除)。dwmapi はダークタイトルバー、
        #      shell32 はプロジェクトフォルダを開くために引き続き必要。
        dwmapi
        shell32
)

# WHY: GameHub の Open は FBZZEditor.exe を必須とするため、Hub ターゲットだけを
#      ビルドした場合にも EditorLauncher を同じ出力ディレクトリへ生成する。
add_dependencies(fbzz_hub fbzz_editor_launcher)

add_custom_command(TARGET fbzz_hub POST_BUILD
    # コア DLL を Hub/ に配置
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_math>    "$<TARGET_FILE_DIR:fbzz_hub>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_physics> "$<TARGET_FILE_DIR:fbzz_hub>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fbzz_engine>  "$<TARGET_FILE_DIR:fbzz_hub>"
    # WHY: fbzz_engine は ImGui / Assimp を DLL として参照するため、Hub 単体起動時も
    #      exe と同じディレクトリに配置して Windows の DLL 検索で見つかるようにする。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:imgui> "$<TARGET_FILE_DIR:fbzz_hub>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${CMAKE_SOURCE_DIR}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:fbzz_hub>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Templates"
        "$<TARGET_FILE_DIR:fbzz_hub>/Templates"
)
