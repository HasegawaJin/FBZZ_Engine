// FBZZ Engine
// PrefabSerializer.hpp | fbzz::editor
// Saves and instantiates GameObject hierarchies as .prefab assets
#pragma once
#include <Editor/Util/PrefabOverrides.hpp>
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

    // overrides を渡すと、プレファブ定義を展開したあと該当プロパティだけを
    // その値で上書きしてから生成する (インスタンス側の個別調整を保ったまま作り直す用)。
    // WHY: 生成後のライブなコンポーネントへ値を書き戻すのは型ごとの分岐が必要になるが、
    //      「展開直前の TOML を書き換える」なら 1 箇所で全コンポーネントに効く。
    static bool Instantiate(scene::Scene& scene,
                            const std::string& path,
                            std::vector<scene::EntityID>& outRootEntities,
                            const PrefabOverrideSet* overrides = nullptr);

    // Apply: インスタンスの現在状態をプレファブアセットに書き戻す。
    // WHY: インスタンスを編集してプレファブに反映する Unity の "Apply to Prefab" に相当する。
    //      go.prefabAssetPath が空の場合は失敗を返す。
    //      projectRoot を渡すことで "Assets/..." 相対パスを絶対パスに解決する。
    //      CWD はエディタ起動時に exe ディレクトリへ変更されるため、相対パスのままでは
    //      プロジェクトの Assets フォルダを指さない。
    static bool Apply(const scene::Scene& scene, scene::EntityID rootEntity,
                      const std::string& projectRoot = "");

    // ApplyAndPropagate: Apply したうえで、同じ .prefab の他インスタンスも新定義へ揃える。
    //
    // WHY 別関数にするか (不具合修正):
    //   Apply 単体はアセットファイルを書き換えるだけで、既に配置済みの他インスタンスは
    //   古い定義のまま残る。それを避けるため UI 側は Apply の直後に必ず
    //   PropagateToInstances を呼んでいたが、**AI の prefab.apply だけが呼んでいなかった**。
    //   同じ「Apply」なのに、人が押すと 100 個の実体へ反映され、AI が実行すると
    //   ファイルだけ変わって画面は何も変わらない、という食い違いになっていた。
    //   2 つを 1 つの操作として閉じてしまえば、呼び忘れが起きる場所が無くなる。
    //
    // 戻り値: 更新した他インスタンス数。Apply 自体に失敗したときは -1。
    // (0 と -1 を分けるのは「他に実体が無かった」と「書き戻せなかった」が別物のため)
    static int ApplyAndPropagate(scene::Scene& scene, scene::EntityID rootEntity,
                                 const std::string& projectRoot = "",
                                 bool preserveOverrides = true);

    // Revert: インスタンスをプレファブアセットの状態に戻す。
    // WHY: インスタンスへの変更を破棄して元の定義に揃える "Revert" に相当する。
    //      旧 GO 階層を Destroy し、同じ Transform/parent 位置に再インスタンス化する。
    //      outNewRoots に再生成された GO の EntityID が入る。
    //      projectRoot を渡すことで "Assets/..." 相対パスを絶対パスに解決する。
    //
    // keepOverrides を渡すと、アセット定義へ戻したあとその差分だけを復元する。
    // nullptr のときは全ての差分を破棄する (Inspector の "Revert" ボタンの挙動)。
    static bool Revert(scene::Scene& scene,
                       scene::EntityID rootEntity,
                       std::vector<scene::EntityID>& outNewRoots,
                       const std::string& projectRoot = "",
                       const PrefabOverrideSet* keepOverrides = nullptr);

    // インスタンスをアセット定義へ作り直しつつ、現在の差分 (override) を保つ。
    // WHY: プレファブ更新の伝播で使う。個別に調整した位置や色まで巻き戻ると、
    //      「プレファブを直すたびに全インスタンスを手直しする」羽目になる。
    static bool RefreshInstanceKeepingOverrides(scene::Scene& scene,
                                                scene::EntityID rootEntity,
                                                std::vector<scene::EntityID>& outNewRoots,
                                                const std::string& projectRoot);

    // PropagateToInstances: 開いているシーン内の「同じ .prefab のインスタンス」を
    // アセットの現在の定義へ揃える。
    //
    // WHY: Apply はアセットファイルを更新するだけで、既に配置済みの他インスタンスは
    //      古い定義のまま残っていた。プレファブを直しても 100 個置いた実体に反映されない、
    //      という「繋がっていないプレファブ」状態の主因がこれ。
    //      各インスタンスの Transform と親は Revert が保持するため、配置は壊れない。
    //
    // exceptRoot に渡した実体はスキップする (Apply の元になったインスタンスは
    // 既にアセットと同一で、作り直すと選択とフォーカスが飛ぶため)。
    // 戻り値は更新できたインスタンス数。
    // preserveOverrides = true のとき、各インスタンスの個別調整を保ったまま更新する。
    static int PropagateToInstances(scene::Scene& scene,
                                    const std::string& prefabAssetPath,
                                    scene::EntityID exceptRoot,
                                    const std::string& projectRoot = "",
                                    bool preserveOverrides = true);

    // CountInstances: 開いているシーン内の同一プレファブのインスタンス数を返す。
    // WHY: Apply する前に「何個に波及するのか」を UI で見せるため。
    //      Scene& が非 const なのは GameObjects() に const 版が無いため (走査のみで変更はしない)。
    [[nodiscard]] static int CountInstances(scene::Scene& scene,
                                            const std::string& prefabAssetPath);

    // 直近 withinSeconds 以内に、このエディタ自身がその .prefab を書いたか。
    //
    // WHY: .prefab のディスク変更を監視してインスタンスへ自動反映すると、
    //      Apply で自分が書いた変更にも監視が反応し、直後にインスタンスを
    //      作り直す無駄 (と、選択が飛ぶ副作用) が起きる。SaveSelection を通る
    //      書き込みは全てここに記録されるので、呼び出し側の配線なしで弾ける。
    [[nodiscard]] static bool WasSelfWrittenRecently(const std::string& diskPath,
                                                     double withinSeconds = 2.0);
};

} // namespace fbzz::editor
