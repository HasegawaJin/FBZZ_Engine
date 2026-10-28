<!-- @file    csharp-runtime-authoring.md -->
<!-- @brief   型付き Component 要件と Script の実行順・エディター機能の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-05 -->
# C# の実行 authoring

`[RequireComponent(typeof(RigidBody), typeof(HealthComponent))]` は同じ GameObject 上の必要な型を宣言する。複数属性と基底クラスの要件を集合として継承する。公開された native facade と同じゲーム世代の Script 型だけを認め、abstract Script 基底への要件は代入可能な具体個体が一つ以上あれば満たす。型名文字列は作者に要求しない。

欠損は Inspector・Play 開始・reload・実行前で具体型名と所有者を表示する。自動追加はしない。型の要件は Awake へ入る前にも照合し、欠損なら requirementsBlocked として停止する。設定参照は Awake で明示接続できるため、RequiredReference の検証時点とは分ける。無効・faulted の Component も型として存在するが、pending removal と別 GameObject の個体は要件を満たさない。属性を満たしていることは他者の状態初期化完了の保証ではない。

`[ExecutionOrder(-100)]` は小さい値を先に配送する。省略は 0、最も近い派生宣言が継承値を置き換える。同値は既存の GameObject / Script 順を保つ。Awake・Start・Update は共通のフレーム snapshot を使い、FixedUpdate・LateUpdate は各 Phase の開始で同じ安定 ID 規則の snapshot を取得する。追加は現在の snapshot に参加せず、削除・置換は再照合で配送を止める。階層の無効化・要求違反・fault の既存条件も保持する。

teardown は既存の順序を維持し、実行順の逆順へ暗黙に変更しない。衝突通知、jobs とイベントは各サービスの配送契約に従い、この属性で並び替えない。

## 世代の所有と Edit 復帰

parked Edit が保持する CLR 個体は code generation lease を所有する。Host の retained 世代用資源枠と current / candidate 用の一時枠を分け、Edit・実行中 Play・reload 候補の三世代が共存できる。retained 世代を退役しようとした要求は、その個体・catalog を変更せず拒否する。lease は参照回数で数え、解放だけでは既存個体を退役しない。

native `RetainAppliedGeneration` は適用中の世代 ID を返し、取得ごとに `ReleaseRetainedGeneration` を必要とする。`ActivateRetainedGeneration` は保持した schema・factory・型 metadata を再選択し、元の Script / CLR 個体を constructor・保存往復・reload で作り直さない。active な世代と生存 Adapter が持つ世代は lease 解放後も存続する。true retirement でだけ metadata と Host context を解放する。全 project の終了前には lease も解放する。

Stop は元の Edit Scene / runtime を復帰させてから、その所有世代を active に戻す。Play 中に更新したコードを元の Edit 個体へ適用する操作は、この復帰と別の明示的な検証・適用とし、失敗で元の個体や Undo 履歴を捨てない。

初回の Play clone も元の managed assembly を新しい ALC 世代へ読み、C# static 状態を Edit と共有しない。保持した Edit 世代から Play の factory を選んでから clone 個体を作成し、元の個体や旧世代の一時 clone に人工的な reload callback を呼ばない。準備・clone 失敗は Play 側を解放してから元の世代へ再選択する。native DLL の static とプロセス全体の .NET / OS 状態は共有したままであり、process sandbox ではない。native DLL の置換は Play 中に延期する。

## 編集 action

`[EditorAction("restore-health", "体力を戻す")]` は同期の引数なし instance `void` メソッドを現在の catalog に登録する。ID は保存キーと同様に安定させ、同じ型の継承階層で一意にする。任意メソッドの探索・呼び出し API は公開しない。constructor・property getter は action の発見で実行しない。

Inspector のボタンと AI は同じ `script.configure.preview` に `action` と既存の正確な `scriptId` を渡す。設定候補を独立複製し、明示 field 変更、action、OnValidate の順に各一回実行する。補正済みの leaf 差分を preview し、全対象の再照合後に一件の Undo として適用する。Undo / Redo で action や OnValidate は再実行しない。action の失敗は live 個体・dirty・Undo を変更しない。

action は設定・定数・純粋な計算にだけ依存する。Engine API、private な実行時 cache、mutable static、外部 I/O と非同期処理を禁止し、OnValidate と同じ analyzer 診断を使う。任意 .NET helper の外部副作用を sandbox 化する保証ではない。世界の構成変更やファイル操作は専用 Editor Op に置く。

## ギズモ

`protected override void DrawGizmos()` と `DrawGizmosSelected()` は native の Scene View capture へ接続する。`Gizmos.Line` / `Sphere` / `Box` は world 座標と m 単位で、有限な座標・色と非負の寸法を要求する。DepthTest は一回の描画 callback ごとに false へ戻す。

描画は constructor を実行せず、保存設定だけを複製した非公開候補で行う。private な実行時 cache とゲーム状態は候補に持ち込まず、候補の設定変更は live 個体や保存へ反映しない。現在の owner に結ぶ読み取り専用 Engine scope を一回だけ開き、Transform と native Component の状態を読める。世界変更、managed Script の取得、jobs・events の予約・通知、asset service は使えない。

現在の候補隔離は callback ごとに全設定 graph を複製するため、処理量と確保量は設定全体の大きさに比例する。描画 callback が読まない巨大な配列も複製対象になる。この費用を catalog の `gizmoPreview.work` / `cost` に公開する。設定の直接書き込みを確実に検知できない revision cache で古い図を再利用しない。大きな処理対象と描画 preview の設定を別 Script へ分けると現在の複製範囲を小さくできる。小さな描画専用データを明示して隔離する API は、通常 field の読み取り制限まで保証できる設計を別途必要とする。

描画 command 数は native 呼び出し側が事前確保する容量で決める。容量不足・callback 失敗は候補全体の描画を返さず、live Script を fault にしない。native は最大 byte 数を先に確保し、作者 callback を容量不足で再実行しない。選択は native 描画 context が判断する。全 entry と個体 ID を描画前に snapshot し、callback 後に再照合する。追加は次の描画から参加し、削除・置換された個体へ続けて描画しない。

Host の型情報は、継承を含む `DrawGizmos` と `DrawGizmosSelected` の実装有無を世代ごとに native へ伝える。要求した callback が無いと分かる場合は、容量・世代・所有者の接続と生存を検証した後、空の描画結果を返し、command バッファの確保と描画 dispatch を省く。二つの callback は独立に判定し、他の callback の実行中は従来どおり拒否する。型情報の公開・reload・保持世代の復元にはこの情報も含め、実装済み callback の隔離・容量契約は変えない。実装有無を通知しない Host には従来の描画 dispatch を使う。
