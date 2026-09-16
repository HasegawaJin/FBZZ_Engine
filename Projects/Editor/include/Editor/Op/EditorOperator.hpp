/// @file    EditorOperator.hpp
/// @brief   WHAT: エディターの「操作」を第一級オブジェクトとして表す型と、その登録簿。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY:  同じ操作がメニュー・ホットキー・コマンドパレット・AI バスの 4 箇所へ
/// 独立して書かれており、特に「実行可能条件」が面ごとに別々の式になっていた。
/// 実際に New Scene は、メニューだけが Prefab 編集中を禁じ、Ctrl+N と
/// コマンドパレットからは素通りする状態になっていた (書き忘れではなく、
/// 条件を 3 箇所へ書く構造そのものの帰結で、面を増やすたび再発する)。
/// 操作の実体・条件・表示名・引数を 1 箇所へ集め、4 つの面をすべて
/// 「登録簿を読んで描く / 呼ぶ」だけの投影にする。
/// 設計の全体像と移行段階は Docs/design/editor-operator-model.md を参照。
#pragma once

#include <Editor/Op/OpData.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Math/Vector3.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fbzz::editor {

struct EditorContext;

// ── 引数 ────────────────────────────────────────────────────────────────────
// WHY: Step 1 で登録する操作 (New Scene / Undo / Play 等) はすべて引数を持たないが、
//      型だけは最初から用意しておく。後から引数の概念を足すと、既に 4 面へ配線した
//      呼び出し規約を全部書き直すことになるため。
enum class OpParamType {
    Bool,
    Int,
    Float,
    String,
    Vec3,
    // GameObject.instanceId (UUID 文字列)。EntityID ではなく instanceId を使うのは、
    // リネームや再ロードを跨いでも同じ対象を指せる安定 ID がこちらだから。
    NodeId,
};

using OpValue = std::variant<std::monostate, bool, int, float, std::string, math::Vector3>;

struct OpParam {
    std::string name;
    OpParamType type = OpParamType::String;
    std::string desc;
    bool        required = true;
    OpValue     defaultValue;   // required == false のときに使う既定値

    // 取りうる値がこれだけに限られる文字列引数の宣言 (空なら制約なし)。
    // WHY: これが無いと、legal な値は desc の文章にしか書けない。文章は
    //      機械可読ではないので AI は綴りを推測するしかなく、外した場合の
    //      弾き方も操作ごとの手書きになる (実際 render.set_view_mode は
    //      exec の中で 4 つの文字列と比較し、独自のエラー文を返していた)。
    //      宣言にすれば op.list が候補をそのまま返し、検問はレジストリ 1 箇所で済む。
    std::vector<std::string> enumValues;

    // 数値引数の許容範囲 (hasRange == false なら制約なし)。
    // WHY: 範囲外を各 exec が個別にクランプすると、「受理されたのに指定値と違う」
    //      という黙った食い違いになる。宣言しておけば入口で BAD_ARG として弾ける。
    bool  hasRange = false;
    float minValue = 0.0f;
    float maxValue = 0.0f;
};

// 実行時に渡される引数の入れ物。
// WHY: JSON へ直接依存させない。AI バス側 (Editor/Ai/Json.hpp) は Op を知っていてよいが、
//      Op が AI 層を知ると、AI を外した構成でエディターが組めなくなる。
class OpArgs {
public:
    void Set(std::string name, OpValue value) { m_values[std::move(name)] = std::move(value); }
    [[nodiscard]] bool Has(std::string_view name) const;

    // 見つからない / 型が違う場合は fallback を返す (呼び出し側で毎回 has 判定を書かせない)。
    [[nodiscard]] bool          GetBool  (std::string_view name, bool fallback = false) const;
    [[nodiscard]] int           GetInt   (std::string_view name, int fallback = 0) const;
    [[nodiscard]] float         GetFloat (std::string_view name, float fallback = 0.0f) const;
    [[nodiscard]] std::string   GetString(std::string_view name, std::string fallback = {}) const;
    [[nodiscard]] math::Vector3 GetVec3  (std::string_view name, math::Vector3 fallback = {}) const;

    [[nodiscard]] bool Empty() const { return m_values.empty(); }

private:
    [[nodiscard]] const OpValue* Find(std::string_view name) const;

