<!-- @file    spatial-water-optimization.md -->
<!-- @brief   Shadow候補index・差分collector・Water波紋焼き込み・DX12容量診断の設計と比較契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-23 -->
# Mesh spatial indexとWater波紋焼き込み

通常meshのshadow候補を絞るGraphics内部index、CPU波紋textureの焼き込み方式、DX12 upload arena容量を、同じ描画条件で比較する。既定は線形shadow収集、全texel波紋焼き込み、16 MiB/frameのupload arenaとする。

本文はAPI・所有権・fallback・検証手順を定義する。性能の判断には別途run単位のsource/binary provenance、正しさの結果、費用とメモリ予算を揃える。

## 適用範囲と費用

| 変更 | sourceで行うこと | 残る処理・追加費用 |
|---|---|---|
| Shadow候補index | lightごとの独立frustumをunionし、通常static/skinned meshの既存leaf処理へ候補を渡す | legacy/full fallbackの全object投影・現在bounds照合、認証journalの変更ordinal更新、tree保持・更新、候補ordinalの所有と順序復元 |
| `DeferredDirtySet` | 同じowner寿命・keyのreason bitsをORし、所有batchへまとめて取り出す | producer側の変更検出は別。今回の利用はindexのbounds差分集約のみ |
| Water tiled bake | 16×16 texelのtileから、寄与し得る波紋だけを共通kernelへ渡す | tile判定・候補配列の費用。全面を覆う波紋では候補がほぼ減らない |
| DX12 upload arena容量診断 | 起動時に一frame当たり16/32/64/128 MiBを選ぶ | 2frame分のcommitted upload resource容量。画質を下げる設定ではなく、メモリ予算を変える |
| 計測 | production scope、named GPU pass、別runの件数・容量診断で比較する | scopeやtrace自体の費用。純粋な可視判定やGPU frame全体へ読み替えない |

cameraの距離・小物・frustum判定とGPU frustum候補経路、reflection各faceの独立所有、instance/fiber/terrainのshadow経路は維持する。今回indexを接続した本番consumerはShadowPassの通常static/skinned mesh収集だけである。helperの独立camera/shadow/reflection volumeテストは、camera/reflection本番への接続を意味しない。

Materialのdirty集約は今回実装していない。既存authoring snapshot/Revision、共有binding cache、GPU content hash等の契約は別に扱う。GPU uploadや全値照合の省略をこのcollectorから導かない。

## APIと更新境界

汎用collectorは[DeferredDirtySet.hpp](../../Projects/Core/include/Core/Containers/DeferredDirtySet.hpp)の`fbzz::core::DeferredDirtySet<Key, ReasonBits, Hash, Equal>`。単一threadの値型であり、event busやcallback dispatcherではない。

- `Reset(ownerGeneration)`へcaller所有の非ゼロ寿命値を設定する。frame番号ではない。
- `Mark(ownerGeneration, key, reasons)`は同じkeyをcoalesceし、古い寿命・ゼロ寿命・空reasonを拒否する。pending中のkeyとHash/Equalは安定している必要がある。
- `TakePending(ownerGeneration, output)`はvectorをswapしてcallerへbatch所有権を渡す。処理中の追加Markは次batchへ残る。callerは前batchを使い終えてから次をtakeする。
- 重複Markはgrow前に判定し、通常drain/resetはepoch更新でhash slotのhigh-water全走査を避ける。保持capacityとhash衝突の費用は残り、epoch wrap時だけstamp全消去を行う。

Graphics内部の[RenderMeshSpatialIndex.hpp](../../Projects/Graphics/src/Renderer/RenderMeshSpatialIndex.hpp)は`Refresh(sceneGeneration, sources, options)`と`QueryUnion(frusta, output, diagnostics, mode)`を持つ。`MeshIdentity`はsource entity index/generation、renderer種別と任意lifetime値を扱う。adapterが持たないComponent寿命tokenを捏造しない。現在boundsが同じ置換は空間結果を変えない。

`Refresh`は現在snapshotの全sphere witnessを比較する。boundsが変わったordinalを`DeferredDirtySet`へ集約し、leafと重複ancestorのrefitへ渡す。既定のlegacy更新では、scene世代・membership・並び・renderer種別等の変更でtopologyを再構築し、変更K件のtree更新でも全R件のprojection/witness照合はO(R)で残る。任意のstable/journal経路は、後述するproducerの完全な変更一覧を`ApplyChanges`へ渡して、この重複した全件投影・照合を省く。認証がない場合や適用条件を満たさない場合は全件`Refresh`へ戻す。

