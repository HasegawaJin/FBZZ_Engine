/// @file    OperatorRegistry.cpp
/// @brief   Editor Operator の登録簿と実行経路。
/// @author  Hasegawa Jin
/// @date    2026-08-22
#include <Editor/Op/EditorOperator.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Core/Logger.hpp>

#include <algorithm>
#include <utility>

namespace fbzz::editor {

/// @name OpArgs

const OpValue* OpArgs::Find(std::string_view name) const
{
    /// @note The small argument map uses linear lookup to avoid allocating a temporary string or requiring heterogeneous hash policies.
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
    /// @note 数値は JSON 側から float で来ることがあるため、縮小変換を許す。
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

const OpData* OpArgs::GetData(std::string_view name) const
{
    const OpValue* value = Find(name);
    return value ? std::get_if<OpData>(value) : nullptr;
}

/// @name 引数の検問
/// @note Menu, palette and AI callers share the declared OpParam constraints rather than defining separate validation rules.

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

        if (param.type == OpParamType::Data && !args.GetData(param.name))
            return OpResult::Err("BAD_ARG", param.name + " は構造化データです");

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

/// @name OperatorRegistry

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

    /// @note Duplicate IDs replace the previous registration with a diagnostic rather than executing both handlers for one action.
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
    if (context.ctx.playMode && !context.ctx.playMode->IsInEditor() && op->kind == OpKind::Mutation &&
        id != "script.configure.apply" && id != "script.set_enabled") return false;
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
    if (context.ctx.playMode && !context.ctx.playMode->IsInEditor() && op->kind == OpKind::Mutation &&
        id != "script.configure.apply" && id != "script.set_enabled")
        return OpResult::Err("PLAY_MUTATION_UNSUPPORTED", "Play supports temporary managed settings transactions only; stop Play to author this operation.");

    /// @note Invalid arguments return BAD_ARG before availability polling, allowing callers to distinguish repairable input from current UI state.
    if (OpResult argCheck = ValidateArgs(*op, args); !argCheck.ok)
        return argCheck;

    /// @note Availability uses the same poll for UI and execution before mutation begins.
    if (op->poll && !op->poll(context, args)) {
        return OpResult::Err("NOT_AVAILABLE",
                             "現在この操作は実行できません: " + op->id);
    }

    OpResult result = op->exec(context, args);

    /// @note Returned commands already applied their edit; Push records them in the active Edit or checked Play history without executing them again.
    if (result.ok && result.command) {
        context.undo.Push(std::move(result.command));
        return result;
    }

    /// @note Both Edit and temporary Play mutations require history while recording is enabled; explicit noChange results carry no command.
    if (result.ok && !result.noChange
        && op->kind == OpKind::Mutation && context.undo.IsRecordingEnabled()) {
        FBZZ_LOG_WARN("OperatorRegistry: Mutation が Undo コマンドを返しませんでした (id=%s)。"
                      "OpResult::command に載せてください",
                      op->id.c_str());
    }

    return result;
}

/// @name パネルからの呼び出し口

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

} /// @note namespace fbzz::editor