    std::unordered_map<std::string, OpValue> m_values;
};

// ── 操作の分類 ──────────────────────────────────────────────────────────────
// WHY: Action と Mutation を分けるのは、Save や Play を Undo 履歴へ載せると
//      「Undo でファイル保存が巻き戻る」という事故になるため。現行の AI バスが
//      scene_open / scene_save / build_run を個別の但し書きで Undo 対象外に
//      していたのと同じ線引きを、宣言として型に持たせる。
enum class OpKind {
    Query,      // 状態を読むだけ。AI の read 権限で許可される
    Action,     // エディター UI 状態やファイル I/O を変える。Undo には載らない
    Mutation,   // シーンの中身を変える。Undo 必須
};

// ── 実行文脈 ────────────────────────────────────────────────────────────────
// WHY: EditorApp そのものは渡さない。渡すと Op 層が EditorApp の全公開面へ依存し、
//      循環と「何でもできる引数」になる。EditorApp 固有の処理が要る操作は、
//      登録時のラムダが this を捕捉する (ホットキー登録が既に取っている形と同じ)。
struct OpContext {
    EditorContext& ctx;
    UndoStack&     undo;
};

struct OpResult {
    bool        ok = true;
    std::string errorCode;   // "NO_SCENE" 等。AI へはこの文字列がそのまま渡る
    std::string message;

    // 非 null ならレジストリが UndoStack へ積む。
    // WHY: Undo の積み忘れを各操作の実装ではなくレジストリ 1 箇所で検問できるようにする。
    //      Docs/design/editor-operator-model.md §3.3
    std::unique_ptr<ICommand> command;

    // 「成功したが何も変わらなかった」。履歴へ残すものが無いのが正常な状態。
    // WHY 必要か: これが無いと、同じ名前でリネームしたような正当な no-op まで
    //      「Mutation なのに Undo を残していない」として検問に引っかかる。
    //      失敗 (ok=false) にして黙らせるのは、呼び出し側から見て嘘になる。
    bool noChange = false;

    // 操作が返す構造化データ。Query はここへ読み取り結果を載せる。
    // WHY: Query 以外も使ってよい。node.create のように「作った対象の id」を
    //      返せないと、AI は直後に scene_find で探し直すことになり、
    //      名前が重複していると別のノードを掴む (呼び出しの結果が
    //      呼び出し自身から判らない、という形の不完全さ)。
    OpData data;

    static OpResult Ok() { return {}; }
    static OpResult Data(OpData value)
    {
        OpResult r;
        r.data = std::move(value);
        return r;
    }
    static OpResult Err(std::string code, std::string msg)
    {
        OpResult r;
        r.ok        = false;
        r.errorCode = std::move(code);
        r.message   = std::move(msg);
        return r;
    }
};

// 実行可能条件。文脈と**引数の両方**を見る。
// WHY 引数も渡すか: 同じ操作でも、対象を引数で指定する呼び出し (AI) と、
//     文脈から暗黙に決まる呼び出し (メニュー・パレット・ホットキー) がある。
//     文脈しか見られないと、選択を必須にすれば引数付きの正当な要求を弾き、
//     シーン有無だけにすればメニュー上で「押せるのに何も起きない」が残る。
//     どちらにも倒せないので、判定する側に両方を見せる。
using OpPoll = std::function<bool(const OpContext&, const OpArgs&)>;
using OpExec = std::function<OpResult(OpContext&, const OpArgs&)>;

// トグル操作の現在状態 (メニューのチェックマークになる値)。null なら状態を持たない操作。
// WHY: 移行前、Debug メニューは `ImGui::MenuItem("Grid", nullptr, &ctx.showGrid)` と
//      書いてフラグを直接指しており、同じフラグを切り替える render.show_grid が
//      operator として別に存在していた。表示と operator が別経路なので、
//      メニューのチェックだけが真を示して operator 側の条件と食い違いうる。
//      さらに AI から見ると、切り替えられるのに**今どちらなのかを読む手段が無い**
//      という非対称が残る (RenderOperators が enabled 省略時に反転する設計に
//      なっているのは、この読み取り不能を回避するための折衷だった)。
//      状態を宣言にすれば、メニューのチェックと op.list の checked が同じ式から出る。
//
// WHY 引数も見るか: 排他選択の操作 (render.set_view_mode の 4 つ) は「今どれが
//     選ばれているか」が引数ごとに変わる。poll と同じ (context, args) にすれば、
//     ラジオ表示のメニューも「その値が現在値か」を同じ宣言から引ける。
using OpCheck = std::function<bool(const OpContext&, const OpArgs&)>;

