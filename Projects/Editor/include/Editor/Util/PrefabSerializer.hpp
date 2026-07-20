// FBZZ Engine
// PrefabSerializer.hpp | fbzz::editor
// Saves and instantiates GameObject hierarchies as .prefab assets
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <string>
#include <vector>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

class PrefabSerializer {
public:
    static bool SaveSelection(const scene::Scene& scene,
                              const std::vector<scene::EntityID>& selectedEntities,
                              const std::string& path);

    // SaveSelectionAndConnect: 選択を .prefab に保存し、選択ルートを「そのプレファブの
    // インスタンス」として接続する (Unity の Create Prefab 相当)。
    // WHAT: SaveSelection で書き出したうえで、選択のうち選択された祖先を持たないルート GO の
    //       prefabAssetPath に Assets 起点の相対パスを設定する。接続したルートを outRoots に返す。
    // WHY: 保存するだけではソース GO が通常オブジェクトのままで、青色表示も Apply / Revert も
    //      出ない「作ったのに繋がっていない」状態になる。保存と接続を 1 箇所に集約して、
    //      Hierarchy の "Save As Prefab" と AssetBrowser への D&D で挙動を揃える。
    //      Undo (ファイル削除 + prefabAssetPath クリア) は UI 層が outRoots を使って構築する。
    static bool SaveSelectionAndConnect(scene::Scene& scene,
                                        const std::vector<scene::EntityID>& selectedEntities,
                                        const std::string& path,
                                        std::vector<scene::EntityID>& outRoots);

    static bool Instantiate(scene::Scene& scene,
                            const std::string& path,
                            std::vector<scene::EntityID>& outRootEntities);

    // Apply: インスタンスの現在状態をプレファブアセットに書き戻す。
    // WHY: インスタンスを編集してプレファブに反映する Unity の "Apply to Prefab" に相当する。
    //      go.prefabAssetPath が空の場合は失敗を返す。
    //      projectRoot を渡すことで "Assets/..." 相対パスを絶対パスに解決する。
    //      CWD はエディタ起動時に exe ディレクトリへ変更されるため、相対パスのままでは
    //      プロジェクトの Assets フォルダを指さない。
    static bool Apply(const scene::Scene& scene, scene::EntityID rootEntity,
                      const std::string& projectRoot = "");

    // Revert: インスタンスをプレファブアセットの状態に戻す。
    // WHY: インスタンスへの変更を破棄して元の定義に揃える "Revert" に相当する。
    //      旧 GO 階層を Destroy し、同じ Transform/parent 位置に再インスタンス化する。
    //      outNewRoots に再生成された GO の EntityID が入る。
    //      projectRoot を渡すことで "Assets/..." 相対パスを絶対パスに解決する。
    static bool Revert(scene::Scene& scene,
                       scene::EntityID rootEntity,
                       std::vector<scene::EntityID>& outNewRoots,
                       const std::string& projectRoot = "");
};

} // namespace fbzz::editor
