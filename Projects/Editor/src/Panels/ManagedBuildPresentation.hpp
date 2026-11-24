/// @file    ManagedBuildPresentation.hpp
/// @brief   ステータスバーとビルド出力で共有する C# ビルド段階の表示名。
/// @author  Hasegawa Jin
/// @date    2026-11-24
#pragma once
#include <Editor/ManagedScriptEditor.hpp>
#include <Editor/Util/Localization.hpp>

namespace fbzz::editor {

inline const char* ManagedBuildStateLabel(ManagedBuildState state)
{
    switch (state) {
    case ManagedBuildState::SCANNING:   return LOCT("Checking sources");
    case ManagedBuildState::QUEUED:     return LOCT("Queued");
    case ManagedBuildState::COMPILING:  return LOCT("Compiling");
    case ManagedBuildState::VALIDATING: return LOCT("Validating");
    case ManagedBuildState::COMPILED:   return LOCT("Compiled");
    case ManagedBuildState::APPLYING:   return LOCT("Applying");
    case ManagedBuildState::APPLIED:    return LOCT("Applied");
    case ManagedBuildState::FAILED:     return LOCT("Failed");
    case ManagedBuildState::DEFERRED:   return LOCT("Deferred");
    case ManagedBuildState::SUPERSEDED: return LOCT("Superseded");
    default:                            return LOCT("Unknown");
    }
}

}
