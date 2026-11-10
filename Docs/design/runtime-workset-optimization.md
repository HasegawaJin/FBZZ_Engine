<!-- @file    runtime-workset-optimization.md -->
<!-- @brief   問い合わせ・描画抽出・階層同期の作業削減と互換性の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-09 -->
# Runtime の作業領域と共通入力

2026-10-24追記: 所属/所有順を構造世代で確認する選択経路と、色可否・skinned走査の融合を追加した。legacy経路の説明は以下に残す。新経路ではScene/ComponentArray/FrameGeometryCacheのnative layoutが変わるためSDK consumerを再ビルドする。[切替・保証境界・計算量](render-extraction-scan-optimization.md)。

| 対象 | 保持する入力 | 各回に更新する値 |
|------|--------------|------------------|
| Physics query | World の非所有 Collider 列に対応する flat BVH | 同期・Step 後に境界を refit。登録が変われば等件数分割で再構築 |
| RenderScene | 同一フレームの不変 RenderObject 配列 | 材質、変形頂点入力、選択、マスク、各 effect のビュー別 snapshot |
| Static Mesh geometry | 現在値を照合したworld・逆転置・境界の私有データ | O(R)の照合、通知による失効、previous world、所属・描画フラグ |
| Object mask | 抽出済み物体の Entity index ごとの連結添字 | 要求の生存、世代、階層、layer、includeChildren |
| Navigation | System が所有する Entity・Surface・回避 hash の vector 容量 | フレーム先頭の Entity snapshot と bucket の所属 |
| Transform | スレッド別の連続 traversal buffer の容量 | local-to-world と描画補間の親先行 BFS、対象の祖先列 |

## Physics query

BVH は Physics 内部に置き、Scene・RHI に依存しない。問い合わせが使われた後の同期・Step の安全点でだけ構築・refit し、query のない World へ索引維持の走査を追加しない。初回、16 個未満の Collider、不正な境界では従来の全候補経路を使う。Raycast / RaycastAll / SphereCast / OverlapSphere の詳細形状判定は共通の既存実装を保つ。同距離の最近接結果は元の登録順を保つ。query traversal は escape 添字を使い、候補ごとの確保と固定長 stack の上限を持たない。

Collider は外部所有で、Update や公開寸法を直接変更できる。単独の問い合わせは寿命証明を先に確認し、生存する個体の現 AABB と同期時の境界を照合する。失効・差分があれば全候補へ戻し、失効した形状・剛体は filter より前に除外する。問い合わせは索引を書き換えず、複数 thread の read と filter 内の再入 query で作業領域を共有しない。Scene sync / Step / Collider 変更と query の並行実行は行わない。

CPU 粒子の通常更新、スクラブ、ループ検証では、実際に最初の球スイープを行う時点で照合し、その読み取り区間の全球スイープで結果を共有する。有効な索引の境界照合は Collider 数を C、球スイープ数を Q として O(QC) から O(C) になる。索引がないか古い場合は、その区間の各スイープで生存する全候補を使う。バッチを使う間は World と登録個体の寿命・形状・姿勢を固定する。

CCD はそのサブステップで最初に掃引が必要になった時点で、剛体ごとの登録順先頭の球を集める。掃引は剛体順に行い、先に速度が制限された剛体の現在速度を後続の判定で読む。Collider と剛体の照合は一度で、残る TOI 判定は CCD を必要とする球と登録済みの球の組数に比例する。

## Navigation のポリゴンとリンク

NavMesh はポリゴンの置換時に XZ 境界の BVH を構築する。高さスナップと経路の端点解決は同じ索引を読み、内包する面を優先し、複数の内包面や最近傍の同距離は元のポリゴン順で決める。既存の float 判定に対して安全な境界を作れない入力は線形探索へ戻す。リンクは出発ポリゴン別に登録順で保持し、A* の展開と経路組み立ては出発可能なリンクだけを読む。

## RenderScene

Graphics の読み取りは `RenderScene::Objects()` を共通境界にする。通常の単独抽出は所有 vector を返し、フレーム cache を使う抽出は同じ const 配列の shared owner を各 snapshot が保持する。cache の退役や次フレームへの移行でも、既に公開した snapshot の姿勢は変わらない。

