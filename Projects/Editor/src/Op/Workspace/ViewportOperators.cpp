/// @file    ViewportOperators.cpp
/// @brief   Game Viewの表示・Play設定をUIとMCPの共通操作へ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Play/PlayModeController.hpp>
#include <Editor/Play/StandalonePlaySession.hpp>
#include <Editor/Viewport/GameViewportOptions.hpp>
#include <Engine/Core/Platform/Cursor.hpp>
#include <algorithm>

namespace fbzz::editor {
void RegisterGameViewportOperators(OperatorRegistry& registry)
{
    static_assert(GAME_VIEWPORT_ASPECT_OPTIONS.size() ==
        static_cast<size_t>(EditorContext::GameViewportAspect::iPhoneLandscape) + 1);
    for (const bool configure : {false, true}) {
        EditorOperator op;
        op.id = configure ? "viewport.game.configure" : "viewport.game.inspect";
        op.label = configure ? "Configure Game View" : "Inspect Game View";
        op.category = "Viewport";
        op.kind = configure ? OpKind::Action : OpKind::Query;
        op.dialog = OpDialog::NONE;
        op.desc = "Game Viewの比率、Play Focused/Maximized/Unfocused、実行方式とCursor Game/Freeを扱う。"
                  "layoutは入力やOSフォーカスを取得せず現在の配置だけを最大化/復元する。";
        if (configure) {
            OpParam aspect{"aspect", OpParamType::String, "画面比率プリセット", false, {}};
            for (const auto& option : GAME_VIEWPORT_ASPECT_OPTIONS) aspect.enumValues.emplace_back(option.key);
            op.params.push_back(std::move(aspect));
            OpParam focus{"playFocusMode", OpParamType::String, "次回Playの表示方法", false, {}};
            focus.enumValues.assign(PLAY_FOCUS_KEYS.begin(), PLAY_FOCUS_KEYS.end());
            op.params.push_back(std::move(focus));
            OpParam execution{"executionMode", OpParamType::String, "次回Playの実行方式", false, {}};
            execution.enumValues = {"inEditor", "standalone"};
            op.params.push_back(std::move(execution));
            OpParam cursor{"cursorOverride", OpParamType::String, "保存しないカーソルの一時解除", false, {}};
            cursor.enumValues = {"game", "free"};
            op.params.push_back(std::move(cursor));
            OpParam layout{"layout", OpParamType::String, "保存しない明示的な配置。automaticは通常のPlay設定に従う", false, {}};
            layout.enumValues = {"automatic", "maximized", "normal"};
            op.params.push_back(std::move(layout));
        }
        op.poll = [configure](const OpContext& context, const OpArgs& args) {
            if (configure && args.Has("layout") && context.ctx.mapEditingMode) return false;
            if (!configure || (!args.Has("playFocusMode") && !args.Has("executionMode"))) return true;
            return (!context.ctx.playMode || context.ctx.playMode->IsInEditor())
                && (!context.ctx.standalonePlay || !context.ctx.standalonePlay->IsActive());
        };
        op.exec = [configure](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx;
            bool persist = false;
            if (configure) {
                if (args.Has("aspect")) {
                    const auto key = args.GetString("aspect");
                    const auto found = std::find_if(GAME_VIEWPORT_ASPECT_OPTIONS.begin(), GAME_VIEWPORT_ASPECT_OPTIONS.end(),
                        [&key](const auto& option) { return key == option.key; });
                    ctx.gameViewportAspect = static_cast<EditorContext::GameViewportAspect>(found - GAME_VIEWPORT_ASPECT_OPTIONS.begin());
                    persist = true;
                }
                if (args.Has("playFocusMode")) {
                    const auto key = args.GetString("playFocusMode");
                    ctx.playFocusMode = static_cast<EditorContext::PlayFocusMode>(
                        std::find(PLAY_FOCUS_KEYS.begin(), PLAY_FOCUS_KEYS.end(), key) - PLAY_FOCUS_KEYS.begin());
                    persist = true;
                }
                if (args.Has("executionMode")) {
                    ctx.playExecutionMode = args.GetString("executionMode") == "standalone"
                        ? PlayExecutionMode::STANDALONE : PlayExecutionMode::IN_EDITOR;
                    persist = true;
                }
                if (args.Has("cursorOverride")) ctx.playCursorOverride = args.GetString("cursorOverride") == "free"
                    ? EditorContext::PlayCursorOverride::Free : EditorContext::PlayCursorOverride::Game;
                if (args.Has("layout")) {
                    const auto layout = args.GetString("layout");
                    ctx.gameViewportLayout = layout == "maximized" ? EditorContext::GameViewportLayout::MAXIMIZED
                        : layout == "normal" ? EditorContext::GameViewportLayout::NORMAL : EditorContext::GameViewportLayout::AUTOMATIC;
                }
                if (persist) ctx.requestEditorSettingsSave = true;
            }
            auto data = OpData::MakeObject();
            const auto aspectIndex = std::clamp(static_cast<int>(ctx.gameViewportAspect), 0,
                static_cast<int>(GAME_VIEWPORT_ASPECT_OPTIONS.size()) - 1);
            data.Set("aspect", GAME_VIEWPORT_ASPECT_OPTIONS[aspectIndex].key);
            data.Set("playFocusMode", PLAY_FOCUS_KEYS[std::clamp(static_cast<int>(ctx.playFocusMode), 0, 2)]);
            data.Set("executionMode", ctx.playExecutionMode == PlayExecutionMode::STANDALONE ? "standalone" : "inEditor");
            data.Set("cursorOverride", ctx.playCursorOverride == EditorContext::PlayCursorOverride::Free ? "free" : "game");
            data.Set("maximized", ctx.gameViewportMaximized);
            data.Set("layout", ctx.gameViewportLayout == EditorContext::GameViewportLayout::MAXIMIZED ? "maximized"
                : ctx.gameViewportLayout == EditorContext::GameViewportLayout::NORMAL ? "normal" : "automatic");
            data.Set("physicalInputAllowed", core::Cursor::IsPhysicalInputAllowed());
            data.Set("width", ctx.gameViewportWidth);
            data.Set("height", ctx.gameViewportHeight);
            return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
