// FBZZ Engine
// PrefabOverrides.hpp | fbzz::editor
// プレファブインスタンスの「プロパティ単位の差分 (override)」の算出・保持・適用
//
// WHY: これまでプレファブは「インスタンス化した瞬間の複製」でしかなく、
//      インスタンス側で何を変えたかを system が把握していなかった。そのため
//        - Inspector で「このインスタンスのどこが元と違うか」が分からない
//        - Apply でアセットを更新すると、他インスタンスの個別調整が巻き戻る
//      という 2 つの問題があった。
//
//      インスタンスとプレファブの対応は GameObject::prefabSourceId が持つ。
//      差分は「シリアライズ済み TOML の値比較」で取る。ランタイム構造体を型ごとに
//      比較するのではなくシリアライズ表現で比べるのは、
//        1. 保存されない一時状態 (GPU ハンドル等) を自動的に無視できる
//        2. コンポーネントが増えても差分ロジックを書き足す必要がない
//      ため。Reflect() を持たない手書きシリアライズのコンポーネントにも等しく効く。
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <toml++/toml.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

// 差分 1 件。path は GO テーブル内のドット区切りパス (例 "transform.position",
// "LightComponent.intensity")。
struct PrefabOverride {
    std::string prefabSourceId;   // プレファブ側 GO の instanceId (対応キー)
    std::string instanceGuid;     // インスタンス側 GO の instanceId
    std::string objectName;       // 表示用 (インスタンス側の名前)
    std::string path;             // 差分のあったプロパティパス
    std::string prefabValue;      // 表示用の元値
    std::string instanceValue;    // 表示用の現在値
};

// 1 インスタンス階層ぶんの差分と、パッチ適用に必要な現在値のスナップショット。
struct PrefabOverrideSet {
    std::string prefabAssetPath;             // Assets 起点の相対パス
    std::vector<PrefabOverride> entries;
    // prefabSourceId → そのインスタンス GO の現在のシリアライズ表現。
    // WHY: entries は「どのパスが違うか」しか持たない。実際に値を書き戻すときは
    //      ここから node を引く。再インスタンス化で GO が消えた後でも参照できるよう、
    //      値をコピーで保持する。
    std::unordered_map<std::string, toml::table> instanceTables;

    [[nodiscard]] bool Empty() const { return entries.empty(); }
};

// rootEntity のプレファブインスタンス階層について、アセット定義との差分を算出する。
// rootEntity が prefabAssetPath を持たない、またはアセットを読めない場合は false。
bool ComputePrefabOverrides(scene::Scene& scene,
                            scene::EntityID rootEntity,
                            const std::string& projectRoot,
                            PrefabOverrideSet& out);

// 指定パスの差分だけを除外した override 集合を返す (「この 1 件だけ元に戻す」用)。
[[nodiscard]] PrefabOverrideSet WithoutEntry(const PrefabOverrideSet& set,
                                             const PrefabOverride& removed);

// ── TOML パス操作 (PrefabSerializer のパッチ処理と共有) ──────────────────────

// ドット区切りパスで node を引く。見つからなければ nullptr。
[[nodiscard]] const toml::node* FindNodeAtPath(const toml::table& table, const std::string& path);

// ドット区切りパスへ node を書き込む。途中のテーブルが無い場合は作らず false を返す。
bool SetNodeAtPath(toml::table& table, const std::string& path, const toml::node& value);

// UI 表示用に node を 1 行の文字列へ整形する。
[[nodiscard]] std::string FormatNodeForDisplay(const toml::node* node);

} // namespace fbzz::editor
