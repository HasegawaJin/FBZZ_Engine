# Light Probe GI (Light Probe Volume)

- 状態: 実装済み (2026-09-19)。比較シーン `GreenWare/Assets/Scenes/Test/LightProbeGI.scene` とシナリオ `GreenWare/Tests/Playtests/LightProbeGI.playtest.json`

**箱の中に格子状のプローブを並べ、各点の周囲の放射輝度を L2 球面調和 (SH) へ焼き、拡散環境光を画素ごとに差し替える。**
IBL キューブは «シーン全体で 1 枚» なので、屋内・軒下・壁際でも空の光がそのまま当たり、
色の付いた壁の照り返し (カラーブリーディング) も出なかった。これを埋める。

## 何が変わるか

| | 変更前 (IBL キューブだけ) | Light Probe Volume の中 |
|---|---|---|
| 屋根の下・部屋の奥 | 空の光がそのまま当たり明るい | 空が遮られて暗くなる |
| 赤い壁の隣の白い床 | 白いまま | 赤みを帯びる |
| 日なたの床の照り返し | 無い | 壁や天井の下面が下から照らされる |
| 屋内の鏡面反射 | 空の青い映り込みが残る | プローブの暗さの比で抑える (鏡面遮蔽) |
| 発光体の周り | 直接光を持たないと何も起きない | 周りの面がうっすら色づく |

鏡面 IBL のキューブ自体は変えない。拡散項を差し替え、鏡面には遮蔽の比だけを掛ける。

## 構成

```
LightProbeVolumeComponent (箱 + 格子 + 設定。ランタイムに生 / 描画用の SH ボリュームと面 RT 6+6 枚)
        │ RenderSystem が b8 を組んだ直後に呼ぶ (グラフの外)
        ▼
ExecuteLightProbeBakePass ── probesPerFrame 個ずつ
   1 プローブ = 6 面をシーン描画 (深度付き 2D RT, 32 px)
             + 6 面を表裏だけ描画 (LightProbeFacing.hlsl, カリング無し)
             → LightProbeProject.cs.hlsl で L2 SH へ射影 → 生のボリューム (有効度付き)
   毎フレーム LightProbeDilate.cs.hlsl で無効プローブを周りで埋める → 描画用ボリューム
        ▼
b8 の probeVolumes[2] + probeSpecularOcclusion / t22 (内側) + t21 (外側)
        ▼
LightProbeGI.hlsli → IBL.hlsli: FBZZ_DiffuseIrradiance(worldPos, N)
   ← PBR / SkinnedPBR / Cloth / DeferredLighting / GreenWare の Skinned 系
Terrain.hlsl (Forward) は定数環境光をボリュームの中だけ置き換える
```

### 焼き

- 面は **深度付き 2D RT を 6 枚**。キューブ RT は深度を持たず、描画順しだいで壁の向こうの物体が手前に出る
  (ReflectionProbe の DynamicScene はこの制約のまま)。
- 何も描かれなかった画素 (深度 = クリア値) は **IBL の環境キューブ** (prefilter の mip 1) で埋める。
  箱の外 (キューブの irradiance) と中 (プローブ) が開けた場所で同じ値へ落ち着くので、境目が段にならない。
  空の輝度は IrradianceConvolution と同じ 4.0 で頭打ちにする (太陽を直接光と二重に数えない)。
- 面を描くマテリアルには本物の b8 の写しを渡す。IBL・天候は効かせ、画面空間 AO / 接触影は切る
  (メインカメラの画素を読むことになるため)。
- 不透明・非スキンの MeshRenderer だけを描く。半透明は光を遮る面ではない。
- `bounces >= 2` なら 2 回目以降は «前回の結果を受けた面» を描く。途中は新旧が混ざるが、反射が 1 回ずつ増えて収束する。
  `realtimeUpdate` はこれを回し続ける。
- 箱の移動・大きさ・格子・面解像度・bounces・deringing・rejectInsideGeometry が変わったら自動で焼き直す。
  古い結果は焼き上がるまで使う。

### 壁に埋まったプローブ (rejectInsideGeometry)

格子は形状を知らないので、プローブが壁・床・物体の中に落ちる。中から見ると裏面カリングで壁が消え、
外の空が素通しに見えるか、至近距離の暗い面だけが見える。どちらも補間で周りへ滲む。

1. 同じ物体をカリング無しで描き、`SV_IsFrontFace` だけを別の面 RT へ書く。
2. 射影 CS が «裏面が見えた立体角の割合» を数え、`kBackfaceLimit` (25%) を超えたら有効度 0 を SHC.w に書く。
3. 膨張 CS が有効度 < 0.5 のプローブを 26 近傍の有効なプローブの 1/距離² 平均で置き換える。

無効なプローブを捨てずに **埋める** のは、描画側のハードウェア三線形補間をそのまま使うため
(引く側で 8 点を読んで重み付けし直すと 1 画素 56 回の Load になる)。

### リンギング抑制 (deringing)

