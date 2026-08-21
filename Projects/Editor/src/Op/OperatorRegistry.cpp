// FBZZ Engine
// OperatorRegistry.cpp | fbzz::editor
// Editor Operator の登録簿と実行経路
#include <Editor/Op/EditorOperator.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Core/Logger.hpp>

#include <algorithm>
#include <utility>

namespace fbzz::editor {

// ── OpArgs ──────────────────────────────────────────────────────────────────

const OpValue* OpArgs::Find(std::string_view name) const
{
    // WHY: unordered_map<std::string> は string_view で直接引けない (C++20 の
    //      heterogeneous lookup は unordered 系だと Hash/KeyEqual の指定が要る)。
    //      引数は 1 操作あたり数個なので、線形探索で十分速く、型を単純に保てる。
    for (const auto& [key, value] : m_values)
        if (key == name) return &value;
    return nullptr;
}

bool OpArgs::Has(std::string_view name) const
{
    const OpValue* v = Find(name);
    return v != nullptr && !std::holds_alternative<std::monostate>(*v);
}

bool OpArgs::GetBool(std::string_view name, bool fallback) const
{
    const OpValue* v = Find(name);
    if (v == nullptr) return fallback;
    if (const bool* b = std::get_if<bool>(v)) return *b;
    return fallback;
}

int OpArgs::GetInt(std::string_view name, int fallback) const
{
    const OpValue* v = Find(name);
    if (v == nullptr) return fallback;
    if (const int* i = std::get_if<int>(v)) return *i;
    // 数値は JSON 側から float で来ることがあるため、縮小変換を許す。
    if (const float* f = std::get_if<float>(v)) return static_cast<int>(*f);
    return fallback;
}

float OpArgs::GetFloat(std::string_view name, float fallback) const
{
    const OpValue* v = Find(name);
    if (v == nullptr) return fallback;
    if (const float* f = std::get_if<float>(v)) return *f;
    if (const int* i = std::get_if<int>(v)) return static_cast<float>(*i);
    return fallback;
}

std::string OpArgs::GetString(std::string_view name, std::string fallback) const
{
    const OpValue* v = Find(name);
    if (v == nullptr) return fallback;
    if (const std::string* s = std::get_if<std::string>(v)) return *s;
    return fallback;
}

math::Vector3 OpArgs::GetVec3(std::string_view name, math::Vector3 fallback) const
{
    const OpValue* v = Find(name);
    if (v == nullptr) return fallback;
    if (const math::Vector3* vec = std::get_if<math::Vector3>(v)) return *vec;
    return fallback;
}

// ── 引数の検問 ──────────────────────────────────────────────────────────────
// WHY ここに置くか: 面ごと (メニュー / パレット / AI バス) に検証を書くと、
//     AI 経路だけが厳しい / 緩いという状態が作れてしまう。宣言 (OpParam) を
//     読んで判定する場所を 1 つに固定すれば、どの面から来ても同じ結論になる。

OpResult ValidateArgs(const EditorOperator& op, const OpArgs& args)
{
    for (const OpParam& param : op.params) {
        if (!args.Has(param.name)) {
            if (param.required) {
                return OpResult::Err("BAD_ARG",
                                     "必須の引数がありません: " + param.name);
            }
            continue;
        }

        if (!param.enumValues.empty()) {
            const std::string value = args.GetString(param.name);
            const bool known = std::find(param.enumValues.begin(), param.enumValues.end(), value)
                               != param.enumValues.end();
            if (!known) {
                std::string allowed;
                for (const std::string& candidate : param.enumValues) {
                    if (!allowed.empty()) allowed += " / ";
                    allowed += candidate;
                }
                return OpResult::Err("BAD_ARG",
                                     param.name + " は " + allowed + " のいずれかです: " + value);
            }
        }

        if (param.hasRange
            && (param.type == OpParamType::Int || param.type == OpParamType::Float)) {
            const float value = args.GetFloat(param.name);
            if (value < param.minValue || value > param.maxValue) {
                return OpResult::Err("BAD_ARG",
                                     param.name + " は " + std::to_string(param.minValue) + " 〜 "
                                     + std::to_string(param.maxValue) + " の範囲です");
            }
        }
    }

    return OpResult::Ok();
}

// ── OperatorRegistry ────────────────────────────────────────────────────────

void OperatorRegistry::Register(EditorOperator op)
{
    if (op.id.empty()) {
        FBZZ_LOG_WARN("OperatorRegistry: id が空の操作は登録できません (label=%s)",
                      op.label.c_str());
        return;
    }
    if (!op.exec) {
        FBZZ_LOG_WARN("OperatorRegistry: exec が無い操作は登録できません (id=%s)",
                      op.id.c_str());
        return;
    }

    // WHY: 二重登録を静かに許すと、ホットキー 1 回で 2 回実行される状態が作れてしまう。
    //      後勝ちで置き換え、上書きが起きたことはログへ残す。
    auto it = std::find_if(m_operators.begin(), m_operators.end(),
                           [&op](const EditorOperator& e) { return e.id == op.id; });
    if (it != m_operators.end()) {
        FBZZ_LOG_WARN("OperatorRegistry: id が重複しています。後の登録で置き換えます (id=%s)",
                      op.id.c_str());
        *it = std::move(op);
        return;
    }

    m_operators.push_back(std::move(op));
}

const EditorOperator* OperatorRegistry::Find(std::string_view id) const
{
    for (const auto& op : m_operators)
        if (op.id == id) return &op;
    return nullptr;
}

bool OperatorRegistry::CanInvoke(std::string_view id, const OpContext& context,
                                 const OpArgs& args) const
{
    const EditorOperator* op = Find(id);
    if (op == nullptr) return false;
    if (!op->poll) return true;
    return op->poll(context, args);
}

OpResult OperatorRegistry::Invoke(std::string_view id, OpContext& context, const OpArgs& args)
{
    const EditorOperator* op = Find(id);
    if (op == nullptr) {
        return OpResult::Err("UNKNOWN_OPERATOR",
                             "未登録の操作です: " + std::string(id));
    }

    // 宣言された制約 (enum / range) の検問。poll より前に置くのは、
    // 「実行可能な状態ではあるが引数が不正」を NOT_AVAILABLE ではなく
    // BAD_ARG として返すため — AI から見て直し方が変わる。
    if (OpResult argCheck = ValidateArgs(*op, args); !argCheck.ok)
        return argCheck;

    // 条件判定は入口で 1 度だけ。exec の先頭に書かせない。
    // WHY: exec 内で早期 return すると、UI 側は「押せるのに何も起きない」を作れてしまう。
    //      グレーアウト表示と実行拒否が同じ述語から出ることを、ここで保証する。
    if (op->poll && !op->poll(context, args)) {
        return OpResult::Err("NOT_AVAILABLE",
                             "現在この操作は実行できません: " + op->id);
    }

    OpResult result = op->exec(context, args);

    // 返ってきたコマンドは「実行済みの編集」なので Execute せず Push だけする
    // (UndoStack::Push はその契約。Execute を呼ぶと同じ編集が二重に適用される)。
    // 履歴に出る文言はコマンド自身が持つ — undoLabel はその宣言であり、
    // AI とドキュメントが実行前に「何が履歴へ残るか」を知るために使う。
    if (result.ok && result.command) {
        context.undo.Push(std::move(result.command));
        return result;
    }

    // ここが Undo の検問。Mutation がコマンドを返さなかった = Undo できない編集が
    // シーンへ入った可能性がある。例外を作らないので、抜け道が残らない。
    //
    // 記録が無効なとき (Play 中) は履歴を持たないのが正しいので対象外。
    // 「何も変わらなかった」ケースも nullptr になるが、poll が実行可能と答えた
    // 直後に何も変わらないなら、それ自体が実装の問題なので黙らせない。
    if (result.ok && !result.noChange
        && op->kind == OpKind::Mutation && context.undo.IsRecordingEnabled()) {
        FBZZ_LOG_WARN("OperatorRegistry: Mutation が Undo コマンドを返しませんでした (id=%s)。"
                      "OpResult::command に載せてください",
                      op->id.c_str());
    }

    return result;
}

// ── パネルからの呼び出し口 ──────────────────────────────────────────────────

bool CanInvokeOperator(EditorContext& ctx, std::string_view id, const OpArgs& args)
{
    if (ctx.operators == nullptr || ctx.undoStack == nullptr) return false;
    const OpContext context{ ctx, *ctx.undoStack };
    return ctx.operators->CanInvoke(id, context, args);
}

OpResult InvokeOperator(EditorContext& ctx, std::string_view id, const OpArgs& args)
{
    if (ctx.operators == nullptr || ctx.undoStack == nullptr)
        return OpResult::Err("NO_REGISTRY", "Operator レジストリが未初期化です");
    OpContext context{ ctx, *ctx.undoStack };
    return ctx.operators->Invoke(id, context, args);
}

} // namespace fbzz::editor
