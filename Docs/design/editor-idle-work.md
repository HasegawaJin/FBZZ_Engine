<!-- @file    editor-idle-work.md -->
<!-- @brief   Editor の未変更フレームで行う処理とキャッシュ失効・Undo の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-10 -->
# Editor の未変更フレーム

2026-10-10 の「全て修正して」に従い、AssetBrowser、Hierarchy、Terrain Inspector、Memory History と ImGui の入力所有・追加ウィンドウを対象にする。機能を停止して見かけの負荷を減らさず、変更時の処理と未変更時の処理を分ける。

## AssetBrowser

List の絞り込み結果と表示順はパネルが所有する。検索語、種別フィルタ、横断検索の状態、列のソート条件、ディレクトリの更新、検索結果の更新が変わった場合だけ再構築する。横断検索は索引のスコア順を保つ。

サイズと更新日時はパスごとに保持し、列の表示と比較で同じ値を使う。RefreshDirectory、InvalidateTreeCache、横断検索結果の変更で失効させる。Modified を含むファイル通知は共有の metadata generation で全パネルへ伝え、選択と表示位置を保って stat と検索キャッシュだけを失効させる。通知が失われた場合は既存の watcher overflow と手動 Refresh で復旧する。未変更フレームで stat や日時の文字列化を行わない。

## Hierarchy

Scene の構造編集を描画後へ遅延する既存の契約を維持する。1 回の巡回中は Entity index が一意なので、訪問済み判定を index のビット配列で行う。畳んだ子孫と非表示の生成物も訪問済みにし、救済ループで重複表示しない。

非表示の生成物数は巡回単位の memo に保持する。Show Generated Objects の有効時は数えない。フレーム間には保持せず、Play の生成・削除や表示切替が次フレームへ即時反映される。

## Terrain Undo

Terrain 本文は汎用の毎フレーム全体 snapshot を使わない。scalar と参照欄は一時値へ描画し、変更を本体へ適用する直前に最初の snapshot を採る。配列を書き換えるボタンも同じ transaction を使う。

ドラッグ中は同じ snapshot を保持し、ActiveID が変わった時点で操作を確定する。クリック操作は当該フレームで確定する。実際に変更した高さ・splat・hole と authoring 値を比較し、変更が無ければ Undo を積まない。Undo 無効時には snapshot を作らない。共通カードの enabled / Reset / Paste / Remove は既存の Undo 経路を維持する。

## Memory History

非表示時も毎フレームの履歴とスパイク採取を維持する。ResourcePool の MemoryDebug が保持する生存 bytes / count を合算し、RENDERER タグへ従来と同じ現在値を加える。詳細一覧は Memory Debug の表示・リーク差分・終了診断で取得し、履歴採取のためには列挙しない。

## 描画入力

[Render Thread](render-thread.md) の入力所有と追加 viewport の寿命契約に従う。完了済みの描画バッファを再利用し、追加ウィンドウの通常フレームでは UI 開始時に待機しない。

## 共通化と追加対象

続く「修正して、同じような処理はある程度 Util にまとめて」に従い、Terrain 以外の Inspector、材質プレビュー、制御点、Prefab 件数、横断検索、アニメーション、Behavior Tree を追加対象にする。code-simplifier の方針で、用途の違う処理を巨大な cache 基盤へまとめず、同じ更新判定・入力索引・値の保存だけを共有する。

`RevisionCache` は処理済みの値キーと寿命世代を持ち、成功後のみ Commit する。検索結果は asset 索引世代と検索語で失効し、エントリの vector が再配置されても古い pointer を保持しない。Scene の名前・Prefab の公開フィールドは直接書き換わるため、共有する SceneQueryIndex が各 UI フレームで軽い一致確認を行い、変更時だけ検索用の正規化と Prefab 集計を作り直す。Undo revision だけを変更検出に使わない。

Prefab の参照キーは AssetDatabase の参照解決 revision とプロジェクト・asset 世代でも失効する。照会自身による GUID の初登録も再同期し、名前検索の世代は資産変更だけでは進めない。遅延登録を揃える再解決は最大2巡に制限し、並行する登録が続いた場合は巡回開始時の古い revision を保持して次の照会でも失効させる。

Inspector の編集前値は共有の値 snapshot の再利用領域に保持し、入力操作を検出した時だけ Component の Undo 状態を構築する。大きい runtime 配列を持つ型は、作者設定だけを型別の非変異な投影へ渡す。最初の入力フレームで変更された値も編集前へ戻して保存し、外部から変更された値は UI を描く前に取り込む。既存の比較・Undo の単位と disabled / clipboard / component menu の経路を維持する。

Reflection の保存範囲が完全でも、読み取り中に clamp・ID 採番・dirty 更新が動く場合があるため、live Component の Reflect を捕捉に使わない。小型型と保存対象の配列だけを持つ型は従来のコピー契約を再利用領域で維持し、ParticleEmitter の専用 codec の値などを推測で落とさない。Water は既存の authoring コピーを共用し、Undo を適用するときだけ runtime cache の失効と dirty 更新を行う。

入力開始・継続・終了の判定は `InspectorEditSession` を汎用・Material・RigidBody・専用 snapshot の4経路で共用する。別の対象を描画しても入力 ID が継続する間は元の対象を保持し、確定後に保存値の所有者が Cancel する。保存・比較・適用は各型の契約を維持する。Prefab 索引の資産世代も共通の値キーで読み、キーの再解決と件数の再集計を同じ更新入口から行う。

材質プレビューは実際の入力と資源世代が一致する静止フレームでは前の RT を表示する。時間を使うプレビューと操作中の視点・照明変更は更新を継続する。アニメーションは clip / skeleton のトラック表を共有 Util に分離し、アセット世代と配列再配置で失効する。Behavior Tree の子数は1回の表示構築でまとめて集計し、ノード表示と接続可否で同じ結果を使う。TargetPoint は Scene の Component 索引から列挙する。

プレビューの GPU ハンドル寿命は `PreviewResourceLifetime` で共用する。所有者と reset 世代の変更によるハンドル破棄は、Shader・asset・project の変更による内容の失効と区別する。Surface・Skinned・Terrain・Water のメッシュ保持は既存の `PreviewMeshCache` へ揃え、GPU データの破棄と成功済み cache の失効は同じ入口から行う。

## 検証

時間を合否条件に使わず、既存の UI と出力境界でキャッシュの失効、生成物の表示、Terrain の変更前データ復元、詳細台帳と集計値の一致を検証する。ImGui は変更後の UI に提出済みの入力が汚染されないこと、処理完了後のバッファ再利用、追加 viewport の同期境界を CPU テストと実 DX12 テストで検証する。CPU 時間の改善幅は別の計測結果が得られるまで報告しない。
