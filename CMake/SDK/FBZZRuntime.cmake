# @file    FBZZRuntime.cmake
# @brief   SDK consumer runtime and shared asset staging.
# @author  Hasegawa Jin
# @date    2026-11-06

function(fbzz_stage_runtime TARGET_NAME)
    if(NOT TARGET "${TARGET_NAME}")
        message(FATAL_ERROR "fbzz_stage_runtime: target '${TARGET_NAME}' does not exist")
    endif()
    add_custom_command(TARGET "${TARGET_NAME}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${FBZZ_SDK_ROOT}/bin/$<CONFIG>"
            "$<TARGET_FILE_DIR:${TARGET_NAME}>"
        COMMENT "Staging FBZZ ${FBZZ_VERSION} runtime for ${TARGET_NAME}"
        VERBATIM)

    # @note A phony prerequisite scans SDK assets even when the executable needs no compilation or link.
    # @see https://cmake.org/cmake/help/v3.25/policy/CMP0112.html TARGET_FILE_DIR does not imply a reverse dependency with CMP0112 NEW.
    add_custom_target("${TARGET_NAME}EngineAssets"
        COMMAND "${CMAKE_COMMAND}"
            "-DSDK_ROOT=${FBZZ_SDK_ROOT}"
            "-DRUNTIME_DIR=$<TARGET_FILE_DIR:${TARGET_NAME}>"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageFBZZEngineAssets.cmake"
        COMMENT "Synchronizing FBZZ ${FBZZ_VERSION} assets for ${TARGET_NAME}"
        VERBATIM)
    add_dependencies("${TARGET_NAME}" "${TARGET_NAME}EngineAssets")
endfunction()
