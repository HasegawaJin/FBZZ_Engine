/// @file    SceneIO.hpp
/// @brief   シーンを TOML 形式でシリアライズ/デシリアライズ。
/// @author  Hasegawa Jin
/// @date    2026-06-06
#pragma once
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fbzz::scene { class Scene; struct EntityID; }

namespace fbzz::editor {

struct EditorSceneState;

struct SceneSaveResult {
    bool succeeded = false;
    bool diskChanged = false;
    std::size_t contentHash = 0;
    std::filesystem::file_time_type writeTime{};
};

class SceneIO {
public:
    static bool Save(const scene::Scene& scene, const std::string& path);
    /// @brief Scene と Editor メタデータの保存値を確定する。返した処理は一度だけ実行できる。
    /// @note バックアップは外部 Terrain を埋め込み、元のアセットを更新しない。
    static std::function<SceneSaveResult()> PrepareSave(scene::Scene& scene,
        const std::string& path, bool backup,
        std::optional<std::filesystem::file_time_type> expectedWriteTime = std::nullopt);
    /// @note Scene と Editor メタデータの準備が成功してから両方を置き換える。
    static bool Load(scene::Scene& scene, const std::string& path, void (*pump)() = nullptr);

    /// @brief PlayMode スナップショット用 (メモリ上の TOML 文字列)。
    static std::string Serialize(const scene::Scene& scene);
    static bool        Deserialize(scene::Scene& scene, const std::string& toml);

    /// @brief 既存シーンに TOML テキストの GameObject を追記する。
    static bool AppendObjects(scene::Scene& scene, const std::string& toml,
                              std::vector<scene::EntityID>& outRoots);

    /// @brief スナップショット用作業ディレクトリを設定する (OpenProject で呼ぶ)。
    static void SetProjectRoot(const std::string& projectRoot);

    /// @brief Editor 専用サイドカーを保存・復元する状態を接続する。SceneIO は非所有で参照する。
    static void SetEditorSceneState(EditorSceneState* state);
};

} /// @note namespace fbzz::editor
