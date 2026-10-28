<!-- @file    baked-bone-animation.md -->
<!-- @brief   Shared bone bake playback, compatibility limits and fixed crowd verification. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-12 -->
# 大量キャラクター向けボーンベイク

全 skeleton node（骨以外の FBX helper を含む）の local translation / quaternion / scale を float32 で一定間隔にベイクする。二つの端点を保持し、時間補間と二クリップの local TRS ブレンド後に階層を合成する。最終行列の線形補間は行わない。同骨格・同クリップの読み取り専用サンプルを共有し、個体ごとの時計は既存 Animator の stateTime / BlendTree / speed / transition を使う。

## GPU姿勢共有への拡張（2026-10-18）

現在・前フレームのGPUパレットを描画へ公開し、明示的な `GPU_POSE_SHARED = 3` ではCPU姿勢生成と互換CB更新を遅延する。[GPU共有姿勢の設計](gpu-shared-pose.md)が追加契約であり、以下の初版のCPU互換・描画入力に関する記述を置き換える。`GPU_SHARED = 2` はCPU互換姿勢を保持する比較用モードとして維持する。初版の測定結果は当時の履歴であり、モード3の結果を表さない。

## 採用と互換性

`.bba` は version / skeleton と clip の内容指紋 / サンプルレート / 全 mesh LOD の頂点半径 / node parent / inverse bind / checksum を持つ派生キャッシュ。little endian の長さを検証し、破損・旧内容・不一致は原本から作り直す。既存 FileSystem の atomic writer で `<Project>/Library/StreamCache/BoneBakes` に保存する。AssetManager の世代更新で CPU と GPU の共有キャッシュを失効する。FBX / `.anim` が正本であり、生成キャッシュは再 import の代わりにならない。プロジェクト外の基点では保存せずメモリ内だけを使う。

設定済みの Animator は asset load / Edit preparation の段階でベイクを準備する。原本更新後の最初の準備にはベイクの費用があるため、測定では cold preparation と定常再生を分ける。古い hash のディスクファイルは読まないが自動削除しない。

| Animator 設定 | 値 / 意味 |
|---|---|
| `boneBakeMode` | 0: 既存方式（既定）、1: CPU ベイク、2: CPU互換を保持するGPU共有、3: GPU姿勢共有（CPU計算・転送を遅延） |
| `boneBakeSampleRate` | 既定 60 Hz、1〜1000 Hz。実間隔は duration を ceil(duration × rate) 等分 |
| `boneBakeMinDistance` | 既定 20 m。近距離またはカメラなしでは既存方式 |
| `boneBakeRequireCpuBones` | 既定 true。骨 Transform / socket / script 読み取りが不要と確認した個体のみ false |

GPU は samples / parent / binding を GPU-local StructuredBuffer に置き、32 byte の個体行（frame offset、補間率、target、blend）から既存の三フレーム DynamicRWStructuredBufferPool に palette を生成する。SkinningBatch と既存の renderer 所有 deformation buffer、描画 snapshot をそのまま使う。新 RHI API / Extractor 契約 / 描画 instancing 機構は増やさない。1 group の上限は 65535 個体。複数 mesh / LOD は同一 palette を使い、骨順が一致しない入力・範囲外 bone index（weight 0 を含む）は既存の検証で拒否する。

初版は CPU palette / nodeGlobals と現在・前フレームの 128 骨 CB を維持する。CPU の key search と骨 GameObject への適用・伝播・palette 再構築は対応個体で省略するが、CPU の補間・階層合成・CB 転送は残る。独立位相では描画 draw 数の削減を約束しない。GPU は骨ごとに ancestor をたどるため、深い階層の GPU 費用も計測する。

