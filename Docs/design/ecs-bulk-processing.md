<!-- @file    ecs-bulk-processing.md -->
<!-- @brief   大量個体の同期、破棄、生成と連続領域の走査を既存の制作契約へ接続する。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-07 -->
# 大量個体の処理

GameObject、EntityID の世代、Component の寿命 token、Scene の保存順と C# API は維持する。型別の安定アドレス chunk を使い、全面的な archetype 移行や GameObject の公開 Transform の移設は行わない。

## 破棄

Cleanup の対象と子孫は従来の入力順、子孫から親の順に処理する。OnDestroy の時点で以前に破棄された個体は検索、GameObjects、GameObjectCount から見えない。現在の個体は callback が終了するまで生存する。再入で同じ個体を二度破棄しない。

Scene の所有配列内の位置を GameObject が保持し、個体の解放でその位置を空にする。GameObjects は空の所有位置を飛ばし、生成は末尾へ追加する。最外側の破棄操作が終了してから一度だけ空位置を詰め、残った個体の相対順を維持する。callback から並べ替える場合は先に詰める。Entity の再利用は世代で区別し、旧所有位置と新しい個体を混同しない。

親子関係の解除、Script callback、GPU 資源の返却、managed 参照の退役は個体ごとの既存順を保つ。兄弟配列の解除処理は別の操作であり、所有配列の一括整理と混同しない。

## 同期と生成

Transform の同期は [座標契約](script-transform-contract.md)に従う。対象がない gameplay pose system は world の描画補間を上書きしない。必要な入力同期と書込後の同期を段階ごとに実行する。

Prefab の一括生成は同一要求で準備した入力と復元用状態を共有する。注入された生成 callback を省略せず、失敗時は要求で追加した個体だけを除去して既存の親子、姿勢、順序を復元する。別要求で保存されたアセットの変更を古い準備で隠さない。

## 連続領域

Component chunk の EntityID span と Component span は同じ長さ、同じ添字で対応する。末尾 chunk の未使用領域を公開しない。走査中の Component の追加、削除、Clear は禁止する。const Scene は const Component span を渡す。

Renderer 候補は型別の密 ID 列と各 ID の現在の所有位置だけを照合し、所属または所有位置が変わった集合だけを Scene 所有順へ並べ直す。Mesh と Skinned の和集合、Skinned のみ、Terrain を加えた和集合を区別し、同一 Entity は一度だけ返す。通常の照合とコピーは対象 Renderer 数に比例し、骨だけの GameObject や空の Entity slot は走査しない。

索引はスレッドごとに最大 4 Scene の ID と所有位置だけを保持し、Scene の既存寿命世代で Clear・移動・同一アドレスの再利用を区別する。削除、複製、密配列の swap、並べ替えも毎回の完全一致照合で検出するため、既存の inline Add/Remove や Scene の配置へ失効用フィールドを足さない。索引の入替えや Scene の寿命後にも借用 Scene/Component を参照しない。型別 span は callback のない照合中だけ借り、呼出し元へ ID をコピーする。破棄 callback からの照会は、その時点で生存する所有位置の穴を含めて扱う。

無効 Renderer、非アクティブな owner、未設定 mesh も候補に残し、描画側は Entity、Component、active、Transform、material をその都度解決する。同一 frame の値変更を索引で隠さず、Script を含む frame geometry の保守的な再検証は省略しない。公開済み ID コピーと RenderScene snapshot は後続の変更から独立する。`Scene::RendererIndexRebuild` scope は集合の再整列時だけ発生し、公開 Profiler snapshot で照合 hit と区別できる。

一括 Transform 更新の密な作業領域は、独立した葉ルートを重複なく操作する場合に使う。親子や重複参照が含まれる要求は従来の入力順同期へ戻す。全件の寿命、物理 body、有限値を検証してから確定し、後半の不正値で前半だけ変更しない。

## 描画 mesh の shared fragment

[MassEntityのshared fragment](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine)を参考に、meshのbuffer/count/stride/local boundsを抽出期間の共有値にする。同じmeshを使う個体はこの値を共有し、描画用の密なitem列へ直接出力する。候補数で出力領域を準備し、中間の個体列を転記しない。Scene順、slot順、previous world、当該frameの変形buffer判定を保つ。材質とshader/texture資源の解決は元の境界で行う。共有値は抽出を跨いでcacheせず、同一frameの公開値編集と既存snapshotの独立性を保つ。

Animatorの個体間バッチはStandaloneの全体比較で改善が再現しなかったため採用しない。全Componentのarchetype移設や並列Scene更新は行っていない。
