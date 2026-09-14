# `.vfx` テンプレート (パーティクル)

`VFXComponent` を載せたプレハブの出発点。1 ファイルが **1 つの仕組みの見本**になっていて、
そのまま鳴らしても «それらしい絵» になる。線 (雷・ビーム) は `Templates/VFXLine/`、
流体を焼くレシピは `Templates/Fluid/`。

素材は `Assets/Materials/Effects/` の既存 `.mat` を指しているので、追加のインポートは要らない。
**見た目のノブ (ブレンド・フリップブック・6-way・ソフトパーティクル) は `.mat` の
`[particle]` が正本**で、`.vfx` 側には無い (2026-08-24 に移設済み)。

> **`.vfx` にコメントを書いても残らない。** エディターが取り込んで保存し直すと、
> TOML はシリアライザーの出力で丸ごと書き直され、`#` 行は消える。仕組みの説明は
> ここ (README) に書くこと。

## 一覧

| ファイル | 見本にしている仕組み | 使い所 |
|---|---|---|
| `Impact_Burst` | 1 つの出来事を 5 層に割る・`renderPriority`・`LightComponent` + `VFXElement` + `VFXLightEnvelope` | 当たり・着弾・破壊 |
| `Ambient_Motes` | 出しっぱなし (`loop` / `autoDestroy = false` / `simulationSpace = World` / `prewarm` / カリング) | 場の塵・埃・胞子 |
| `Spark_Trails` | 粒子ごとの Trail。点の尾 (`trailRibbon = false`) と帯の尾 (`true`) を並べてある | 火花・破片・剣閃 |
| `Bounce_Debris` | `collisionMode` / `collisionResponse` と、床に貼る `HorizontalBillboard` | 跳ねる破片・落下物 |
| `Charge_Gather` | `ForceField` (Attract + Vortex) と `forceFieldChannels`・`emitRateCurve`・`sizeCurve` | 溜め・吸収・チャージ |
| `Gpu_Swarm` | `simulationMode = 1` (GPU) と縮退条件 | 数万粒の火の粉・雪・胞子 |
| `Firework_Split` | `deathSubEmitter` による二段構え | 打ち上げ・破裂・二次爆発 |
| `Scorch_Decal` | `DecalComponent` + `VFXDecalEnvelope` (跡が残る) | 焼け跡・血痕・ひび |
| `Smoke_Flipbook` | 焼いたフリップブック (`.fluid` の出口) を 1 粒で出す | 煙・爆煙・蒸気 |

## 各テンプレートの読みどころ

### `Impact_Burst`
層ごとに寿命を変えて «速い順に前へ» 並べる。閃光 0.06 秒 → 輪 0.18 秒 → 火花 0.3 秒 →
煙 0.9 秒。全部同じ寿命にすると、当たった «瞬間» が絵の中から消える。`renderPriority` は
小さいほど奥 (芯 80 / 輪 72 / 火花 64 / 煙 20)。輪の `sizeCurvePower = 0.28` は
«出た瞬間に開いて減速する» 形で、1.0 (線形) だと風船が膨らむ動きになり衝撃に見えない。
光は `ParticleEmitter` と違って時間を持たないので、`VFXElement` (窓) と
`VFXLightEnvelope` (減衰) を付けて動かす。`LightComponent.intensity` はピーク値。

### `Ambient_Motes`
当たりのエフェクトと逆になる 4 点 — `loop` / `autoDestroy = false` (終わらない)、
`simulationSpace = 1` (World。親が動いても粒はその場に残る)、`prewarm` (画面に入った瞬間に
«1 粒目» が見えない)、カリングと LOD (常時居るものは必ず入れる)。湧く体積は `boxExtents`。
広げるときは `emitRate` も一緒に上げること (範囲だけ広げると «まばらなゴミ» になる)。
風は子の `ForceField` に置いてあり、同じ 1 本を複数のエミッターへ効かせられる。

### `Spark_Trails`
`trailRibbon = false` は粒を数珠つなぎに描く (細い火花・破片)。`true` は 1 本の帯として
描くので、太くしても粒の連なりが見えない (剣閃・魔法の軌跡)。帯は 1 エミッター 1 DrawCall
なので、«少ない粒を長く» 引くほうが読める絵になる。`trailSampleInterval` は履歴を刻む間隔で、
短いほど滑らかだが `trailPointCount × 粒子数` だけ頂点が増える。
Trail は CPU 経路専用 — `simulationMode = 1` にすると黙って CPU へ縮退する。