| 用途 | 初版の経路 / 制限 |
|---|---|
| Geometry / shadow / ray geometry | 既存 deformation buffer の成功時 publication を使用。GPU replay 失敗時は CPU palette に戻る |
| Velocity / deformation history | 既存の original vertices + 現在／前 CB を維持。generation / 骨格変更時は history を無効化 |
| 特殊 VS skinning | 現在の CPU CB を維持。共有 GPU palette は compute 用 |
| 複雑な骨制御 | IK / SpringBone / externalPose / MotionWarp、CPU bone reader は既存方式 |
| 非対応 animation | root motion / stripping、layer / mask、morph、非 Linear track、property track、mutable clip、3 個以上の同時ブレンドは既存方式 |
| bounds | 全 LOD 半径、各 node の translation norm と scale 最大値を階層伝播した保守的な球。二 clip は node ごとの最大を統合 |

任意の runtime mesh / skeleton / clip の直接書き換えは対象外。原本更新は AssetManager の世代を更新する経路を使う。骨 Transform を直接動かす用途で `boneBakeRequireCpuBones=false` を指定しない。Runtime の `fallback` と `bakeStatus` で理由を確認でき、DEBUG ログは前フレームの確定理由が変化したときに出す。

別 Model の mesh LOD は skeleton 内容と骨順が一致し、頂点半径が元 bake の範囲内に収まる場合に共有する。不一致では GPU bake palette を適用せず、既存 CPU palette の経路を保つ。骨順が異なる LOD の remap と、別 Model のより大きい頂点範囲を統合する cook は初版の対象外。各 pair の atlas は target 分のサンプルを含むため複製を伴う。GPU atlas cache は合計 256 MiB 上限、超過時は CPU palette に戻る。既存 skinning cache 解放で atlas も解放する。CPU 評価の一時行列は Animator ごとの workspace で再利用する。

Inspector の playback 設定と理由表示、`animation.state` の `boneBakeMode / boneBakeActive / boneBakeFallback / boneBakeStatus` を追加する。保存と passive Play clone は四つの設定だけを保存し、GPU handle / workspace / 実行中 palette は保存しない。

## 距離による姿勢更新の間引き

明示的に `poseLodEnabled=true` とした対応個体は、CPU ベイク・GPU 共有再生にも既存の距離と更新間隔を使う。全有効 CameraComponent のうち最も近いカメラを基準とし、カメラがない場合は毎フレーム評価する。既定の LOD 無効設定は維持する。状態時計・BlendTree 位相・遷移判定・イベントは毎フレーム進め、更新時はその時点の再生時刻を評価する。

保持できるのは、現在のモデル・クリップ・sampleRate・AssetManager 世代に対応する準備済みの共有ベイクが、直前に成功した姿勢と一致する場合だけとする。確認で I/O やベイク生成は行わない。保持フレームでは CPU palette / nodeGlobals、GPU replay の frame / blend 行、bounds をまとめて保持し、現在パレットの CB 更新を行わない。前パレットの snapshot と互換 CB の既存経路は継続するため、最初の保持フレーム以降の Velocity は保持中の姿勢を前姿勢として使う。ビューごとの時計は追加せず、色・影・マスク・Velocity は同じ公開姿勢を使う。

遷移中、近距離・カメラなし、CPU bone reader、IK / SpringBone / MotionWarp、外部ポーズ、layer / mask、morph、RootMotion など既存の非対応条件は保持を許可しない。再 import、保持姿勢と対応しないモデル・参照クリップ・sampleRate の変更、無効な再生時刻、GPU 側の fallback 後も、そのフレームで通常の再評価へ戻す。同じ内容の共有ベイクへ解決するモデル・クリップは保持できる。未対応ベイクを新たに LOD 対象へ広げず、既存の個体ごとの fallback を保つ。

`BakedBoneAnimationTest.BakedDistanceLod*` は本番の Animator 更新と読み込んだ `.anim` を通し、両 mode の時計・イベント・interval / camera 復帰、保持した GPU replay 行と bounds、現在／前 CB の全 128 骨、CPU bone / rig / layer / morph fallback、同じ骨格を持つモデル差替え、再 import・同名 state のクリップ差替えを CPU fake backend で検証する。GPU の数値 readback や実シーンの画質・速度向上はこの試験で代替しない。

