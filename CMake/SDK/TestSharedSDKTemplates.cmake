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
    string(FIND "${PROJECT_TEXT}" "sdk_root = \"{{SDK_ROOT}}\"" SDK_PROJECT_POS)
    if(SDK_PRESET_POS EQUAL -1 OR SDK_PROJECT_POS EQUAL -1)
        message(FATAL_ERROR "${TEMPLATE_KIND}: SDK root metadata is missing")
    endif()
endforeach()
