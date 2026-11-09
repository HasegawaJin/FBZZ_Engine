<!-- @file    fiber-rendering.md -->
<!-- @brief   用途を限定しない表面繊維の Shell / Fin / Blade 描画契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-09-17 -->
# Fiber — 表面繊維の共通描画

## 静的 Mesh の表面ペイントと大量配置 (2026-10-11)

Scene View で Mesh またはモデルの親を選び、`Add Grass` で Short lawn / Meadow / Tall grass を追加する。
`Start empty` なら空から塗り、通常は全面配置から道などを消す。親を選ぶと子の静的 Mesh を一括登録する。
既存の `FiberComponent` を使い、用途別の Component 型は増やさない。

- `chunkedPlacement` は新規の静的 Blade 配置を選ぶ。旧シーンの既定は false で、従来の根元・密度単位を維持する。
- 新規配置の `bladeDensity` はワールド面積 m² あたり。幅はワールド m。非一様スケールも面積と幅へ反映する。
- `chunkSize` はローカル m。草だけを内部チャンクに分け、Mesh と Hierarchy は変更しない。
- `maxSlope` はワールド上方向からの度数。既定 55 度で側面・裏面を除く。
- `maxBlades` は個体全体の根元数の予算 (既定 100 万、最大 200 万)。32,768 本は GPU バッファ 1 ページの上限として維持し、必要なページ数へ分割する。
- `bladeSegments` は高さの分割数 [1,6]。既定 6 は従来と同じ 72 頂点、3 は 36 頂点、1 は 12 頂点。根元・幅・毛丈・風・接触は共通だが、少ない分割は曲がった輪郭を直線で近似するため同品質ではない。短い芝を遠くから見る用途で明示して選ぶ。
- `shadowBladeSegments` は影だけの高さ分割数 [0,6]。既定 0 は本描画を継承し、1〜6 は影だけを上書きする。例えば本描画 3・影 1 で提出頂点を減らせる。既存シーンとマテリアルは 6・0 のまま外観を維持する。距離 LOD の密度・消失距離とは独立した設定で、自動の段階切替は行わない。
- `castShadows` は Fiber 自身の投影を制御する。既定 true で旧シーンを維持し、Mesh / Skinned Mesh の投影設定と AND して描画入力へ渡す。Terrain の草も個別に無効化できる。受影・元表面の影・色・根元の数と資源・風・velocity・選択はそのまま保つ。
- `.fiberpaint` は個体別の CPU アセット。Component の `paintPath` は GUID 参照。共有 `.mat` は変更しない。
- ペイントは元の三角形番号と重心座標の格子へ保存する。材質 UV は使わず、UV なし・重複 UV を受理する。追加時は最大ワールドスケールから格子の目安 0.5 m、チャンクの目安 4 m をローカル単位へ変換する。長辺の最大分割数は 512、総セル数の上限は 1,048,576。
- トポロジー・頂点位置の署名が変わったペイントは拒否する。`Add Grass` で新しい面へ結合し直す。同じ形状へのプリセット変更は既存の塗りを複製して保持する。
- ファイルは `version=1`、`mesh_signature`、`cell_size`、`default_density`、`default_length` と疎な `cells` を持つ。位置・草の根元一覧・GPU 資源・ランタイム世代は保存しない。検証後に原子的保存する。
- 密度は候補根元の生成時に一度だけ反映する。従来の fiberMask (tex5) は独立した材質マスクとして残す。密度 0 のセル・長さ 0 のセルは根元を確保しない。
- 乱数は三角形・格子セル・候補番号・Seed から独立に決める。消去・高さ変更で他の根元を並び直さない。
- 個体ごとに配置キャッシュを持ち、入力世代が同じ間は三角形・マスクを走査しない。編集時も内容署名が変わったチャンクだけ根元と GPU ページを作り直す。
- ページごとの毛丈・曲げ込み bounds と安定した描画 ID を既存 Fiber パスへ渡す。画面外判定・距離 LOD・影・選択・velocity・風・踏み倒しは共通経路を使う。根元バッファ更新時は velocity 履歴をリセットする。

`Paint Grass` は Density / Height / Smooth、ワールド半径、Softness、選択 (子孫を含む) / 登録済み Mesh 全体、Visible surface only を提供する。
可視面限定はセル中心からカメラへの三角形交差で遮蔽物も確認し、正投影では平行な視線を使う。
高速ドラッグは画面の線分上でレイを補間する。ヒットのない区間は結ばない。
ドラッグ中は適用候補の線とブラシを表示し、離したときに一括適用する。Esc / カメラ操作 / Play / 選択・シーン・履歴変更では中断する。
Density / Height の目標値は同じ範囲への再適用で累積しない。Smooth は近傍値を平均する。

操作は `fiber.add` / `fiber.paint_mode` / `fiber.stroke` / `fiber.fill` / `fiber.commit_stroke` に登録する。
ストロークと一括追加はそれぞれ一件の Undo。Undo/Redo はシーン、Component 寿命、形状、ペイント内容を検証し、衝突時は履歴もデータも変更しない。
未保存の塗りは AssetDirtyRegistry の型付き draft に保持する。Ctrl+S / Save All と終了時確認へ接続し、Discard はディスクの内容へ戻す。

Terrain は既存の層ペイントによる Fiber 配置を使う。ここでの面ペイントは静的 Mesh 向け。
GPU 間接描画・複数個体の描画一括化・スキン Blade は次の段階とし、現在のページは共通パスへ個別提出する。

## 境界

