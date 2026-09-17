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

/// Inspector が持っていた操作 (Transform のコピー/貼り付け/リセット、
/// コンポーネントの並べ替え)。移行前は Inspector の右クリックメニューにしか無く、
/// AI からは到達できなかった。
void RegisterInspectorOperators(OperatorRegistry& registry);

/// Animation Graph の操作 (Controller の保存、キャンバス自動整列、ステート座標)。
/// 移行前は AI から Animator の構造 (state / transition / motion / parameter) は
/// 編集できるのに、**保存する手段が無かった**。編集だけして保存できない状態は、
/// 「AI が直したはずなのに次回起動すると戻っている」という形でしか現れない。
void RegisterAnimationOperators(OperatorRegistry& registry);

/// アセット共通の操作 (dirty なアセットの個別保存)。
void RegisterAssetOperators(OperatorRegistry& registry);
/// AssetBrowser の更新・参照先表示・インポート設定モーダルを公開する。
/// @note AssetBrowser は一覧取得だけではなく、人が参照欄から辿る操作も AI の検証ループに必要になる。
void RegisterAssetBrowserOperators(OperatorRegistry& registry);

/// Behavior Tree の操作。
/// @note bt_* の AI ツール 29 個をすべて Operator にはしない。大半は「パスと id と名前を指定して値を書く」API でコマンドパレットから呼びたい場面が無く、層が 1 つ深くなるだけになる。実装の共有は Editor/GraphEditor/BehaviorTreeOps.hpp が担う。
void RegisterBehaviorTreeOperators(OperatorRegistry& registry);

/// デバッグ表示とビューモード。
/// 移行前は Debug メニューからしか切り替えられず、**AI は viewport を撮れるのに
/// 診断用の表示を出せなかった** (NavMesh の穴も Collider の形も画に出せない)。
void RegisterRenderOperators(OperatorRegistry& registry);

/// GameObject そのものを対象にする操作 (リネーム)。
/// 移行前は Hierarchy パネルと AI が別々の実装を持ち、Undo の重さも履歴ラベルも
/// 経路によって違っていた。
void RegisterNodeOperators(OperatorRegistry& registry);

/// Terrain のデータ編集操作。
/// Terrain のリサイズは heightData / splat / holeData を同時に整合させる必要があり、
/// columns / rows の直接書き換えを AI から許すと描画・物理の前提を壊すため、
/// TerrainComponent::Resize() とスナップショット Undo を 1 つの操作へ集約する。
void RegisterTerrainOperators(OperatorRegistry& registry);

/// アセットを「開く」操作と Prefab 編集モードの出入り、AI Command Bus の待受制御。
/// 移行前、.animcontroller / .behaviortree / .vfx を開く経路は AssetBrowser の
/// ダブルクリックにしかなく、AI は中身を編集できるのに開くことができなかった
/// (パネルが開いていることを前提にした操作を、自分で前提を満たせない)。
void RegisterDocumentOperators(OperatorRegistry& registry);

/// 手続き効果音 (.synth) の生成・調整・プレビュー・保存。
/// @note AI は音を聴けないため、パラメーターを動かした結果を数値で読み返せないと反復オーサリングが成立せず、読み取り (sfx.inspect) も Query として登録簿へ載せ editor.op.query からそのまま引けるようにする。
void RegisterSfxOperators(OperatorRegistry& registry);

/// ParticleEmitter / VFXComponent の再生制御。
/// Inspector の再生ボタンと同じコンポーネント API を使い、AI からも Play / Stop /
/// Restart / Clear / Burst / Trigger を一つの操作入口で呼べるようにする。
void RegisterEffectOperators(OperatorRegistry& registry);

/// @brief 静的メッシュの ClothAsset 書き出し。
void RegisterClothOperators(OperatorRegistry& registry);

} // namespace fbzz::editor
