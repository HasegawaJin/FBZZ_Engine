# C# 制作・実行経路の完成

ゲームの配布まで同じ API と保存形式で実行する。通常の C# のアクセス規則を変更しない。callback は基底 Script の protected virtual メソッドを protected override で実装し、設定は public field、変数は明示した型を使う。旧private・名前検出方式の互換を要求せず、誤記・override欠落をビルド時に検出する。callback宣言と検出・診断は共通契約から同期する。

## オブジェクトと Component

`GameObject.SetActive`、`ActiveSelf`、`ActiveInHierarchy`、`Layer`、`Tag`、`Parent`、`Children`、`GetDescendants` を追加する。検索は Scene と指定 root の範囲を区別し、反復走査で深い階層と循環を検証する。返す参照は Scene・Entity の寿命 ID を照合し、表示名を保存 ID として使わない。

`AddComponent<T>` と `RemoveComponent<T>` は SDK の native facade と managed Script 派生型を対象にする。Transform は常設で削除できない。既存 Component は重複生成しない。Script 追加は新しい個体を準備してから登録し、初期 callback は ScriptSystem が駆動する。実行中の Script 削除は callback 終了後へ延期する。`Script.Enabled` は配送制御であり、通常の public メソッド呼び出しの可否を変更しない。

Prefab 一括生成は全 pose・子孫を含む容量を先に検証し、失敗時に今回追加した全個体を戻す。変更要求を容量不足で再送しない。再利用 pool は Scene と所有者に結び付け、貸出と返却を明示し、Scene の変更や所有者破棄で残る参照を失効させる。世代交換で旧 pool の個体を Cleanup に破棄し、AfterReload で新しい pool を作る。caller buffer 検索は容量不足で出力を部分更新せず、Host の転送配列と参照 cache を再利用する。Scene/参照を温めた後の managed 確保をゼロにできるが、native 全件走査の作業領域もゼロになるとは扱わない。

## 物理とゲームサービス

Collider と既存 Animator / Audio / UI / Camera / Navigation / CharacterController は C++ の実装へ typed facade を接続する。finite 値・範囲・Scene・Phase を検証し、内部 pointer を managed 側へ渡さない。物理状態の変更は FixedUpdate、登録・表示等の通常操作は同期 callback 内とする。

Query は layer mask、trigger の有無、除外対象を明示できる。既存 shape の同期時点を維持し、spawn や Transform 設定直後に暗黙の物理更新をしない。

## 保存と Play 差し替え

Quaternion、一次元 List、Serializable の class / struct と DataAsset 参照を正式 codec に追加する。任意の getter・constructor を保存時に実行しない。循環、非対応の深さ、非有限値、非対応メンバーを診断し、復元は全候補検証後に確定する。配列・List の総件数はスクリプト側で決め、表示・転送・保持用の容量とは分離する。大きな値の契約は [設定 transaction](csharp-settings-transactions.md) に従う。

Play 中の再読み込みは候補の型・設定・保持状態・直接参照を準備してから一括交換する。失敗した候補は破棄して現行コードを維持する。Awake / Start の二重呼び出しを避け、旧世代の仕事と購読を解除する。保持属性は数値等の状態を選び、直接の GameObject / Component cache は ID で新世代へ接続する。

イベント、待機、Tween は登録した Script が所有する。停止・fault・requirementsBlocked の所有者への配送を止め、破棄・世代退役で仕事と購読を解除する。Task へ Engine の実行権限を持ち越さない。

## 配布と設定経路

出力ゲームは `Managed/game.managed.toml` から game assembly と sidecar を読み、exe の `Managed` に同梱した Host を起動して Scene より先に登録する。依存 assembly・deps・runtimeconfig・型 sidecar を同じ出力へ含める。managed package の欠損は起動・出力失敗として名指しする。

配布は EditorApp 所有の `PackageBuildService` と既存 `BuildPipeline` を共有する。`build.package_check` は候補設定の事前確認、`build.package` は非同期開始、`build.package_status` は要求 ID と終端状態、`build.package_cancel` は取消を共通操作登録簿へ載せる。Build Settings パネル・AI・hidden batch は同じ設定と状態を参照し、進行はパネルの描画ではなく Editor update が駆動する。Play / Pause / Stop 復元待ち、C# / shader の開始待ち・ビルド中は配布開始を拒否し、配布中の別ビルドも開始させない。

開始時とコミット時の出力先検証・SDK 構成の事前確認・一時出力からの確定を維持する。要求 ID は `package:<番号>`、状態は `idle / running / done / failed / cancelled`。`terminal`、診断、`outputExe` / `managedManifest` と存在フラグを公開し、`outputReady` は今回の成功要求と必要成果物の存在が揃う時だけ true とする。前回の exe が残っていても失敗した今回の完了にはしない。既存 `build_run(target="package", package={...})` / `build_get_status(packageRequestId=...)` は同じ操作へ投影し、取消と事前確認は既存 `editor_op_invoke` / `editor_op_query` を使う。

一時出力は毎要求で未存在の専用フォルダを排他的に作成し、失敗・取消は自身が所有したフォルダだけを後始末する。既存の `<output>_tmp` を削除しない。配布シーンは実体がプロジェクト内にあるものだけ受理し、内部の絶対パスを相対へ正規化する。外部・`..` の指定を拒否し、Windows の大小文字や link による出力保護の回避も検証する。

SDK の公開型・メンバー・引数・単位・座標・利用 Phase・失敗条件を機械可読 catalog として公開する。property getter は実行しない。Inspector と AI 設定は同じ Configure Op を通し、正確な個体 ID、全件検証、Undo、再送の重複防止を共有する。

## 検証

managed codec / lifetime / jobs、native bridge と reload、実 Editor の作成・設定・再読み込み、出力ゲーム起動を確認する。100 / 1000 対象の生成・query・batch、callback の確保量と GC、保存から反映までの時間は実測値を記録する。処理速度や開発時間の改善を未計測のまま断定しない。
