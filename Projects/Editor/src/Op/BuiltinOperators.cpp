/// @file    BuiltinOperators.cpp
/// @brief   エディター標準操作の登録 (Operator モデル Step 1)。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// メニュー・ホットキー・パレットに別々に書かれていた「実行可能条件」を poll へ 1 本化する。
/// パネル表示トグルと Recent Scenes 等は単一の出所 (m_panels 等) から導出されるためここに登録しない。
/// @see Docs/design/editor-operator-model.md
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Util/ViewportCamera.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Vector3.hpp>

#include <imgui.h>

#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {

void EditorApp::RegisterBuiltinOperators()
{
    /// @note 登録を読みやすくする小さな組み立てヘルパー (引数を持たない操作専用)。
    ///       引数を取る操作は params の宣言が要るので、下の addWindowToggle のように
    ///       個別に組むか、EditorContext だけで完結するものは OperatorGroups 側の
    ///       登録関数へ置く。checked はトグル操作の現在状態 (メニューのチェックになる)。
    auto add = [this](const char* id, const char* label, const char* category,
                      const char* desc, OpKind kind,
                      OpExec exec, OpPoll poll = {}, OpCheck checked = {}) {
        EditorOperator op;
        op.id       = id;
        op.label    = label;
        op.category = category;
        op.desc     = desc;
        op.kind     = kind;
        op.exec     = std::move(exec);
        op.poll     = std::move(poll);
        op.checked  = std::move(checked);
        m_operators.Register(std::move(op));
    };

    /// @note ツールウィンドウの表示トグル。メニュー項目はチェックボックスとして描かれ押すと閉じられる
    ///       ため、開くことしかできない operator にすると「閉じられないメニュー」になってしまう。
    ///       enabled を省略すると反転、指定すればその値 (render.show_* と同じ規約)。
    const auto addWindowToggle = [this](const char* id, const char* label, const char* desc,
                                        bool EditorContext::*field) {
        EditorOperator op;
        op.id       = id;
        op.label    = label;
        op.category = "Tools";
        op.desc     = desc;
        op.kind     = OpKind::Action;

        OpParam enabledParam;
        enabledParam.name     = "enabled";
        enabledParam.type     = OpParamType::Bool;
        enabledParam.desc     = "省略すると現在値を反転する";
        enabledParam.required = false;
        op.params = { enabledParam };

        op.checked = [field](const OpContext& c, const OpArgs&) { return c.ctx.*field; };
        op.exec = [field](OpContext& c, const OpArgs& args) -> OpResult {
            bool& value = c.ctx.*field;
            const bool next = args.Has("enabled") ? args.GetBool("enabled") : !value;
            OpResult result;
            result.noChange = (next == value);
            value = next;
            return result;
        };
        m_operators.Register(std::move(op));
    };

    /// @name 共通の述語
    /// @note 同じ条件がメニュー・ホットキー・パレットへ別々に書かれていたため、ここで 1 度だけ定義し
    ///       必要な操作が共有する。
    const auto hasScene      = [](const OpContext& c, const OpArgs&) { return c.ctx.activeScene != nullptr; };
    const auto inPrefabEdit  = [](const OpContext& c, const OpArgs&) { return c.ctx.InPrefabEditMode(); };
    const auto hasSelection  = [](const OpContext& c, const OpArgs&) { return !c.ctx.selectedEntities.empty(); };
    const auto inEditor      = [this](const OpContext&, const OpArgs&) { return m_playMode.IsInEditor(); };

    /// @note 「シーンを編集できる状態か」— Play 中と Prefab 編集中は対象が違う。
    const auto canEditScene = [hasScene, inEditor](const OpContext& c, const OpArgs& a) {
        return hasScene(c, a) && inEditor(c, a);
    };
    const auto canEditSelection = [canEditScene, hasSelection](const OpContext& c, const OpArgs& a) {
        return canEditScene(c, a) && hasSelection(c, a);
    };

    /// @name File
    /// @note Prefab 編集中にシーンを新規作成/切り替えできてはいけない。
    ///       移行前はメニューだけがこれを禁じており、Ctrl+N とパレットは通っていた。
    add("scene.new", "New Scene", "File",
        "編集中のシーンを閉じて新しいシーンを作る。未保存の変更があれば確認する。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { RequestNewScene(); return OpResult::Ok(); },
        [inPrefabEdit](const OpContext& c, const OpArgs& a) { return !inPrefabEdit(c, a); });

    add("scene.open", "Open Scene...", "File",
        "ファイルダイアログからシーンを開く。未保存の変更があれば確認する。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { RequestOpenSceneFromDialog(); return OpResult::Ok(); },
        [hasScene, inPrefabEdit](const OpContext& c, const OpArgs& a) { return hasScene(c, a) && !inPrefabEdit(c, a); });

    /// @note Prefab 編集中は SaveScene が SavePrefabEdit へ読み替わるため、ここでは禁じない。
    ///       未保存アセットも一緒に書くのは、Ctrl+S が «今の編集を全部残す» つもりで押されるため
    ///       (Material 配列から開いた .mat のインライン編集は «Save .mat» を押さないと消えていた)。
    add("scene.save", "Save", "File",
        "現在のシーンと未保存のアセットを保存する (Prefab 編集中は編集中の Prefab を保存する)。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) {
            AssetDirtyRegistry::SaveAll();
            SaveScene();
            return OpResult::Ok();
        },
        hasScene);

    add("scene.save_as", "Save Scene As...", "File",
        "保存先を選び直してシーンを保存する。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { SaveSceneAsDialog(); return OpResult::Ok(); },
        [hasScene, inPrefabEdit](const OpContext& c, const OpArgs& a) { return hasScene(c, a) && !inPrefabEdit(c, a); });

    add("asset.save_all", "Save All Assets", "File",
        "未保存の編集を持つアセットをまとめて保存する。",
        OpKind::Action,
        [](OpContext&, const OpArgs&) { AssetDirtyRegistry::SaveAll(); return OpResult::Ok(); },
        [](const OpContext&, const OpArgs&) { return AssetDirtyRegistry::HasAny(); });

    add("app.exit", "Exit", "File",
        "エディターを終了する。未保存の変更があれば確認する。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { RequestExit(); return OpResult::Ok(); });

    /// @name Edit
    add("edit.undo", "Undo", "Edit",
        "直前の編集を取り消す。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) { c.undo.Undo(); return OpResult::Ok(); },
        [](const OpContext& c, const OpArgs&) { return c.undo.CanUndo(); });

    add("edit.redo", "Redo", "Edit",
        "取り消した編集をやり直す。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) { c.undo.Redo(); return OpResult::Ok(); },
        [](const OpContext& c, const OpArgs&) { return c.undo.CanRedo(); });

    /// @note Mutation は Undo コマンドを返し、レジストリが 1 箇所で UndoStack へ積む。各操作が自分で
    ///       積むと、レジストリの検問 (Undo を残していない Mutation を指摘する) を素通りする経路が
    ///       残るため。SceneEditUtils の Make*Command で実体を共有し「積む」責務だけレジストリへ寄せる。
    add("edit.delete_selected", "Delete Selected", "Edit",
        "選択中の GameObject を削除する。",
        OpKind::Mutation,
        [](OpContext& c, const OpArgs&) {
            OpResult result;
            result.command = MakeDeleteSelectedCommand(c.ctx);
            return result;
        },
        canEditSelection);

    add("edit.duplicate", "Duplicate", "Edit",
        "選択中の GameObject を複製する。",
        OpKind::Mutation,
        [](OpContext& c, const OpArgs&) {
            OpResult result;
            result.command = MakeDuplicateSelectedCommand(c.ctx);
            return result;
        },
        canEditSelection);

    add("edit.copy", "Copy", "Edit",
        "選択中の GameObject をクリップボードへコピーする。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            CopySelectedToClipboard(c.ctx);
            return OpResult::Ok();
        },
        canEditSelection);

    add("edit.paste", "Paste", "Edit",
        "クリップボードの GameObject を貼り付ける。",
        OpKind::Mutation,
        [](OpContext& c, const OpArgs&) {
            OpResult result;
            result.command = MakePasteClipboardCommand(c.ctx);
            return result;
        },
        [canEditScene](const OpContext& c, const OpArgs& a) {
            return canEditScene(c, a) && HasGameObjectClipboard();
        });

    add("edit.paste_as_child", "Paste As Child", "Edit",
        "クリップボードの GameObject を、選択中のノードの子として貼り付ける。",
        OpKind::Mutation,
        [](OpContext& c, const OpArgs&) {
            OpResult result;
            result.command = MakePasteClipboardCommand(
                c.ctx, c.ctx.selectedEntities.size() == 1 ? c.ctx.selectedEntities[0]
                                                          : scene::EntityID{});
            return result;
        },
        [canEditScene](const OpContext& c, const OpArgs& a) {
            return canEditScene(c, a) && HasGameObjectClipboard();
        });

    add("edit.rename", "Rename", "Edit",
        "Hierarchy で選択中のノード名をインライン編集する。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            c.ctx.requestRenameSelected = true;
            return OpResult::Ok();
        },
        [canEditScene](const OpContext& c, const OpArgs& a) {
            return canEditScene(c, a) && c.ctx.selectedEntities.size() == 1;
        });

    /// @name Selection
    add("select.all", "Select All", "Selection",
        "ロックされていない全 GameObject を選択する。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            std::vector<scene::EntityID> all;
            for (auto& go : c.ctx.activeScene->GameObjects())
                if (!c.ctx.IsLocked(go.GetID()))
                    all.push_back(go.GetID());
            /// @note 全選択で Hierarchy を先頭へ飛ばさない (どこを見ていたか分からなくなる)。
            SelectEntities(c.ctx, std::move(all), SelectionReveal::Skip);
            return OpResult::Ok();
        },
        canEditScene);

    add("select.clear", "Clear Selection", "Selection",
        "選択を解除する。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            ClearEntitySelection(c.ctx);
            return OpResult::Ok();
        },
        hasSelection);

    add("select.back", "Selection Back", "Selection",
        "選択履歴を 1 つ前へ戻る。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { NavigateSelectionHistory(-1); return OpResult::Ok(); });

    add("select.forward", "Selection Forward", "Selection",
        "選択履歴を 1 つ先へ進む。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { NavigateSelectionHistory(1); return OpResult::Ok(); });

    /// @name Viewport
    add("view.frame_selected", "Frame Selected", "Viewport",
        "選択中のオブジェクトが収まる位置へ Scene View カメラを寄せる。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            math::Vector3 center{};
            float radius = 0.0f;
            if (!ComputeSelectionBounds(c.ctx, center, radius))
                return OpResult::Err("NO_BOUNDS", "選択からバウンズを計算できませんでした");
            c.ctx.focusTargetPosition    = center;
            c.ctx.focusTargetRadius      = radius;
            c.ctx.requestFocusOnSelected = true;
            return OpResult::Ok();
        },
        hasSelection);

    /// @note 軸ビューはナビゲーションギズモのクリックでしか行けず、マウスドラッグ中は切り替えられ
    ///       なかった。メニュー・パレット・ホットキー・AI の 4 面から同じ経路で呼べるようにする。
    add("view.toggle_projection", "Toggle Orthographic", "Viewport",
        "Scene View を遠近投影 / 平行投影で切り替える。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            if (!c.ctx.editorCamera)
                return OpResult::Err("NO_CAMERA", "Scene View カメラがありません");
            SetEditorCameraProjection(c.ctx,
                IsEditorCameraOrthographic(c.ctx) ? renderer::ProjectionMode::Perspective
                                                  : renderer::ProjectionMode::Orthographic);
            return OpResult::Ok();
        },
        {},
        [](const OpContext& c, const OpArgs&) { return IsEditorCameraOrthographic(c.ctx); });

    struct AxisViewEntry { const char* id; const char* label; AxisView view; };
    static constexpr AxisViewEntry kAxisViews[] = {
        { "view.axis_front",  "View Front",  AxisView::Front  },
        { "view.axis_back",   "View Back",   AxisView::Back   },
        { "view.axis_left",   "View Left",   AxisView::Left   },
        { "view.axis_right",  "View Right",  AxisView::Right  },
        { "view.axis_top",    "View Top",    AxisView::Top    },
        { "view.axis_bottom", "View Bottom", AxisView::Bottom },
    };
    for (const AxisViewEntry& entry : kAxisViews) {
        const AxisView view = entry.view;
        add(entry.id, entry.label, "Viewport",
            "注視点を保ったまま、その軸の真正面へ視点を向ける。",
            OpKind::Action,
            [view](OpContext& c, const OpArgs&) {
                if (!c.ctx.editorCamera)
                    return OpResult::Err("NO_CAMERA", "Scene View カメラがありません");
                SetEditorCameraAxisView(c.ctx, view);
                return OpResult::Ok();
            });
    }

    /// @name Gizmo
    /// @note 右ドラッグ中の W/A/S/D はカメラのフライ移動でギズモ切替と衝突するため、押下中は無効にする
    ///       (Unity と同じ調停)。poll に置くことでコマンドパレットからも同じ条件で淡色表示される。
    const auto gizmoEnabled = [inEditor](const OpContext& c, const OpArgs& a) {
        return !ImGui::IsMouseDown(ImGuiMouseButton_Right) && inEditor(c, a);
    };

    /// @note ギズモモードは排他選択なので、checked が無いと AI から見て「切り替えたつもりで既に
    ///       そのモードだった」と「切り替わっていない」が区別できない。op.list の checked で読める。
    add("gizmo.move", "Gizmo: Move", "Gizmo",
        "ギズモを移動モードにする。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            c.ctx.gizmoMode = EditorContext::GizmoMode::Translate;
            return OpResult::Ok();
        }, gizmoEnabled,
        [](const OpContext& c, const OpArgs&) {
            return c.ctx.gizmoMode == EditorContext::GizmoMode::Translate;
        });

    add("gizmo.rotate", "Gizmo: Rotate", "Gizmo",
        "ギズモを回転モードにする。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            c.ctx.gizmoMode = EditorContext::GizmoMode::Rotate;
            return OpResult::Ok();
        }, gizmoEnabled,
        [](const OpContext& c, const OpArgs&) {
            return c.ctx.gizmoMode == EditorContext::GizmoMode::Rotate;
        });

    add("gizmo.scale", "Gizmo: Scale", "Gizmo",
        "ギズモをスケールモードにする。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            c.ctx.gizmoMode = EditorContext::GizmoMode::Scale;
            return OpResult::Ok();
        }, gizmoEnabled,
        [](const OpContext& c, const OpArgs&) {
            return c.ctx.gizmoMode == EditorContext::GizmoMode::Scale;
        });

    add("gizmo.toggle_space", "Gizmo: World / Local", "Gizmo",
        "ギズモの座標系を World と Local で切り替える。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            c.ctx.gizmoSpace = (c.ctx.gizmoSpace == EditorContext::GizmoSpace::World)
                ? EditorContext::GizmoSpace::Local
                : EditorContext::GizmoSpace::World;
            return OpResult::Ok();
        }, gizmoEnabled,
        /// @note チェック = Local (World が既定なので、切り替わっている側を示す)。
        [](const OpContext& c, const OpArgs&) {
            return c.ctx.gizmoSpace == EditorContext::GizmoSpace::Local;
        });

    add("gizmo.toggle_pivot", "Gizmo: Pivot / Center", "Gizmo",
        "ギズモの基準点を Pivot と Center で切り替える。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            c.ctx.gizmoPivot = (c.ctx.gizmoPivot == EditorContext::GizmoPivot::Pivot)
                ? EditorContext::GizmoPivot::Center
                : EditorContext::GizmoPivot::Pivot;
            return OpResult::Ok();
        }, gizmoEnabled,
        [](const OpContext& c, const OpArgs&) {
            return c.ctx.gizmoPivot == EditorContext::GizmoPivot::Center;
        });

    add("gizmo.toggle_snap", "Toggle Grid Snap", "Gizmo",
        "グリッドスナップの ON / OFF を切り替える。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            c.ctx.snapEnabled = !c.ctx.snapEnabled;
            return OpResult::Ok();
        }, gizmoEnabled,
        [](const OpContext& c, const OpArgs&) { return c.ctx.snapEnabled; });

    /// @name Play
    /// @note スクリプトのコンパイル/リロード中は Play を開始できない (旧実装は Play ツールバーの
    ///       ボタンだけがこの条件を持ち、Ctrl+P とコマンドパレットは素通りしていた)。
    const auto scriptBusy = [](const OpContext& c, const OpArgs&) {
        return c.ctx.scriptReloadBusy
            || c.ctx.hotReloadState == EditorContext::HotReloadState::Compiling
            || c.ctx.hotReloadState == EditorContext::HotReloadState::Reloading;
    };

    add("play.start", "Play", "Play",
        "Play モードを開始する。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { StartPlayMode(); return OpResult::Ok(); },
        [hasScene, inEditor, scriptBusy](const OpContext& c, const OpArgs& a) {
            return hasScene(c, a) && inEditor(c, a) && !scriptBusy(c, a);
        });

    add("play.stop", "Stop", "Play",
        "Play モードを停止して編集状態へ戻す。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { StopPlayMode(); return OpResult::Ok(); },
        [hasScene, inEditor](const OpContext& c, const OpArgs& a) { return hasScene(c, a) && !inEditor(c, a); });

    /// @note 開始と停止を 1 キーへまとめたもの。停止は常に許し、開始だけ scriptBusy を見る。
    add("play.toggle", "Play / Stop", "Play",
        "Play モードを開始/停止する。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { TogglePlayMode(); return OpResult::Ok(); },
        [hasScene, inEditor, scriptBusy](const OpContext& c, const OpArgs& a) {
            if (!hasScene(c, a)) return false;
            return inEditor(c, a) ? !scriptBusy(c, a) : true;
        });

    add("play.pause", "Pause", "Play",
        "Play 中のシーンを一時停止する / 再開する。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { m_playMode.Pause(); return OpResult::Ok(); },
        [inEditor](const OpContext& c, const OpArgs& a) { return !inEditor(c, a); });

    add("play.step", "Step", "Play",
        "一時停止中のシーンを 1 フレームだけ進める。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { m_playMode.RequestStep(); return OpResult::Ok(); },
        [this](const OpContext&, const OpArgs&) { return m_playMode.IsPaused(); });

    add("script.reload", "Reload Scripts", "Play",
        "スクリプト DLL を再ビルドしてホットリロードする。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            c.ctx.requestScriptReload = true;
            return OpResult::Ok();
        },
        [scriptBusy, inEditor](const OpContext& c, const OpArgs& a) {
            return inEditor(c, a) && !scriptBusy(c, a);
        });

    /// @name Tools
    add("tools.map_editing_mode", "Map Editing Mode", "Tools",
        "マップ編集用のレイアウトへ切り替える。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) {
            c.ctx.requestMapEditingModeToggle = true;
            return OpResult::Ok();
        },
        canEditScene,
        [](const OpContext& c, const OpArgs&) { return c.ctx.mapEditingMode; });

    add("tools.build_settings", "Build Settings...", "Tools",
        "ビルド設定を開く。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) { c.ctx.requestOpenBuildSettings = true; return OpResult::Ok(); });

    add("tools.project_settings", "Project Settings...", "Tools",
        "プロジェクト設定を開く。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) { c.ctx.requestOpenProjectSettings = true; return OpResult::Ok(); });

    add("tools.analysis", "Analysis", "Tools",
        "解析パネルを開く。",
        OpKind::Action,
        [](OpContext& c, const OpArgs&) { c.ctx.requestOpenAnalysis = true; return OpResult::Ok(); });

    addWindowToggle("tools.terrain", "Terrain Tool",
        "地形編集ツールウィンドウの表示。", &EditorContext::showTerrainTool);

    /// @name Panels
    add("panel.command_palette", "Command Palette", "Panels",
        "コマンドパレットを開く。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) { m_commandPaletteOpen = true; return OpResult::Ok(); });

    add("panel.shortcut_list", "Shortcut List", "Panels",
        "ショートカット一覧オーバーレイの表示を切り替える。",
        OpKind::Action,
        [this](OpContext&, const OpArgs&) {
            m_showShortcutsOverlay = !m_showShortcutsOverlay;
            return OpResult::Ok();
        },
        {},
        [this](const OpContext&, const OpArgs&) { return m_showShortcutsOverlay; });

    /// @name EditorApp に依存しない操作群
    /// @note パネル表示・UI スケール・Prefab 編集モード。m_panels を引くため
    ///       EditorApp のメンバー関数だが、量があるので別ファイルへ置いてある。
    RegisterPanelOperators();

    /// @note 以下は EditorContext と UndoStack だけで完結し EditorApp のメンバーを知る必要がないため
    ///       分ける。操作を足すたびに EditorApp が肥大化しない形にしておく (パネルの実装詳細にも依存しない)。
    RegisterNodeOperators(m_operators);
    RegisterRenderOperators(m_operators);
    RegisterInspectorOperators(m_operators);
    RegisterAnimationOperators(m_operators);
    RegisterBehaviorTreeOperators(m_operators);
    RegisterAssetOperators(m_operators);
    RegisterAssetBrowserOperators(m_operators);
    RegisterDocumentOperators(m_operators);
    RegisterTerrainOperators(m_operators);
    RegisterEffectOperators(m_operators);
    RegisterClothOperators(m_operators);
    RegisterSfxOperators(m_operators);
    RegisterDeveloperOperators(m_operators);
}

/// @brief 4 面 (メニュー / ホットキー / パレット / AI) が共有する実行文脈。
/// @note 生成箇所を 1 つにしておかないと、面ごとに違う UndoStack や EditorContext を渡す事故が起きうる。
OpContext EditorApp::MakeOpContext()
{
    return OpContext{ m_ctx, m_undoStack };
}

bool EditorApp::CanInvokeOperator(std::string_view id, const OpArgs& args)
{
    const OpContext context = MakeOpContext();
    return m_operators.CanInvoke(id, context, args);
}

OpResult EditorApp::InvokeOperator(std::string_view id, const OpArgs& args)
{
    OpContext context = MakeOpContext();
    return m_operators.Invoke(id, context, args);
}

} // namespace fbzz::editor
