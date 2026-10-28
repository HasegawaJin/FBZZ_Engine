# GPU Skinning の需要と所有 publication

`FBZZ_GPU_SKINNING_DEMAND` は既定無効で、明示的な `1` の場合だけ需要経路を有効にする。未設定・`0`・その他の値では C3 の全準備・dispatch 経路を使い、追加の入力捕捉、ticket 準備、observer 登録、snapshot 補完は行わない。有効時も、以下の証明が成立しない呼出しは同じ全経路へ戻る。

同条件の Release 比較では dispatch 省略が FPS 改善につながらず、CPU の準備・待機費用が増えたため opt-in とする。入力 snapshot cache を含む機能と正しさの回帰は維持し、実時間の利益を確認できた利用条件で明示的に選ぶ。

目的は画面外で不要な GPU dispatch の実行を省くことであり、Animator の時計・イベント・公開骨・前フレーム palette、RenderScene の geometry、材質、world、slot、順序を変えるものではない。CPU 準備だけを省く従来の signature 直接比較とは別の処理である。

## 元経路の観測点

C3 は actor ごとに入力を準備し、全 actor の走査後に ExecuteSkinningRequests を実行する。legacy の公開 flag/signature はその前に設定され、owned 経路の成功 publication は GPU の native 書込 receipt 後になる。追加準備を行う間も、この元の順序と公開値を維持する。元 dispatch の位置を越えて未実行の仕事を残すのは、全入力を所有する完成 ticket と外部観測前の完了境界を準備できた場合だけとする。

## 本当に計算済み VB を読む経路

| 経路 | 現在の変形入力 |
| --- | --- |
| GBuffer | 計算済み VB を優先し、無い場合は元 VB と骨 CB |
| directional / punctual shadow | 計算済み VB を優先し、無い場合は元 VB と骨 CB |
| object / selection mask | 計算済み VB を優先し、無い場合は元 VB と骨 CB |
| Ray / Hybrid | 成功内容版付きの計算済み VB が必須 |
| Fiber shell | velocity 以外では計算済み VB を利用し得る |
| Forward / transparent | 元 VB と骨 CB を使う |
| Velocity | 元 VB と現在・過去の骨 CB を使う |
| Decal receiver mask | 元 VB と骨 CB を使う |
| built-in reflection / light probe capture | 現実装は skinned object を収集対象から除く |

需要はカメラだけで決めない。GBuffer の layer/LOD/frustum/distance、各 directional cascade と punctual shadow の提出範囲、mask、ray、fiber を和集合にする。無効な bounds、未知の view/pipeline/native consumer、現在の登録状態から閉じない callback、未対応 backend/facade、bake/morph 等の未対応入力は全経路へ戻す。既存 pass と同じ判定を共有し、shadow の式を別実装へ複製しない。

旧 `PROBE_GI` は count-only 診断の保守的な reason であり、実 consumer の判定ではない。上表の組込み probe は skinned object を除外するので、この存在だけでは GPU 出力を要求しない。初期 native Script/user pass、mask/outline、Fiber、morph/bake、owned-batch 比較モードは今回の対象外として元経路を保つ。

## 入力と GPU 命令の所有

ticket は Scene、Component、Model、Mesh の可動参照や ResourceManager の raw pointer を保持しない。元時点の source native 内容版と実バイト列、全 palette、定数、PSO/root signature、descriptor lease、専用 output 世代、command allocator/list、成功 receipt を所有する。GPU handle は所有 lease とみなさない。既存の 3 lane pool も、ticket/snapshot と GPU fence の両方の退役を保証するまで再貸出しない。

Prepare は通常の callback を起こし得る準備処理であり、戻った後に元 Scene/Component/資源 identity を再検証する。source の in-place 更新や stale handle を検知したら、捕捉したと見なさず元の全経路へ戻す。元 output を既に外部へ貸し出したまま遅延 write することは認めず、遅延用 output は専用 lease で保つ。

Submit は準備済みの native 命令を投入し成功 receipt を更新するだけにする。Scene/ResourceManager の再探索、native virtual getter、PSO/descriptor/upload の準備、メモリ確保、Logger、asset、wait pump、任意の callback を呼ばない。GPU 完了そのものと、成功した DIRECT queue 命令の投入・内容版 publication を混同しない。未対応・epoch 失効・投入失敗を成功の内容版で公開しない。

現 backend の上限は 256 行、65536 行列、入力と出力各 32 MiB、16 live group / 合計 256 MiB。上限超過は元経路へ戻る。GPU 未使用の ticket は view 終了時に投入せず、SMR または旧 snapshot が所有している間だけ保持する。同じ source/pose なら次 view で再捕捉せず、需要が生じた時に投入する。次の dirty pose は別の専用出力を用い、旧 snapshot の出力を上書きしない。pool は ticket 所有と描画 fence の双方が退役するまで同じ native 出力を再貸出しない。

SMR の `preparedSkinningFrame` は、入力の完全一致を最後に確認した `Time::frameCount` であり、ticket の RHI `ResourceFrame` や成功投入 frame とは混ぜない。commit・同入力再利用時だけ更新し、invalid/dirty/reset は棄却する。次 frame に LOD 等で Skinning 対象を訪問しなかった場合、旧 ticket を新たな geometry snapshot / Fiber へ転写しない。既に公開した旧 snapshot の ticket はそのまま利用できる。

