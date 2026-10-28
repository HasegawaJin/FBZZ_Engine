<!-- @file    render-thread.md -->
<!-- @brief   ゲーム更新と RHI 命令処理を分離する描画スレッドの所有権と移行契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-10 -->
# Render Thread とゲーム更新の分離

## 採用する方式

2026-10-10 に選択された方式 A を採用する。メインスレッドがゲーム更新と描画入力の構築を行い、専用 Render Thread が所有済みの入力から RHI 命令を処理する。前フレームの命令処理と次のゲーム更新を並行させ、CPU の未完了フレームは最大 1 個に制限する。

`OnRender()` を別スレッドで呼ぶだけの変更は行わない。現在の `RenderSystem` は Script の描画 callback、Debug パス、ゲーム UI、Scene の派生状態を参照する。これらの参照を Render Thread に持ち込むと、Play/Stop、削除、Undo、次フレームの更新と競合する。

Graphics は現行 DX12 バックエンドを対象とする。DX11 の復活は含めない。既存の [Graphics 境界](graphics-library.md)、[RenderGraph](render-graph.md)、[アセットのスレッド境界](asset-streaming.md)を維持し、描画スレッド新設を今回の追加範囲とする。

## 所有権

| 実行場所 | 所有する処理 |
|---|---|
| メインスレッド | Window と入力、Time、Scene/Script/Physics、描画入力抽出、Editor 操作、ImGui の UI 構築 |
| Render Thread | RHI 命令の実行、DX12 コマンド記録、状態追跡、GPU 転送、提出、Present、GPU の完了確認 |
| TaskSystem | 既存の CPU ジョブ、ファイル読み込み、デコード。専用 Render Thread の代わりに長時間占有しない |

描画入力は値または所有済みの不変データとする。DrawCall/ComputeCall の値、転送元バイト列、ビュー設定、ImGui の頂点・インデックス・コマンド列をフレームの完了まで所有する。Scene/Component、関数内の配列、ImGui の次フレームで再利用される DrawList を参照したまま投入しない。

GPU ハンドルだけでは資源の寿命を保証しない。記録済み命令から参照される資源の解放・差し替え・台帳の再配置には完了境界を設ける。定数バッファ更新は描画との順序を保ち、転送元は呼び出し時に所有する。同じ定数バッファへ複数オブジェクトの値を順番に書いた場合も、それぞれの DrawCall が対応する値を使う。

## フレームと同期

メインスレッドはフレーム N の入力を確定して提出し、Render Thread のフレーム N の処理中にフレーム N+1 を更新できる。未完了フレームが上限に達した場合だけバックプレッシャーを掛ける。無制限にキューへ積まず、ゲーム時刻や入力が表示より何フレームも先へ進む状態を作らない。

同期境界はリサイズ、GPU 読み戻し、デバイス再生成、資源の破棄・差し替え、Play/Stop の寿命変更、終了とする。同期 API の bool は「キューに入った」と「実際に記録した」を混同しない。特に `TryDispatch` と加速構造構築の成功契約は維持する。

Win32 の入力と OS ウィンドウ操作はメインスレッドで行う。Render Thread の完了待ちと OS メッセージによる相互待ちを作らない。追加 ImGui ウィンドウの作成・破棄はプラットフォーム処理と GPU 処理を分け、共有状態に必要な同期を明示する。

## 診断

現在の Core Profiler はプロセス内で 1 個の recorder と descriptor 表を共有するため、そのまま Render Thread から記録しない。スレッドごとに recorder を所有し、完了済みの snapshot を application frame serial とともに公開する。Main/Render/GPU の値を単純加算してフレーム時間と表示しない。

既存の `RenderDebugOverlay`、Render Pass Viewer、MCP、Playtest は完了済みフレームの serial/scene generation/device epoch を保持する。読み戻しや画像比較は要求されたフレームの完了を待ち、最新のゲーム状態と旧描画結果を取り違えない。

## 実装と検証の順序

1. 有界の専用 executor と、所有済み命令を運ぶフレーム境界を追加する。
2. Renderer/ResourceManager の命令実行・転送・破棄の順序と寿命を固定する。
3. Editor/Standalone の共通起動と停止に接続し、ImGui のプラットフォーム処理と GPU 処理を分離する。
4. Profiler と完了済み描画 snapshot の公開を接続する。
5. 実際の DX12 描画で旧経路と比較し、成立した経路だけを既定にする。

テストは sleep や実時間の推測を使わず、latch/barrier と既存 TestKit で次を表明する。

