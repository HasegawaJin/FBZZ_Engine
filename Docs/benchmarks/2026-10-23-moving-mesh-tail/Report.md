<!-- @file    Report.md -->
<!-- @brief   変更あり1万体のP99調査とsnapshot保存領域再利用の同条件比較。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-23 -->
# 変更あり1万体のP99と描画snapshotの保存領域再利用

**1%変更・全件変更の1万体で、フレームP99をそれぞれ9.0063 → 6.2325ms（30.8%短縮）、11.8057 → 7.6697ms（35.0%短縮）へ改善した。** 独立再測定でも両条件が短縮した。採用変更は、退役済みsnapshotの空vector容量を再利用し、毎フレームの大きな確保・解放を減らすEngine私有実装。1,000体は主比較で悪化、再測定で短縮しており、全条件で安定して改善したという主張はしない。

## 原因と確認できた範囲

前回の遅いフレームはPresentやBeginFrameよりRenderSystemが支配的だった。[前回記録の再解析](PreviousPhaseEvidence.json)。今回の集約scopeでもExtractRenderSceneが主な処理時間を占めた。個体ごとのscopeを大量発行した初期調査では全件変更の600フレーム中33件が記録overflowしたため、不完全なscope時間を原因判定や比較値に使わなかった。後続は集約GeometryObjectsだけを記録し、全600件が完全な記録であることを検証した。[初期調査](Discovery-ProfileFacts.json.gz)、[RenderSystem内訳](Phases-ProfileFacts.json.gz)、[最終集約scope](FinalObjects-ProfileFacts.json.gz)。入れ子のinclusive時間は加算しない。

このビルドのRenderObjectは296byte、RenderMeshItemは968byte。1万体ではそれぞれ2.96MBと9.68MBの配列を毎回作り、compact共有配列も別に作っていた。warmup後でも、RenderSystemの前後で取得したプロセスPageFaultCount差分は約3,426回／フレームあった。配列容量の再利用後は333回まで約90.3%減り、同じ区間の描画スレッドcycle数中央値も約32～33%減った。大配列を使い捨てるCPU側の処理が、エンジンから減らせる大きなコストだった。

| 診断条件 | ページフォールト中央値 前 / 後 | thread cycles中央値 前 / 後 | RenderSystem中央値 前 / 後 |
|---|---:|---:|---:|
| 1万体・1%変更 | 3,427 / 333 | 15,229,103.5 / 10,156,608.0 | 7.0865 / 4.7340ms |
| 1万体・全件変更 | 3,426 / 333 | 16,110,018.0 / 10,939,209.5 | 7.4265 / 5.0325ms |

これは診断機能を有効にした別実行の値で、後述の正式時間比較には混ぜない。PageFaultCountはプロセス全体の差分でsoft faultも含み、ディスクI/Oや個々の割当先を確定する値ではない。残る333回すべての帰属や、過去の個別P99スパイクが同じ機序だったことまでは断定しない。[診断原本集計](PoolMemory-MemoryFacts.json)、[Microsoft PROCESS_MEMORY_COUNTERS](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters)。

## CPUスタックの追加確認

ユーザーの管理者許可後、正式比較の終了後に変更前DLLだけで1%変更／全件変更を各2回、計2,400フレーム記録した。8,879,774イベントを読み、イベント・バッファ欠落は0。遅い上位1%の各6フレームも主にscheduled状態で、今回の記録では長いスレッド待機が主体ではなかった。scheduled時間には割込み実行等が含まれ、純粋なEngine実行時間とはしない。

遅いフレームのサンプルにExtractRenderScene、RenderObject/RenderMeshItem構築、vector<RenderMeshItem>::_TidyとRenderScene破棄が現れた。全計測フレーム中、item vectorの_Tidyを含むstackは各試行290・266・379・201標本。大配列の生成・退役が描画経路にあることをCPU stackでも確認した。kernel内部は未解決module+RVAを保ち、関数名やページフォールトの種類を推測していない。stack標本数は経過時間の内訳ではなく、前後のETW時間比較にも使っていない。[取得・欠落・実行物の監査](CpuTraceEvidence.json)、[解決したstack](ResolvedFrameSamples.json.gz)、[scheduler内訳](ScheduleSummary.json.gz)。

## 実装と寿命

