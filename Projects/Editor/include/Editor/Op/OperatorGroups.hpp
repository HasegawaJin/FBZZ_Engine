/// @file    OperatorGroups.hpp
/// @brief   標準 Operator の登録関数群。EditorApp::RegisterBuiltinOperators がまとめて呼ぶ。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note EditorApp のメンバーを叩く必要がある操作 (New Scene / Play 等) と、EditorContext + UndoStack だけで完結する操作を分ける。後者は EditorApp を一切知らない自由関数として書けるため、操作の追加が EditorApp の肥大化に直結しない。
/// @see Docs/design/editor-operator-model.md
#pragma once
#include <Editor/Op/EditorOperator.hpp>

namespace fbzz::editor {

/// @brief Inspector の Transform 操作とコンポーネントの並べ替え。
void RegisterInspectorOperators(OperatorRegistry& registry);

/// @brief Animation Graph の保存、自動整列とステート座標操作。
void RegisterAnimationOperators(OperatorRegistry& registry);

/// @brief dirty なアセットの個別保存。
void RegisterAssetOperators(OperatorRegistry& registry);
/// @brief AssetBrowser の更新・参照先表示・インポート設定モーダルを公開する。
/// @note AssetBrowser は一覧取得だけではなく、人が参照欄から辿る操作も AI の検証ループに必要になる。
void RegisterAssetBrowserOperators(OperatorRegistry& registry);

/// @brief Behavior Tree の操作。
/// @note bt_* の AI ツール 29 個をすべて Operator にはしない。大半は「パスと id と名前を指定して値を書く」API でコマンドパレットから呼びたい場面が無く、層が 1 つ深くなるだけになる。実装の共有は Editor/GraphEditor/BehaviorTreeOps.hpp が担う。
void RegisterBehaviorTreeOperators(OperatorRegistry& registry);

/// @brief デバッグ表示とビューモード。
void RegisterRenderOperators(OperatorRegistry& registry);

/// @brief GameObject の操作と Undo 記録。
void RegisterNodeOperators(OperatorRegistry& registry);

/// @brief Terrain のデータ編集操作。
/// @note リサイズは heightData / splat / holeData を同時に整合させ、スナップショット Undo を記録する。
void RegisterTerrainOperators(OperatorRegistry& registry);

/// @brief アセットを開く操作、Prefab 編集モードと AI Command Bus の待受制御。
void RegisterDocumentOperators(OperatorRegistry& registry);

/// @brief 手続き効果音 (.synth) の生成・調整・プレビュー・保存。
/// @note AI は音を聴けないため、パラメーターを動かした結果を数値で読み返せないと反復オーサリングが成立せず、読み取り (sfx.inspect) も Query として登録簿へ載せ editor.op.query からそのまま引けるようにする。
void RegisterSfxOperators(OperatorRegistry& registry);

/// @brief ParticleEmitter / VFXComponent の再生制御。
/// @note Inspector と AI は同じコンポーネント API と操作入口を共有する。
void RegisterEffectOperators(OperatorRegistry& registry);

/// @brief 静的メッシュの ClothAsset 書き出し。
void RegisterClothOperators(OperatorRegistry& registry);

/// @brief 開発者モードの切り替えと、開発者モードでだけ出す操作 (わざと落とす など)。
/// @see Docs/design/developer-mode.md
void RegisterDeveloperOperators(OperatorRegistry& registry);
/// @brief PIX UI 起動と起動時 GPU capture readiness の読み取り。
/// @see Docs/design/pix-profiling.md
void RegisterPixOperators(OperatorRegistry& registry);

} /// @note namespace fbzz::editor