`FiberComponent` を `MeshRenderer` / `SkinnedMeshRenderer` / `TerrainComponent` と同じ GameObject に置く。元の表面は既存 Material が描き、繊維だけを追加する。色・密度・長さ・曲げ応答は Fiber 用 `.mat` が正本。Component は参照と品質・描画方式だけを保存する。長さと曲げ変位はワールド m、UV の密度は UV 1 単位あたりのセル数とする。

Shell は元メッシュのインスタンス、Fin は元メッシュの辺から作る板。Hybrid は両方を描き、Fin の寄与を輪郭近傍へ絞る。Fin 単独は輪郭判定で消さない。短い芝は Grass の照明とワールド XZ の根元座標を使う。近距離の芝と背の高い草は Blade モードの連続した交差リボンで扱う。

## 導入順

1. 静的 Shell、共有 material 設定、環境風、Inspector / 保存、Fur / Grass のプリセット。
2. 静的 Fin、Hybrid。辺の重複・不正インデックス・UV 継ぎ目のテスト。
3. ランタイム検証を拡張し、影への投射・深度 / GBuffer / velocity の全経路を共通の形状 / coverage 関数へ接続する。
4. スキニング済みの表面と前フレーム姿勢への追従。
5. 地形パッチの配置・距離 LOD・Blade・残留する踏み倒し。

各段階でコンパイルと該当する契約テストを通す。未実装のモードを Inspector に出して Shell へ黙って代替しない。

## 描画契約

- `DrawCall` / `ResourceManager` と Platform/Backend の抽象を使う。現行ターゲットは DX12 / SM 6.8 ([DX11 撤去](dx11-removal.md))。Geometry Shader は必須にしない。
- 根元を固定し、正規化高さ h の二乗で曲げを与える。これは軽量な見た目用近似で、繊維の物理シミュレーションではない。
- 風は `Scene::FlowFrame().ambient` の m/s を読む。応答係数は material。シーンに置いた FlowField も頂点ごとに受ける (「局所 FlowField」節)。
- Shell の全層が同じ根元分布を読む。層ごとに乱数を変えない。先細りと高さ分布だけを変える。
- 基本は Alpha Clip と深度書き込み。半透明合成による柔らかい毛先は現段階の対象外。
- カリング球は毛丈と最大曲げ変位を含める。元メッシュの遮蔽判定をそのまま流用しない。
- 元の Material は土台の描画を担当する。Fiber 用 `.mat` は Fiber の欄へ割り当てる。
- 草の薄い葉の透過と毛の異方性反射は照明の選択で分け、形状評価は共有する。

## 段階 3 の描画経路

`FiberSurface.hlsli` が Shell / Fin の頂点変形と Alpha Clip を共有し、カラー・影・GBuffer・velocity の各ラッパーから使う。Hybrid の輪郭判定は、そのパスのカメラまたはライトから評価する。方向光の各カスケード、Spot、Point の各面へ繊維を提出し、土台の `castShadows` を尊重する。

GBuffer に繊維の法線・色・roughness・深度を書き、metallic は 0 とする。Deferred と Forward の GBuffer prepass の両方で同じ形状を描く。Deferred の専用 Fur / Grass 照明は LESS_EQUAL・深度書き込みなしで重ね、SSAO / Contact Shadow も受け取る。

Colorパスは共通Forward資源に加え、IBLのirradiance・prefilter・BRDF LUTをt16/17/18へ束縛する。IBL有効時は定数ambientを置き換えるため、これらを省くと直接光を遮った草だけが黒くなる。`FiberRenderPerfTest.ShadedBladesReceiveEnvironmentLightWithoutDirectLight`が直接光0の実GPU描画で環境色と無効化を検査する。

velocity は土台の world 行列が静止していても描く。ビューごとに前フレームの world・風・時刻・曲げパラメーターを保存し、現在と過去の変形後位置からジッターを除いた UV 差を求める。初回・非連続フレーム・時刻巻き戻し・描画方式 / 層数変更では履歴をリセットする。完成した HDR 深度との比較で、静止した遮蔽物の背後から速度が漏れるのを防ぐ。履歴はランタイム専用でシリアライズしない。

影は Alpha Clip であり、毛の透過を積分した影ではない。

## fiberMask テクスチャ

- Fiber 用 `.mat` の `[textures] tex5` に置く RGBA。Inspector では «tex5» の枠に出る (シェーダーの `tex5Index`)。どのチャンネルも .mat の値への乗数で、未設定は白 1×1 (従来と同じ見た目)。

| チャンネル | 掛ける先 | 引く場所 |
|---|---|---|
| R | `fiberDensity` | 画素 |
| G | `fiberLength` と曲げ量 (短い毛ほど曲がらない) | 頂点 (ミップ 0) |
| B | `clumping` | 画素 |
| A | 根元色→毛先色へ進む割合 (0 で毛全体が根元色。縞・斑に使う) | 画素 |

- メッシュの UV で引く。`worldMapping` でも UV で引く。サンプラーは s0 (異方性・ラップ)。
- 値はデータなので、テクスチャの取り込み設定を sRGB にしない。
- 密度と束は毛の根元でなく画素で引く (近傍 9 本ぶん引くと重い)。マスクの境をまたぐ毛はそこで欠ける。
- 長さは頂点単位でしか変えられない。velocity パスはマスクが時間で変わらない前提で前フレームにも同じ値を掛ける。
- bindless 添字は `ResolveFiberMaskTexture` が CB の直前に毎フレーム書く。本編は材質ごとに 1 フレーム 1 回、プレビューは描くたび。

## 個体ごとの上書き (FiberComponent)

