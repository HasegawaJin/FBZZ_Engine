// FBZZ Engine
// SceneIO.hpp | fbzz::editor
// シーンを TOML 形式でシリアライズ/デシリアライズ
#pragma once
#include <string>
#include <vector>

namespace fbzz::scene { class Scene; struct EntityID; }

namespace fbzz::editor {

struct EditorSceneState;

class SceneIO {
public:
    static bool Save(const scene::Scene& scene, const std::string& path);
    static bool Load(scene::Scene& scene, const std::string& path);

    // PlayMode スナップショット用 (メモリ上の TOML 文字列)
    static std::string Serialize(const scene::Scene& scene);
    static bool        Deserialize(scene::Scene& scene, const std::string& toml);

    // 既存シーンに toml テキストの GameObject を追記する (シーン全体を破棄しない)
    static bool AppendObjects(scene::Scene& scene, const std::string& toml,
                              std::vector<scene::EntityID>& outRoots);

    // スナップショット用作業ディレクトリを設定する (OpenProject で呼ぶ)
    static void SetProjectRoot(const std::string& projectRoot);

    // Editor 専用サイドカーを保存・復元する状態を接続する。SceneIO は非所有で参照する。
    static void SetEditorSceneState(EditorSceneState* state);
};

} // namespace fbzz::editor
