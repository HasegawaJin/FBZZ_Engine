/// @file    DeveloperOperators.cpp
/// @brief   開発者モードの切り替えと、開発者モードでだけ使える操作。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note 開発者モードの判定は Engine の DeveloperMode が正本。エディターは設定の保存だけを受け持つ。
/// @see Docs/design/developer-mode.md
#include <Editor/Op/OperatorGroups.hpp>

#include <Engine/Core/CrashHandler.hpp>
#include <Engine/Core/DeveloperMode.hpp>

#include <string>

namespace fbzz::editor {

void RegisterDeveloperOperators(OperatorRegistry& registry)
{
    {
        EditorOperator op;
        op.id       = "developer.toggle_mode";
        op.label    = "Developer Mode";
        op.category = "Developer";
        op.desc     = "エンジン開発者向けの操作 (わざと落とす など) を出す。設定として保存される。";
        op.kind     = OpKind::Action;

        OpParam enabled;
        enabled.name     = "enabled";
        enabled.type     = OpParamType::Bool;
        enabled.desc     = "省略すると現在値を反転する";
        enabled.required = false;
        op.params = { enabled };

        op.checked = [](const OpContext&, const OpArgs&) { return core::DeveloperMode::IsEnabled(); };
        op.exec = [](OpContext&, const OpArgs& args) -> OpResult {
            const bool next = args.Has("enabled") ? args.GetBool("enabled") : !core::DeveloperMode::IsEnabled();
            if (!next && core::DeveloperMode::IsForcedByLaunch())
                return OpResult::Err("FORCED_BY_LAUNCH",
                                     "--developer か FBZZ_DEVELOPER_MODE=1 で起動しているため、このセッションではオフにできません。");
            core::DeveloperMode::SetPreference(next);
            OpResult result;
            result.message = next ? "開発者モードを有効にしました" : "開発者モードを無効にしました";
            return result;
        };
        registry.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id       = "developer.crash";
        op.label    = "Crash";
        op.category = "Developer";
        op.desc     = "クラッシュレポート (Saved/Crashes) の受け口を確かめるため、指定した落ち方でエディターを落とす。"
                      "report だけは落とさずにレポートを書く。デバッガーを付けていると受け口を通らない。";
        op.caution  = "report 以外はエディターが即座に終了し、未保存の変更は失われる。";
        op.kind     = OpKind::Action;

        OpParam kind;
        kind.name       = "kind";
        kind.type       = OpParamType::String;
        kind.desc       = "落ち方";
        kind.enumValues = core::CrashHandler::TriggerNames();
        op.params = { kind };

        op.poll = [](const OpContext&, const OpArgs&) {
            return core::DeveloperMode::IsEnabled() && core::CrashHandler::IsInstalled();
        };
        op.exec = [](OpContext&, const OpArgs& args) -> OpResult {
            core::CrashTrigger trigger = core::CrashTrigger::Report;
            if (!core::CrashHandler::ParseTrigger(args.GetString("kind"), trigger))
                return OpResult::Err("BAD_ARG", "kind が不正です。");
            if (!core::CrashHandler::Trigger(trigger))
                return OpResult::Err("NOT_WRITTEN", "レポートを書けませんでした。ログを確認してください。");
            OpResult result;
            result.message = "Saved/Crashes にレポートを書きました";
            return result;
        };
        registry.Register(std::move(op));
    }
}

} // namespace fbzz::editor
