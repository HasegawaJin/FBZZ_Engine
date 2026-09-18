/// @file    EditorSerializer.hpp
/// @brief   Scene に付随する Editor 専用メタデータの保存・復元。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#pragma once

#include <string>

namespace fbzz::editor {

struct EditorSceneState;

class EditorSerializer final {
public:
    /// Scene 本体とは別のサイドカーへ Editor 状態を書き出す。
    static bool Save(const EditorSceneState& state, const std::string& scenePath);

    /// サイドカーが存在しない旧 Scene は正常系として扱い、空の状態で開始する。
    static bool Load(EditorSceneState& state, const std::string& scenePath);

    static std::string MetadataPath(const std::string& scenePath);
};

} // namespace fbzz::editor
