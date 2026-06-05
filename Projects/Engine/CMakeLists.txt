file(GLOB_RECURSE ENGINE_SOURCES
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

add_library(fbzz_engine SHARED ${ENGINE_SOURCES})
set_target_properties(fbzz_engine PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    OUTPUT_NAME "FBZZEngine"
)

set(DIRECTXTEX_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/include")
set(DIRECTXTEX_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Debug/DirectXTex.lib")
set(DIRECTXTEX_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/DirectXTex/lib/x64/Release/DirectXTex.lib")

set(ASSIMP_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/include")
set(ASSIMP_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/lib/Debug/assimp-vc145-mtd.lib")
set(ASSIMP_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Assimp/lib/Release/assimp-vc145-mt.lib")

foreach(REQUIRED_PATH
    "${DIRECTXTEX_INCLUDE}"
    "${DIRECTXTEX_LIB_DEBUG}"
    "${DIRECTXTEX_LIB_RELEASE}"
    "${ASSIMP_INCLUDE}"
    "${ASSIMP_LIB_DEBUG}"
    "${ASSIMP_LIB_RELEASE}"
)
    if(NOT EXISTS "${REQUIRED_PATH}")
        message(FATAL_ERROR "Required third-party dependency was not found: ${REQUIRED_PATH}")
    endif()
endforeach()

target_include_directories(fbzz_engine
    PUBLIC  include
    PRIVATE src
    PRIVATE ${DIRECTXTEX_INCLUDE}
    PRIVATE ${ASSIMP_INCLUDE}
)

target_link_libraries(fbzz_engine
    PUBLIC
        fbzz_physics
        fbzz_math
        tomlplusplus
        xaudio2
        mfplat
        mfreadwrite
        mfuuid
        winmm
    PRIVATE
        imgui
        "$<$<CONFIG:Debug>:${DIRECTXTEX_LIB_DEBUG}>"
        "$<$<CONFIG:Release>:${DIRECTXTEX_LIB_RELEASE}>"
        "$<$<CONFIG:Debug>:${ASSIMP_LIB_DEBUG}>"
        "$<$<CONFIG:Release>:${ASSIMP_LIB_RELEASE}>"
)
