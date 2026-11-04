/// @file    InputBindingOperators.cpp
/// @brief   全入力バインドの設定・保存・再読込・リバインド・プレビュー操作。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include "SettingsDocument.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/InputBindingsEditing.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_set>

namespace fbzz::editor {
namespace {
bool ValidBindings(const std::vector<input::InputBinding>& bindings)
{
    if (bindings.size() > 1024) return false;
    for (const auto& binding : bindings) {
        if (!std::isfinite(binding.scale) || !std::isfinite(binding.buttonThreshold)
            || binding.buttonThreshold < 0.0f || binding.buttonThreshold > 1.0f
            || binding.padIndex < -1 || binding.padIndex > 3) return false;
        switch (binding.source) {
        case input::BindingSource::KEY: if (binding.code > 255) return false; break;
        case input::BindingSource::MOUSE_BUTTON: if (binding.code > 2) return false; break;
        case input::BindingSource::MOUSE_AXIS: if (binding.code > 2) return false; break;
        case input::BindingSource::GAMEPAD_BUTTON:
            if (binding.code >= static_cast<unsigned>(input::GamepadButton::COUNT)) return false; break;
        case input::BindingSource::GAMEPAD_AXIS:
            if (binding.code >= static_cast<unsigned>(input::GamepadAxis::COUNT)) return false; break;
        default: return false;
        }
    }
    return true;
}

bool ValidDocument(const input::InputBindingsDocument& document)
{
    if (document.actions.size() > 1024 || document.axes.size() > 1024) return false;
    std::unordered_set<std::string> names;
    for (const auto& action : document.actions) {
        if (action.name.empty() || action.name.size() > 128 || !names.insert(action.name).second || !ValidBindings(action.bindings)) return false;
    }
    names.clear();
    for (const auto& axis : document.axes) {
        if (axis.name.empty() || axis.name.size() > 128 || !names.insert(axis.name).second
            || !std::isfinite(axis.deadZone) || axis.deadZone < 0.0f || axis.deadZone >= 1.0f
            || !std::isfinite(axis.gravity) || axis.gravity < 0.0f
            || !std::isfinite(axis.sensitivity) || axis.sensitivity < 0.0f
            || !ValidBindings(axis.positive) || !ValidBindings(axis.negative) || !ValidBindings(axis.analog)) return false;
    }
    return true;
}

std::string InputPath(const EditorContext& context)
{
    return util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(context.projectRoot) / "ProjectSettings" / "Input.inputactions");
}

OpData State(const EditorContext& context)
{
    OpData data = OpData::MakeObject();
    data.Set("path", OpData(InputPath(context)));
    data.Set("settings", settingsdocument::Inspect(CaptureInputBindings().ToToml(true)));
    data.Set("previewEnabled", OpData(input::InputActionMap::IsEnabled()));
    data.Set("rebinding", OpData(input::InputActionMap::IsRebinding()));
    return data;
}
} /// @note namespace

void RegisterInputBindingOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "configure", "reset", "reload", "save", "rebind", "cancel_rebind", "preview"}) {
        const std::string actionName = action;
        const bool mutation = actionName == "configure" || actionName == "reset" || actionName == "reload";
        EditorOperator op;
        op.id = "input.bindings." + actionName; op.label = op.id; op.category = "Input";
        op.kind = mutation ? OpKind::Mutation : actionName == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "入力のaction/axis配列を照会・履歴付き編集する。configureはsettingsを部分指定し、saveでInput.inputactionsへ保存する。rebindはinspectのrebindingで追跡する。";
        if (actionName == "configure") {
            OpParam settings; settings.name = "settings"; settings.type = OpParamType::Data;
            settings.desc = "inspect.settingsと同じaction/axis配列。配列は全置換。bindings.sourceはKey/MouseButton/GamepadButton/GamepadAxis/MouseAxis。";
            op.params.push_back(std::move(settings));
        } else if (actionName == "preview") {
            OpParam enabled; enabled.name = "enabled"; enabled.type = OpParamType::Bool;
            op.params.push_back(std::move(enabled));
        } else if (actionName == "rebind") {
            OpParam kind; kind.name = "kind"; kind.type = OpParamType::String; kind.enumValues = {"action", "axis"};
            OpParam name; name.name = "name"; name.type = OpParamType::String;
            OpParam slot; slot.name = "slot"; slot.type = OpParamType::Int; slot.required = false; slot.defaultValue = 0;
            slot.hasRange = true; slot.minValue = 0; slot.maxValue = 2; slot.desc = "axisの0=positive/1=negative/2=analog";
            OpParam index; index.name = "bindingIndex"; index.type = OpParamType::Int; index.required = false; index.defaultValue = 0;
            index.hasRange = true; index.minValue = 0; index.maxValue = 1024;
            op.params = {kind, name, slot, index};
        }
        op.poll = [actionName](const OpContext& context, const OpArgs&) {
            return !context.ctx.projectRoot.empty()
                && (actionName == "inspect" || actionName == "preview" || !context.ctx.playMode || context.ctx.playMode->IsInEditor());
        };
        op.exec = [actionName, mutation](OpContext& context, const OpArgs& args) {
            if (mutation) {
                if (input::InputActionMap::IsRebinding()) return OpResult::Err("INPUT_REBIND_PENDING", "リバインドの完了または取消後に編集してください。");
                const auto before = CaptureInputBindings();
                auto after = before;
                if (actionName == "reset") {
                    input::InputActionMap::LoadDefaults();
                    after = CaptureInputBindings();
                } else {
                    std::string text;
                    if (actionName == "reload") {
                        if (!util::FileSystem::ReadText(InputPath(context.ctx), text)) return OpResult::Err("INPUT_READ_FAILED", "入力設定を読み取れません。");
                    } else {
                        const OpData* patch = args.GetData("settings");
                        if (!patch || !settingsdocument::Patch(before.ToToml(true), *patch, text)) return OpResult::Err("BAD_ARG", "入力設定の型が不正です。");
                    }
                    if (!after.LoadFromToml(text) || !ValidDocument(after)) return OpResult::Err("BAD_ARG", "入力名・バインド・軸の設定が不正です。");
                    if (actionName == "configure" && !settingsdocument::Preserves(after.ToToml(true), *args.GetData("settings")))
                        return OpResult::Err("BAD_ARG", "未知の入力設定、または解析できないバインドが含まれています。");
                }
                OpResult result;
                result.command = CommitInputBindingsEdit(context.ctx, before, after);
                result.noChange = !result.command;
                result.data = State(context.ctx);
                return result;
            }
            if (actionName == "save") {
                if (input::InputActionMap::IsRebinding()) return OpResult::Err("INPUT_REBIND_PENDING", "リバインドの完了または取消後に保存してください。");
                if (!input::InputActionMap::SaveToFile(InputPath(context.ctx))) return OpResult::Err("INPUT_WRITE_FAILED", "入力設定を保存できません。");
            } else if (actionName == "preview") input::InputActionMap::SetEnabled(args.GetBool("enabled"));
            else if (actionName == "cancel_rebind") input::InputActionMap::CancelRebind();
            else if (actionName == "rebind") {
                const auto name = args.GetString("name");
                if (args.GetString("kind") == "action") {
                    if (!input::InputActionMap::FindAction(name)) return OpResult::Err("INPUT_NOT_FOUND", "その入力アクションはありません。");
                    input::InputActionMap::BeginRebindAction(name, args.GetInt("bindingIndex"));
                } else {
                    if (!input::InputActionMap::FindAxis(name)) return OpResult::Err("INPUT_NOT_FOUND", "その入力軸はありません。");
                    input::InputActionMap::BeginRebindAxis(name, args.GetInt("slot"), args.GetInt("bindingIndex"));
                }
            }
            return OpResult::Data(State(context.ctx));
        };
        registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