L2 で強い日だまりを表すと、反対側に負の輪が出る (max(0) で潰れて «黒いしみ» に見える)。
帯域 1 / 2 に Hann 窓 (0.75 / 0.25) を deringing で掛けて弱める。上げるほど光の向きがぼやける。

### 係数の並び

1 プローブ = 7 texel (RGBA16F)。Z 方向へ係数ごとに `gridZ` 枚ずつ積む
(`depth = gridZ * 7`)。中身は Sloan の多項式形で、余弦ローブの畳み込み (Â_l / π) を焼き込み済み。

```
E(n)/π = A·(n, 1) + B·(xy, yz, zz, zx) + C·(x² − y²)
slice 0..2 = A (r, g, b)   slice 3..5 = B (r, g, b)   slice 6 = C.rgb + 有効度 (w)
```

一様な放射輝度 L を焼くと L が返る。irradiance キューブと同じ単位なので、`lerp` でそのまま混ぜられる。
プローブは箱を格子に割った **各セルの中心** に置く (texel 中心 = プローブ)。箱の縁の半セルは最寄りのプローブで埋まる。
角に置くと壁面ちょうどのプローブが壁の裏 (空) を見て、壁際だけ明るく漏れる。
Z の補間は係数の区画の中に閉じる (texel.z ∈ [0.5, gridZ − 0.5])。

### 引き

- ワールド軸の箱、中心は `worldPosition`。回転とスケールは見ない (ReflectionProbe の箱と同じ)。
- 点を `normalBias` だけ法線方向へずらしてから三線形補間する。
- **同時に 2 つ**。カメラに近い 2 つを選び、小さい箱を内側 (t22) として外側 (t21) の上に重ねる。
  `lerp(lerp(IBL, 外側, w外), 内側, w内)`。各 w は箱の縁 `edgeFade` [m] で 0 へ落ちる。箱の外では 0 で、従来の絵と一致する。
- **鏡面遮蔽**: `ratio = 輝度(プローブ) / 輝度(IBL キューブ)` を [0,1] に留め、`probeSpecularOcclusion` の強さで鏡面 IBL と clearcoat に掛ける。
  明るくはしない。強さは内側のボリュームの設定を使う (1 画面に 1 つ)。
- Cloth は従来どおり彩度補正前の値 (DiffuseGI.sheen) を使う。
- Terrain (Forward) は `ambientColor` を fallback にしてボリュームの中だけ置き換える。Deferred の地形は DeferredLighting 経由で IBL と同じ扱い。

## スロット

| 枠 | 用途 | 時分割の相手 |
|---|---|---|
| t22 | 内側の SH ボリューム (Texture3D) | `TEX_LUT_COLOR_GRADE` (Composite だけが読む。同じ Texture3D) |
| t21 | 外側の SH ボリューム (Texture3D) | `TEX_TAA_HISTORY` (TAA だけが読む。bindless なので次元の違いは問題にならない) |
| b8 末尾 112 バイト | `ProbeVolumeParams[2]` + 鏡面遮蔽の強さ | — (AdvancedGraphicsCB は 352 → 464 バイト) |

## 見た目の調整

- GTAO はプローブより細かい遮蔽 (物の接地・隅) を受け持つ。プローブの間隔 (既定 1 m) より小さい陰りは GTAO に任せる。
- GI で屋内が暗くなるぶんは自動露出で持ち上げる。比較シーンは `Assets/PostProcess/LightProbeGI.fzdata` で GTAO と自動露出を有効にしてある。

## 限界

- 可視性 (深度モーメント) を持たないので、薄い壁 1 枚を挟んだ両側のプローブは補間で混ざる。
  DDGI のように深度モーメントを持てば消せるが、ボリュームが 3 倍になる。格子を壁に合わせるか、部屋ごとにボリュームを分ける。
- 同時に効くボリュームは 2 つ。3 つ目以降は遠いものから落ちる。
- IBL が引けないフレーム (Environment Light 無効) では効かず、焼きも進まない。拡散 IBL の置き換えなので。
- パーティクルは受けない。
- 影マップは前フレームの中身を読む (焼きがグラフの外なので)。カスケードの外にあるプローブは影なしで焼かれる。
- 有効判定を入れると、1 プローブあたりのシーン描画が 2 倍になる。

## 参考

- Ramamoorthi & Hanrahan, "An Efficient Representation for Irradiance Environment Maps" (SIGGRAPH 2001) — https://cseweb.ucsd.edu/~ravir/papers/envmap/envmap.pdf
- Sloan, "Stupid Spherical Harmonics (SH) Tricks" (GDC 2008) — https://www.ppsloan.org/publications/StupidSH36.pdf
- Driscoll, "Cubemap Texel Solid Angle" — http://www.rorydriscoll.com/2012/01/15/cubemap-texel-solid-angle/
- Lagarde & de Rousiers, "Moving Frostbite to Physically Based Rendering 3.0" (SIGGRAPH 2014) §4.10.2 Specular occlusion — https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf
