/// @file    PrefabSerializer.hpp
/// @brief   Saves and instantiates GameObject hierarchies as .prefab assets.
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

/// ドロップやコマンドでシーンへそのまま実体化できるアセット拡張子か (先頭ドット付き・小文字)。
/// @note 同じ判定が Scene View / Hierarchy の 2 つのドロップ先 / AI の prefab.instantiate と
///       4 箇所に散っていたため 1 箇所へまとめる。散っていると対応拡張子を増やすたびに
///       «Hierarchy には置けるのに Scene View には置けない» 取りこぼしが出る。
/// @note .vfx は .prefab と同じ prefab 形式 (TOML) で、展開経路 (InstantiatePrefabAsset) も
///       拡張子を見ないため含める。
[[nodiscard]] inline bool IsInstantiableAssetExtension(std::string_view ext)
{
    return ext == ".prefab" || ext == ".vfx";
}

class PrefabSerializer {
public:
    static bool SaveSelection(const scene::Scene& scene,
                              const std::vector<scene::EntityID>& selectedEntities,
                              const std::string& path);

    /// 選択を .prefab に保存し、選択ルートをそのプレファブのインスタンスとして接続する
    /// (Unity の Create Prefab 相当)。祖先が選択されていないルート GO の prefabAssetPath に
    /// Assets 起点の相対パスを設定し、接続したルートを outRoots に返す。
    /// @note 保存だけではソース GO が通常オブジェクトのままで Apply/Revert も出ない。保存と
    ///       接続を 1 箇所に集約し、Hierarchy の "Save As Prefab" と AssetBrowser への D&D で
    ///       挙動を揃える。Undo (ファイル削除 + prefabAssetPath クリア) は UI 層が構築する。
    static bool SaveSelectionAndConnect(scene::Scene& scene,
                                        const std::vector<scene::EntityID>& selectedEntities,
                                        const std::string& path,
                                        std::vector<scene::EntityID>& outRoots);

    /// overrides を渡すと、プレファブ定義を展開したあと該当プロパティだけをその値で上書きして
    /// 生成する (インスタンス側の個別調整を保ったまま作り直す用)。
    /// @note 生成後のライブなコンポーネントへ書き戻すのは型ごとの分岐が要るが、「展開直前の
    ///       TOML を書き換える」なら 1 箇所で全コンポーネントに効く。
    /// preserveGuids は「プレファブ側 instanceId → 名乗らせたい instanceId」。既存インスタンスの
    /// 作り直しで元の guid を引き継ぐために Revert が渡す (詳細は Engine/Scene/PrefabInstantiate.hpp)。
    static bool Instantiate(scene::Scene& scene,
                            const std::string& path,
                            std::vector<scene::EntityID>& outRootEntities,
                            const PrefabOverrideSet* overrides = nullptr,
                            const std::unordered_map<std::string, std::string>* preserveGuids = nullptr);

    /// インスタンスの現在状態をプレファブアセットに書き戻す (Unity の "Apply to Prefab" 相当)。
    /// go.prefabAssetPath が空の場合は失敗を返す。
    /// @note projectRoot を渡すと "Assets/..." 相対パスを絶対パスに解決する。CWD はエディタ
    ///       起動時に exe ディレクトリへ変わるため、相対パスのままではプロジェクトの Assets
    ///       フォルダを指さない。
    static bool Apply(const scene::Scene& scene, scene::EntityID rootEntity,
                      const std::string& projectRoot = "");

    /// Apply したうえで、同じ .prefab の他インスタンスも新定義へ揃える。
    /// @note Apply 単体はファイルを書き換えるだけで既存の他インスタンスは古い定義のまま残る。
    ///       呼び忘れが起きないよう Apply と伝播を 1 操作に閉じる。
    /// @return 更新した他インスタンス数。Apply 自体に失敗したときは -1 (「他に実体が無かった」
    ///         0 と区別する)。
    static int ApplyAndPropagate(scene::Scene& scene, scene::EntityID rootEntity,
                                 const std::string& projectRoot = "",
                                 bool preserveOverrides = true);

    /// インスタンスをプレファブアセットの状態に戻す。旧 GO 階層を Destroy し、同じ
    /// Transform/parent 位置に再インスタンス化する。outNewRoots に再生成された GO の
    /// EntityID が入る。projectRoot を渡すと "Assets/..." 相対パスを絶対パスに解決する。
    ///
    /// keepOverrides を渡すと、アセット定義へ戻したあとその差分だけを復元する。
    /// nullptr のときは全ての差分を破棄する (Inspector の "Revert" ボタンの挙動)。
    static bool Revert(scene::Scene& scene,
                       scene::EntityID rootEntity,
                       std::vector<scene::EntityID>& outNewRoots,
                       const std::string& projectRoot = "",
                       const PrefabOverrideSet* keepOverrides = nullptr);

    /// インスタンスをアセット定義へ作り直しつつ、現在の差分 (override) を保つ。
    /// @note プレファブ更新の伝播で使う。個別調整まで巻き戻ると、直すたびに全インスタンスを
    ///       手直しする羽目になる。
    static bool RefreshInstanceKeepingOverrides(scene::Scene& scene,
                                                scene::EntityID rootEntity,
                                                std::vector<scene::EntityID>& outNewRoots,
                                                const std::string& projectRoot);

    /// 開いているシーン内の「同じ .prefab のインスタンス」をアセットの現在の定義へ揃える。
    /// exceptRoot に渡した実体はスキップする (Apply の元になったインスタンスは既にアセットと
    /// 同一で、作り直すと選択とフォーカスが飛ぶため)。preserveOverrides = true のとき、各
    /// インスタンスの個別調整を保ったまま更新する。
    /// @note Apply はファイルを更新するだけなので、これを呼ばないと配置済みの他インスタンスは
    ///       古い定義のまま残る。各インスタンスの Transform と親は Revert が保持するため配置は
    ///       壊れない。
    /// @return 更新できたインスタンス数。
    static int PropagateToInstances(scene::Scene& scene,
                                    const std::string& prefabAssetPath,
                                    scene::EntityID exceptRoot,
                                    const std::string& projectRoot = "",
                                    bool preserveOverrides = true);

    /// 開いているシーン内の同一プレファブのインスタンス数を返す。
    /// @note Apply する前に「何個に波及するのか」を UI で見せるために使う。Scene& が非 const
    ///       なのは GameObjects() に const 版が無いため (走査のみで変更はしない)。
    [[nodiscard]] static int CountInstances(scene::Scene& scene,
                                            const std::string& prefabAssetPath);

    /// 直近 withinSeconds 以内に、このエディタ自身がその .prefab を書いたか。
    /// @note .prefab のディスク変更監視は、Apply で自分が書いた変更にも反応すると直後に
    ///       インスタンスを作り直す無駄と選択が飛ぶ副作用を起こす。SaveSelection を通る
    ///       書き込みは全てここに記録されるので、呼び出し側の配線なしで弾ける。
    [[nodiscard]] static bool WasSelfWrittenRecently(const std::string& diskPath,
                                                     double withinSeconds = 2.0);
};

} // namespace fbzz::editor
