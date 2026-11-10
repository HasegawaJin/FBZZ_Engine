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

Scene の構造編集は描画後へ遅延する。Scene 世代と階層 revision が変わったときだけ、Scene 順のルートと親を失った救済対象を取り直す。訪問済み判定は巡回した行だけを世代付き EntityID で記録する。畳んだ枝や非表示の生成物の子孫は表示巡回から省く。

非表示の生成物数と展開矢印の有無は階層 revision の間だけ再利用する。作成・破棄・親付け・並べ替え・`SetRuntimeGenerated` が revision を進める。Show Generated Objects の切替は展開矢印の判定を失効させ、表示中は非表示件数を数えない。

選択・ロック・強制展開の所属判定は、描画単位で世代付きの EntityID を索引化する。親の有効状態は子へ渡し、行ごとの祖先探索を省く。複製・コピー・ドラッグの対象ルート抽出は共通で、同じ祖先の判定を再利用する。一括削除は既存の Scene バッチへまとめ、保存用配列の圧縮を一度だけ行う。

Scene View のアイコンは投影時の選択判定を描画へ引き渡す。複数選択のギズモも共通の対象ルート抽出を使い、選択済みの親と子を同じ操作で二重に動かさない。

## Viewport のスナップと選択

頂点・面スナップはメッシュの Component 索引を Scene 順で照合し、選択集合を問い合わせ中に共有する。頂点投影のビュー・投影行列は問い合わせの先頭で取得する。面スナップ・クリック選択・材質プレビューは、CPU ジオメトリを公開したアセットで同じローカル BVH を共有する。詳細判定は元の三角形順とワールド空間の交差を維持する。バインドポーズの頂点列は静的頂点列と分ける。未公開、規模上限を超える形状、非対応の変換では元の三角形走査を使う。

BVH の寿命は CPU ジオメトリの公開 revision で識別し、境界再計算でその公開を失効させる。Editor は最大8件かつ合計1,048,576三角形を保持する。初回の問い合わせで構築し、Scene View の空オブジェクトやアイコン、動く対象の評価には現フレームの Scene を使う。

## Dependency View と Behavior Tree

Dependency View は従来の保存済み GUID・パス検索をワーカーで行う。UI は一度に一件だけ投入する。アセットやプロジェクトが変わると古い走査を中断し、その結果を表示せず最新の要求を進める。Asset Browser の構造化された参照証拠と、このビューの文字列照合はそれぞれの対象範囲を維持する。

Behavior Tree の接続ドラッグは、ドラッグ元の向きに応じて祖先または子孫と子数上限をまとめて評価する。ノード表示とピンの減光は同じ候補集合を参照する。

## Terrain Undo

Terrain 本文は汎用の毎フレーム全体 snapshot を使わない。scalar と参照欄は一時値へ描画し、変更を本体へ適用する直前に最初の snapshot を採る。配列を書き換えるボタンも同じ transaction を使う。

ドラッグ中は同じ snapshot を保持し、ActiveID が変わった時点で操作を確定する。クリック操作は当該フレームで確定する。実際に変更した高さ・splat・hole と authoring 値を比較し、変更が無ければ Undo を積まない。Undo 無効時には snapshot を作らない。共通カードの enabled / Reset / Paste / Remove は既存の Undo 経路を維持する。

## 有効状態とシーン保存

Hierarchy と Inspector の有効切り替えは、対象の保存値だけを一つの履歴に積む。Editor 専用非表示中は保存値を更新し、編集画面では非表示を維持する。Prefab の差分表示は一覧を開いたときに対象階層だけを照合する。

メニュー、ショートカット、オートセーブからの保存は、Scene と Script を扱う所有スレッドで保存値を確定する。ワーカーは確定したデータの整形とファイル書き込みを担当する。完了後は保存済みテキストのハッシュを再利用し、保存開始後の編集があれば未保存扱いを維持する。オートセーブは外部 Terrain のデータをバックアップへ含める。Command Bus の `scene.save` は書き込み完了を待って結果を返す。

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
