# コンポーネント登録・Inspector・シリアライズ自動化設計

## 目的

新しい単純なコンポーネントを、コンポーネント本体の `Reflect()` と
`ComponentRegistry.hpp` の登録1行だけで利用可能にする。

## 単一情報源

`ComponentRegistry` の各要素は以下を保持する。

- C++型
- 永続化テーブル名
- Editor表示名
- Editorカテゴリ
- Inspector方式（Reflect自動生成／既存の特殊UI／非表示）
- シリアライズ方式（Reflect自動生成／既存の特殊処理）
- Add Componentへの公開可否

`ComponentList` はRegistryから型だけを抽出して生成する。SceneのSoAストレージと
Script DLL ABIは従来どおり `ComponentList` を参照する。

## ハイブリッド方針

通常フィールドは `Reflect()` をInspectorとTOML保存・復元の両方へ利用する。
以下のように副作用や可変長データを持つ型は既存の特殊処理を維持する。

- Mesh／Materialのロードとプレビュー
- Colliderのメッシュ構築と深いUndo
- CameraのLayerMask
- Terrain／Foliage／Navigationの配列、Bake、編集ツール
- AnimatorControllerなどの複合アセット編集

特殊処理はRegistryの `Custom` 方針で明示し、単純な新型は既定の `Automatic` を使う。

初期移行ではUI 6型、Audio Source／Listener、LifetimeをAutomaticへ移した。
これらは個別InspectorとSceneSerializerの型別分岐を持たない。

## 新しい単純コンポーネントの追加

1. `Components/XxxComponent.hpp` を追加し、`GetTypeName()` と `Reflect()` を実装する。
2. `ComponentRegistry.hpp` の `ComponentRegistry` に `FBZZ_COMPONENT(...)` を1行追加する。

これによりScene格納、Add Component、Inspector、基本TOML保存・復元が自動接続される。
特殊な初期化やEditor操作が必要になった場合だけEditor側の追加フックへ昇格する。