- `colorTint` (根元色・毛先色の RGB に掛ける)・`lengthScale`・`densityScale`・`clumpingScale` は共有 .mat の解決結果へ掛ける。長さ・密度・毛束は .mat と同じ上限 (2 m / 1 / 1) で丸める。
- `maskPath` を入れるとこの個体だけ fiberMask を差し替える。空なら .mat の tex5。
- スクリプトは `GetComponent<FiberComponent>()` で直接書き換える (濡れ・毛色違い・撃破時に短くする等)。値は毎フレーム CB へ上げるので、書いた次の描画から効く。
- 長さの上書きはカリング球の余白にも入る。

## 負荷と影 (2026-09-20)

`FiberPerf.playtest.json` (FiberSkinning.scene、Player 1 体、RTX 4070) の GPU パス時間の合計。

| 構成 | 変更前 | 変更後 |
|---|---|---|
| 繊維なし | 0.4 ms | 0.4〜1.0 ms |
| Hybrid 24 層 | 32.8 ms | 7.0 ms |
| Hybrid 16 層 + 距離 LOD (既定) | — | 5.5 ms |
| Shell 24 層 | 32.3 ms | 6.4 ms |
| Shell 16 層 | 22.1 ms | 4.7 ms |
| Shell 8 層 | 12.0 ms | 3.1 ms |

- 変更前は 85% が影だった。影はカスケード・光源の面ごとに全層を描き、1 層ごとに近傍 3×3 の毛を探していた。
- 影の Shell は `shadowShellCount` (既定 6) で層数を絞り、毛 1 本ずつの形 (円・束・3×3 の探索) を解かずに毛のセル単位で残すか決める (`FiberClipShellShadow`)。被覆率は密度 × 届く割合 × 先細りで、根元はほぼ埋まり毛先ほど薄い。
- 影の乱数はセルごとで層に依らない。最初は影の RT の画素ごとに乱数を取り、被覆率も πr² (根元で 3 割) にしたため、受け手に黒い点々しか落ちなかった。
- 既定は 16 層・距離 LOD 有効 (Hybrid で 5.5 ms、24 層では 7.0 ms)。距離 LOD は `lodNear` (既定 8 m) より手前では全層を描くので、カメラに近いキャラクターでは効かない。キャラクターは `lodNear` を 3 m 程度へ下げる。
- Hybrid の影では Fin を描かない。Shell の被覆で足りる。
- スキンは Animator が焼いた骨の広がりの球 (`ComputeSkinnedWorldBounds`) でカメラ・カスケード・光源の錐台と距離 LOD を判定する。球が得られなければ従来どおりカリングしない。
- コンピュートスキニング済み頂点があれば Shell は静的メッシュとして描き、層 × パスごとのボーン合成を省く。velocity だけは前フレームの骨が要るので VS スキニングのまま。
- Shell はインスタンス 0 を最も外側にし、外から描く。手前の層に隠れた奥の層の画素を深度テストが先に弾く。
- 残る最大の項目は GBuffer (24 層で約 3.5 ms)。層数と距離 LOD (`distanceLod`) で調整する。
- GPU のクロック状態で同じ構成でも 2 倍程度ぶれる。比較は同じ実行の中の構成どうしで行う。

## 局所 FlowField

- 繊維は環境風に加えて、シーンに置いた FlowField (Uniform / Sink / Source / Vortex / Curl / Baked) の媒質速度 [m/s] を根元で受ける。曲げは `(環境風 + 突風 + 局所流速) × windResponse` で、環境風と同じ応答係数・曲げ上限を使う。
- 評価式は GPU 粒子と共有する `Rendering/FlowField.hlsli` の `SampleFlowFields`。CPU の `FlowFieldEval.cpp` と同じ式で、Baked は粒子と同じ速度場アトラス (t26) を読む。場の詰め方も `FlowFieldGpu.hpp` の `PackGpuFlowField` を共有する。
- 環境流は `FlowFieldFrame::fields` の末尾にも場の形で入っているが、繊維はそれを `AmbientWind` (突風の式を含む) で受けている。二重に足さないよう、先頭 `sceneFieldCount` 本だけを読む。
- シーン単位・1 フレーム 1 回で場を StructuredBuffer (VS の `vsBuffers[1]`) へ詰め、前フレームの一覧を後ろへ連結する。velocity パスは前フレームの範囲を前フレームの時刻で評価する。連続しないフレームでは前フレーム = 今フレームとし、場の変化を速度に出さない。
- `FiberComponent::flowChannels` (既定 -1 = 全チャンネル) と場の `channels` が 1 ビットでも重なる場だけを受ける。0 なら局所の場を受けない。環境風はチャンネルに関係なく受ける。
- 物体の bounds に届く場が 1 本も無ければ本数 0 で送り、VS の評価を丸ごと省く。1 本でも届けばシーンの全本数を頂点ごとに評価する (場ごとの距離判定は VS 側)。スキンは bounds を持たないので常に評価する。
- 照明の接線は VS で評価した局所流速を補間して使い、ピクセルごとに場を引き直さない。
- 流れは見た目の変形であり、繊維から場や剛体への反作用は無い。

## スキニング・地形・Blade・選択

