# @file    CMakeLists.txt
# @brief   描画と Engine が共有する上位依存のない基盤 DLL。
# @author  Hasegawa Jin
# @date    2026-09-21
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp include/*.hpp)
add_library(FBZZCore SHARED ${CORE_SOURCES})
set_target_properties(FBZZCore PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON EXPORT_NAME Core)
target_include_directories(FBZZCore PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>")
fbzz_use_std_pch(FBZZCore)
