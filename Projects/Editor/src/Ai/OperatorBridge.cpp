/// @file    OperatorBridge.cpp
/// @brief   OperatorRegistry ↔ Editor Command Bus (AI / MCP) の変換。
/// @author  Hasegawa Jin
/// @date    2026-08-22
#include <Editor/Ai/OperatorBridge.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace fbzz::editor::ai {

namespace {

std::string LowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool ContainsCI(const std::string& haystack, const std::string& lowerNeedle)
{
    if (lowerNeedle.empty()) return true;
    return LowerAscii(haystack).find(lowerNeedle) != std::string::npos;
}

const char* KindName(OpKind kind)
{
    switch (kind) {
    case OpKind::Query:    return "query";
    case OpKind::Action:   return "action";
    case OpKind::Mutation: return "mutation";
    }
    return "action";
}

const char* ParamTypeName(OpParamType type)
{
    switch (type) {
    case OpParamType::Bool:   return "bool";
    case OpParamType::Int:    return "int";
    case OpParamType::Float:  return "float";
    case OpParamType::String: return "string";
    case OpParamType::Vec3:   return "vec3";
    case OpParamType::NodeId: return "nodeId";
    }
    return "string";
}

JsonValue ParamsToJson(const EditorOperator& op)
{
    JsonValue params = JsonValue::MakeArray();
    for (const OpParam& p : op.params) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(p.name));
        entry.Set("type", JsonValue(ParamTypeName(p.type)));
        entry.Set("required", JsonValue(p.required));
        if (!p.desc.empty()) entry.Set("desc", JsonValue(p.desc));

        /// @note 取りうる値と範囲は宣言から出す。desc の文章に書いてあっても
        ///       機械可読ではないので、AI は綴りを推測することになる。
        if (!p.enumValues.empty()) {
            JsonValue values = JsonValue::MakeArray();
            for (const std::string& candidate : p.enumValues)
                values.Push(JsonValue(candidate));
            entry.Set("enum", std::move(values));
        }
        if (p.hasRange) {
            entry.Set("min", JsonValue(static_cast<double>(p.minValue)));
            entry.Set("max", JsonValue(static_cast<double>(p.maxValue)));
        }
        params.Push(std::move(entry));
    }
    return params;
}

/// @brief Operator が返した構造化データを JSON へ写す。
/// @note Op 層は AI 層 (JsonValue) を知らない契約になっている (Editor/Op/OpData.hpp)。
///       境界であるこの層だけが両方を知る。
JsonValue OpDataToJson(const OpData& data)
{
    switch (data.GetType()) {
    case OpData::Type::Null:   return JsonValue(nullptr);
    case OpData::Type::Bool:   return JsonValue(data.AsBool());
    case OpData::Type::Int:    return JsonValue(data.AsInt());
    case OpData::Type::Float:  return JsonValue(static_cast<double>(data.AsFloat()));
    case OpData::Type::String: return JsonValue(data.AsString());
    case OpData::Type::Vec3: {
        /// @note 読んだ形をそのまま書き戻せるよう、引数側が受ける {x,y,z} で返す。
        const math::Vector3 v = data.AsVec3();
        JsonValue object = JsonValue::MakeObject();
        object.Set("x", JsonValue(static_cast<double>(v.x)));
        object.Set("y", JsonValue(static_cast<double>(v.y)));
        object.Set("z", JsonValue(static_cast<double>(v.z)));
        return object;
    }
    case OpData::Type::Array: {
        JsonValue array = JsonValue::MakeArray();
        for (const OpData& element : data.AsArray())
            array.Push(OpDataToJson(element));
        return array;
    }
    case OpData::Type::Object: {
        JsonValue object = JsonValue::MakeObject();
        for (const OpData::Member& member : data.AsObject())
            object.Set(member.first, OpDataToJson(member.second));
        return object;
    }
    }
    return JsonValue(nullptr);
}

/// @brief 1 つの Vec3 引数を JSON から読む。[x,y,z] と {x,y,z} の両方を受ける。
/// @note 応答側 (scene.tree 等) が {x,y,z} を返す一方、AI は配列で書きたがることが多い。
///       どちらかだけを受けると「読んだ形をそのまま書き戻す」ができない場面が出る。
bool ReadVec3(const JsonValue& value, math::Vector3& out)
{
    if (value.IsArray()) {
        const JsonValue::Array& array = value.AsArray();
        if (array.size() != 3) return false;
        for (const JsonValue& component : array)
            if (!component.IsNumber()) return false;
        out = math::Vector3{ static_cast<float>(array[0].AsNumber()),
                             static_cast<float>(array[1].AsNumber()),
                             static_cast<float>(array[2].AsNumber()) };
        return true;
    }
    if (value.IsObject()) {
        const JsonValue* x = value.Find("x");
        const JsonValue* y = value.Find("y");
        const JsonValue* z = value.Find("z");
        if (x == nullptr || y == nullptr || z == nullptr) return false;
        if (!x->IsNumber() || !y->IsNumber() || !z->IsNumber()) return false;
        out = math::Vector3{ static_cast<float>(x->AsNumber()),
                             static_cast<float>(y->AsNumber()),
                             static_cast<float>(z->AsNumber()) };
        return true;
    }
    return false;
}