geometry の候補は MeshRenderer と SkinnedMeshRenderer の Entity 列だけから収集する。Scene の所有位置で元の GameObjects 順へ並べ、同じ Entity を重複除去してから静的、スキンドの順に抽出する。骨だけの GameObject は候補にならない。Scene の配置を変えず、スレッド別の上限4 Scene の索引に Entity と所有位置を保持する。各取得で現在の Component 所属・所有位置・Scene 世代を O(R) で完全照合し、変化時だけ O(R log R) の再整列を行う。追加・削除、所有位置の穴、並べ替え、世代再利用を通知の欠落で見落とさない。返却先は呼出し側所有の Entity vector とし、Component・姿勢・材質の値は処理時に現在の Scene から読む。

Scene 全体の影境界も、同じ所有順索引の MeshRenderer・SkinnedMeshRenderer・Terrain 候補だけから合成する。同じ Entity 内の静的、スキンド、地形の順と直接 heightData 変更の即時反映を保つ。PrepareShadows の実行時点と lighting snapshot は変えない。影を無効にしていても native script・particle・volumetric・材質から行列を読む可能性があるため、設定だけで行列準備を省略しない。

callbackのないGeometryObjects走査では、現在のobject.worldと最大軸normをsubmesh間で共有し、TransformのTRS構築とscale算出を繰り返さない。Skinnedのbind-pose境界合成も同じ呼出し内の行列を使い、submesh順・半径重みの加算順を保つ。骨の境界球は従来どおりAnimator owner自身の行列で変換する。asset解決callbackを持つInstances経路では、従来位置で現在のTransformを読み直す。

通常の非SkinnedなMeshRendererは、Scene/Entity/Componentの寿命と現在のpose・Mesh入力を照合し、変わらない行列・逆転置・境界をフレーム間で再利用する。公開snapshotは独立した値を持ち、previous worldとview固有状態は毎回更新する。変更通知は同一フレームのgeometry共有を失効させ、行列は入力が変わった個体だけ作り直す。直接書き込みの照合はO(R)で残る。詳細は[静的Mesh再利用](static-mesh-render-reuse.md)。

ビューごとの選択は `IsObjectSelected(index)` で読む。材質と mesh 入力の配列は引き続き各 snapshot が所有するため、後から開いた Ray view の GPU 変形や別ビューの材質解決を先行ビューへ混ぜない。cache の Scene 世代・ResourceManager・reset 版・フレーム・RT 入力・palette の照合と、描画中に Scene を変更する場合の cache 禁止は維持する。

mask の索引は有効な要求があるときだけ作る。同一 Entity に MeshRenderer と SkinnedMeshRenderer がある場合も両方を保持し、元の物体順と階層順を変えない。Entity generation と layer を要求処理で再確認する。

### GBuffer材質の同値共有

GBufferの不透明列は、物体ごとに確保した材質CBの識別子ではなく、Hybrid markerを含む最終96バイトの値で材質順を決める。同値のCBは当該パス内で共有し、スロットが所有する元CBや公開したsnapshotを書き換えない。既存の自動instancingの描画比較は緩めず、テクスチャ・PSO・メッシュ範囲・全定数束縛の一致を引き続き検査する。透明の順序とForwardの独自材質は変更しない。これは[初期のinstancing設計](gpu-instancing.md)の次の最適化段で、不透明の材質グループの鍵だけを変更する。

GBufferのソート済み最終96byte群はgroup境界で保留batchを先に提出し、view所有のgbufferMaterialCBを既存のowned Updateで再利用する。同値groupの有効なdirect slot CBは従来どおり優先し、slot所有CBは変更しない。Update/Drawの命令順とnative Submit時のUploadArena sliceで先行drawの値を保持し、固有値ごとの一時CB Create/Releaseと長期cacheは持たない。未初期化view CBの生成に失敗した時は従来どおり当該passを実行せず、次pass/viewでは現在値から再分類する。色・テクスチャ・markerが異なる材質を同値とみなさず、次フレームの編集は新しい値として再分類する。