struct EditorOperator {
    // 安定識別子 ("scene.new")。表示名を変えても変わらない。
    // WHY: キーバインドの保存・AI からの呼び出し・マクロ記録の鍵になる。
    //      HotkeyManager::Rebind が表示名で対象を引いていたため、ラベルを変えると
    //      保存済みリバインドが行方不明になっていた。
    std::string id;
    std::string label;      // UI 表示名 ("New Scene")
    std::string category;   // メニュー / パレットの分類 ("File")
    std::string desc;       // 1 行説明。ツールチップと AI が共有する
    std::string caution;    // 「実行すると失われるもの」。AI の事故防止に使う

    std::vector<OpParam> params;
    OpKind      kind = OpKind::Action;
    // Mutation のとき履歴へ出る名前。実際の文言はコマンド自身が持つので、
    // ここは AI とドキュメントが実行前に「何が履歴へ残るか」を知るための宣言。
    std::string undoLabel;

    OpPoll  poll;      // null なら常に実行可能
    OpCheck checked;   // null なら状態を持たない (トグルではない) 操作
    OpExec  exec;
};

// 宣言された制約 (required / enumValues / range) を検証する。
// WHY 公開するか: AI バスは実行前 (dryRun) にも同じ判定を返す必要がある。
//     dryRun だけ別式にすると「dry-run では通ったのに実行すると BAD_ARG」が起きる。
[[nodiscard]] OpResult ValidateArgs(const EditorOperator& op, const OpArgs& args);

// ── 登録簿 ──────────────────────────────────────────────────────────────────
class OperatorRegistry {
public:
    // 同じ id が既にあれば置き換える。
    // WHY: ホットリロードや再初期化で二重登録されたとき、静かに 2 回実行される方が危険。
    void Register(EditorOperator op);

    [[nodiscard]] const EditorOperator* Find(std::string_view id) const;
    [[nodiscard]] const std::vector<EditorOperator>& All() const { return m_operators; }

    // poll を評価する。id が無い場合は false (存在しない操作は実行できない)。
    // args は poll へそのまま渡す (引数付きの呼び出しと引数なしの呼び出しで
    // 実行可否が変わる操作があるため)。
    [[nodiscard]] bool CanInvoke(std::string_view id, const OpContext& context,
                                 const OpArgs& args = {}) const;

    // 実行する。poll を満たさない場合は exec を呼ばずに拒否する。
    // WHY: 条件判定を exec の先頭へ書くと、UI 側は「押せるのに何も起きない」を作れてしまう。
    //      判定は必ず poll に置き、入口で 1 度だけ評価する。
    OpResult Invoke(std::string_view id, OpContext& context, const OpArgs& args = {});

    void Clear() { m_operators.clear(); }

private:
    std::vector<EditorOperator> m_operators;
};

// ── パネルからの呼び出し口 ──────────────────────────────────────────────────
// WHY: パネルは EditorApp を知らない (EditorContext だけを受け取る)。
//      レジストリと UndoStack はどちらも EditorContext から辿れるので、
//      OpContext の組み立てをここへ 1 つ置けば、どのパネルからでも
//      「メニューやホットキーが呼ぶのと同じ実体」を呼べる。
//      これが無いと、パネルは結局ヘルパー関数を直接叩き続けることになり、
//      poll による実行可否の一元化から外れる。
//
// レジストリまたは UndoStack が未結線なら CanInvokeOperator は false、
// InvokeOperator は NO_REGISTRY を返す (EditorApp::Init の途中でも落ちない)。
[[nodiscard]] bool CanInvokeOperator(EditorContext& ctx, std::string_view id,
                                     const OpArgs& args = {});
OpResult InvokeOperator(EditorContext& ctx, std::string_view id, const OpArgs& args = {});

} // namespace fbzz::editor
