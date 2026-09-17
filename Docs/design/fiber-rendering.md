<!--
/// @file    fiber-rendering.md
/// @brief   毛皮と短い芝が共有する Shell / Fin 描画の契約と段階導入。
/// @author  Hasegawa Jin
/// @date    2026-09-17
-->
# 表面繊維 — Fur と Grass の共通描画

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

velocity は土台の world 行列が静止していても描く。ビューごとに前フレームの world・風・時刻・曲げパラメーターを保存し、現在と過去の変形後位置からジッターを除いた UV 差を求める。初回・非連続フレーム・時刻巻き戻し・描画方式 / 層数変更では履歴をリセットする。完成した HDR 深度との比較で、静止した遮蔽物の背後から速度が漏れるのを防ぐ。履歴はランタイム専用でシリアライズしない。

影は Alpha Clip であり、毛の透過を積分した影ではない。

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
- Terrain と併用すると高さデータからパッチを生成する。`terrainPatchCells` は分割するセル数。地形の内容署名を毎フレーム一度確認し、編集時にパッチを再構築する。密度はローカル面積あたりで、スケールを変えるとワールド面積あたりの密度も変わる。
- 繊維のある地形は、根元と地面の高さがずれないよう本体と影の地形 LOD を 0 に固定する。繊維自体は距離 LOD で削減するが、地面の三角形数は減らない。
- Blade は三角形の面積に比例して根元を配置する。各葉は高さ方向 6 分割のリボン 2 枚で、元メッシュの辺から離れた場所にも生える。Shell の層間や Fin の辺の隙間を厚く塗って隠す方式ではない。近距離の草には `mode = 3` を指定する。
- `bladeDensity`・`bladeWidth`・`seed` で分布を制御する。CPU / GPU の過剰確保を避けるため 1 メッシュ / 地形パッチは最大 32768 葉。超過時は警告して生成を拒否するので、密度かパッチを小さくする。
- Blade は頂点バッファを持たない。CPU は 1 葉 64 バイトの根元 (`FiberBladeRoot`: 位置・相対高さ・法線・乱数・2 リボンの半幅ベクトル・UV) だけを作り、GPU ローカルの StructuredBuffer (VS の `vsBuffers[0]`) に置く。VS が `SV_VertexID` から 1 葉 72 頂点 (リボン 2 × 高さ 6 区間 × 6 頂点) を組み立て、インデックスなしで描く。三角形の並びと各頂点の高さ・左右は旧インデックスバッファと同じで、乱数の消費順も変えていないため形状は一致する。32768 葉で以前は約 108 MB (28 頂点 × 108 バイト + 72 インデックス) を CPU で生成・転送していたが、約 2 MB になる。距離 LOD は提出する頂点数を 72 の倍数で減らす。
- `distanceLod` を有効にすると `lodNear`～`lodFar` で Shell 層数と Blade の提出本数を減らす。最後の 10% は根元の決定的ディザで消し、far 以降は提出しない。影も同じ閲覧カメラの LOD を使う。層数が変わったフレームは速度履歴をリセットする。
- 接触の走査は頂点ごとに最大 32 枠。b10 の末尾 (`FiberContactCB::m_count` / `m_previousCount`) までで打ち切り、接触 0 件と `grassShading = 0` (毛皮) では走査しない。今フレームは発生済みで回復しきっていない最後の枠 + 1、前フレームは評価時刻が速度履歴でずれるため半径を持つ最後の枠 + 1 (`UpdateFiberContactCounts`)。
- `FiberInteractorComponent` は足元のワールド位置を中心とする球。`radius` は m、`strength` は 0～1、`recoverySeconds` は復元にかかる秒数。32 接触の循環履歴を保持し、静止中の接触は近い記録を更新する。現在と前フレームの履歴を分け、踏み倒し・復元も velocity に含める。草への見た目の変形であり、物理力ではない。
- 選択マスクも同じ頂点変形・Alpha Clip・距離 LOD を使い、両面を描く。元の表面だけを囲む輪郭を繊維まで拡張する。

