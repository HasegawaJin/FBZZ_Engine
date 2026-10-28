# ParticleEmitterのオーサリング

既存のParticleEmitter設定・Scene/TOML codec・CPU/GPU描画を正本として使う。専用Inspectorは操作の目的で5分類に分け、既存のserializedキー・enum値・素材GUIDを変えない。

| 分類 | 編集するもの |
|---|---|
| Emission + Shape | 周期・遅延・寿命・seed・CPU/GPU・座標系、時間/距離発生、発生量カーブ、Burst、発生形状と初速、Starter |
| Motion | 加速度、流れへの結合、移動速度の継承、寿命速度・回転・抵抗曲線、内蔵/外部の流れ、衝突、SubEmitter |
| Color + Size | 開始/終了サイズと補間カーブ、軸倍率、開始/終了色またはグラデーション、速度による倍率、黒体温度 |
| Rendering | マテリアル、ビルボード・ストレッチ・ソート、軌跡、粒子光源 |
| Budget | 最大粒子数、時間発生の生存数目安、粒子保存領域の目安、境界、距離LOD、透明overdrawの案内 |

## 制作時の操作

`Apply starter + replay`はエフェクト設定とmaterial参照を置き換える明示操作。enabled・randomSeed・runtimeのVFX scopeを保持する。以前の速度モジュール・衝突・光源・SubEmitterが残って新しいStarterへ混ざることを防ぐ。素材ファイルや旧sceneは書き換えない。

`Replay`は既存のResetPlaybackを使い、粒子・遅延・Burst周期・距離発生・乱数列を巻き戻す。`Pause emission`と`Stop`は新しい発生を止め、既存粒子は寿命を終えるまで動く。全シミュレーションのプレビュー停止には`Preview speed = 0`を使う。プレビュー倍率はruntime専用で保存せず、Inspectorを閉じると既存の2フレームの期限で通常速度に戻る。

単位は秒、m、m/s、m/s²、rad/sを表示する。Lifetime Randomは基準寿命の±割合。Size Curveは開始→終了サイズの**補間率**で、サイズへの乗算ではない。速度によるSize Curveは寿命サイズへの乗算。発生量カーブのXは周期時間/Duration、色・サイズ・速度・抵抗の寿命カーブのXは粒子age/lifetime。

色グラデーションを使うと開始/終了色は粒子には使われないため隠す。連続リボンも有効な場合だけ、リボンの長さ方向の色として両方を表示する。黒体放射ではグラデーションのalphaを使い、RGBは温度から作る。

## Starter

`Engine/Asset/ParticleEmitterPresets.hpp`に8種をまとめ、Inspectorとテストが同じfactoryを使う。

| Starter | 表現 | 最大粒子数 | モード |
|---|---|---:|---|
| Fire | Coneの炎、暖色と寿命fade | 192 | CPU |
| Smoke | 小さな発生球、prewarm、ソート、薄い煙 | 128 | CPU (prewarm) |
| Sparks | 重力と細いストレッチ | 128 | CPU |
| Explosion | 60粒の単発Burst | 96 | CPU |
| Bubbles | 小さなリング、浮上、穏やかな膨張、両端fade | 96 | CPU |
| Sand | 18粒の単発Burst、弱い初速、抵抗と重力 | 48 | CPU |
| Light Trace | cyan/violet、連続リボン、距離発生 | 64 | CPU (trail) |
| Water Flow | lateral flow、薄いstretched billboard | 80 | GPU条件を満たす |

素材は既存の標準ライブラリを使い、Bubblesだけ`BubbleRing_Alpha.mat`を追加する。既存circle_01.pngを参照するため、新しいshaderやbitmapは必要ない。泡マテリアルのGUIDは`4a32cfb5ec7a4a59a89b22f471f6d481`。Engineとempty/standardテンプレートの3コピーで一致させる。配布SDKの更新は共通ビルドの担当と調整して行う。

Starterは汎用の制作開始点。海V2のpool・水面/海底・camera距離・AABB・イベント予算の契約を代行しない。OceanEcosystem/Effects/V2の素材・共有C#・シーンには変更しない。

## 性能と互換性

時間発生の生存数目安は`emitRate * lifetime * (1 + lifetimeRandom)`で、発生量カーブ、LOD、Burst、距離発生、SubEmitterを含まない。保存領域目安は`maxParticles * sizeof(ParticleまたはGpuParticle)`で、描画・sort・spawnバッファを含まない。GPUの生存数をreadbackせずにCPU生存数と同じ指標で表示しない。