材質のCPU準備は一回のRenderScene抽出内で共有する。共有assetとshader descriptorから作る既定値を一度だけ組み立て、個体のfloat・整数上書き、Sprite UV、テクスチャの利用権・品質段とbindless添字は毎回適用する。GPU CBは各Materialの所有を保つ。GBufferの正規化値もshader handleと最終CPUバイト列の完全一致時だけ再利用し、異なる上書きやテクスチャ常駐段を混ぜない。次のビューでは準備を作り直すため、編集・asset差し替え・shader reloadを永久キャッシュへ閉じ込めない。

確認済みの標準shader descriptorはResourceManagerのowner/reset/shader版と生存する標準能力の一致時だけ再利用し、個体ごとの同期RHI照会を省く。custom shaderは従来の実体検証を使う。shaderのReleaseは能力台帳から旧世代を除き、reloadはshader版を変える。GBufferの同値バイト列はasset当たり最大32種類を当該抽出内に保持し、shader版も鍵に含める。上限を越えた固有パラメータは通常の正規化へ戻す。

descriptorのレイアウト比較は同じオブジェクトの場合だけ即時一致とし、別のpublicationやnative descriptorとの比較は値で検証する。GBuffer正規化は標準descriptorから10項目のコピー添字とoffsetを当該抽出内で準備する。既存の32種類の完成バイト列を先に探し、未登録の値だけ現在のnative descriptorの全変数名順・対象型・書込可否・offsetを照合して計画を使う。不一致やcustom shaderは従来の名前検索へ戻す。重複名の先頭優先、現在の入力バイト範囲、textureMaskのoffsetと96バイトの完全一致を保ち、shader reload・owner/reset・材質publicationの失効条件を緩めない。

texture同期の空参照はcallbackを呼ばず無効handleを返すため、そのslot内の重複witness照合だけを省く。実際のResolveGpu直後、全slot解決後、Sprite処理後、Upload前後は従来どおり現在のpublicationと資源版を照合し、変化時は回数を限定して再導出する。

[Render Thread](render-thread.md)のbuffer shadow照会は有効なBufferTagだけを同期する。静的メッシュの無効なdeformed bufferでCPU命令列をflushしない。材質準備後の変形版の再検証は、Ray入力・skinned・有効な内容版がそろう場合だけ行う。renderer shutdownの後に残るFontAtlasやMaterialのReleaseは、既に解放済みの所有資源へ何もせず、切り離したrendererのvirtual関数を呼ばない。

Raster抽出はmesh宣言のstrideとGPU handleを使い、Ray専用のshadow内容版を取得しない。Ray/Hybrid viewでは従来のvertex/index/deformed内容版を取得して実効strideを検証する。同一フレームの共有geometry cacheはRay入力の有無を鍵に含むため、後から開いたRay viewへ未解決版を渡さない。

geometryは従来どおり材質の機能判定だけを行う。materialsとinstance材質の同期前には、slotと共有assetの不変publicationを一組取得し、機能判定とqueue・両面・depth biasをまとめて導出する。EnsureMaterialAsset後は現在のslotを読み直す。別slotのcallbackでshaderの成功reload、ResourceManagerのowner/reset、asset世代が変わった場合は、次の材質状態の導出前に能力照会を失効する。再利用は一回の抽出内に閉じ、itemごとの追加保持領域やslotの可動アドレス、SceneのComponentを保持しない。

通常のRenderSystem呼出しはprivateな材質需要scopeを置く。材質の機能・queue等は従来どおり毎回導出し、その後でカメラ、layer/LOD、GPU frustum候補、shadow、static probe、rayの利用可能性を判定する。全利用先で不要と証明できるitemだけ、GPU材質同期・テクスチャ要求・パラメータ転送を省く。RenderObject/itemの所属・順序・visible・authoringは変更せず、次のviewは独立して再判定する。

native Script entry、既存user pass、object mask/selection要求、scope外または再入の抽出は従来の全同期へ戻す。ManagedScriptAdapterはnativeのOnSetupRenderPasses/OnPreRender/OnPostRenderをoverrideしないため、その登録だけでは未知consumerとしない。script/probeは抽出開始・公開前と利用先の境界で読み直す。数値的に安全でないboundsや未検証の材質opacityでは省略を拡大しない。MaterialDemandTraceの整数集計はこの同じ値判定を観測し、計測の有無で省略条件を変えない。

