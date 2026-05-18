file(GLOB_RECURSE ENGINE_SOURCES
    src/*.cpp
    src/*.hpp
    include/*.hpp
)

add_library(fbzz_engine STATIC ${ENGINE_SOURCES})

set(DIRECTXTEX_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/DirectXTex/include")
set(DIRECTXTEX_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/DirectXTex/lib/x64/Debug/DirectXTex.lib")
set(DIRECTXTEX_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/DirectXTex/lib/x64/Release/DirectXTex.lib")

target_include_directories(fbzz_engine
    PUBLIC  include
    PRIVATE src
    PRIVATE ${DIRECTXTEX_INCLUDE}
)

target_link_libraries(fbzz_engine
    PUBLIC
        fbzz_physics
        fbzz_math
    PRIVATE
        "$<$<CONFIG:Debug>:${DIRECTXTEX_LIB_DEBUG}>"
        "$<$<CONFIG:Release>:${DIRECTXTEX_LIB_RELEASE}>"
)