### `Bounce_Debris`
`collisionMode`: 0 なし / 1 Physics (シーンのコライダー) / 2 Plane (水平面 1 枚) /
3 Depth (深度バッファ)。`collisionResponse`: 0 Bounce / 1 Kill / 2 Stop。
Plane は床が平らと分かっている場面で最も安く、`collisionPlaneY` は**ワールドの高さ**なので
置く場所に合わせる。`collisionBounciness` は 0.3 前後が «石が跳ねる» 見え方 (0.7 超はゴム毬)。
Physics は本物のコライダーへ当たるが粒子ごとのレイキャストになるので、数を出すなら
`collisionLayerMask` で相手を絞る。着地の土煙は `renderMode = 2` (HorizontalBillboard) で
床に貼り付ける (通常のビルボードだと立て札になる)。

### `Charge_Gather`
集束は初速では作れない (中心へ向かう向きを粒ごとに計算できない)。外周に湧かせて、中心の
Attract へ引かせる。真っ直ぐ吸うだけだと «落ちてくる» 絵になるので Vortex を少し混ぜる。
`forceFieldChannels`: 力場は既定で全エミッターに効くため、吸引を置いた瞬間に同じ場の煙まで
中心へ collapse する。ここでは吸引と受け手の両方をビット 1 (= 2) に揃えて 2 者だけの
取り決めにしてある。`emitRateCurve` は «エミッターの再生時刻 / duration» → `emitRate` の倍率
(粒の寿命ではない)。溜めは後半で 3 倍まで上げないと «溜まっている» と読めない。
`sizeCurve` は `sizeStart`〜`sizeEnd` の間を進む係数で、芯は 0 → 1 → 0.35 で膨らんでしぼむ。

### `Gpu_Swarm`
CPU 経路は 1 粒ずつ更新するので数千で飽和する。GPU 経路は位置も速度も GPU に置いたまま
積分するので万単位が出せる。**縮退の条件** (どれか 1 つでも踏むと黙って CPU へ戻る。
`ParticleGpuSimulation.hpp` が正本): `simulationSpace = Local` / `prewarm` / Trail /
SubEmitter / 粒子光源 (`lightEnabled`) / `collisionMode` が Depth 以外 / セルフシャドウ /
`useSpeedSizeCurve`・`useSpeedColorGradient` / フレーム間補間・MV 付きフリップブック。
«数を増やしたのに重い» ときはこの一覧のどれを踏んだかを最初に疑う。
`maxParticles` は GPU バッファの確保数なので、定常数 (`emitRate × lifetime` ≒ 7700) と
釣り合わせる。シーンに置いた力場は GPU 経路でも効く (力の式は CPU / HLSL で同じもの)。

### `Firework_Split`
1 段目 (`Shell`) の粒が寿命で消えた «その場所と速度» から 2 段目 (`Burst`) が湧く。
口は `deathSubEmitter` / `birthSubEmitter` / `collisionSubEmitter` の 3 つで、1 イベントで
撒く数は発火元の `subEmitterBurstCount`。受け側の `subEmitterInheritVelocity` は発火元の
速度を初速へ継ぐ割合 [0,1] で、1.0 にすると弾けず «同じ向きへ流れる» だけになるため 0.1〜0.3 に
留める。受け側は自分では湧かない (`emitRate = 0` / `bursts = []`) ようにして、`duration` は
1 段目の寿命より長く取る (注入を受け付けている窓)。散り方は受け側の `shape` が決める。

### `Scorch_Decal`
粒子は «出来事» を、デカールは «盤面が変わったこと» を伝える。時刻の正本は
`DecalComponent` 自身 (`age` / `lifetime`) なので `VFXElement` は付けず、濃さの減り方だけを
`VFXDecalEnvelope.fadeCurve` が上書きする (焼け跡はしばらく濃いまま残って最後に急に消える。
`fadeTime` の直線フェードだと置いた瞬間から薄まり «跡が残った» にならない)。
投影は自分の **Y 軸**に沿い、UV はローカル XZ 平面。床へ貼るなら回転は素のまま、壁なら
Y 軸が壁の法線を向くように倒す。`scale` は [横幅, 投影の厚み, 奥行き]。
`angleFadeDegrees` より寝た面には乗らない (斜めのデカールは伸びて筋になるため)。
ルートの `duration` はデカールの寿命まで伸ばすこと (`autoDestroy` が先に片付けないように)。

