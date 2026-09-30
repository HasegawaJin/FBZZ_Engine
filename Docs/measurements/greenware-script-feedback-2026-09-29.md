# GreenWare から Engine へ戻した3件の改善

2026-09-29。基準 `534b075e` のゲームコードを確認し、以下を実装した。初期制作の「エンジン改良を含む1週間」とは別の追加改善。

## 1. 初期化順序

個々の Script を Awake → Start → Update まで進めていた処理を、フレーム開始時の全対象の Awake → 全対象の Start → 全対象の Update に変更。EntityID・ScriptEntry 識別子・実体を照合して、コールバック中の追加・削除・差し替えを扱う。

GreenWare の CombatManager で Player の IDamageable 登録を確認するために置いていた0.5秒の猶予タイマーを削除し、最初の Update で一度確認する。これは警告を遅らせていた処理であり、ゲーム全体の開始を0.5秒高速化したという意味ではない。

途中で追加された Script や、開始時に非アクティブだった対象は次フレームから参加。Start 同士の依存解決や動的生成されたボスの出現待ちは対象外。既存の ManagerWatch にある動的生成向けの猶予は維持する。

## 2. 必須設定の検証

FBZZ_REQUIRED_ASSET を共通検証へ接続し、FBZZ_REQUIRE_SCRIPT と FBZZ_REQUIRED_REF を追加。未設定、ファイル不在、読み込み失敗、型不一致、参照切れ、同型 Script の曖昧さを診断する。必須 Script / Asset / Ref に不備がある場合は Start と以後の実行を止める。

Player の手動 HasRequiredAssets と停止処理、Boss03 の必須 Script 3種類の個別チェックを宣言へ移行した。既存 Component の必須宣言は診断のみの互換契約を維持。Inspector ではロードせず、Play 前と Awake 後の Start 前で実データの解決まで検査する。既存の保存キーを維持し、実際の SceneSerializer で確認した。

Awake 後に Start が止まった場合も OnDestroy で後始末できるよう、Scene の終了条件を修正した。

## 3. 独立ループ音の API

AudioLoop ハンドルと UpdateLoop / StopLoop / ReleaseLoop を追加し、GreenWare の LoopVoice を公開 API の呼び出しへ移行。主音声と別の音量・ピッチを持つ音源の生成と内部フラグ操作を Engine に移した。

同じラベルの複数個体を名前検索で共有しない。Script 無効化・破棄・再生成・ライフサイクル初期化で寿命トークンを失効させ、AudioManager が停止フェードを経て回収する。Script の破棄処理から Scene を参照しない。テストで見つかった ComponentArray の末尾削除時の所有物残留も修正し、停止フェードの重複開始を防いだ。

UpdateLoop の戻り値は要求の受付。クリップの読み込みやデバイス再生の成功を保証しない。同じクリップ・バスの更新では再生を再開始しない。ハンドルはコピー可能で、Release は同じループのコピーも失効させる。

## 検証結果

- Development 構成、FBZZTestsEngineAuto のビルド成功。エラー・警告0。
- 関連117件が成功、失敗0。今回の新規26件は初期化8、必須設定9、音声8、ComponentArray の所有物解放1。
- 既存の ScriptModules、Player、戦闘、観測API、Scene、シリアライズ、音声、ComponentArray の関連ケースも同じ実行に含む。
- AgentLint --changed はエラー・警告0。
- 最終テストログ: `build/agent/test-20260929-141602-47020.log`。実行ログ上の合計4.10秒。性能測定値ではない。
- 最終ビルドログ: `build/agent/build-20260929-141201-45112.log`。
- 音声のテストは Proxy / AudioSystem / AudioManager を実装のまま通し、OSへの出力だけを記録用デバイスへ置き換えた。

初回の関連77件では4件が失敗。音声の3件から上記の寿命と停止処理を修正した。残る1件は EntityID を保存しない汎用 TOML reflector をテストで使用していたため、製品で使う SceneSerializer に置き換え、参照先の永続IDまで照合した。最終実行ではすべて成功。

実行入口は Tools/AgentBuild.ps1 の check → build FBZZTestsEngineAuto → test。フィルター:

```text
ScriptLifecycleTest|ScriptRequirementsTest|ScriptAudioLoopTest|GreenWareScriptObservationTest|PlayerScriptModulesTest|ScriptModulesTest|Boss03CounterTest|BossBreakTest|ParryRushTest|SceneManagerTest|AudioStreamingTest|AudioBusTest|ComponentArrayTest|SceneSerializerTest
```

## 確認範囲と互換性

実機での聴感、GreenWare全体の操作・描画、Editor上でのDLL再読み込み操作は未確認。リフレクタ ABI 6、Script vtable ABI 8 への更新を含むため、Editor とゲームの Script DLL は同じ最新版 SDK で再ビルドが必要。

開発時間の短縮率やゲーム全体の性能向上率は測定していない。観測APIの「21変数・30代入を削除、1013→977行」は別の移行段階の記録であり、今回の変更を混ぜて再集計していない。同名JSONに全117テスト名、追加ケース、ログとソースのSHA-256を保存した。