既定のlegacy更新では、初回の通常MeshRendererはstatic、skinnedはdynamic。通常meshのbounds center/radiusが変わるとdynamicへ昇格し、同じ個体の間はstaticへ戻さない。最初の1%移動には昇格による両tree再構築費が入り得る。warmの継続移動では同topologyのdirty leaf/ancestorをrefitする。sphereが変わらない回転は空間index更新を要しない。

50%を超えるbounds変更では、`refit`、`rebuild`、`linear` policyを比較できる。legacyのmembership変更や初回promotionによる再構築は別条件であり、`refit`指定ですべて回避できるわけではない。stableでも件数・boundedness・明示分類等のtopology変更は再構築する。dense linear fallbackでtreeが古くなった場合は、次にtreeを使うRefreshで現在値から再構築する。

## 変更ジャーナルと安定した葉の更新（継続最適化）

`FBZZ_MESH_SPATIAL_UPDATE_MODE=stable` と `FBZZ_MESH_SPATIAL_PRODUCER=journal` を組み合わせると、既存の現在値検査から不変の変更一覧を発行し、Graphics側の重複した全件投影・球照合を省ける。どちらも既定はlegacyであり、shadowの既定linearも変えない。比較では同じbinaryで値を切り替え、dense policyも明示する。

`RenderScene::spatialUpdate` の `RenderSpatialUpdate` は `version`、`baseVersion`、`objectCount`、`fullReplacement`、`changedObjectOrdinals` を持つ。versionはgeometry publicationごとの非ゼロ一意値で、viewのsnapshotSerialとは別。即時の前版、Scene世代、件数、paddingと全optionsが一致したときだけ差分を適用する。同じversionの再利用もこれらの互換条件が必要。cold、欠落版、未認証の手動snapshot、件数・順序・renderer寿命変更、無効なordinalでは従来の全件検査へ戻す。versionが飽和した場合も認証を止める。

この通知は現在のgeometry抽出中に、既存のstatic pose/shared-mesh witnessとbounds再計算の結果から発行する。新たな全Objects照合をproducerへ移してはいない。skinned・uncached・dense fallbackのboundsは保守的にすべて通知し、同じimmutable journalをviewやmaterial-onlyコピーで共有する。consumerごとに消費して空にするqueueではない。公開Transform/meshの直接変更、親子同期、Undoの値復元、serializerを守る既存producerのO(N)走査とpayload生成は残る。全Engineをsetter起点のO(k)更新にしたものではない。

`stable` の葉は現在snapshotのordinalに対応する。件数と各ordinalのboundedness・明示static/skinned分類が一致すれば、個体IDの置換・並べ替え・通常meshの移動だけではtreeを作り直さない。値とidentityを現在版へ更新し、変更した葉と祖先をrefitする。通常meshが移動しても、legacyの自動dynamic昇格は行わない。分類は不変物体であることの保証ではなく、両treeとも変更を反映する。件数・boundedness・明示分類・Scene・update modeの変更は再構築する。

stableの構築はcentroidの範囲で分割し、外向きに丸める球AABBを葉で一度作って親をmergeする。疎なrefitは祖先stampと降順sortを使い、祖先がnode数の1/4を超える場合は逆preorder走査で更新する。`FBZZ_MESH_SPATIAL_DENSE_UPDATE=refit` を明示してdense rebuildと分けて測る。長距離移動では固定topologyの選択性が落ち得るため、常に速いとは仮定しない。

validateは変更一覧で更新したcache同士を比較せず、現在のsnapshot Objectsを独立に全件投影した線形oracleと照合する。不一致なら元のfull shadow scanへ戻し、そのlineage認証を無効化する。意図的な通知漏れ・同versionの誤用も回帰テストで検出する。任意の外部producerが不完全なjournalを正しいものとして発行してよい契約ではない。

比較用の追加設定は完全一致で読む。update modeは初回固定、producer modeはgeometry build境界で読むため、変更するとlineageが再開する。実行中の設定切替をゲーム作者向けAPIとはしていない。

