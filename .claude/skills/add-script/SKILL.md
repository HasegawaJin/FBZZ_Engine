---
name: add-script
description: GreenWare (または他のプロジェクト) にゲームスクリプト (Script コンポーネント) を 1 本追加・削除する手順。「〇〇するスクリプトを作って」「コンポーネントを足す」「スクリプトを消す」のときに使う。1 スクリプト 1 ヘッダーで、生成物と .meta とシーン参照の扱いを含む。
---

# ゲームスクリプトの追加・削除

正本は `AGENTS.md` «スクリプト» 節と `Projects/Engine/include/Engine/Scene/Script.hpp` (マクロ一覧)。

## 追加

1. `<Project>/Assets/Scripts/<領域>/XxxComponent.hpp` を 1 本だけ作る (`.cpp` 不要)。4 行ヘッダー + `#pragma once`
2. `namespace sandbox { class XxxComponent : public Script { FBZZ_SCRIPT(XxxComponent) ... }; FBZZ_REFLECT(XxxComponent) }` (namespace を入れ子にしない。ScriptCodeGen が壊れる)
3. 依存コンポーネントは `FBZZ_REQUIRE_COMPONENT(...)`。公開フィールドは `FBZZ_FIELD*` / `FBZZ_REF`、名前に `m_` を付けない
4. エンジン機能は `ScriptProxy` のメンバー (`transform` / `input` / `physics` …) から呼ぶ。無ければ `add-script-proxy` スキル
5. Play は SceneSerializer を往復する。**シリアライズしないフィールドは Play 開始で既定値へ戻る**
6. 後始末は `OnDestroy` (破棄では `OnDisable` は呼ばれない)
7. `ScriptList.inl` / `DataAssetList.inl` は生成物。手で書かない (lint が `generated-file` で止める)

## 削除

- ヘッダーと `.meta` を消し、`ScriptList.inl` は再生成に任せる
- `grep -r "XxxComponent"` で全件: シーン (`*.scene`)・プレファブ (`*.prefab`)・他スクリプト・`Docs/`
- シーンやプレファブの参照はファイルを直接差分で直す (生成スクリプトで作り直さない)

## 検証

- スクリプト DLL はエディターの `build_run` (MCP) → `build_get_status` で diagnostics を読む
- 動作は `playtest` スキルでシナリオを書いて確かめる (例: `waitUntil` で `node_get_components` の値を見る)