ロードcallbackや後段のsetupで需要が増えた場合、省略した全itemを一回だけ新しい材質cacheで補完する。抽出中は公開前の出力を完成し、公開後は同じgeometryを保持したcopy-on-write snapshotを作る。先行snapshotは変更せず、補完からの再入抽出は全同期へ戻し、補完を再試行しない。初回抽出の診断と公開後の補完scopeは別の観測として扱う。snapshotのframeStampはTime::frameCount、資源の寿命確認はResourceManager::FrameStampを使い、それぞれ同じ時計の値だけを比較する。

同じ公開snapshotで需要がないと確認した後は、後続のconsumer境界で判定入力がbit単位で一致するときだけ全itemの再走査を省く。キーはカメラ位置・向き、投影・距離・余白・layer設定、視錐台6平面の値、影・GPU frustum・probe・Ray・未知consumerの利用条件を含み、paddingやポインターの一致に依存しない。script・probeの現在の利用条件と資源owner/reset/frame・asset世代・Scene世代は毎回確認する。snapshotを置き換えた場合は記録を失効する。保持は一回の描画scope内に閉じ、次のframeや別viewへ判定結果を持ち越さない。

1%変更を含む実描画の比較と、需要変更を見落とさない負例検証は[部分移動の計測報告](../benchmarks/2026-10-23-partial-mesh-motion/Report.md)に記録する。

描画用SkinningComputeの候補は Scene の SkinnedMeshRenderer 専用索引から所有順の Entity 列を複製する。処理時には世代付きEntityから現在のGameObjectとComponentを引き直す。再入の深さごとに別の候補vectorを借り、終了時にはEntity値を消して容量だけを残す。programやCBが無効な場合は非アクティブ・無効Rendererも含めて出力publicationを失効する。索引は姿勢signature、入力と出力のnative検証、需要判定、GPU dispatchと既存出力の再利用条件を緩めない。

signatureが隣接して読むsource/outputのnative値は、actorごとに一回のCPU-frame drain後にまとめて解決する。戻り値はnative内容版・size・stride・UAV添字の値だけとし、GPU pointerやScene参照を保持しない。pump後はScene世代・Component lifetime・ResourceManager owner/reset/frame・現在のhandleを照合し、poseも現在の祖先から引き直す。handle変化時は従来照会へ戻り、資源更新/生成後まで読取結果をcacheしない。GetBufferMetadataのaccepted CPU版はnative成功版に代用しない。入力CopyData、出力確保、dispatch/最終書込receiptとfallbackは維持する。

owned skinning の再利用候補があるビューでは、シャドウ範囲の計算前に既存の描画命令を CPU フレームにつき一度だけ先行提出できる。描画ワーカーが空いている場合だけ提出し、CPU 上のシャドウ準備と並行する。native ディスクリプタに触れる処理へ進む前に、先行提出した命令の完了だけを待つ。source/output の内容版と UAV の照会は従来の順序で行う。`FBZZ_SKINNING_WAIT_OPTIMIZATION=0` で先行提出を無効化して計測できる。

描画用のprivateなBufferStateReadScopeは、登録済みの同じThreadedRendererとproducerに限り、待機完了後の空queue・worker idle・submission/read境界版・owner/reset/frameを非待機で確認する。この証明が現在も成立する間だけ空のdrainを省き、native poolの世代と各getterの現在値は毎回読み直す。Source準備と出力検証は従来のGet位置・内容版読取順を保ち、actor冒頭の値を別のnative getterを跨いで再利用しない。既に借りたIBuffer pointerからの直接更新も後続の読取で観測する。追加queue、Sync、通常Get、再入、世代変更、未知facade、workerからの呼出しは従来経路へ戻す。最初に必要な待機やGPUの処理自体を省略するものではない。