- Shell / Fin / Hybrid は SkinnedMeshRenderer の可視サブメッシュと親 Animator を使う。根元と法線を現在のボーンで変形し、velocity は前フレームのボーンも評価する。Fin の各高さへ元頂点の 4 ウェイトを保持する。初回の過去パレットなしでは速度を 0 にする。Blade は静的メッシュと地形向け。
- Terrain と併用すると高さデータからパッチを生成する。`terrainPatchCells` は分割するセル数。密度はローカル面積あたりで、スケールを変えるとワールド面積あたりの密度も変わる。大地形の生成範囲・編集・寿命は「地形の近傍常駐」で定める。
- 地形の層で生やす範囲を決める (`terrain-layers.md` §5)。`terrainLayer = -1` (既定) は全面、0 以上ならその層だけ。4 隅の層の重みの最大値が `terrainLayerThreshold` (既定 0.25) 未満のセルと穴セルはパッチから省き、三角形 0 枚のパッチは作らない。パッチ頂点の色 A に層の重み (全面なら 1) を入れ、層指定時だけ `BuildFiberBlades(..., densityFromVertexAlpha = true)` が棄却法で根元を間引く。内容署名は高さ・穴・層設定に加え、層指定時は番号と重みも含める。
- 繊維の根元と地面の高さがずれないよう、Fiber の距離範囲に届く地形チャンクは本体と影の地形 LOD を 0 に固定する。離れた地形は通常の LOD を使い、Fiber の距離 LOD を無効にした場合だけ全チャンクを LOD 0 にする。
- Blade は三角形の面積に比例して根元を配置する。各葉は高さ方向 6 分割のリボン 2 枚で、元メッシュの辺から離れた場所にも生える。Shell の層間や Fin の辺の隙間を厚く塗って隠す方式ではない。近距離の草には `mode = 3` を指定する。
- `bladeDensity`・`bladeWidth`・`seed` で分布を制御する。CPU / GPU の過剰確保を避けるため 1 メッシュ / 地形パッチは最大 32768 葉。超過時は警告して生成を拒否するので、密度かパッチを小さくする。
- Blade は頂点バッファを持たない。CPU は 1 葉 64 バイトの根元 (`FiberBladeRoot`: 位置・相対高さ・法線・乱数・2 リボンの半幅ベクトル・UV) だけを作り、GPU ローカルの StructuredBuffer (VS の `vsBuffers[0]`) に置く。VS が `SV_VertexID` から 1 葉 72 頂点 (リボン 2 × 高さ 6 区間 × 6 頂点) を組み立て、インデックスなしで描く。三角形の並びと各頂点の高さ・左右は旧インデックスバッファと同じで、乱数の消費順も変えていないため形状は一致する。32768 葉で以前は約 108 MB (28 頂点 × 108 バイト + 72 インデックス) を CPU で生成・転送していたが、約 2 MB になる。距離 LOD は提出する頂点数を 72 の倍数で減らす。
- `distanceLod` を有効にすると `lodNear`～`lodFar` で Shell 層数と Blade の提出本数を減らす。最後の 10% は根元の決定的ディザで消し、far 以降は提出しない。影も同じ閲覧カメラの LOD を使う。層数が変わったフレームは速度履歴をリセットする。
- 接触の走査は頂点ごとに最大 32 枠。b10 の末尾 (`FiberContactCB::m_count` / `m_previousCount`) までで打ち切り、接触 0 件と `planarShading = 0` (毛皮) では走査しない。今フレームは発生済みで回復しきっていない最後の枠 + 1、前フレームは評価時刻が速度履歴でずれるため半径を持つ最後の枠 + 1 (`UpdateFiberContactCounts`)。
- `FiberInteractorComponent` は足元のワールド位置を中心とする球。`radius` は m、`strength` は 0～1、`recoverySeconds` は復元にかかる秒数。32 接触の循環履歴を保持し、静止中の接触は近い記録を更新する。現在と前フレームの履歴を分け、踏み倒し・復元も velocity に含める。草への見た目の変形であり、物理力ではない。
- 選択マスクも同じ頂点変形・Alpha Clip・距離 LOD を使い、両面を描く。元の表面だけを囲む輪郭を繊維まで拡張する。

`FiberStage4.scene` は短い Blade、背の高い Blade、丘の Blade と足元の Interactor を比較する。`FiberStage4.playtest.json` は押し倒し・回復・近接・遠距離・選択を撮影する。回復比較では乱流を 0 にして時刻による風の差を除く。`FiberSkinning.scene` / `FiberSkinning.playtest.json` は既存 Player モデルへ繊維を追加し、アニメーション姿勢と 3 モードを検査する。

### 透過テクスチャの草束 (2026-11-04)

Alpine の既定は `GrassCards.mat`。Blade の根元・高さ乱数・90 度交差する板を使い、各板の全面 UV で色と透過輪郭を読む。固定した板は視点が変わっても向きが変わらず、影とモーションの形状を共有できる。草束でも地形レイヤーによる根元の選択と近傍常駐を使う。

`GrassCard` シェーダー一式は Blade の 128 バイト材質契約を共有し、`albedo` に RGB と Alpha を持つ草束、`cardAlphaCutoff` に透過のしきい値を持つ。根元を留めた曲げには環境風と局所の流れに加えて `cardSway` [m] と `cardSwayHz` [Hz] を使う。モーションは現在と前フレームの時刻で同じ式を評価する。Terrain Tool の既定は幅 0.5 m、密度 4 束 / local m²、表示距離 80 m。

`cardSpeciesAtlas = 1` で `albedo` を 2 × 2 のアトラスとして使う。`cardSpeciesWeights` の 4 要素は左上、右上、左下、右下の順で、値は草束を選ぶときの相対比率。アトラスが無効か全要素が 0 なら画像全体を 1 枚のカードに使う。各セルの縁に透過の余白を取る。

`cardSpeciesHeight` と `cardSpeciesWidth` はそれぞれ毛丈と束幅に対する倍率 [0.05, 1]。`cardSpeciesSway` と `cardSpeciesSwayHz` は揺れの振幅と周波数に対する倍率。種の選択はローカルの根元位置から求め、一部の束は近くで同じ種を選ぶ。根元バッファと描画パスは既存の草カードと共通。

### 地形の近傍常駐 (2026-11-04)