## 比較の固定条件

実魚モデル 1 種（AzureLanternMinnowSkinned）、1000 個体、同一 scene / camera / transform / material / LOD 設定 / clip / shadow / ray 設定、固定 dt=1/60 を使う。既存 School script が最初の 1 秒に `fract(i × 0.61803398875)` を速度へ加えて位相をずらし、その後は全個体を速度 1 に戻す。同じ初期化と経過 frame を 0 / 1 / 2 の各 mode で使う。scene と script の SHA256 は比較 manifest に記録する。実 scene と魚配置は配置担当が所有するため、この変更では編集しない。既定 mode は 0 のままとする。

cold preparation を別記録し、120 frame warm-up 後の 600 frame を採取する。CPU frame p50 / p95、GPU skinning / 全 frame、VRAM、転送、draw / dispatch の取得可否を記録する。取得できない値は null とし、異なる counter で代用しない。測定は Release、同じ backend / adapter / 解像度。readback / screenshot / debug layer を定常測定に混ぜない。

既存 `statsSkinningPaletteUploadBytes` は CPU packed palette、BAT 初回 resident payload と per-frame replay row を数える。互換 CB、SkinningBatch row / constants、source vertices、instance data を含まない。通常再生中は互換 CB が 8192 byte × 2 × 更新個体数 / frame 残る（初 frame の previous 無効時を除く）。全転送量の比較ではこれらも別途足し、既存カウンターだけを全転送量として表示しない。BAT の palette resident は GPU pool の三 lane 分と source / target pair の atlas を含める。VRAM は adapter の実測値と payload 算出値を区別する。

画質は元 clip のサンプリングと baked palette の行列・変形頂点誤差を sample endpoint / 中間 / seam / transition で採る。60 Hz の再標本化は任意 key 境界の動きを厳密保存しない。実魚の raster / shadow / velocity / ray 画像、LOD 切替、近距離への fallback、再 import を同時刻で確認する。比較結果が出るまで速度向上・画質同等を断言しない。

| 指標 | 実行結果（2026-10-12） |
|---|---|
| Release / MSVC | EditorLauncher / EngineAuto / RayTracingEngine をリンク、warning / error 0。初期 unity scoped check の NOT-COMPILED は full dependent build で補完 |
| 自動検証 | CPU BAT 16 + 既存 Animator save/clone 1 PASS、GPU replay 2 PASS（1000 rows × 6 lanes、float32 1e-4） |
| 既存 RaySkinning | 18/19 PASS。canonical skinned material を static component に替えた際の ray eligibility 1 件は親が BAT 前にも確認した既知失敗。Legacy 0 でも残る |
| 実魚 1000 / 600 frame CPU p50 / p95 ms | Legacy 28.527 / 29.767、CPU bake 28.001 / 29.484、GPU shared 29.556 / 33.802。各1回、欠落・破棄・error 0。GPU共有の高速化は確認できない |
| palette 系 bytes / dispatch / draw（末尾 snapshot） | Legacy・CPU: 192000 / 1 / 72、GPU: 32080 / 2 / 72。現在／前 CB の算出 16,384,000 byte/frame は残る。全転送量ではない |
| process DedicatedUsage peak MiB | Legacy 1023.22、CPU 1045.76、GPU 1023.22。Windows GPUProcessMemory の約1秒間隔計測、startup込み。厳密 resident VRAM ではない |
| GPU ms | 実 skinning preparation view が query に出ないため未取得。game view の SkinningCompute=0 は実 skinning 時間ではない。全 GPU frame も未取得 |
| 実魚品質 | 3 個体 × 4 時点、node global matrix 最大差 5.514e-7。8画像とも標準 Playtest tolerance 内（mean<=0.01、bad ratio<=0.005 at 0.1）。水・postprocessを含むため幾何誤差と同義ではない |
| 未検証 | 実魚影（source sceneはcastShadows=false）/ velocity / ray の画像、実 runtime LOD切替 / 実魚reimport、cold preparation単独時間 |

