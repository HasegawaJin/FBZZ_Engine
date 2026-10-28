/// @file    PrefabSerializer.hpp
/// @brief   Prefab の保存、個別差分を保つ更新、安全な差し替え。
/// @author  Hasegawa Jin
/// @date    2026-05-26
#pragma once
#include <Editor/Util/PrefabOverrides.hpp>
#include <Engine/Scene/Entity.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

[[nodiscard]] inline bool IsInstantiableAssetExtension(std::string_view ext)
{
    return ext == ".prefab" || ext == ".vfx";
}

/// @see Docs/design/prefab-safety.md
class PrefabSerializer {
public:
    /// @note 出所リンクを含めず、内部参照をアセットのソース ID へ戻して保存する。
    /// @return Water/SplineのPOINT_OBJECTSまたは明示Cloth接続が保存範囲外/欠損なら、ファイルを変更せずfalse。
    static bool SaveSelection(const scene::Scene& scene,
                              const std::vector<scene::EntityID>& selectedEntities,
                              const std::string& path);

    /// @note ルートを GUID 参照へ接続し、各実体のソース ID と基準定義を記録する。
    static bool SaveSelectionAndConnect(scene::Scene& scene,
                                        const std::vector<scene::EntityID>& selectedEntities,
                                        const std::string& path,
                                        std::vector<scene::EntityID>& outRoots);

    static bool Instantiate(scene::Scene& scene,
                            const std::string& path,
                            std::vector<scene::EntityID>& outRootEntities,
                            const PrefabOverrideSet* overrides = nullptr,
                            const std::unordered_map<std::string, std::string>* preserveGuids = nullptr);

    /// @note 単一ルートの出所だけを書き戻す。出所未解決・複数ルートなら変更せず拒否し、成功した場合だけ基準定義を進める。
    static bool Apply(scene::Scene& scene, scene::EntityID rootEntity,
                      const std::string& projectRoot = "");

    /// @return 更新した他インスタンス数。差分採取または保存の失敗は -1。
    /// @note 保存前に差分を採り、未変更の古い値を個別変更として保護しない。
    /// @note In preserve mode, instances with local topology changes are skipped until explicit structure review and Apply.
    static int ApplyAndPropagate(scene::Scene& scene, scene::EntityID rootEntity,
                                 const std::string& projectRoot = "",
                                 bool preserveOverrides = true);

    /// @return 失敗時は旧階層を維持し false。成功時は新ルートを outNewRoots へ返す。
    /// @note Null keepOverrides explicitly discards local changes; local topology changes in keepOverrides are refused without replacing objects.
    static bool Revert(scene::Scene& scene,
                       scene::EntityID rootEntity,
                       std::vector<scene::EntityID>& outNewRoots,
                       const std::string& projectRoot = "",
                       const PrefabOverrideSet* keepOverrides = nullptr);

    /// @note Preserves property overrides; local additions, deletions or parent changes retain the old objects and baseline until explicit structure review and Apply.
    /// @note Incoming template topology updates retain the existing safe replacement behavior when local topology matches the baseline.
    static bool RefreshInstanceKeepingOverrides(scene::Scene& scene,
                                                scene::EntityID rootEntity,
                                                std::vector<scene::EntityID>& outNewRoots,
                                                const std::string& projectRoot);

    /// @return 更新に成功したインスタンス数。
    /// @note Preserve mode skips locally changed topology; structural three-way merge is available through explicit Preview/Apply only.
    static int PropagateToInstances(scene::Scene& scene,
                                    const std::string& prefabAssetPath,
                                    scene::EntityID exceptRoot,
                                    const std::string& projectRoot = "",
                                    bool preserveOverrides = true);

    [[nodiscard]] static int CountInstances(scene::Scene& scene,
                                            const std::string& prefabAssetPath);

    /// @note 自己書き込みの監視イベントによる二重更新を防ぐ。
    [[nodiscard]] static bool WasSelfWrittenRecently(const std::string& diskPath,
                                                     double withinSeconds = 2.0);
};

} /// @note namespace fbzz::editor