- Terrain の Blade に `terrainSparseLodStart` [world m] を設定すると、その距離から生成時の根元密度を 1/8 にし、カードの高さ分割を 1 にする。切替までに提出本数を 1/8 へ補間し、同じ seed の根元の先頭を引き継ぐ。疎な範囲でも `lodFar` まで間引きとフェードを行う。距離 LOD が有効で設定値が `lodNear` と `lodFar` の間にあるときだけ適用する。
- 地形 Blade の `terrainGroundCover` [0, 1] は、草が遠景で減るにつれて `terrainGroundColor` (線形 RGB) を地表のアルベドへ混ぜる。既定は 0。距離 LOD の `lodNear` から疎な LOD の開始距離 (無効なら `lodFar`) まで滑らかに強め、草束の描画範囲を越えても地表色を残す。草の層としきい値を使い、自動・高さブレンドを掛ける前の塗りを参照する。地表の材質の明暗を使うので、追加のテクスチャ参照やジオメトリ提出は発生しない。洞窟の追加サーフェスには適用しない。

地形全域の CPU メッシュと Blade 根元を先に作ってから描画時に距離で捨てると、見える草の量が同じでも地形面積に比例して初期生成と常駐メモリーが増える。生成対象は既存の距離 LOD と同じ範囲まで絞り、CPU の根元生成と GPU 確保より前に判定する。

- `distanceLod` が有効で距離が有限なら、`max(lodFar, max(lodNear, 0) + 0.1)` [world m] の球を対象とする。カメラ位置を Terrain の逆 TRS で local へ移し、軸ごとの半径を world scale の絶対値で割って候補の格子範囲を求める。候補パッチの AABB 中心を world へ戻し、既存描画と同じ中心距離で最終判定する。錐台では絞らないので、画面外の草が落とす影も残る。距離 LOD を無効にした場合は既存の全地形生成を維持する。
- キャッシュの単位は Scene 世代・Terrain 個体・パッチ格子座標。三角形の順序、頂点、`seed`、葉の生成規則を維持し、離れた場所から戻っても同じ地形から同じ草を再生成する。未変更のパッチは GPU ハンドルを維持し、既存の速度履歴を継続する。
- 各ビューが自分の候補範囲を要求する。同一フレームの最初のカメラだけで生成を打ち切らない。直近 2 フレームの要求と、編集で置き換えた旧世代を保持し、それより古いパッチの頂点・インデックス・Fin・Blade 根元をまとめて返却する。先に抽出した別ビューの非所有ハンドルを同一フレーム中に失効させない。
- 高さ・穴・植生の変更世代とパッチ設定を確認する。変更時の内容署名は要求されたパッチの高さ、法線計算に必要な隣接 1 頂点、穴、指定層の重みだけを読む。地形全域のハッシュは取らない。離れた編集で未変更の近傍パッチを作り直さない。地形の移動・回転・非一様スケールでは生成範囲を更新し、local 根元を共有する。
- 1 パッチ最大 32768 葉の既存上限を維持する。総量は地形全体ではなく、有効な距離・セル寸法・パッチ寸法・直近のビュー要求で決まる。上限を超えたパッチは従来どおり警告付きで拒否し、見えているパッチを任意の総数で黙って切り捨てない。作者向けの即時設定は距離 LOD を有効にする。
- CPU の抽出・根元生成・確保数は実際の `ExtractRenderFibers` と ResourceManager を通して測る。追跡用 CPU Renderer の時間は DX12 の GPU 時間として扱わない。冷状態、同じカメラの再抽出、移動、同一フレームの別カメラ、局所編集を区別し、変更前後の地形寸法・密度・距離を揃える。

### 地形の近傍常駐の計測と検証 (2026-11-04)

Release の `TerrainFiberStreamingTest` を変更前後の別プロセスで実行した。513 × 513 頂点、cellSize 4 m、2048 m 四方の平坦な地形、8 セル四方のパッチ、層 0 を全面 100%、Blade 密度 4 本 / local m²、距離 LOD 20～80 m、カメラ高 3 m の同じ条件。下表の時間は `ExtractRenderFibers` の CPU 経過時間で、先行する `ExtractRenderTerrains` の時間は別に記録する。

| 項目 | 変更前 | 変更後 |
|---|---:|---:|
| 初回の草抽出・生成 | 1231.622 ms | 5.043 ms |
| 同じカメラ 8 フレームの中央値 | 8.819 ms | 0.098 ms |
| 64 m ずつ移動する 8 フレームの中央値 | 8.481 ms | 2.440 ms |
| 近傍の高さ 1 頂点を編集 | 1242.150 ms | 1.688 ms |
| 同じ頂点の層を編集 | 1367.818 ms | 1.335 ms |
| 同じ場所の穴 1 セルを編集 | 1334.854 ms | 0.614 ms |
| 初回の抽出パッチ数 | 4096 | 16 |
| 初回の抽出根元数 | 16,777,216 | 65,536 |
| 初回の根元 StructuredBuffer 要求サイズ | 1024 MiB | 4 MiB |

実際の地形・草の抽出、根元計算、ResourceManager の確保経路を使うが、Renderer は資源追跡用の CPU 実装である。ドライバー転送・DX12 の GPU 実行・Present・画面全体のフレーム時間は測っていない。容量は ResourceManager へ要求したバッファのバイト数で、実機 VRAM の実測ではない。初回と各編集は各 1 回、定常と移動は各 8 回であり、時刻の値をテストの合否条件にはしない。

`root_bytes` はそのビューへ抽出した根元だけを数える。別ビューや移動前のパッチと編集前の世代は 2 resource frame 保持するため、常駐量はこの値より大きくなる。`live_bytes` は地面本体を含む全追跡資源の要求サイズを数える。距離 LOD を明示的に無効にすると全域生成へ戻るため、この比較値は適用しない。