`FiberStage4.scene` は短い Blade、背の高い Blade、丘の Blade と足元の Interactor を比較する。`FiberStage4.playtest.json` は押し倒し・回復・近接・遠距離・選択を撮影する。回復比較では乱流を 0 にして時刻による風の差を除く。`FiberSkinning.scene` / `FiberSkinning.playtest.json` は既存 Player モデルへ繊維を追加し、アニメーション姿勢と 3 モードを検査する。

### 段階 4 の検証 (2026-09-17)

関連 44 テスト、25 シェーダーの VS / PS 計 50 コンパイルが成功。`FiberStage4` の 29 手順と静止姿勢専用の `FiberSkinningBindPose` の 18 手順が成功した。近接の連続した葉、踏み倒し、葉先までの選択輪郭を画像で確認した。選択の撮影は `render.set_view_mode` で Lit を指定する。シーンは `GreenWare/Assets/Scenes/Test/` に配置する。

アニメーション中の検証は `FiberSkinning` の Play 開始が `SCRIPT_RELOAD_BUSY` で拒否され、未完了。静止姿勢の合格をアニメーション追従の実証とは扱わない。既存 `FiberLifecycle` も最初の画像で meanDiff=0.030960 / badPixelRatio=0.109215 となり不合格。以前の基準と現在の風変形・形状に差があり、基準は更新していない。結果はそれぞれ `build/agent/FiberStage4/report.json`、`FiberSkinningBindPose/report.json`、`FiberSkinning/report.json`、`FiberLifecycleStage4/report.json` に残す。

## エディターのプレビュー

- AssetBrowser のサムネイルと Preview / Inspector パネルは `MaterialPreviewCore` の `Flavor::Fiber` で焼く。シェーダーパスが `Fiber/Fiber*` の .mat が対象。以前は Surface として球へ流れ、b5 が 0 のまま (LOD の残存率 0) 全画素 clip されて何も出なかった。
- 本編と同じシェーダーと b2 (`ResolveFiberMaterial` で丸めた値) / b5 (`FiberFrameCB`) / b10 (`FiberContactCB`) のレイアウトを使う。両 CB は `FiberRenderPass.hpp` が正本。接触と局所 FlowField は 0 件、環境風はパネルの Wind スライダー (+X 方向 [m/s]、風速の半分の突風付き) だけ。
- .mat は描画方式を持たないので、パネルで Shell / Fin / Hybrid / Blade と層数を切り替える。サムネイルはシェーダー名から推した方式 (現行の .mat はすべて Shell)。Blade の密度・幅は FiberComponent の既定 (400 / 0.015)。上限を超える形状では密度を半分ずつ下げる。
- 土台は根元色を暗くした単色 Lit。本編の MeshRenderer 側の材質はプレビューに含めない。枠は形状の境界に毛丈と曲げ上限を足して決める。
- サムネイルの形状は、草 (`grassShading >= 0.5`) なら平面、毛皮なら球。表示チャンネルは Shaded のみ。

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

## 参考資料

- [GPU Gems 3, Motion Blur as a Post-Processing Effect](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-27-motion-blur-post-processing-effect): 現在 / 過去の投影位置による速度。繊維では過去の風変形も独自に再評価する。

- [Lengyel et al., Real-Time Fur over Arbitrary Surfaces](https://hhoppe.com/fur.pdf): Shell と輪郭を補う Fin、同じ毛のモデルからの断面生成。実装は lapped textures や毛の事前シミュレーションまでは採用せず手続き分布で開始する。
- [GPU Gems, Rendering Countless Blades of Waving Grass](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-7-rendering-countless-blades-waving-grass): 板による草と根元を固定した風の変形。背の高い草の Blade 段階の参考。
- [流れの場](flow-field.md): 媒質の流速と消費者側の応答係数の分離。

参照 URL は該当する C++ / HLSL の `@see` にも置く。論文どおりの実装と独自の軽量近似をコメントで区別する。