| 環境変数 | 値 | 動作 |
|---|---|---|
| `FBZZ_MESH_SPATIAL_UPDATE_MODE` | 未指定・`legacy`・不明値 | 従来のpromotion・再構築・全件更新 |
| 同上 | `stable` | 互換なordinal topologyと認証journalの受理。dense policyは独立で既定rebuild、dense refitには`FBZZ_MESH_SPATIAL_DENSE_UPDATE=refit`を明示 |
| `FBZZ_MESH_SPATIAL_PRODUCER` | 未指定・`legacy`・不明値 | journalなし。既存の全件互換経路 |
| 同上 | `journal` | 現在値検査の結果をimmutable journalとして公開 |

起動時の `SPATIAL_UPDATE_CONFIG mode=...` と、別diagnostic runの `deltaApplied`、`geometryReused`、`witnessedSources` を確認する。環境変数を渡しただけで実際に差分が使われたとは判定しない。traceの `refitSortedAncestors` と `refitScannedNodes` は別の更新費を示す。

N=current object数、k=journalに挙がったordinal数、A=重複除去した祖先数、V=独立volume数、c=返却数とする。適用可能な差分は期待O(k)のhash集約・値照合に加え、疎なrefitがO(k+A log(A+1))、dense側がO(k+A+nodeCount)。後者の分岐条件ではnodeCountがAの定数倍以内。full fallbackはO(N)照合、構築は通常O(N log N)。queryは訪問数とvolumeに依存し、返却sort込みで最悪O(VN+N log N)。常時O(log N)ではない。hash衝突・grow、epoch wrap、vector破棄とcache退役の費用も残る。

producerはO(N)のidentity容量、snapshotはO(k)のjournal容量を保持する。古いsnapshot/batchが残ればjournalも生存し、4 Scene slotはbyte上限ではない。`activeSlotRetainedBytes`はこのproducer/journal容量やprocess peakを含まない。process endpointと保持容量を分けて記録する。

## snapshot・世代・候補の寿命

[RenderSceneSpatialQueries.hpp](../../Projects/Graphics/src/Renderer/RenderSceneSpatialQueries.hpp)の`RenderSceneSpatialCache::Collect`がimmutable `RenderScene`を値入力へ投影する。cacheはthread_localで最大4 Scene世代。Scene/Component/Mesh/resource pointerは保持せず、値index・source scratchとweak snapshot ownerを保持する。

legacyまたは認証journalのない経路では、shared owner、非ゼロ`snapshotSerial`、`frameStamp`、`sceneGeneration`、padding、optionsの一致で同publicationを再利用し、serial=0なら毎回全件照合する。stableの認証journal経路は、Scene世代・件数・padding・optionsが互換なら同じgeometry versionを再利用でき、この再利用にsnapshotSerialの非ゼロは要求しない。異なるversionの差分適用には即時のbaseVersion一致も必要。poolの同addressやobject pointer一致だけではhitにしない。返却`CandidateBatch`はsnapshotのstrong ownerとordinal vectorを所有するため、後のRefresh、別view、slot evictionは既存batchを変更しない。Scene退役は別Scene query時にexpired weak ownerを検出して行い、次queryがなければ値capacityはthread終了まで残り得る。

[ShadowPass.cpp](../../Projects/Graphics/src/Passes/Geometry/ShadowPass.cpp)はcascadeとSpot/Point shadow viewのunionを一度作り、同じbatchをstatic/skinned収集へ渡す。camera frustumをshadow候補へ混ぜない。candidateの元snapshotはcaster提出完了まで保持し、resource callbackが別snapshotを公開しても既存item pointerのownerを失わない。無効snapshotは元のSceneInput経路へ戻す。

既存のcastShadows、layer/LOD、colorEligible、draw distance、cascade/punctual texel判定、skinned submesh、material/geometry条件はleaf側に残る。candidate ordinalは元のObjects順へ戻す。全RenderScene object/item payloadとpreviousWorld更新も維持する。BVH導入をpayload生成削減や全consumerの可視性変更として扱わない。

未知半径・非有限値・未検証の巨大sphere、退化frustum等は保守的に候補へ残す。node AABBの外向き丸めとendpointの座標規模を含むmarginで、巨大center/radiusの相殺を扱う。`validate`は現在sphereの線形unionと照合し、不一致時はERRORを出して元のfull shadow scanへ戻す。

## 起動時の比較設定とfallback

shadow mode・dense policy・update mode・traceはprocess内の初回読取りで固定する。例外の`FBZZ_MESH_SPATIAL_PRODUCER`はgeometry build境界で読み、journalを無効にしたbuildを挟むとlineageを再開する。比較は同じexe/DLL/shaderの別processで、childの設定を明示して行う。実行中の設定切替、Inspector項目、保存sceneの品質設定としては提供しない。