- 命令処理の thread ID がメインと異なり、次のゲーム更新が処理中に進める。
- 更新元の配列を変更・破棄しても、提出済み命令の値が変わらない。
- 同じ GPU ハンドルに対する Update/Draw/Release の順序を保つ。
- キュー上限、停止、リサイズ、Play/Stop、読み戻しで未完了データを破棄しない。
- 未対応の GPU 機能や記録失敗を、非同期投入成功として隠さない。
- Scene/Game/UI の画像、TAA・露出の履歴、スキニング、Terrain/Water/Particle の出力を既存 Playtest で比較する。

速度改善はまだ保証しない。並行化の成立と描画の正しさを先に確認し、同じ条件の Main/Render/GPU とフレーム時間で採否を判断する。

## RHI 境界の実装

`Application` の Editor/Standalone 共通ループから `BeginCpuFrame` を開始する。Scene 抽出、パス構築、Script の描画 callback はメインスレッドに残し、`ThreadedRenderer` が DrawCall/ComputeCall と所有済みの転送元を収集する。`EndFrame` が専用 executor へ渡し、native backend のコマンド記録と Present を処理する。

`TryDispatch`、加速構造構築、リソース生成、読み戻しは実際の処理結果を返す同期境界とする。CPU shadow/内容版を読む Buffer 解決も保留中の更新を確定する。GPU 成功 receipt を、単なる非同期受付として返さない。

隣接するBufferのnative値は`ResourceManager::GetBufferStates`でまとめて読む。有効な入力がある場合だけ既存のCPU-frame drainを一回実行し、pumpが終了してから各handleの世代を解決する。返すのはsize、stride、native内容版、UAV添字の値だけで、native pointerを保持しない。新しいoperation packetは追加しない。これはGPU fenceの完了確認ではなく、CPU側のnative記録の完了境界である。

Skinningのsignatureは当該actorのsource/outputをこの境界で読み、資源の更新・生成を挟まないsignatureと既存出力の再利用判定だけに使う。pump後にScene世代、Component lifetime、ResourceManagerのowner/reset/frameと現在のhandleを照合し、poseの所有者も引き直す。handleが変われば従来の照会へ戻る。入力CopyData、出力確保、native dispatch成功と書込版の最終確認は従来の境界を保ち、非同期受付時に進む`GetBufferMetadata`のCPU内容版で代用しない。

ImGui は共有 DLL 内の TLS で context の選択を分け、Render Thread の backend 用 context は producer の UI 状態を持たない。動的アトラス更新と texture ID 公開は同期済みにしてから、DrawData の頂点・インデックス・描画コマンドと viewport 情報を所有する。描画コマンドの TextureRef は数値 ID に固定し、次の UI フレームの texture/atlas 列を参照しない。

追加 viewport の DrawData と viewport 値も所有する。RendererUserData の参照寿命は、Renderer_CreateWindow / Renderer_SetWindowSize / Renderer_DestroyWindow を専用 executor の同期境界にすることで守る。OS の platform callback はメインスレッドに残し、GPU の RenderWindow / SwapBuffers は backend 用 context から snapshot を使う。追加ウィンドウの存在だけを理由に UI フレーム境界で待機しない。未知の DrawCallback や platform 描画 callback は従来の同期経路を維持する。

DrawData の snapshot は完了後に再利用する。pool 以外の shared_ptr 参照が無いことが入力の寿命を示し、worker の最終アクセスとは完了フラグの release/acquire で同期する。未提出の入力は producer 上の CancelCpuFrame で破棄する。ImVector の代入は容量を破棄するため、resize とバイトコピーで頂点・インデックス・描画コマンドの容量を維持する。処理中の snapshot を書き換えず、GPU ハンドルは既存の解放境界を守る。

資源解放は受付済みの CPU 命令を完了させてから行い、GPU 側の退役は従来の fence 管理を維持する。Application より ResourceManager が長く生存する場合は `PrepareRendererShutdown` で参照を切る。Win32 の送信メッセージを完了待ち中にも処理し、Present とホスト側の相互待ちを避ける。

## 同期要求のpacket統合

同期APIはpending commandと同期operationを一つの所有済みpacketへまとめ、実際のoperation完了を一回だけ待つ。producer上のcommand順、値の所有、未完了packet最大一個、TryDispatchの成功receiptは変えない。従来のpending packet完了待ちと別の同期packet完了待ちの二往復を除く。Render Threadからの再入はその場で実行し、資源破棄・リサイズ・読み戻しの必要な完了境界は維持する。

Main側のSync、CPU frame wait、packet提出のbackpressureをProfilerの個別scopeで記録する。並行するworker時間と足してframe時間を作らず、完成したapplication frameのwall時間と合わせて確認する。