`Writes.recorded` は元の投入履歴であり、外部から出力内容を明示変更した後の正しさまで表さない。各行の native 内容版と独立 lease の一致は `IsWriteCurrent`、全 ticket の有効性は Submit の再検証で確認する。元 pose の出力を証明できない場合は描画しない。別世代の mutable bone CB/source を使って元 pose と見なす復帰はしない。

同じ group が多数 actor の出力を持つ場合も、通常再利用は actor の実 slot の現在版だけを照合する。全 group の Submit は未投入時または実際の外部 callback 境界で行い、後者も group ごとに一度へ集約する。actor ごとの再利用で全 group を再走査する二乗処理を作らず、内容版の完全比較を省略する永続 success cache も持たない。

成功済み Submit の再呼出しでも現在 physical frame の退役 marker を更新し、同 frame の外部 raw 借用を守る。固定 17 marker は EndFrame の actual main fence でだけ確定し、slot 再利用は ticket 所有終了・private writer fence・final main fence の全完了を要する。

既知 consumer の Resolve 出力は ticket の借用であり、呼出し元は GPU request へ owner を転写して Context frame pin を使う。成功済みかつ現在版の row は O(1) 照合で Notify と全 group Submit を省略し、未投入の最初の利用だけ全 pending 完了へ進む。

## 不変 snapshot と遅い利用

geometry 抽出を後から全再実行しない。各 item は元の source/slot/submesh と専用 output ticket を対応付ける所有 sidecar を持つ。既知 consumer は現在の需要を評価し、必要ならその元入力の ticket を完了してから結果を使う。

`ctx.renderScene` の copy-on-write 差替えだけでは不十分である。pass が既に得た const RenderScene 参照や queue の item pointer は旧 snapshot を保持するため、built-in consumer はその旧 item からも sidecar の完成 binding を解決する。元 snapshot の geometry、world、材質、共有 owner は変更しない。native へ渡す完成 snapshot は元 geometry を保った別 publication とし、材質の保留 index も同じ対応先へ移す。

callback が Scene/公開 SMR を直接観測し得る場合は、その callback を実行する前に全 pending を完了する。前回のように callbackful な Dispatch/Sync を observer 内で呼び、再入 guard で隠す方式は使わない。完了 handler の native 部分は上記 Submit だけとする。準備期の callback は元の観測順を維持して全経路へ倒し、元 dispatch の観測点以降は完成 ticket のみを pending として扱う。失敗時は全未完成 publication を無効化してから通知を返し、通知側は外部 callback 自体を省略しない。

SMR の `gpuSkinnedThisFrame=false` は未投入 ticket の可用性を正確に示す。`skinnedVertexBuffers` は従来経路の mutable 再利用資源であり、現在姿勢の公開境界は `ResolveSkinnedVertexBuffer` / `ResolveSlotSkinnedVertexBuffer` と可用性である。非所有の過去 `IBuffer*` は、出力世代の変更後も将来の pose を永久追従する参照ではない。C# SDK と native managed binding (`Projects/Managed` / `Engine/src/Scene/Managed`) は計算済み GPU 出力を公開しない。未知 native Script/user pass のある view は初めから全経路を使う。scope 外の旧 snapshot consumer は Scene を参照せず、自身の ticket だけで元入力を完成する。

Core の owner-thread 境界は Logger、Importer、asset path、render wait pump、native render hook の実行前に通知する。handler は全 native 結果と現在 SMR の可用性を確定してから、実際に公開済みの snapshot が変わる場合だけ標準所有値の COW を行う。旧 owner は handler の外まで保持する。Core 汎用通知が callbackful な再準備を実行することはない。

Editor 統計の `skinningDemandMode` は設定、`Candidates/Required` は renderer 数、`Prepared/Skipped` はその view が保持する未投入 request 行数（別 view からの再利用を含む）、`Submitted` はその scope が実際に初投入した行数。別 view の group がまとめて必要になると Submitted がその view の Prepared を超え得る。GPU 省略の最終値は view 末尾の Skipped で確認し、これを FPS の改善量と同一視しない。

## 検証

同一 binary の flag OFF/ON で、layer/frustum/distance 外の実 GPU dispatch 減少と再出現時の頂点を検証する。shadow だけ、ray 別 view、mask、fiber、未知 native、nested scope、loader/Logger/wait-pump の callback で需要・pose・source・model が変わる場合も含める。旧 snapshot を保持したまま新 publication が完成しても、元 world/mesh/material と捕捉した pose が混ざらないことを確認する。native 失敗、resource reset/release、保持済み source pointer の直接更新、output lease の生存と再貸出、flag OFF の追加準備ゼロも実出力で表明する。

dispatch 省略件数、準備/完了/fallback、GPU native 成功 receipt と FPS/CPU/GPU 時間を別々に記録する。省略件数や回帰テスト合格から実時間の改善を推定しない。比較は [PIX の契約](pix-profiling.md) に従う。
