# Map Editing Mode

## 目的

Map Editing Mode は通常 Editor の Workspace を一時保存し、Scene Viewport を中心とした
Terrain / Water / Detail / Foliage 専用レイアウトへ切り替える。

## レイアウト

- 左: `Scene Hierarchy`
- 中央: `Scene`
- 右: `Inspector` / `Map Tools` タブ
- 下: `Asset Browser`
- 下端: `StatusBar`

Map Mode 終了時は、切替前の Dock 配置と各 Panel の表示状態を復元する。
Map Mode 中は ImGui のレイアウト自動保存を停止し、専用配置が通常レイアウトファイルへ
上書きされることを防ぐ。

## Map Tools

`Sculpt / Paint / Water / Detail / Foliage` は排他的に切り替える。
選択中ツールだけが Viewport 入力を受け取り、設定 UI は既存 Tool の `DrawContent`
系 API を共有する。

通常 Editor の Inspector はデータ構造とアセット参照の編集場所として残し、
Map Editing Mode はブラシ・配置・Bake などの作業導線を担当する。

## Map Filter

- Hierarchy: `Map Objects Only` が既定ON。Terrain / Water / Detail / Foliageを持つ
  GameObjectだけを表示する。
- Inspector: `Map Components Only` が既定ON。TransformとMap関連Componentだけを表示する。
- どちらもチェックを外せば、Map Modeのまま通常表示へ戻せる。
- Asset InspectorはMaterialやTexture設定に必要なため、フィルター中も通常どおり表示する。