### `Smoke_Flipbook`
流体を焼くと «8×8 = 64 コマのアトラス + それを指す `.mat`» ができる (`Templates/Fluid/`)。
コマ数・進め方・6-way・ソフトパーティクルは全部 `.mat` の `[particle]` 側で、`.vfx` が
決めるのは «どこに・いくつ・どれだけの大きさで・何秒»。焼いた煙は 1 粒が «湧いて崩れるまで»
を丸ごと持っているので、大きく・少なく・寿命いっぱいで出す (増やすと同じ動きが重なって
偽物に見える)。`flipbook_mode = "lifetime"` のコマ送りは粒の寿命に張り付くので、`lifetime` が
煙の速さになる (焼いたときの尺と揃えるのが基準)。ライティングを持つ煙は自分で影を作るので、
`colorStart` は白ではなく地の色を入れる。

## 使い方

1. **中身を見る / 調整する** — Asset Browser でダブルクリックすると Prefab 編集モードで開く。
   Inspector で詰めたら **別名で保存**して自分の `.vfx` にする (ここは原本なので直接は育てない)。
2. **鳴らす** — スクリプトから `SpawnVFX(scene, "Assets/VFX/…/FX_Xxx.vfx", pos, rot)`。
   プールから取り出して頭出しまでやる入口はこれ 1 本 (`VFXComponent.hpp`)。
   GreenWare では `VfxManagerComponent` が枠を持ち回すので、鳴らす側は GameObject を作らない。
3. **シーンへ置く** — Asset Browser から `.vfx` をシーンへドラッグしても**置けない**
   (配置経路は `.prefab` だけを受け付ける)。常設のもの (`Ambient_Motes` など) は
   Prefab 編集モードで開いてシーンへコピーするか、`.prefab` として保存し直す。

## 組むときの約束

- **時刻の正本は 1 オブジェクトにつき 1 つ。** 自分で時間を持つ部品
  (`ParticleEmitter` の `startDelay`/`duration`/`loop`・`DecalComponent` の `lifetime`) には
  `VFXElement` を付けない。付けるのは時間を持たない `LightComponent` / `ForceField` /
  `MeshRenderer` / 画面効果だけ。
- **`localForces` は必ず書く。** 省くと «内蔵の力が個別フィールドだった頃» の移行処理が走り、
  旧 `gravity` の既定 (下向き 5 m/s²) が入る。重力なしにしたいなら `localForces = []`。
  種類は `fieldType`: 0 Wind (= 重力・一定風) / 1 Attract / 2 Repulse / 3 Vortex /
  4 Turbulence / 5 Drag / 6 VectorField (`.vfield`)。`radius = 0` は減衰なしで全体へ。
- **SubEmitter の名前はエフェクトの中で一意に。** 参照は GameObject の名前で、探索範囲は
  VFX ルート配下 (`VFXSystem` が頭出しのときに `subEmitterScopeRoot` を配る)。同じ名前が
  枝の中に 2 つあると、先に見つかった方が吹く。
- **数より立ち上がり。** 命中感は総量ではなく «速い層があるか» で決まる。鈍いときは
  `maxParticles` を増やす前に、寿命を短くする方を試す。
- **加算は枚数を数える。** 白飛びは 1 粒の明るさではなく重なり枚数で起きる。
- **GreenWare の `.vfx` に画面効果を入れないこと。** ヒットストップ・カメラ揺れ・振動は
  `ImpactFeedbackManagerComponent` が 1 つの出来事あたりの配分を持っている
  (`Assets/VFX/Game/README.md`)。

## 注意 / 未検証

- **未検証** — エディターを起動しての見え方は確認していない (このリポジトリでは
  ターミナルからビルドしないため)。TOML として読めること、キー名が
  `ParticleEmitterAssetCodec` / 各 `Reflect()` / `SceneSerializer` と一致すること、
  参照している `.mat` / テクスチャが実在すること、親子の `instanceId` が閉じていることまでは
  スクリプトで確認済み。数値は Inspector で詰めること。