/// @brief 宣言された params に従って JSON を OpArgs へ変換する。未知のキーと型違いは
///        受理せずエラーにする。
/// @note 「保存は通るのに実行時に黙って無視される」形の壊れ方を作らない。shader の存在しない
///       変数名や BT の非該当フィールドと同じで、綴り違いは原因に辿り着けなくなる。
bool ConvertArgs(const EditorOperator& op, const JsonValue* argsValue,
                 OpArgs& out, std::string& error)
{
    /// @note 宣言されていないキーが来ていないかを先に見る。
    if (argsValue != nullptr && argsValue->IsObject()) {
        for (const JsonValue::Member& member : argsValue->AsObject()) {
            const bool declared = std::any_of(
                op.params.begin(), op.params.end(),
                [&member](const OpParam& p) { return p.name == member.first; });
            if (!declared) {
                error = "この操作は引数 '" + member.first + "' を受け取りません";
                return false;
            }
        }
    }

    for (const OpParam& param : op.params) {
        const JsonValue* value =
            (argsValue != nullptr) ? argsValue->Find(param.name) : nullptr;

        if (value == nullptr || value->IsNull()) {
            if (param.required) {
                error = "必須の引数がありません: " + param.name;
                return false;
            }
            if (!std::holds_alternative<std::monostate>(param.defaultValue))
                out.Set(param.name, param.defaultValue);
            continue;
        }

        switch (param.type) {
        case OpParamType::Bool:
            if (!value->IsBool()) { error = param.name + " は bool です"; return false; }
            out.Set(param.name, value->AsBool());
            break;
        case OpParamType::Int:
            if (!value->IsNumber()) { error = param.name + " は数値です"; return false; }
            out.Set(param.name, value->AsInt());
            break;
        case OpParamType::Float:
            if (!value->IsNumber()) { error = param.name + " は数値です"; return false; }
            out.Set(param.name, static_cast<float>(value->AsNumber()));
            break;
        case OpParamType::String:
        case OpParamType::NodeId:
            if (!value->IsString()) { error = param.name + " は文字列です"; return false; }
            out.Set(param.name, value->AsString());
            break;
        case OpParamType::Vec3: {
            math::Vector3 vec{};
            if (!ReadVec3(*value, vec)) {
                error = param.name + " は [x,y,z] または {x,y,z} です";
                return false;
            }
            out.Set(param.name, vec);
            break;
        }
        }
    }
    return true;
}

JsonValue OperatorToJson(const EditorOperator& op, bool available,
                         const OpContext& opContext)
{
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("id", JsonValue(op.id));
    entry.Set("label", JsonValue(op.label));
    entry.Set("category", JsonValue(op.category));
    entry.Set("kind", JsonValue(KindName(op.kind)));
    entry.Set("available", JsonValue(available));
    /// @note トグル操作の現在値。メニューのチェックマークと同じ式から出る。無いと
    ///       「切り替えられるのに今どちらか読めない」ままになる。必須引数を持つ操作は
    ///       目録では評価しない。対象不定のまま false を出すと「OFF」と誤読されるため、
    ///       確定は invoke の dryRun で行う。
    const bool checkableWithoutArgs = std::none_of(
        op.params.begin(), op.params.end(),
        [](const OpParam& p) { return p.required; });
    if (op.checked && checkableWithoutArgs) {
        const OpArgs noArgs;
        entry.Set("checked", JsonValue(op.checked(opContext, noArgs)));
    }
    if (!op.desc.empty())      entry.Set("desc", JsonValue(op.desc));
    if (!op.caution.empty())   entry.Set("caution", JsonValue(op.caution));
    if (!op.undoLabel.empty()) entry.Set("undoLabel", JsonValue(op.undoLabel));
    if (!op.params.empty())    entry.Set("params", ParamsToJson(op));
    return entry;
}

/// バス側から見た「使える状態か」。EditorApp が結線を終える前に要求が来ても落ちないようにする。
bool ResolveRegistry(editor::EditorContext& context,
                     OperatorRegistry*&    registry,
                     UndoStack*&           undo)
{
    registry = context.operators;
    undo     = context.undoStack;
    return registry != nullptr && undo != nullptr;
}

} // namespace