| 環境変数 | 値 | 動作 |
|---|---|---|
| `FBZZ_MESH_SPATIAL_MODE` | 未指定・空・`linear`・不明値 | 既定の元shadow loop。source projection/index維持/queryをbypass |
| 同上 | `bvh` | 小件数でもBVHを明示使用 |
| 同上 | `auto` | 64件未満は線形query、64件以上はBVH候補。projection/witnessは残る |
| 同上 | `validate` | BVHと線形unionを照合。不一致ならfull scan |
| `FBZZ_MESH_SPATIAL_DENSE_UPDATE` | 未指定・`rebuild`・不明値 | dense時に再構築 |
| 同上 | `refit` / `linear` | 同topologyのdense更新をrefit / 線形fallback |
| `FBZZ_MESH_SPATIAL_TRACE` | 未指定・空 | 件数traceを記録しない。計時runの設定 |
| 同上 | 未使用の出力file path | 別diagnostic runで有界JSONLを保存 |
| `FBZZ_WATER_RIPPLE_BAKE_MODE` | 未指定・空・`full`・不明値 | 既定の全texel bake |
| 同上 | 完全一致の`tiled` | 16×16 tile選択と共通bake kernel |
| `FBZZ_DX12_UPLOAD_ARENA_MIB` | 未指定 / `16` / `32` / `64` / `128` | 一frame当たりのDX12 upload arena容量。未指定は16 MiB |
| 同上 | その他の明示値 | WARNを出し16 MiBへfallback |

文字列は大小文字・余分な空白を補正しない。`auto`の64件とdense比率50%の採否は同条件の計測で判断する。不具合・悪化の切り分けは新processで`linear`、`full`、arena `16`へ戻す。assetやshaderの変更は要らない。

## DX12 upload arenaとメモリ予算

[DX12Renderer.cpp](../../Projects/Graphics/src/Renderer/Platform/DX12/DX12Renderer.cpp)はrenderer初期化時に`FBZZ_DX12_UPLOAD_ARENA_MIB`を読み、[DX12UploadArenaSizing.hpp](../../Projects/Graphics/src/Renderer/Platform/DX12/DX12UploadArenaSizing.hpp)の`ParseUploadArenaMiB`で完全一致の4値だけを受ける。符号、単位、空白、先頭ゼロ、空文字、範囲外の数値を変換せず、明示無効値は既定16 MiBとする。初期化後に容量を動的変更しない。

arenaは2frame分のcommitted upload resourceを所有する。既定16 MiB/frameは計32 MiB、比較用64 MiB/frameは計128 MiBで、追加capacityは96 MiBになる。これは確保容量であり、実使用量、process working set、GPU専用memory使用量の実測値ではない。初期化logのbytes/frame・frameCount・totalBytesをrun記録へ残す。

容量変更はshaderや描画品質を変更しない。比較ではarena不足の有無と描画結果を確認し、費用の差と追加メモリを併記する。値を大きくすること自体を高速化と見なさず、メモリ予算を満たす条件で選ぶ。

## Waterのbyte契約と既存CPU処理

[RenderWaterExtractor.cpp](../../Projects/Engine/src/Scene/Systems/RenderWaterExtractor.cpp)の`UpdateRippleState`はfull/tiled共通の`bakeRegion`を使う。texel-center座標、per-texelの既存ゼロtail条件、波紋の加算順、[WaterComponent.hpp](../../Projects/Engine/include/Engine/Scene/Components/WaterComponent.hpp)の`WaterRippleWave`、RGBA8量子化を維持する。tiledは順序を保つ浅い候補配列を渡し、空tileの中立値はRGB=127、A=255とする。

tile world AABBと距離判定は丸めを保守膨張する。非有限・overflowなど除外を証明できない境界は従来のper-texel経路へ残す。signature再利用、GPU resource存在確認、入力変更時の再bake、最後の波紋消失時のclear、upload経路を維持する。active非有限入力には既存抽出policyを適用し、NaNからbyteへの変換同値を主張しない。

tiledは波紋の局所支持で評価数を減らし得るが、全面支持ではtile判定・候補管理費が加わる。自動適応は追加しておらず、実測前に既定化しない。波紋なしの広いocean、Gerstner計算、GPU微細波、SSR・屈折が主費用の場合の短縮も仮定しない。

