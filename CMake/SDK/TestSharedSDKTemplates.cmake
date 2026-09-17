# FBZZ Engine
# TestSharedSDKTemplates.cmake | CMake
# 新規/移行後プロジェクトが Engine ソース参照へ退行しないことを検証する

if(NOT DEFINED SOURCE_ROOT OR SOURCE_ROOT STREQUAL "")
    message(FATAL_ERROR "TestSharedSDKTemplates: SOURCE_ROOT is required")
endif()

foreach(TEMPLATE_KIND standard empty)
    set(TEMPLATE_ROOT "${SOURCE_ROOT}/Projects/GameHub/Templates/${TEMPLATE_KIND}")
    file(READ "${TEMPLATE_ROOT}/CMakeLists.txt" CMAKE_TEXT)
    file(READ "${TEMPLATE_ROOT}/CMakePresets.json" PRESET_TEXT)
    file(READ "${TEMPLATE_ROOT}/.fbzz_proj" PROJECT_TEXT)

    foreach(REQUIRED_TEXT "find_package(FBZZ {{ENGINE_VERSION}} EXACT CONFIG REQUIRED)"
                          "FBZZ::Engine" "fbzz_stage_runtime")
        string(FIND "${CMAKE_TEXT}" "${REQUIRED_TEXT}" REQUIRED_POS)
        if(REQUIRED_POS EQUAL -1)
            message(FATAL_ERROR "${TEMPLATE_KIND}: CMakeLists.txt is missing ${REQUIRED_TEXT}")
        endif()
    endforeach()
    foreach(FORBIDDEN_TEXT "FBZZ_ENGINE_ROOT" "add_subdirectory")
        string(FIND "${CMAKE_TEXT}" "${FORBIDDEN_TEXT}" FORBIDDEN_POS)
        if(NOT FORBIDDEN_POS EQUAL -1)
            message(FATAL_ERROR "${TEMPLATE_KIND}: CMakeLists.txt still contains ${FORBIDDEN_TEXT}")
        endif()
    endforeach()
    string(FIND "${PRESET_TEXT}" "FBZZ_SDK_ROOT" SDK_PRESET_POS)
    string(FIND "${PROJECT_TEXT}" "sdk_id = \"{{SDK_ID}}\"" SDK_PROJECT_POS)
    if(SDK_PRESET_POS EQUAL -1 OR SDK_PROJECT_POS EQUAL -1)
        message(FATAL_ERROR "${TEMPLATE_KIND}: SDK selection metadata is missing")
    endif()

    # Engine シェーダーは SDK の share/fbzz/Assets を共有する。テンプレートへ複製すると
    # 新規プロジェクトが SDK と食い違ったコピーを抱え、エンジン側の修正が届かなくなる。
    # 自前のシェーダーを持ちたいプロジェクトは後から Assets/Shaders を丸ごとコピーする (Editor の HLSL ホットリロードはその所有者判定で切り替わる)。
    if(EXISTS "${TEMPLATE_ROOT}/Assets/Shaders")
        message(FATAL_ERROR "${TEMPLATE_KIND}: Assets/Shaders must not exist in a template (engine shaders come from the SDK)")
    endif()

    if(TEMPLATE_KIND STREQUAL "standard")
        # プロジェクトが自前のシェーダーツリーを持ったときに Standalone ビルドへ組み込む経路は残す。
        foreach(REQUIRED_SHADER_TEXT "add_custom_target(CompileShaders"
                                     "Assets/Shaders/*.hlsli"
                                     "add_dependencies({{TARGET_NAME}}Standalone CompileShaders)")
            string(FIND "${CMAKE_TEXT}" "${REQUIRED_SHADER_TEXT}" SHADER_TEXT_POS)
            if(SHADER_TEXT_POS EQUAL -1)
                message(FATAL_ERROR "standard: shader build integration is missing ${REQUIRED_SHADER_TEXT}")
            endif()
        endforeach()
    endif()
endforeach()