Syncの外側へsource_locationの同期要求元を記録し、SubmitPacketの内側をPacketAcceptanceとPacketCompletionWaitへ分ける。SubmitPacketは同期packetのCPU完了待ちを含むinclusive値であり、queueへの投入だけの時間やGPU実行時間ではない。受付と完了待ちを親のSubmitPacketへ加算しない。Edit/Playの差を調べるときは、呼び出し回数と要求元APIを照合し、GPU fence・Present・resource操作・command記録のどれが待ちを延ばしたかを確認する。

Flushはpacket完了待ち中にpumpが追加したcommandも再提出し、空になるまで完了を待つ。submission slotを待つ間はcommand mutexを保持せず、producer上のpumpからSyncへ再入できる。Sync / WaitForCpuFrameのscopeはexecution mutex取得前から計測する。CPU32件と実GPU67件が合格した。性能分布と残るスパイクは[VolumetricLightの同期統合検証](volumetric-light.md#2026-10-10-の同期統合の検証)を参照する。

## 参考

- [Epic Games Threaded Rendering](https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine)：スレッド別の状態所有と描画側へのデータ受け渡し。
- [Microsoft Direct3D 12 多重スレッド記録](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles)：command allocator と command list の同時利用制約。
- [Dear ImGui](https://github.com/ocornut/imgui)：UI context と DrawData の寿命は、RHI 命令の処理完了まで別に管理する。

## GPU計測ビューの順序付き記録

`GpuProfBeginView` は従来どおり native 開始結果を同期して返す。RenderPipeline と skinning 準備処理には、結果をメインスレッドへ返す必要がないため、`GpuProfRecordView` / `GpuProfEndRecordedView` を使う。前者の true は区間の受付であり、native 開始成功とは異なる契約として明記する。true の場合だけ終了を対応させ、同じ物理フレーム内で閉じる。

ThreadedRenderer は metadata とパス名を値で所有し、開始・パス計測・描画・終了を既存の命令順で処理する。native 開始結果は worker だけが持つ stack に保存する。開始に失敗した区間のパス計測と終了を抑止し、描画命令は通す。失敗した入れ子を抜けると親の計測へ戻り、失敗中の子孫は native 開始も呼ばない。Scene とパス本体、既存の graph hook 自体は producer で実行する。

途中の TryDispatch、資源生成・更新・解放、読み戻しは従来の同期と成功 receipt を保つ。必要な同期で命令列が分割されても stack は worker に残る。CancelCpuFrame は未提出入力だけを破棄するため、既に同期実行された開始の終了だけが取り消された場合は、次の BeginFrame・EndFrame・Shutdown で成功済みの区間を閉じる。未提出の開始と終了は native に副作用を残さない。

単一スレッド backend の既定実装は従来の開始・終了へ委譲する。timestamp query の容量、frame serial、view/plan/resource の出自、GPU fence、PIX event の command-list 制約は変更しない。能力照会や GPU 成功判定を推測して待機を外す最適化ではない。

参考は [Godot RenderingServer.call_on_render_thread](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class-renderingserver-method-call-on-render-thread)。描画側で完結できる処理を値とともに順序付きで渡す考え方を採用する。計測結果と再現条件は [同期削減の比較](../benchmarks/2026-10-22-render-synchronization/report.md) に記録する。

## 空キューのCPU完了境界

WaitForCpuFrameはexecution mutexと同期再入guardを従来どおり取得し、pending queueをcommands mutexで保護したままRenderThread::TryIsIdleを確認する。空キューとworkerの完全なidleが同時に成立する場合だけ、既存の全drainループを省略する。CPU shadow/内容版の値はキャッシュせず、呼び出し側は現在のnative値を読む。

worker実行中、command所有値の破棄中、submission予約中、try-lock失敗、pending commandありの場合は従来のFlushへ戻る。待機pumpの追加command、ネストしたSync、資源破棄・reset・読み戻しの順序と寿命、frameが後続のproducerへ開いたままである契約を維持する。GPU完了を推測する判定ではない。診断のCPU_FRAME_WAITはAPI呼び出し数、waitCallsは実際にRenderThreadの待機境界へ入った回数なので、idle照会では後者が増えない。

## UI Canvasの収集

UISystemと選択maskはCanvas componentの現在のmembershipから有効なCanvasだけを収集する。0または1個ならScene全体のroot一覧・再帰走査は不要。複数の場合は現在のroot順と子sibling indexから階層keyを構築し、元の深さ優先走査と同じ列に戻してから、既存のsortOrder比較をそのまま適用する。同値sortOrderを含む描画順・入力順を変えず、無効な祖先、nested Canvas、root/child並べ替え、reparentとcomponent追加削除を呼び出しごとに評価する。キャッシュやScene寿命を越えるpointerは持たない。