既存のCPU波処理には`WaterComponent::PrepareWaveCache`の係数共有と[WaterSurfaceQuery.cpp](../../Projects/Engine/src/Scene/WaterSurfaceQuery.cpp)の高さ・水面照会がある。Simulation/LiveQueryのsnapshot契約も既存のまま。この変更はそれらの再導入でも、任意のuser HLSLを自動解析してCPU浮力へ同期する仕組みでもない。波形・meshFade・height scale等のCPU/GPU契約は[water-waves.md](water-waves.md)と実装直近の注記を正本とする。shader、LOD、SSR・反射品質は変更していない。

## profilerとメモリの読み方

| production scope | 含むもの・制限 |
|---|---|
| `RenderSceneExtractor::GeometryObjects` | renderer候補取得、現在値witness、bounds/worldとobject/item生成、cache処理が融合。純粋な候補抽出時間ではない |
| `RenderSceneExtractor::Materials` | 需要判定、reuse、parameter準備、同期・copy・texture、ray入力等。後段binding復元を含む全material費とも限らない |
| `RenderMeshSpatial::SourceProjection` | legacy/full fallbackは全N Objects、適用可能な認証journalは変更k ordinalのMeshSource投影 |
| `RenderMeshSpatial::WitnessAndUpdate` | legacy/full fallbackは全N照合、認証journalは変更k ordinal照合。rebuild/refit/fallback・容量算出を含む |
| `RenderMeshSpatial::{Rebuild,Refit,QueryUnion}` | tree更新とunion query。後段のexact leaf/payload/submitは別 |
| `RenderMeshSpatial::{ShadowCandidates,ValidateUnion}` | 候補処理全体と現在snapshotの独立全件投影・線形検証。validate費をBVH通常計時へ混ぜない |
| `WaterRipple::{Bake,Upload}` | CPU焼き込みとCPU側upload呼出し。UploadはGPU copy timestampではない |

親子scopeのinclusive時間を足さない。Materialの細分scopeは既存の明示filter診断時に限られ、時計費用とexternal durationによるSelf unavailableを含む。scope名から未分離の候補・可視性・payloadを推定しない。GPUは実際に返るnamed passごとに集計し、available/complete、view、寸法、device epoch、source/physical frameのprovenanceを確認する。部分pass和をGPU frame全体やFPSと呼ばない。

spatial traceは最大4096 query行のprefixを保持し、正常Graphics teardown時にheader/rows/summaryを書き出す。親directoryは事前に存在する必要があり、既存fileは排他作成で上書きしない。MSVC narrow CRTのpath規則を用いる。traceのreserve、mutex、copy、終了時I/Oが加わるため、**計時runではTRACEをunsetにし、同条件の別診断runで件数を取る**。dropありはprefixであり、summary欠落を完成captureと扱わない。

`activeSlotRetainedBytes`はactive slotのindex/dirty/source scratchのcapacity gauge。queryごとに加算せず、同条件の最大値等として読む。4slot全体、返却candidate vector、共有RenderScene payload、process working set/commit、GPU memory、総allocation回数ではない。trace buffer容量も別項目である。既定linearはCollect自体を呼ばないため、traceが0行でも候補0件やmemoryゼロを意味しない。

## 再現手順