姿勢signatureは骨数prefix・全行列の行優先32-bit列・ベイク情報・slot/resource suffixの順を保つ。必要な容量を一回reserveし、連続した行列をresize後にmemcpyする。Matrix4の配置はコンパイル時に検査し、NaNのpayloadや符号付きゼロも正規化せず比較する。各viewは公開boneMatricesの現在値を読み直すため、同フレームの直接変更も検出する。traceの有無でコピー経路を変えず、reserveの実際の容量成長は既存のreserve・allocation集計へ記録する。骨数の増減と再入は既存の独立したscratch leaseで扱い、前actorの末尾や非所有参照を残さない。

SkinningのCPU候補は以前からSkinnedMeshRendererに限定されている。専用索引は候補の所有順sortをhit時に省く変更であり、GPU変形対象の追加削減ではない。signatureの直接比較は、actor冒頭の従来の一括読取が返した同じbufferStatesとuseBufferStatesが成立する場合だけ、既存gpuSkinningContentに対して同じ骨数prefix・全行列の生bit列・全slot/source suffixを照合する。一致かつ従来と同じoutputsReady条件と現在のidle読取境界が成立する場合だけ作業vectorの構築を省き、従来の出力再利用処理を共有する。出力未準備・未知facade・再入・ベイクは従来準備へ戻し、署名の不一致が確定した場合だけ後段の重複比較を省く。

追加の成功record・native reader登録・native再読取・paletteコピーは保持しない。GPU書込receipt、source/outputのfresh native versionとpump後の再解決は元の経路を維持する。公開boneMatricesや保持済みIBufferの直接変更も、元と同じ入力値に対する比較と既存のdirty準備で扱う。既存の出力再利用条件を厳しくも緩くもせず、dirty GPU dispatchの追加削減やsnapshotの変更は行わない。RenderingのSkinningPreparedReuse/SkinningFullPreparationは各passに最大一つの直接比較による再利用・通常構築の有無markerであり、CPU時間やactor件数ではない。traceのsourceObservedSlotsは構築経路のslot観測だけを数え、直接比較で構築を省いたslotは含まない。

## Animator のローカル姿勢作業領域

画面外の GPU dispatch を実際に省く経路は既定無効で、`FBZZ_GPU_SKINNING_DEMAND=1` の明示指定時だけ有効にする。需要・元入力所有・外部 callback 前の完了・不変 snapshot の契約は [GPU Skinning の需要と所有 publication](gpu-skinning-demand.md) に従う。

通常 FK と Bone 適用が共有する作業領域は、骨名・track 名のコピーと対応添字、二つの traversal visited 配列を actor 間で再利用する。骨格と clip の名前列は借用ごとに値で照合し、完全一致優先、同名なら元の先頭、完全一致なしなら正規化名の先頭という既存の探索順を保つ。アセットや前 actor の pointer は保持しないため、同名 clip のキー更新は現在の入力から読み取る。

マスク IO 中のログ通知で再入しても、深さごとに別の保存域を借りる。IO が可変 clip を変更した場合は従来どおり共有姿勢を失効し、Bone 適用で現在の track を再探索する。姿勢時計、Pose LOD、RootMotion、骨の公開と前フレームのパレット更新は既存の契約を維持する。

## Navigation と Transform

Navigation は通知中に Component が追加されてもフレーム先頭の一覧だけを処理する。flat hash は占有率を半分以下に保ち、衝突時は線形探索する。各 bucket の連結添字は入力順に追加し、回避の浮動小数点加算順を保つ。Surface は従来と同じく typeId ごとの最初の有効なものを使う。Update 後に非所有参照と Entity 値を clear し、容量だけを次回へ残す。

Transform は既存の即時同期・非アクティブ子孫の更新・物理描画補間を保つ。callback を呼ばない同期中だけスレッド別作業領域を借り、終了時に非所有参照を clear する。dirty 判定と公開 Transform の配置変更は導入しない。

## 検証と計測

WorldQueryIndexTests は候補外の詳細形状判定が省かれること、直接変更、refit、削除・再登録、最近接 tie、再入、無限距離と境界を既存の公開操作で検証する。RenderSceneExtractorTests は shared 配列の寿命、ビュー別選択、複数 renderer、mask の layer と generation を検証する。NavigationWorkspaceTests と TransformSystemTests は Scene 切替、通知中の追加、bucket 移動、広い非アクティブ階層を検証する。

