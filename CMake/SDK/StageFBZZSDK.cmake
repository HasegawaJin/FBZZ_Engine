# FBZZ Engine
# StageFBZZSDK.cmake | CMake
# install export外の構成別runtimeとEditor toolを共有SDKへ同期する

foreach(REQUIRED SDK_ROOT CONFIG ENGINE_DLL IMGUI_DLL ASSIMP_DLL EDITOR_DIR)
    if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
        message(FATAL_ERROR "StageFBZZSDK: ${REQUIRED} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${SDK_ROOT}/bin/${CONFIG}" "${SDK_ROOT}/tools/${CONFIG}/Editor")

# Assimp は CMake target ではなく install export に含まれないため、runtime を明示配置する。
# ImGui は FBZZ::ImGui の install 処理が bin へ配置するため、ここでは重複コピーしない。
file(COPY "${ASSIMP_DLL}" DESTINATION "${SDK_ROOT}/bin/${CONFIG}")

# DX12の遅延ロードDLLは利用可能な場合だけEngine出力から引き継ぐ。
get_filename_component(ENGINE_BINARY_DIR "${ENGINE_DLL}" DIRECTORY)
foreach(OPTIONAL_DLL dxcompiler.dll dxil.dll)
    if(EXISTS "${ENGINE_BINARY_DIR}/${OPTIONAL_DLL}")
        file(COPY "${ENGINE_BINARY_DIR}/${OPTIONAL_DLL}" DESTINATION "${SDK_ROOT}/bin/${CONFIG}")
    endif()
endforeach()

file(COPY "${EDITOR_DIR}/" DESTINATION "${SDK_ROOT}/tools/${CONFIG}/Editor")
# WHY: Editor出力側のPOST_BUILD状態に依存せず、SDK生成時にも起動必須のimgui.dllをexe隣へ保証する。
file(COPY "${IMGUI_DLL}" DESTINATION "${SDK_ROOT}/tools/${CONFIG}/Editor")

# WHY: DX12 の DXIL reflection は dxcompiler.dll を LoadLibraryW で exe 隣から解決する。
#      bin/ だけに置くとEditorからは見えず、shaderロードが全滅する。imgui.dll と同じく
#      Editor出力側のPOST_BUILD状態に依存せず、SDK生成時にexe隣へ保証する。
foreach(OPTIONAL_DLL dxcompiler.dll dxil.dll)
    if(EXISTS "${ENGINE_BINARY_DIR}/${OPTIONAL_DLL}")
        file(COPY "${ENGINE_BINARY_DIR}/${OPTIONAL_DLL}"
             DESTINATION "${SDK_ROOT}/tools/${CONFIG}/Editor")
    endif()
endforeach()

# 構成別の公開時刻。作業世代は毎回同じIDへ上書きされるため、IDだけでは最新性を判定できない。
# WHY: install/file(COPY)は内容が同じfileをcopyせずmtimeも更新しないので、manifestや
#      binaryの時刻は公開時刻として信用できない。公開のたびに必ず書き換わるstampを置く。
string(TIMESTAMP SDK_PUBLISH_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
file(WRITE "${SDK_ROOT}/bin/${CONFIG}/published.stamp" "${SDK_PUBLISH_TIME}\n")