repository rootで、正規の`Tools/AgentBuild.ps1`を用いて対象source/testを確認する。build ownerを一本にし、raw cmake/msbuildや別presetへの逃避をしない。新規テスト登録と同じbinary世代の統合を確認してから実行する。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 test -Filter 'DeferredDirtySetTest|RenderMeshSpatialIndexTest|RenderSceneSpatialQueriesTest|WaterRenderResourceTest|DX12UploadArenaSizingTest'
```

次はReleaseの既存render harnessを、一条件だけ独立processで起動する例。exeの実在と今回source/DLL世代を先に確認する。envはchildだけへ渡し、親Editorや他runへ残さない。

```powershell
$runInfo = [System.Diagnostics.ProcessStartInfo]::new()
$runInfo.FileName = (Resolve-Path '.\build\Release\Binaries\Release\Tests\FBZZTestsRayTracingEngine.exe').Path
$runInfo.WorkingDirectory = (Get-Location).Path
$runInfo.UseShellExecute = $false
$runInfo.Arguments = '--gtest_filter=RegularMeshRenderPerformanceTest.ShadowOffscreenHeavy10000'
$runInfo.EnvironmentVariables['FBZZ_MESH_SPATIAL_MODE'] = 'bvh'
$runInfo.EnvironmentVariables['FBZZ_MESH_SPATIAL_DENSE_UPDATE'] = 'rebuild'
$runInfo.EnvironmentVariables['FBZZ_DX12_UPLOAD_ARENA_MIB'] = '16'
$runInfo.EnvironmentVariables.Remove('FBZZ_MESH_SPATIAL_TRACE')
$runInfo.EnvironmentVariables['FBZZ_REGULAR_MESH_MOVE_PERCENT'] = '1'
$runInfo.EnvironmentVariables['FBZZ_REGULAR_MESH_CHURN_PERCENT'] = '0'
$runInfo.EnvironmentVariables['FBZZ_REGULAR_MESH_CAPTURE_COLD'] = '1'
$runInfo.EnvironmentVariables['FBZZ_REGULAR_MESH_WARMUP'] = '120'
$runInfo.EnvironmentVariables['FBZZ_REGULAR_MESH_SAMPLES'] = '120'
$runInfo.EnvironmentVariables['FBZZ_REGULAR_MESH_TIMING_ONLY'] = '1'
$runProcess = [System.Diagnostics.Process]::Start($runInfo)
$runProcess.WaitForExit()
$runProcess.ExitCode
```

計時時は他のMaterialDemand/RenderCpuTrace/FrameTimeline診断も無効であることをrun envelopeへ記録する。詳細scopeとspatial traceは別processへ分け、traceには新しいfile名を渡す。`linear`→`bvh`→`auto`を同じlayout・移動率・frame数で比較し、順序を交互にした複数runを残す。`validate`は正しさ診断として別実行する。arenaの16対64 MiBを比べる場合はchildのenv値だけを変え、2frame合計容量と追加96 MiBを記録する。

最低限のmatrixはshadow有効のall-visible/partial/offscreen-heavy、移動0%/1%/100%、生成削除、cold/初回promotion/warm。pixel hash・非background coverage・camera可視数・shadow結果の一致を確認し、同じexe/Engine/Graphics/RHI DLL・shader SHA、実module path、GPU/driver、描画寸法、camera、設定、warmup/sample数を保存する。

Waterは同じbinaryを`full`と`tiled`の別processで起動し、`WaterRenderResourceTest.*Ripple*`の全RGBA oracle・signature/消失/境界回帰を両modeで確認する。既存Water CPU照会・相互作用回帰とGraphics Water shader回帰も別に実行する。Control=波紋0、Heavy=局所24、Covered=全面支持24を同条件内で比較し、固定frameの画像も照合する。局所Heavyと小水域CoveredのGPU時間を直接比較しない。Playtestのlockstep固定dtやsnapshot採取費から通常FPSを逆算しない。

## 回帰テストと適用限界

| 境界 | 回帰の確認対象 |
|---|---|
| `DeferredDirtySetTest` | 寿命違い拒否、reason OR、hash衝突、drain中通知、重複grow抑止、burst後sparse batch |
| `RenderMeshSpatialIndexTest` | 独立volume・元順序、直接bounds変更、refit/dense policy、membership/世代、境界sphere・巨大相殺・退化frustum |
| `RenderSceneSpatialQueriesTest` | 既定bypass、無効snapshot fallback、同publication再利用、旧snapshot保持、pool同address新serial、slot eviction |
| `WaterRenderResourceTest`と既存Water回帰 | full/tiled全RGBA oracle、canonical順序・zero tail、signature再利用、last-ripple clear、数値境界、CPU照会とGPU描画契約 |
| `DX12UploadArenaSizingTest` | 未指定16 MiB、4つの許可容量、20種類の無効な明示tokenの16 MiB fallback |

sizing testはparserの契約を検証し、実deviceのresource確保成功やarena不足を証明するものではない。spatial helperの候補一致は、後段の全shadow描画結果や未接続camera/reflectionの高速化を証明するものではない。Waterのbyte oracle、実GPU描画、計時、メモリ容量はそれぞれ別の検証結果として記録する。

実測レポートにはraw rows、採用・除外理由、source/binary/shader SHA、run設定、正しさの結果、CPU p50/p95、named GPU pass、active slot容量とarena確保容量を残す。scope/traceの追加費用を分け、機能や画質を変えるablationを同品質の速度差へ混ぜない。既定や接続consumerの拡大は、この比較結果とメモリ予算に基づいて判断する。