元データは `build/agent/terrain-fiber-before.json` と `build/agent/terrain-fiber-after.json`。実行時の XML と標準出力も同じディレクトリの `terrain-fiber-before.xml` / `terrain-fiber-after.xml`、`terrain-fiber-before-console.log` / `terrain-fiber-after-console.log` に残す。計測用の 2 ケース、常駐・編集・寿命を検証する 8 ケース、Alpine 素材を実際の読込経路で確認する 1 ケースの計 11 ケースが成功した。回帰には地形寸法を大きくしても近傍確保数が一定であること、定常時のハンドル再利用、同一フレームの別カメラ、ResourceManager の 3 フレーム後の返却、再生成した根元の内容一致、局所編集、符号付き非一様 TRS、複数所有者と Reset を含む。

再計測はリポジトリルートからビルドを直列に終えた後、明示実行のテストを有効にして行う。出力先は実際のチェックアウトに合わせた絶対パスへ変える。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 build FBZZTestsEngineAuto -Preset release
$env:FBZZ_TERRAIN_FIBER_BENCH_OUTPUT = 'K:/FBZZ_Engine/build/agent/terrain-fiber-after.json'
& 'K:/FBZZ_Engine/build/Release/Binaries/Release/Tests/FBZZTestsEngineAuto.exe' --gtest_also_run_disabled_tests '--gtest_filter=TerrainFiberStreamingTest.*' '--gtest_output=xml:K:/FBZZ_Engine/build/agent/terrain-fiber-after.xml'
```

### 段階 4 の検証 (2026-09-17)

関連 44 テスト、25 シェーダーの VS / PS 計 50 コンパイルが成功。`FiberStage4` の 29 手順と静止姿勢専用の `FiberSkinningBindPose` の 18 手順が成功した。近接の連続した葉、踏み倒し、葉先までの選択輪郭を画像で確認した。選択の撮影は `render.set_view_mode` で Lit を指定する。シーンは `GreenWare/Assets/Scenes/Test/` に配置する。

アニメーション中の検証は `FiberSkinning` の Play 開始が `SCRIPT_RELOAD_BUSY` で拒否され、未完了。静止姿勢の合格をアニメーション追従の実証とは扱わない。既存 `FiberLifecycle` も最初の画像で meanDiff=0.030960 / badPixelRatio=0.109215 となり不合格。以前の基準と現在の風変形・形状に差があり、基準は更新していない。結果はそれぞれ `build/agent/FiberStage4/report.json`、`FiberSkinningBindPose/report.json`、`FiberSkinning/report.json`、`FiberLifecycleStage4/report.json` に残す。

## エディターのプレビュー

- AssetBrowser のサムネイルと Preview / Inspector パネルは `MaterialPreviewCore` の `Flavor::Fiber` で焼く。シェーダーパスが `Fiber/Fiber*` の .mat が対象。以前は Surface として球へ流れ、b5 が 0 のまま (LOD の残存率 0) 全画素 clip されて何も出なかった。
- 本編と同じシェーダーと b2 (`ResolveFiberMaterial` で丸めた値) / b5 (`FiberFrameCB`) / b10 (`FiberContactCB`) のレイアウトを使う。両 CB は `FiberRenderPass.hpp` が正本。接触と局所 FlowField は 0 件、環境風はパネルの Wind スライダー (+X 方向 [m/s]、風速の半分の突風付き) だけ。
- .mat は描画方式を持たないので、パネルで Shell / Fin / Hybrid / Blade と層数を切り替える。サムネイルはシェーダー名から推した方式 (現行の .mat はすべて Shell)。Blade の密度・幅は FiberComponent の既定 (400 / 0.015)。上限を超える形状では密度を半分ずつ下げる。
- 土台は根元色を暗くした単色 Lit。本編の MeshRenderer 側の材質はプレビューに含めない。枠は形状の境界に毛丈と曲げ上限を足して決める。
- サムネイルの形状は、草 (`planarShading >= 0.5`) なら平面、毛皮なら球。表示チャンネルは Shaded のみ。

## 使用と比較

`GreenWare/Assets/Scenes/Test/FiberTest.scene` は、奥が毛皮、手前が芝、左から Shell / Fin / Hybrid。Mesh Renderer の Material 欄は `FurBase.mat` / `GrassBase.mat`、Fiber の materialPath 欄は `Fur.mat` / `Grass.mat` を使う。長さ・密度・風は Fiber 用 `.mat` のパラメーターから編集する。

`GreenWare/Tests/Playtests/FiberSmoke.playtest.json` は Play / Stop 往復で 6 個の Fiber が残ること、シェーダー診断にエラーが無いこと、固定フレームの比較画像を表明する。初回の基準画像は `--update-baselines` で出力し、目視確認してから採用する。

`GreenWare/Tests/Playtests/FiberCapture.playtest.json` は編集状態で 6 個の Fiber とシェーダー診断を検査し、Game View を撮影する。2026-09-17 に DX12 の非表示バッチで成功し、3 方式の毛皮・芝の描画を目視確認した。関連 35 テスト、エディタービルド、両配置先の Shell / Fin の VS / PS コンパイルも成功。Play / Stop のバッチは既存の Script 再読み込みが 3600 フレーム以内に終わらず未完了であり、画像回帰用の基準画像はまだ採用していない。

### 次段階へ進む前の回帰検証 (2026-09-17)

`FiberLifecycle.playtest.json` を追加し、編集状態の 93 手順が成功した。Shell / Fin / Hybrid への切り替え、無効化と再有効化、材質参照なし、1 / 64 層、範囲外の mode の補正、カメラの cullingMask、元設定への復帰を検査する。材質はロード後に GUID からパスへ解決されるため、検査は解決後のアセットパスで照合する。シナリオはディスクへシーンを保存しない。

10 枚を目視確認して `GreenWare/Tests/Golden/FiberLifecycle/` に採用し、基準を更新しない別プロセスで画像比較を実行した。10 枚すべて `meanDiff = 0` / `badPixelRatio = 0`、シェーダー診断もエラー 0 件。許容値は平均差 0.001、不一致画素率 0.005。基準は 1548 × 871 の Game View であり、異なる解像度やレンダー設定では別途確認する。これは編集状態での画像回帰で、動く風の時間履歴を保証するものではない。

関連する `FiberTest|SceneSerializerTest|RenderPipeline` の 35 テストも再実行して全件成功した。ログは `build/agent/test-20260917-153930-37488.log`、画像比較の結果は `build/agent/FiberLifecycle/report.json`。

Play / Stop は待機を 36000 フレームへ延ばしても `scriptReloadBusy` が true のまま終了した。診断ログの直近の `ScriptDll` メッセージは `load failed; scheduling dependency rebuild`。この待機の根本原因は未確定であり、Play の画像基準 `Fiber_30f` は未採用のまま。編集状態の合格で置き換えない。

### 段階 3 の検証 (2026-09-17)

`FiberStage3.scene` に受影用の床と Spot / Point、TAA / Motion Blur / GTAO の検証用 Volume を追加した。`FiberStage3.playtest.json` の 63 手順が成功し、繊維の有無・方向光 / 局所光・物体移動の 11 枚を撮影した。方向光・局所光の描画と移動時の最終画像を目視確認し、シェーダー診断はエラー 0 件だった。結果は `build/agent/FiberStage3/report.json`。

関連 `FiberTest|SceneSerializerTest|RenderPipeline` の 39 テスト、エディター / テストのビルド、8 シェーダーの VS / PS コンパイルが成功した。新しい履歴テストは静止 world での風変化、同一フレームの再描画、履歴リセット、独立したビューを検査する。GBuffer / velocity の GPU 値の数値照合と、カメラ移動・風だけの連続変形の画質評価は未実施。Lifecycle の 10 枚は影投射導入後に目視確認して更新し、基準更新なしの別プロセスで 93 手順を再実行した。全画像が meanDiff = 0 / badPixelRatio = 0 で合格した。結果は `build/agent/FiberLifecycleStage3/report.json`。

再実行は既存のエディターを閉じる必要のない、別プロセスのバッチを使える。以下はリポジトリのルートで実行する。`--update-baselines` は付けない。

```powershell
$fiberRoot = (Get-Location).Path
$fiberArguments = '--project "{0}/GreenWare" --batch "{0}/GreenWare/Tests/Playtests/FiberLifecycle.playtest.json" --hidden' -f $fiberRoot
$fiberRun = Start-Process -FilePath "$fiberRoot/build/Development/Binaries/Development/Editor/FBZZEditor.exe" -ArgumentList $fiberArguments -WindowStyle Hidden -Wait -PassThru
$fiberRun.ExitCode
```

Fin の CPU キャッシュは静的 VB / IB の世代を鍵にする。同じバッファ内の頂点を書き換える手続きメッシュは現段階の対象外。Fin 単独で平面中央が薄いことは方式の制限で、草の葉を全面配置する Blade の代替にはならない。

### 風だけによる変形の検証 (2026-09-17)

`FiberStage3.scene` の環境風を方向 `[1, 0, 0.3]`、風速 `2.0 m/s`、乱流 `1.4`、周期周波数 `0.8 Hz` に設定した。この検証は環境風だけを対象にするため、局所 FlowField の GameObject ではなくシーンの `[environment]` に置く。`FiberTest.scene` は既存の画像回帰用設定を維持する。

`FiberWind.playtest.json` はカメラと物体を固定し、1/60 秒のロックステップで時間を進める。通常描画 6 枚、TAA / Motion Blur / GTAO 有効時 6 枚、繊維を無効にした対照 2 枚を撮影する。46 手順が成功し、シェーダー診断はエラー 0 件。通常描画の隣接画像で、RGB のいずれかが 2/255 を超えて変化した画素の割合は 10.24–13.48%、対照画像は差分 0 だった。時刻は `Time::Tick()` で編集中も進むため、これは Play / Stop を含まない描画検証である。

結果は `build/agent/FiberWind/report.json`、画像差分の集計は `build/agent/FiberWind/motion-analysis.json`、連続表示は `build/agent/FiberWind/Raw.gif` と `Temporal.gif`。画像で風変形と時間効果有効時の表示を確認したが、velocity の GPU 値の数値的な正しさや残像の品質閾値までは表明しない。

## CPU 側の最適化 (2026-09-17)

描画結果は変えず、1 フレームに何度も呼ばれる経路の重複を削った。Fiber の描画関数は Color / GBuffer / Velocity / 選択に加え、方向光の各カスケードと Spot・Point の各面ごとに呼ばれる。

- 対象 GameObject の収集を 1 回の走査にまとめた。以前は `IsEnabled` と描画ループで 2 回走査していた。繊維の無いシーンでは GPU 資源を作らない。
- シェーダーハンドルを (パス, Shell/Fin/Blade, スキン) ごとに保持する。以前は面ごとに 3 本のパス文字列を組み立てて検索し、描画方式に関係なく Blade も読み込んでいた。失敗したパスは 1 フレーム 1 回だけ再試行する。
- 材質の解決 (`AssetManager::Load` と 17 項目のパラメーター検索) をパスごと 1 フレーム 1 回にした。影のカスケードやビューでは再利用する。材質の編集は次のフレームに反映される。
- 材質・フレーム・速度・接触の CB は直前の転送内容と一致すれば `Update` を省く。DX12 の CB は Update のたびに次の提出でアリーナへ複製し直すため、影のカスケードごとに 2 KB の接触 CB を送り直していた。
- 世代切れキャッシュの掃除を 1 フレーム 1 回にした。風の検証・world 行列と逆転置行列は面ごとでなくパス / 物体ごとに求める。
- 地形の内容署名は float 2 個ずつの 64bit 語で混ぜ、1 標本あたりの乗算を半分にした。編集検出の契約 (内容で判定、dirty フラグは共有しない) は変えない。
- Fin の構築で位置の溶接と辺の集計を `std::map` から整列済み配列へ置き換えた。溶接番号・辺の順序・代表の面は従来と同じで、出力する頂点列は一致する。

検証: `Fiber|Cloth|RenderPipeline|SceneSerializer` 98 件が合格。Playtest の画像回帰 (`FiberLifecycle` など) と実機でのフレーム時間の計測は未実施。

## Lifecycle 基準画像の更新 (2026-09-21)

基準画像は初期実装の `d0d08252` で採用したままで、`d5e257f8` の材質・シェーダー変更を反映していなかった。Fur.mat は fiberFrequency / fiberLength / fiberTaper / gravityBend を変更し、clumping / clumpTwist / colorVariation / secondarySpecular / specularShift を追加している。Graphics 分割の前後で実画像は完全一致していたため、描画コードを変更せず現在の材質・描画に基準を合わせた。

10 場面を目視確認し、無効化・材質参照なし・Shell / Fin / Hybrid・1 / 64 層・カリング・設定復帰を確認した。正式シナリオの基準解像度は従来と同じ 1548 × 871、許容値も平均差 0.001 / 不一致画素率 0.005 のまま。シーン・材質・シェーダーは変更していない。

基準更新なしの別プロセスで 93 手順が合格し、全 10 枚が meanDiff = 0 / badPixelRatio = 0、シェーダー診断もエラー 0 件。結果は `Scratch/FiberLifecycleFix/verify/report.json`。任意のエディターレイアウトや別解像度での一致は保証しない。

## 空の IBL との接続 (2026-09-21)

Fiber の色パスは `iblIntensity > 0` の場合、定数の ambientColor を共通の `EvaluateIBL` に置き換える。描画側が既に供給する irradiance / prefilter / BRDF LUT と AdvancedGraphicsConstants を使い、DynamicSky と静的 IBL の同じ設定を受ける。無効時は従来の定数環境光へ戻る。両者は加算しない。

繊維法線と非金属の split-sum による近似で、roughness と specularStrength を環境反射にも反映する。直接光の二葉ローブを環境全体で積分する実装ではない。根元遮蔽と画面 AO は共通関数へ渡し、拡散へ一度だけ掛ける。worldPos を渡すため Light Probe の拡散置換も共通契約に従う。

`FiberSkyLighting.playtest.json` は Sky System と同じ 4 コンポーネントを検証中に追加し、DynamicSky の強度 0 / 4 を撮影する。シーンは保存しない。修正後に毛・草が空の強度へ追従することを目視確認し、強度 0 と空なしの画像は修正前と SHA256 が一致した。両配置先の Fiber シェーダー各 50 ステージがコンパイル成功。既存 FiberLifecycle は基準更新なしで 93 手順合格・10 枚の画素差 0 (`Scratch/FiberIBLRegression/report.json`)。

## 参考資料

- [GPU Gems 3, Motion Blur as a Post-Processing Effect](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-27-motion-blur-post-processing-effect): 現在 / 過去の投影位置による速度。繊維では過去の風変形も独自に再評価する。

- [Lengyel et al., Real-Time Fur over Arbitrary Surfaces](https://hhoppe.com/fur.pdf): Shell と輪郭を補う Fin、同じ毛のモデルからの断面生成。実装は lapped textures や毛の事前シミュレーションまでは採用せず手続き分布で開始する。
- [GPU Gems, Rendering Countless Blades of Waving Grass](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-7-rendering-countless-blades-waving-grass): 板による草と根元を固定した風の変形。背の高い草の Blade 段階の参考。
- [流れの場](flow-field.md): 媒質の流速と消費者側の応答係数の分離。

参照 URL は該当する C++ / HLSL の `@see` にも置く。論文どおりの実装と独自の軽量近似をコメントで区別する。

## 内部名と保存キーの互換性 (2026-10-11)

`FiberComponent` と登録・保存型キー `FiberComponent` は既に汎用名なので維持する。毛・芝などは用途例で、別の Component 型を増やさない。

| 旧名 | 正本 | 意味 |
|---|---|---|
| `FiberMaterialSettings::m_grass` | `m_planarShading` | 0 は繊維方向の照明、1 は平面の Lambert / 透過照明、その間は補間 |
| HLSL / `.mat` の `grassShading` | `planarShading` | 照明モデルの選択を用途名から分離 |
| HLSL の `fur` / `furDiffuse` | `strandWeight` / `strandDiffuse` | 繊維側の照明項 |

読込と保存で旧 `.mat` キーを正規化し、両方ある場合は新キーを優先する。C# の Material API の署名は維持し、旧 property 文字列による照会・取得・設定・解除も同じ正本値に接続する。旧・新シェーダーへの描画束縛と個体 override にも互換を設ける。定数バッファの順序・サイズは変えない。`Fur.mat` / `Grass.mat` / `TallGrass.mat` は実際の外観を表すプリセット名として保持し、GUID を変更しない。
