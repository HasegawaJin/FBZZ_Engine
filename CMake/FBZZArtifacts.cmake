# 生成物レイアウトの正本と、そこから外れた残骸の掃除。
#
#   Binaries/<Config>/          共有 DLL だけ (FBZZMath / FBZZPhysics / FBZZFluid / FBZZEngine / ImGui / assimp / dxc)
#   Binaries/<Config>/Editor/   FBZZEditor.exe        (ターゲット名は FBZZEditorLauncher)
#   Binaries/<Config>/Sandbox/  Sandbox.exe / SandboxStandalone.exe / SandboxScripts.dll
#   Binaries/<Config>/Tests/    FBZZTests*.exe / FBZZTestBench.exe
#
# exe は必ずどれかのサブディレクトリに出る。ルート直下の exe は、出力先を分ける前の
# ビルドが置いていった死骸で、名前だけ現行と同じで中身が古い。
# CMake はターゲットの出力先を変えても、ターゲットを消しても、既に置いた成果物を消さない。

# 退役したターゲットの名前。サブディレクトリからも掃除する。
set(FBZZ_RETIRED_RUNTIME_TARGETS
    FBZZVFXEditor
)

# 残骸を configure 時に消す。
# WHY ビルド時ではなく configure 時か: 掃除の判断材料は「今どのターゲットがどこへ出るか」で、
#     それが決まるのは configure。ビルド時に走らせると、掃除と生成の順序が構成次第になる。
function(fbzz_prune_stale_runtime_artifacts)
    set(binariesRoot "${CMAKE_BINARY_DIR}/Binaries")
    if(NOT IS_DIRECTORY "${binariesRoot}")
        return()
    endif()

    set(removed "")

    set(configs ${CMAKE_CONFIGURATION_TYPES})
    if(NOT configs)
        set(configs ${CMAKE_BUILD_TYPE})
    endif()

    foreach(config IN LISTS configs)
        set(configRoot "${binariesRoot}/${config}")
        if(NOT IS_DIRECTORY "${configRoot}")
            continue()
        endif()

        # ルート直下。DLL と、その DLL に対応する pdb だけを残す。
        file(GLOB rootEntries "${configRoot}/*")
        foreach(entry IN LISTS rootEntries)
            if(IS_DIRECTORY "${entry}")
                continue()
            endif()

            get_filename_component(extension "${entry}" LAST_EXT)
            get_filename_component(stem      "${entry}" NAME_WLE)
            string(TOLOWER "${extension}" extension)

            if(extension STREQUAL ".exe" OR extension STREQUAL ".ilk")
                list(APPEND removed "${entry}")
            elseif(extension STREQUAL ".pdb" AND NOT EXISTS "${configRoot}/${stem}.dll")
                # DLL が居ない pdb は、消えた exe の置き土産。
                list(APPEND removed "${entry}")
            endif()
        endforeach()

        # 退役ターゲットの成果物。サブディレクトリ 1 段だけ見れば足りる。
        foreach(retired IN LISTS FBZZ_RETIRED_RUNTIME_TARGETS)
            file(GLOB retiredFiles
                "${configRoot}/${retired}.*"
                "${configRoot}/*/${retired}.*"
            )
            list(APPEND removed ${retiredFiles})
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES removed)
    if(NOT removed)
        return()
    endif()

    foreach(stale IN LISTS removed)
        file(RELATIVE_PATH shown "${CMAKE_BINARY_DIR}" "${stale}")
        message(STATUS "[FBZZ] 古い成果物を削除: ${shown}")
    endforeach()
    file(REMOVE ${removed})
endfunction()