OperatorBridgeResult ListOperators(editor::EditorContext& context, const JsonValue& payload)
{
    OperatorRegistry* registry = nullptr;
    UndoStack*        undo     = nullptr;
    if (!ResolveRegistry(context, registry, undo))
        return OperatorBridgeResult::Err("NO_REGISTRY", "Operator レジストリが未初期化です");

    const JsonValue* searchValue   = payload.Find("search");
    const JsonValue* categoryValue = payload.Find("category");
    const JsonValue* includeValue  = payload.Find("includeUnavailable");

    const std::string search   = LowerAscii(
        (searchValue != nullptr && searchValue->IsString()) ? searchValue->AsString() : std::string{});
    const std::string category =
        (categoryValue != nullptr && categoryValue->IsString()) ? categoryValue->AsString() : std::string{};
    /// @note 既定で不可の操作も返す。「無い」と「今は使えない」を区別できないと、
    ///       AI は存在する手段を諦めて別のやり方を組み立て始める。
    const bool includeUnavailable =
        (includeValue == nullptr) ? true : includeValue->AsBool();

    const OpContext opContext{ context, *undo };

    JsonValue list = JsonValue::MakeArray();
    int matched = 0;
    for (const EditorOperator& op : registry->All()) {
        if (!category.empty() && op.category != category) continue;
        if (!search.empty()
            && !ContainsCI(op.id, search)
            && !ContainsCI(op.label, search)
            && !ContainsCI(op.desc, search))
            continue;

        /// @note 目録は引数なしの評価。引数で対象を指定する操作は、その呼び出し方での
        ///       可否が変わりうるので、確定は editor.op.invoke の dryRun で行う。
        const OpArgs noArgs;
        const bool available = !op.poll || op.poll(opContext, noArgs);
        if (!available && !includeUnavailable) continue;

        ++matched;
        list.Push(OperatorToJson(op, available, opContext));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("operators", std::move(list));
    result.Set("count", JsonValue(matched));
    result.Set("total", JsonValue(static_cast<int>(registry->All().size())));
    return OperatorBridgeResult::Ok(std::move(result));
}

OperatorBridgeResult InvokeOperator(editor::EditorContext& context,
                                    const JsonValue&       payload,
                                    bool                   dryRun)
{
    OperatorRegistry* registry = nullptr;
    UndoStack*        undo     = nullptr;
    if (!ResolveRegistry(context, registry, undo))
        return OperatorBridgeResult::Err("NO_REGISTRY", "Operator レジストリが未初期化です");

    const JsonValue* idValue = payload.Find("id");
    if (idValue == nullptr || !idValue->IsString() || idValue->AsString().empty())
        return OperatorBridgeResult::Err("BAD_ARG", "id は必須です");

    const std::string     id = idValue->AsString();
    const EditorOperator* op = registry->Find(id);
    if (op == nullptr)
        return OperatorBridgeResult::Err("UNKNOWN_OPERATOR",
                                         "未登録の操作です: " + id + " (editor.op.list で一覧できます)");

    OpArgs      args;
    std::string argError;
    if (!ConvertArgs(*op, payload.Find("args"), args, argError))
        return OperatorBridgeResult::Err("BAD_ARG", argError);

    /// @note 宣言された制約 (enum / range) は dryRun でも同じ判定を返す。
    ///       ここを省くと「dry-run では通ったのに実行すると BAD_ARG」になり、
    ///       dry-run が「実行前に確かめる」用途を果たさなくなる。
    if (const OpResult argCheck = ValidateArgs(*op, args); !argCheck.ok)
        return OperatorBridgeResult::Err(argCheck.errorCode, argCheck.message);

    OpContext  opContext{ context, *undo };
    /// @note 実際に渡された args で判定する。dryRun の available もこれと同じ値になるので、
    ///       「dry-run では可だったのに実行すると NOT_AVAILABLE」が起きない。
    const bool available = !op->poll || op->poll(opContext, args);

    /// @note dryRun: 引数検証と実行可否だけを返し、シーンには一切触れない。
    if (dryRun) {
        JsonValue result = JsonValue::MakeObject();
        result.Set("id", JsonValue(op->id));
        result.Set("label", JsonValue(op->label));
        result.Set("kind", JsonValue(KindName(op->kind)));
        result.Set("available", JsonValue(available));
        result.Set("dryRun", JsonValue(true));
        /// @note 渡された引数での現在状態。排他選択 (set_view_mode 等) は
        ///       「その値が今の値か」がここで初めて確定する。
        if (op->checked) result.Set("checked", JsonValue(op->checked(opContext, args)));
        if (!op->undoLabel.empty()) result.Set("undoLabel", JsonValue(op->undoLabel));
        if (!available)
            result.Set("reason", JsonValue("現在の状態ではこの操作を実行できません"));
        return OperatorBridgeResult::Ok(std::move(result));
    }

    /// @note poll の再評価は Invoke 側でも行われる。ここで先に返すのはエラーコードを
    ///       NOT_AVAILABLE として明示し、上の available と食い違わせないため。
    if (!available)
        return OperatorBridgeResult::Err("NOT_AVAILABLE",
                                         "現在の状態ではこの操作を実行できません: " + id);

    const OpResult opResult = registry->Invoke(id, opContext, args);

    if (!opResult.ok) {
        return OperatorBridgeResult::Err(
            opResult.errorCode.empty() ? "OP_FAILED" : opResult.errorCode,
            opResult.message);
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(op->id));
    result.Set("label", JsonValue(op->label));
    result.Set("kind", JsonValue(KindName(op->kind)));
    result.Set("applied", JsonValue(true));
    /// @note 「成功したが何も変わらなかった」を成功と区別して返す。区別が無いと、AI は
    ///       同じ要求を「効かなかった」と読んで別の値で再試行し続けてしまう。
    if (opResult.noChange) result.Set("noChange", JsonValue(true));
    /// @note 何が Undo 履歴に残ったかを返す。AI が editor.undoHistory と突き合わせて
    ///       自分の編集を識別できるようにするため。
    if (undo->CanUndo()) result.Set("undoEntry", JsonValue(undo->GetUndoDescription()));
    if (!opResult.message.empty()) result.Set("message", JsonValue(opResult.message));
    if (!opResult.data.IsNull()) result.Set("data", OpDataToJson(opResult.data));
    return OperatorBridgeResult::Ok(std::move(result));
}

OperatorBridgeResult QueryOperator(editor::EditorContext& context, const JsonValue& payload)
{
    OperatorRegistry* registry = nullptr;
    UndoStack*        undo     = nullptr;
    if (!ResolveRegistry(context, registry, undo))
        return OperatorBridgeResult::Err("NO_REGISTRY", "Operator レジストリが未初期化です");

    const JsonValue* idValue = payload.Find("id");
    if (idValue == nullptr || !idValue->IsString() || idValue->AsString().empty())
        return OperatorBridgeResult::Err("BAD_ARG", "id は必須です");

    const std::string     id = idValue->AsString();
    const EditorOperator* op = registry->Find(id);
    if (op == nullptr)
        return OperatorBridgeResult::Err("UNKNOWN_OPERATOR",
                                         "未登録の操作です: " + id + " (editor.op.list で一覧できます)");

    /// @note read 権限で到達できる入口なので、ここを通せるのは Query だけ。黙って実行すると
    ///       read 権限のつもりの接続から Action/Mutation が通ってしまい、黙って無視すると
    ///       AI が「呼んだのに何も返らない」を不具合として調べ始めるため、エラーで返す。
    if (op->kind != OpKind::Query)
        return OperatorBridgeResult::Err("NOT_A_QUERY",
                                         "editor.op.query は kind=query の操作専用です: " + id
                                         + " (kind=" + KindName(op->kind) + " は editor.op.invoke へ)");

    OpArgs      args;
    std::string argError;
    if (!ConvertArgs(*op, payload.Find("args"), args, argError))
        return OperatorBridgeResult::Err("BAD_ARG", argError);
    if (const OpResult argCheck = ValidateArgs(*op, args); !argCheck.ok)
        return OperatorBridgeResult::Err(argCheck.errorCode, argCheck.message);

    OpContext opContext{ context, *undo };
    if (op->poll && !op->poll(opContext, args))
        return OperatorBridgeResult::Err("NOT_AVAILABLE",
                                         "現在の状態ではこの操作を実行できません: " + id);

    const OpResult opResult = registry->Invoke(id, opContext, args);
    if (!opResult.ok) {
        return OperatorBridgeResult::Err(
            opResult.errorCode.empty() ? "OP_FAILED" : opResult.errorCode,
            opResult.message);
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(op->id));
    result.Set("label", JsonValue(op->label));
    result.Set("kind", JsonValue(KindName(op->kind)));
    if (!opResult.message.empty()) result.Set("message", JsonValue(opResult.message));
    result.Set("data", OpDataToJson(opResult.data));
    return OperatorBridgeResult::Ok(std::move(result));
}

} // namespace fbzz::editor::ai
