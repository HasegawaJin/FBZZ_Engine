<!-- @file    csharp-engine-notifications.md -->
<!-- @brief   Animation・Sequence・Navigation の C# 同期通知と所有値の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-06 -->
# Engine から C# への通知

AnimatorSystem・SequenceSystem・NavigationSystem・NavMeshSensorSystem が既存の `Script` へ配送する通知は、`ManagedScriptAdapter` を通して C# の protected override に同じ同期タイミングで届く。独立した通知 queue や更新ループを作らない。Edit では配送しない。無効・faulted・requirementsBlocked・旧世代・除去済みの個体を実行しない。

| callback | payload と意味 |
|---|---|
| `OnAnimationEvent(AnimationEvent info)` | 名前・intParam・floatParam・clipTime [s] を持つ Event Track の合図 |
| `OnAnimatorMove(RootMotionInfo info)` | local の変位 [m]・回転 XYZW、world の変位 [m]・速度 [m/s]、抽出差分時間 [s]、Engine 適用済みか |
| `OnSequenceEvent(SequenceEvent info)` | 名前・intParam・floatParam・sequenceTime [s]・sequenceName を持つ binding 付き EventTrack の合図 |
| `OnSequenceFinished(string sequenceName)` | Once の終端と snapshot 復帰を終えた合図 |
| `OnNavMeshDestinationReached()` / `OnNavMeshPathFailed()` | 到達と経路失敗 |
| `OnNavMeshTargetSpotted()` / `OnNavMeshTargetLost()` | Sensor の視界への出入り |

各 payload は現在のフレームの所有値で、callback 後も保持できる。文字列は strict UTF-8、packet は 1 MiB 以下、浮動小数は有限を要求する。packet 不正と user callback の失敗は受信 Script の停止と所有 job・購読・asset の解除へ接続する。Host は毎回新しい scope を開き、同期再入から戻ると発火元の scope を復元する。RequiredComponent を既存 callback と同じく再検証する。

Engine API の読み取り・非物理の変更・所有仕事と購読の登録・ゲーム側イベントの発火は live 権限で行える。`Time.DeltaSeconds` は時間積分用 callback ではないため利用できない。root motion は payload の `DeltaSeconds` を使う。剛体・collider・joint の物理変更は次の `FixedUpdate` に渡す。ExtractOnly の姿勢適用は作者が行い、同一フレームの抽出値を受け取るため一フレーム古い getter を代用しない。

binding を持たない Sequence EventTrack は、既存の process-wide `ScriptEventBus` の raw `SequenceEvent` として発火する。`Events.Subscribe<SequenceEvent>` は通常の `managed:Fbzz.SequenceEvent` 購読に加えて、この既存 channel へも一件購読する。型を明示した専用 marshaller が raw 値を packet に変換し、汎用 JSON payload と混同しない。任意の native channel や raw pointer を C# に公開しない。通常の `Events.Publish<SequenceEvent>` も従来の managed 値配送を保持する。

二件の native 購読は一つの owned handle に結び、解除・無効化・fault・破棄・reload で両方を解除する。C# handler に一回の `EVENT` scope を与え、停止した一人が他の購読者への同期配送を止めない。Connect の staging と reload 後の再接続も既存 Events と共通である。

共通宣言は `ManagedBridgeContract.inl` の callback ID・型・policy を正本とする。SDK の override、Host の署名検証、Analyzer、API catalog は同じ宣言から読む。