条件: Release / DX12 / RTX 4070 / driver 32.0.16.1088 / 1904×871、lockstep 1/60、120 warmup / 600 sample。completed application CPU frame を計測し lockstep の 16.67 ms は使わない。filter は AnimatorSystem、GPU画像readbackは測定窓外。benchmark start の scenario / camera / configuration 文字列は未指定で tool comparison eligibility warning が残るため、外部 manifest の immutable scene / script SHA256と三報告の一致を検証した。組み込み reference 比較は使用していない。

CPU bake cache は generation scoped だが合計メモリ budget は初版では持たない。複数 sampleRate の旧 bake は generation 更新まで残る。派生 `.bba` の旧hashファイルも自動削除しない。SDK と GameHub template の反映は親の別枠で行う。

## 自動検証

`BakedBoneAnimationTest.*`（CPU）: 通常 key sampling と palette の比較、1000 個体の位相・速度、loop seam、逆／停止／clamp、antipodal quaternion、local 二 clip blend、保守 bounds、骨／LOD 不一致、source 更新指紋、壊れた hierarchy / key / clock / range、`.bba` truncation / corruption / count 防御。

追加 CPU 検証は別 Model / 個体の content 共有、reimport 世代更新後の cache 置換と履歴無効化、mutable clip の fallback、workspace 再利用後の失敗 atomicity、設定の Save / passive clone 往復、旧 scene の既定 Legacy を含む。実魚比較シナリオ三本は既存 FishSkinning1000.scene の 1000 GUID を読み取り、Play コピーへ component.set で設定する。元 scene は編集しない。各 mode の step 数、120 warmup / 600 sample、カメラ、既存位相初期化を揃え、先頭個体の bake 有効状態と fallback 理由を測定前に assert する。バッチ起動時は別の Playtest プロセスを使い、ユーザーの Editor を終了しない。

`BakedBoneReplayTest.*`（既存 opt-in `RayTracingEngine`）: 1000 個体 × 2 骨を 6 frame lane で float32 読み戻しし CPU と比較、二 clip blend / 位相 / 逆・停止・正速度、範囲外 / 非整列 / NaN / stale owner で palette 非公開を確認。通常 CPU suite では GPU を起動しない。GPU readback 合格は実魚画像の合格を代替しない。

## 設計参考

- [Epic AnimToTexture API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/AnimToTexture): species 共有 bone samples と個体 playback 情報の分離、sample rate / frame / bounds の考え方を採用。Unreal plugin への依存と texture 特有の UV / filtering は採用しない。
- [Microsoft DirectXTex](https://github.com/microsoft/DirectXTex/wiki/DirectXTex): float DDS の保存／ロードは可能。ただしこの基盤の GPU structured upload と bone metadata に合わせ `.bba` + StructuredBuffer を採用。RGBA8 PNG は精度の理由で使わない。DirectXTex は既存 texture / capture 経路のまま利用する。

変更所有: BAT の新規ファイルと Animator / Engine・Graphics SkinningCompute のフック。`RenderSceneExtractor.cpp` / `ShaderReloadTests.cpp` は Extractor 担当、実 scene / 配置は scene 担当が所有。Git index / commit / SDK 配布 / 共有起動は親の調整に従う。


## Prepared Animator constant-buffer batches (2026-10-18)

Deferred renderers batch full previous/current compatibility palettes for pure, prepared actors. The same ordered 128-matrix payloads and identity tails are retained; pose timing and calculation are unchanged. Automatic eligibility requires loaded immutable clips, current asset/controller/resource identities, existing CB handles, ready GPU resources, cached bake preparation, settled morph buffers and no events/property tracks/root motion/layers/masks or live IK/spring/warp controls. Actor diagnostics and phase-distribution experiments keep the previous path. Missing hierarchy, logging callbacks, bind fallback, CPU materialization and ineligible actors flush pending updates before observable work. Existing live bone bindings are callback-free and can stay within a packet. Immediate renderers keep their original update path.
