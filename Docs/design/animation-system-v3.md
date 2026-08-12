# Animation System v3

## 目的

FBZZ Engine の `AnimationClip` を Skinned Animation 専用データから、Unity の Animation/Animator と同様に GameObject、Component、Material、Morph、Skeleton を同じ時間軸で制御できる汎用クリップへ拡張する。

Blender/Maya の Constraint、Driver、Expression、IK Solver、NLA など DCC 固有の実行系そのものは移植しない。FBX にベイクされた Transform、Bone、BlendShape/Morph、カスタムプロパティと、FBZZ 側で定義した Layer/Event/Root Motion をランタイムから完全制御する。DCC 固有機能は書き出し前に Bake Animation する。

## 互換性

- `.anim` v2 は引き続き読み込める。
- `.anim` v3 は可変長セクション方式とし、未知セクションを読み飛ばせる。
- 既存 `AnimatorComponent` の単一 Clip、1D/2D BlendTree、State Machine は維持する。
- 既存 Bone Track は `NodeAnimationTrack` として維持し、GameObject Transform にも同じ Track を適用できる。

## 実装チェックリスト

- ✅ `.anim` v3 バイナリと v2 読み込み互換
- ✅ 通常 GameObject の Transform Clip
- ✅ Float / Vector2 / Vector3 / Vector4 / Color / Int / Bool の Component Property Track
- ✅ Material Property Track と Material Slot 指定
- ✅ Animation Event の区間検出、Loop、逆再生、Script callback
- ✅ Root Motion の Position / Rotation 抽出と適用
- ✅ BlendShape / Morph Target の Mesh 格納、FBX 取込、Weight 再生
- ✅ Override / Additive Animation Layer
- ✅ Transform Path ベース Avatar Mask
- ✅ Skeleton Retarget Profile と Rest Pose 補正値
- ✅ 冗長キー除去、Quaternion 正規化、サンプリングの二分探索
- ✅ Blender/Maya FBX の Node / Bone / Morph と命名規約 Event の取込
- ✅ ScriptProxy から Layer、Root Motion、Morph を制御し、Event callback を受信
- ✅ Controller Asset の Layer / Mask / Retarget 永続化
- ✅ Scene Serializer の新規 Animator 設定永続化
- ✅ `git diff --check` による静的差分検証
- ✅ Blender 側 Root Motion ノード整備とエクスポート規約 (`Tools/BlenderExport/`, `Docs/conventions/blender-export.md`)
- ⏳ Visual Studio 2022 での全体ビルドと実機 FBX 再インポート
  - MiniBot 30 クリップの FBX は書き出し済み (`Assets/Models/MiniBot/`)。
    FBX バイナリ検査ではテイク名・骨 53 本・`Root_Motion` の T/R/S チャンネルまで確認済み。
    残るは Editor での実インポートと、Blender 由来 FBX の軸補正 (`FbxImportTool` の axisFix) が
    実データで正しく効くかの確認。

## ランタイム適用順

1. State Machine と BlendTree から加重クリップ集合 (`WeightedClip`) を構築する。
2. Root Motion を加重合成し、適用先へ反映してから `Script::OnAnimatorMove()` を発火する。
3. 加重クリップ集合からベース姿勢を評価する (Root 成分は除去済み)。
4. Override Layer を Mask 範囲へ正規化 Weight で合成する。
5. Additive Layer の Rest Pose との差分を合成する。
6. Retarget Profile で Source Path を Target Path へ変換する。
7. Skeleton、GameObject Transform、Component Property、Material、Morph へ適用する。
8. 前回時刻から今回時刻の区間にある Event を一度だけ通知する。

Root Motion を姿勢評価より **前** に置くのは、`AnimatorSystem` が `TransformLateUpdate` の
後に走るためである。後に回すとボーン伝播が適用前の Transform を親として使い、
見た目が 1 フレーム遅れる。

## Root Motion の受け取り方

`AnimatorComponent::rootMotion` (`RootMotionSettings`) が、適用先・解決方法・軸マスクを
直交した設定として持つ。