最後の所有snapshotが退役した後、itemをclearして変形ticketなどの所有権を解放し、空のitem/object vector容量だけをorigin poolへ返す。新snapshotはこの容量を借り、現在の値を従来どおり全件抽出する。shared姿勢vectorも最後の所有者が消えるまで貸し出さない。aliasing shared_ptrで公開型と所有期間を保ち、別スレッドでの退役はorigin poolのmutexで受ける。Scene・Component・GPU資源を空きpoolへ残さない。[C++ aliasing constructor](https://eel.is/c++draft/util.smartptr.shared.const)、[設計](../../design/static-mesh-render-reuse.md)。

空きpoolは抽出スレッドごとにitem最大2本・各16MiB、object最大2本・各8MiB、合計最大48MiB。上限超過やslot満杯は通常解放する。使用中snapshotの必要容量はこの上限とは別。公開の値返しExtractRenderSceneGeometryは従来の所有方式のまま。同一フレーム共有用compact配列の確保は残る。公開layout/API、描画品質・可視判定・previousWorldの規則は変更していない。

## 同条件の正式比較

ユーザー承認済みの既存FBZZ Release LTO環境。i7-14700F / RTX4070、DX12、1920×1080、Forward Unlit、instancing・shadow・VSync無効。affinity240（4論理CPU）、High priority、HighQoS検証済み。ABBAAB、各版3プロセス。1,000体は600 warmup、他は120 warmup後、各600標本。P50は各プロセスP50の中央値、P95/P99は各版1,800標本を結合して線形補間。単位ms、負の変化は短縮。Profiler・ETW・FrameTimeline・thread cycle・page fault診断を停止し、読戻しと出力は計測外。前後同一exe・shaderでEngine DLLだけを隔離した。[環境とhash](Final-gpu-Environment.json)、[分布](Final-gpu-Summary.json)。

CPUのフレーム経過時間は変更適用、WaitForCpuFrame、BeginFrame、RenderSystem、EndFrameを含む。このプロトコルは非表示HWNDの画面外RenderTargetへのnative実描画で、「全件可視」はfrustum内という意味。通常の画面表示やEditorの入力遅延、GPU総所要時間の測定ではない。前回の不要Present削減は両版に入っている。

| 条件 | P50 前 / 後 | P50変化 | P95 前 / 後 | P99 前 / 後 | P99変化 |
|---|---:|---:|---:|---:|---:|
| 1,000体・全件可視 | 0.6971 / 0.7458 | +7.0% | 0.7854 / 1.0082 | 0.8466 / 1.0923 | +29.0% |
| 10,000体・100体可視・無変更 | 7.3657 / 4.7343 | -35.7% | 9.6146 / 6.4770 | 10.5455 / 8.6509 | -18.0% |
| 10,000体・1%変更 | 6.6737 / 4.7617 | -28.6% | 8.1643 / 5.5109 | 9.0063 / 6.2325 | -30.8% |
| 10,000体・全件変更 | 7.4304 / 5.2193 | -29.8% | 9.7168 / 6.5176 | 11.8057 / 7.6697 | -35.0% |
| 10,000体・反対向き2ビュー | 15.9228 / 11.9864 | -24.7% | 19.2060 / 15.2200 | 20.9884 / 17.6392 | -16.0% |

![全5条件のP99変化](chart.svg)

全計測フレームのobject・cull・draw・triangle件数と1ビュー目の最終画像hashが一致した。2ビュー目は従来と同じ320×180で、反対側9,900体の描画件数と非背景画素を検査し、その画像hashは取得していない。

## 独立再測定と同一版対照

変更あり2条件を同じ順序・標本数でもう一度計測した。[再測定](Confirm-gpu-Summary.json)。

| 条件 | P50 前 / 後 | P50変化 | P95 前 / 後 | P99 前 / 後 | P99変化 |
|---|---:|---:|---:|---:|---:|
| 10,000体・1%変更 | 7.7073 / 4.6483 | -39.7% | 9.5876 / 6.6996 | 11.0448 / 7.6595 | -30.7% |
| 10,000体・全件変更 | 7.5108 / 5.3427 | -28.9% | 9.9384 / 6.6096 | 11.6180 / 7.9063 | -31.9% |

同一baseline DLL同士のA/A。P99は両条件とも増加し、環境によるtailの変動が残った。正式比較・再測定の短縮と、診断のfault/cycle減少を合わせて採用を判断した。A/Aを差し引いた因果推定や統計的有意差の主張はしない。[対照](Control-gpu-Summary.json)。

| 条件 | P50 前 / 後 | P50変化 | P95 前 / 後 | P99 前 / 後 | P99変化 |
|---|---:|---:|---:|---:|---:|
| 10,000体・1%変更 | 7.6555 / 6.9147 | -9.7% | 9.4679 / 8.9842 | 12.3019 / 13.2677 | +7.9% |
| 10,000体・全件変更 | 7.4678 / 7.5447 | +1.0% | 8.9386 / 10.2594 | 10.7251 / 11.9890 | +11.8% |

1,000体の独立再測定と、その同一版A/Aも残した。主比較P99は+29.0%、再測定は-6.7%、同一版A/Aでも-15.6%動いた。1,000体の診断ではfault中央値は前後とも0であり、大配列のページ供給削減と同じ利益は確認していない。小規模Sceneでの安定した改善は未達である。

| 条件 | P50 前 / 後 | P50変化 | P95 前 / 後 | P99 前 / 後 | P99変化 |
|---|---:|---:|---:|---:|---:|
| 1,000体・全件可視 | 0.7882 / 0.7701 | -2.3% | 1.0259 / 0.9735 | 1.1825 / 1.1034 | -6.7% |

同一版A/A:

| 条件 | P50 前 / 後 | P50変化 | P95 前 / 後 | P99 前 / 後 | P99変化 |
|---|---:|---:|---:|---:|---:|
| 1,000体・全件可視 | 0.7912 / 0.7180 | -9.3% | 0.9598 / 0.8206 | 1.0543 / 0.8895 | -15.6% |

## 正しさとビルド

新しい実抽出の回帰テストは、保持中snapshotの不変性、別スレッドでの最後の所有者退役、次の抽出でのMesh・visibility変更、previousWorld、Scene Clearを公開出力から検証する。返却前のitems.clearを意図的に除いた本体変異では、3回目のitems件数が1ではなく2となり失敗した。本体と入力mapを保存したbyteへ正確に戻し、同じtest exeで合格を確認した。[失敗](Negative.json)、[復元後](Restored.json)、[実行物](Tests.json)。

最終Engine DLLと一致する隔離runtimeで抽出・材質・geometry100、実DX12描画230（既存disabled3）、Graphics CPU304、Skinning58、重複を除いて計692合格。新規回帰はAgentBuild経由のCTestでも合格。変更C++のcheckと既存Release LTO buildをAgentBuild経由で実施した。native入力1,358件のmembershipと全source hash、6 DLLのexport名・ordinalを照合し、既存の6つの.defはbyte同一。前後の5 DLLと計測exeも同一で、生産差はEngine DLLだけ。[最終監査](FinalArtifactAudit.json)、[入力監査](InputAudit.json)、[成果物同一性](ArtifactEvidence.json)、[作業差分](TaskDelta.patch)。

## 保存物

採用コードの正式比較・再測定・A/Aと別診断のREGULAR_MESH_JSON全記録を[原本manifest](RawManifest.json)から参照できる。各圧縮JSONはconfig、全600フレーム、validation等の記録を保持する。診断の負荷を正式時間比較へ混ぜていない。試した初期化方法だけの変更は採用せず、初期試行・取得失敗・一回限りのスクリプトはScratch/MovingMeshTail20261023に保存した。OS・ドライバー設定や必要なGPU fence待機は変更していない。

## 計測後の共有ビルド状態

正式比較と最終native監査の完了後、21:02 JSTの別configureで共有Release cacheのFBZZ_RELEASE_LTOがOFFになっていることを確認した。本作業はその設定を戻していない。計測したLTO版DLLは各隔離runtimeへ保存済みで、現在のrootの6 DLLも計測時のhashと一致する。上記Release LTOの結果は保存した実行物についてのもの。計測時点の監査と現在状態を区別する。[計測後状態](PostMeasurementState.json)。

現在は別作業で新規のnative入力も追加され、計測時の1,358件とmembershipが異なる。計測に使った1,358件のsource hashは現時点でもすべて一致している。追加分を今回の計測・検証済みとして扱わず、共有input mapのmembershipも書き換えていない。