実時間の改善率はこれらの回帰テストから推定しない。CPU/GPU の実時間確認は [PIX の計測契約](pix-profiling.md) に従い、同じ Scene・設定・Release 構成で baseline と候補を比較する。

起動時の固定フレーム待機だけでは、描画準備の完了や同じsimulation時点を保証しない。実時間の比較は、完了済みGPUパスの世代・出力・解像度とphysical serialの進行、必要パスの実行を記録し、その確認後の固定した実時間を描画しながら待ってから始める。Playtest の waitSeconds は単調時計の要求秒・実測秒を報告する。simulation の observedSeconds も別に記録する。FPSを開始条件や除外条件に使わない。短い区間と長い区間は、同じ連続captureの先頭600と全6000フレームなど、同じraw行から別々に集計する。内部の全streaming/PSO待ちを公開していない場合、その完了まで証明したとは扱わない。

## 参考

- [PBRT 4e §7.3 Bounding Volume Hierarchies](https://pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies)
- [Script Transform の同期契約](script-transform-contract.md)


## Skinning の同期を一回の owned 作業へまとめる

通常の Skinning は既存の owned 経路を選択し、ビュー内の要求・行列を所有値として捕捉してから、既存の `ExecuteResourceOperation` 一回で全 mesh group を処理する。`FBZZ_OWNED_SKINNING_BATCH=0` は従来の group ごとの同期と fallback、`=1` は owned 経路を明示的に選ぶ。未指定時は owned が既定だが、`FBZZ_GPU_SKINNING_DEMAND=1` を明示した場合は需要処理の control 経路を保ち、owned を明示した時だけ従来どおり owned を優先する。公開 API・クラス配置・export は変更しない。

shader/asset の解決は producer に残し、worker には所有済み要求・行列と型付き handle だけを渡す。group の native dispatch 失敗時は同じ operation 内で単体 dispatch へ戻り、成功した native 出力版の receipt が得られた時だけ Engine が現在の Scene 世代・Component lifetime・pose・source・owner/reset/frame を再照合して公開する。pump/callback に備える入力コピーと再照合は省かない。worker 内の rows と出力 handle 配列は一つの owned 作業内で容量だけを再利用し、次の group では内容を消す。group 数を G とした時、producer/worker 間の完了同期は従来の O(G) から一回の operation へまとまる。GPU dispatch 数や必要な palette/source 検証は削減せず、入力捕捉のコピー費用も残る。速度改善量は同条件の Release 計測で検証する。


## 静的Rasterのビュー別項目復元

`FBZZ_RENDER_EXTRACTION_SCAN_MODE=fused` と `FBZZ_RENDER_GEOMETRY_RESTORE_MODE=reuse` の両方を明示した場合だけ、静的Rasterの後続ビューで私有の既定項目を一度だけ初期化して再利用する。既定・`legacy`・未知値は従来の復元を維持する。混在Skinned、Ray、未認証の静的範囲は従来経路へ戻る。自動的な方式昇格はしない。

共通geometryの値だけをprototypeへ上書きし、出力は引き続きビューごとに所有する。prototypeへ材質・可視性・選択状態を書き戻さず、コールバック前に破棄する。最初のビューの圧縮と退役は次の出力がバッファを借りる前に完了する。材質の現在値、共有snapshotの寿命、直接変更検出、影・反射・Rayの独立した需要判定は変えない。

Iをメッシュ項目数、Vをビュー数とすると、対象の既定状態初期化は後続ビューでO(I(V-1))回からO(V-1)回になる。全項目のgeometry復元と幅広い項目コピー自体はO(I(V-1))のまま。共有compact配列はO(I)、保持するビュー出力はO(VI)で、追加の永続キャッシュはない。Scene照合・材質解決などの全件走査を取り除く変更ではない。

抽出部分の短縮をフレーム全体の高速化とは扱わない。1/2/4ビュー、最初のsnapshot保持と退役、Skinned/Ray fallback、更新・削除・再入を同じ実行物で比較する。コピー先への直接構築と圧縮／復元の融合は一部条件で悪化したため採用していない。