| 設定 | 値 | 意味 |
|---|---|---|
| `mode` | `None` | 抽出しない。`poseMode` がポーズの扱いを決める |
| | `ApplyToTransform` | エンジンが適用先 Transform を動かす (既定) |
| | `ExtractOnly` | Transform に触れず `OnAnimatorMove` だけ発火。移動は Script の責任 |
| | `ApplyToRigidBody` | 水平成分を RigidBody 速度へ渡す。落下と衝突は物理が解く |
| `poseMode` | `Strip` / `Keep` | `mode = None` のとき、その場再生にするか DCC のまま前進させるか |
| `source` | `ClipDefined` | `.anim` が指定したトラックのみ (既定) |
| | `AutoDetect` | 候補名 → Skeleton ルート名の順に自動解決 |
| | `NodeName` | `nodeName` で明示指定 |
| `targetPath` | 相対パス | 空 = Animator 自身。`..` で親 (RigidBody が親にある構成) |
| `applyXZ` / `applyY` / `applyRotation` | `UseClip` / `Disabled` / `Enabled` | クリップ側フラグの三値オーバーライド |
| `positionScale` / `rotationScale` | float | アニメの歩幅とゲーム速度を合わせる倍率 |

移動量は `WeightedClip` ごとにクリップ自身の前フレームサンプルとの差分で求め、Weight で
加重合成する。BlendTree の Walk↔Run も遷移中のクロスフェードも、ポーズと同じ比率で
移動量が混ざる。

出力は `rootMotionDeltaPosition` (ローカル) / `rootMotionWorldDelta` /
`rootMotionWorldVelocity` / `rootMotionDeltaTime` として公開する。`ScriptAnimatorProxy` の
ポーリングは 1 フレーム遅れるため、移動を Script が握る場合は `OnAnimatorMove` を使う。

## DCC 連携上の境界

| DCC データ | FBZZ Engine |
|---|---|
| Object / Bone Transform Key | ネイティブ Track として制御 |
| Shape Key / BlendShape | Morph Track として制御 |
| Material/Custom Property | 型付き Property Track へ変換 |
| Constraint / IK / Driver | DCC でベイク後、Transform/Morph として制御 |
| NLA / Animation Layer | Bake または FBZZ Animator Layer へ再構成 |
| Cloth / Hair / Particle Simulation | Alembic等の頂点キャッシュは別システム対象、骨/Morphへベイクした場合は制御可能 |

Blender/Maya 側で `FBZZ_EVENT__Footstep` のように命名した補助ノードへ Position Key を置くと、各 Key 時刻を `Footstep` Event として取り込む。Position X は `intParam`、Y は `floatParam` に変換する。

Root Motion ノードの判定は `asset::ClassifyRootMotionNodeName` に集約し、インポーターと
ランタイムで同じ規則を共有する。名前は namespace / パス区切り / Assimp の補助ノード
suffix を落とし、区切り記号と大小文字を無視して比較する。

| 段階 | 名前 | 扱い |
|---|---|---|
| Explicit | `RootMotion` / `MotionRoot` / `RootMotionNode` / `FBZZ_RootMotion` / `Trajectory` | インポート時に `hasRootMotion = 1` を立てる |
| Skeletal | `Root` / `Reference` / `Armature` / `Skeleton` / `COG` / `Hips` / `Pelvis` / `Bip01` 等 | トラック位置だけ記録し、有効化は `RootMotionSource::AutoDetect` に委ねる |

Skeletal 段階を既定で有効化しないのは、その場アニメ (Mixamo の in-place クリップ等) の
腰の揺れまで移動量として抜き出してしまい、キャラクターが漂うため。どの候補にも当たらない
リグは、インポート設定 `FbxImportOptions::rootMotionNodeName` か Animator 側の
`RootMotionSource::NodeName` でノード名を明示指定する。

Root Motion ノードは姿勢評価時に bind へ固定される (`AnimatorSystem::StripRootMotionFromPose`) ため、DCC 側では「Root ボーンからの抽出」が必要になる。単にコピーすると GameObject と骨で二重に動く。Blender 側の具体的な手順・スクリプトは `Docs/conventions/blender-export.md` と `Tools/BlenderExport/` を参照。

「DCC の全機能」はファイル形式とランタイムモデルが異なるため無条件には移植できない。FBX が保持できる再生結果を欠落なく取り込み、プロシージャル機能はベイクを境界にする。