距離LODは既存契約どおり**時間発生だけ**を減らす。Burstと距離イベントの数は維持する。距離・screen coverage・粒子数だけでは透明overdrawは決まらないため、粒子カードの画面面積・寿命・重なりもParticle Overdraw表示で確認する。Pause When Culledは発生と生存粒子を凍結し、再び見える時に続きから動く。既存GPU描画経路はカリング中に粒子更新をdispatchしないため、Pause When Culledを無効にしてもCPUと同じ画面外の移動にはならない。画面外で粒子を動かし続ける必要がある効果はCPUを選ぶ。今回GPU更新スケジュールは変えない。

公開Burst(int)・通常再生/スクラブの時刻指定Burst・Prewarmはpending requestをmaxParticlesへ飽和させ、過剰呼び出しのsigned int overflowを防ぐ。容量を超える分は同じフレームで破棄する既存描画経路に揃える。C#の署名・既存設定schema・GPU粒子レイアウトは変更しない。

runtimeGradientのcache判定へグラデーション補間方式・色空間・温度カーブ補間方式を加える。キー値を変えない編集でもCPU/GPUが参照するruntimeGradientへ反映する。グラデーションのキーRGBは常にsRGBで、色空間タグは保存空間ではなく補間空間を選ぶ。黒体の既存sRGB格納処理は維持し、一定温度で補間色空間を変えても輝度を保つ契約をテストする。ResetPlaybackはClearParticlesを共用し、古い描画数・カリング状態・GPU spawn数を残さない。

自動境界はParticle専用extractor内で更新する。回転後の軸倍率・速度ストレッチのカード上限、trailの全使用履歴点、リボン幅と接合部の最大2倍の拡張を包含する。Localの位置と履歴点は既存World変換を使い、負のTransform scaleを含める。カード自体のサイズへTransform scaleを二重適用しない。空poolには発生形状・寿命ゆらぎ・初速/重力/Local移動の保守的な包絡を使い、CPUに読めないGPU poolは既存flow boundsの履歴も保持する。

境界距離はdoubleで計算してfloat距離二乗のoverflowを避ける。NaN/Infinityやfloat範囲を超える境界は、有限の中心と最大半径の球へ退避する。中心が既知ならLOD距離のため保持し、距離計算がfloat範囲を超える場合だけ原点へ退避する。無効な設定を黙って小さい境界へ落として効果全体を消さないための保守動作であり、無効入力そのものを修復するものではない。BoundsPaddingは追加の手動余白として残す。

## 検証場と手順

`Assets/Samples/ParticleAuthoring/ParticleAuthoring.scene`は4効果とカメラのみの独立scene。砂は単発で0.5秒時点が確認しやすい。光跡はカメラへ見える横/上方向へ初速を調整する。Water FlowはGPU要求、ほかの3種はCPU。

固定60Hzの`Projects/Tests/Fixtures/ParticleAuthoring/ParticleAuthoringSmoke.playtest.json`は0.5秒と2.5秒にGame Viewをcaptureし、Emitter数・scene.validate・error logを表明する。画像比較のbaselineは未作成。実描画が未確認なら、このcapture手順だけで見た目の合格とはしない。

共通CPU枠の取得後に、次を既存のAgentBuild入口で直列実行する。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 check Projects/Engine/src/Scene/Systems/RenderParticleExtractor.cpp Projects/Editor/src/Util/ParticleEmitterModules.cpp Projects/Tests/Engine/Auto/ParticleCurveTests.cpp Projects/Tests/Engine/Auto/ParticleCodecTests.cpp Projects/Tests/Engine/Auto/ParticleLoopInspectionTests.cpp
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 build FBZZTestsEngineAuto
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 test -Filter 'ParticleCurveTest|ParticleCodecTest|ParticleLoopInspectionTest'
```

既存3テストファイルに13件を追加した。補間方式/色空間/温度補間だけのcache更新3件と、黒体の定温時の色空間による輝度維持1件、Starterの競合設定解除とcodec往復2件、8種の有限・予算内・決定的なCPU再生、Burst overflow、GPU縮退契約3件。自動境界4件は実CPU更新経路を使い、回転・軸倍率・ストレッチ、World/Localと負scaleでのbillboard/ribbon履歴、空poolの形状/Local移動、非有限入力と巨大な有限距離を検証する。新しいテストファイルやCMake登録は不要。

GPU枠とSDK更新後はScene Viewの選択Inspectorを狭い幅でも操作し、各タブ・Replay・Preview speed 0/0.25/1・Undo/保存再読込を確認する。固定カメラで昼夜の背景、画面端の境界、LOD near/far、GPU Water FlowとCPUへ切替えた結果、Particle Overdrawを確認する。実機画像を生成した場合はLibraryへ登録して親タスクにIDを渡す。

現時点の独立検査: AgentLint 7ファイル、errors=0・warnings=0。Python標準tomllibでscene/material/metaをparseし、参照GUID、素材3コピー、curveキー、pool上限、scenarioを検査。C++コンパイル・13件の実行・SDK更新・Editor/GPU・画像baselineは共通枠の調整後に行う。
