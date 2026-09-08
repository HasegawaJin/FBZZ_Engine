# Active Ragdoll — XPBD 関節体への移行設計

- Status: Draft
- Author: Hasegawa Jin
- Date: 2026-09-02
- Scope: `fbzz::physics` (ソルバ・拘束), `fbzz::scene` (RagdollSystem / RagdollComponent), GreenWare (ボスの被弾反応)

> **2026-09-04: GreenWare 側の使い手 (`BossRagdollComponent`) は撤去した。**
> 転倒は `Boss_Crash` クリップ、被弾のひるみは脚の揺れもの + `BossCollapsePostureComponent`
> の傾けに戻してある。本書の GreenWare への言及は、撤去前の経路の記録として読むこと。
> エンジン側 (RagdollSystem / XPBD) の設計と実装はこの決定で変わらない。
>
> **2026-09-05: «部位だけ» 落とす形で戻した。**全身を物理にすると 21 クリップの予兆が
> 崩れるという撤去理由は、落とす範囲を部位に絞れば起きない。`RagdollComponent` に
> 根を複数取れるようにし (`extraRootBones`)、**壊れた脚の `Thigh_XX` だけ**を根にして
> Passive で落とす (`BossPolarityRigComponent::RefreshRagdollLegs`)。生きている脚と胴は
> Animator のままなので、予兆は 1 コマも壊れない。§4.4 の «根は繋ぎ止めが要る» は
> 根ごとに独立した部分木として効く ─ 繋ぎ止めの強さもその根の部分木だけで数える
> ように直した (以前は全身の合計を全部の根へ配っていた)。

骨を質点として落とす現行ラグドールを、**剛体と関節で組む XPBD ベースの関節体**へ置き換える。
狙いは 2 つ。GreenWare のボスに「機械が力負けする」画を出すことと、
後から Human Fall Flat 型のプレイヤー操作ラグドールを**同じソルバのパラメータ違い**で作れる形にすること。

---

## 1. 現状と課題

### 1.1 置き換え前の実装 (〜M2b-1 / 2026-09-03 に撤去済み)

[`RagdollComponent`](../../Projects/Engine/include/Engine/Scene/Components/RagdollComponent.hpp) /
[`RagdollSystem`](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp) は、
骨 1 本を質点 1 個とする Verlet + 距離拘束で組んであった。

| 要素 | 実装 |
|---|---|
| 積分 | Verlet (固定刻み 1/60、最大 4 substep) |
| 拘束 | 親子の距離拘束 + 祖父–孫の「筋交い」(`braceStiffness`) |
| 接地 | 水平面 1 枚 (`groundHeight`) + 水平減衰 |
| 筋力 (Active) | 毎フレームの FK 姿勢を目標とする位置射影 |
| ブレンド | `BlendIn → Hold → BlendOut` と適用率 `weight` |
| 出力 | `CommitBoneWorldPose` でスキニング行列とボーン Transform を対で更新 |

Active モード（立ったまま筋力で支える）は成立している。目標を毎フレーム取り直すので、
無負荷なら釣り合い点がアニメーションそのものになり、押されたぶんだけ沈んで戻る。
GreenWare のボスの被弾ひるみはこの経路を使っている
([`BossRagdollComponent::Stagger`](../../GreenWare/Assets/Scripts/Combat/BossRagdollComponent.hpp))。

### 1.2 構造的な限界 — 「回転を持っていない」

質点系は 1 骨 = 3 DOF (位置) しか持たず、骨の姿勢は隣の質点への向きから後から組み立てている。
残っている欠落はすべてここから派生している。

| 欠落 | 原因 |
|---|---|
| ツイストが作れない | `FromToRotation` は軸周りの成分を持たない。構造的に不可能 |
| 角度制限が書けない | 制限したいのは関節の相対回転 (swing/twist) だが、その量が状態として存在しない |
| 慣性が無い | 質点なので回転の抵抗が無い。腕を振った反動が胴へ伝わらない |
| 自己衝突が無い | 腕・脚が胴を貫通する |
| 環境と噛み合わない | 床が水平面 1 枚。坂・段差・壁を無視する |
| 質量分布が無い | `invMass` は全質点 1.0 固定。COM が定義できず、バランス制御も書けない |
| たわみが刻みに依存する | 筋力が「1 ステップで詰める割合」なので、定常たわみが `g·dt²/k` になる |

角度制限や COM をこの上に足すと「近似の上の近似」になる。**回転を状態として持つ**のが根本の修正。

### 1.3 スケジューラ側の宣言漏れ (M0 で修正済み / 2026-09-02)

[`SystemScheduler`](../../Projects/Engine/src/Core/Scheduler/SystemScheduler.cpp) は
同一バッチの System を `TaskSystem` で並列実行し、バッチは `ComponentAccess` の競合から決まる。
ところが `RagdollSystem::GetAccess()` は `Writes<RagdollComponent, AnimatorComponent, BoneComponent>` だけで、
**実際に読む `SkinnedMeshRenderer` と、実際に書く骨の `Transform` を宣言していない**
(AnimatorSystem / IKSystem / SpringBoneSystem も同様)。

後続の ConstraintSystem 以降が `Unrestricted()` で全体バリアとして働いているため
被害が広がっていなかったが、**`LODSystem` は `Writes<SkinnedMeshRenderer>` を宣言しており、
上記 4 システムはその `model` / `nodeEntities` を読む**。宣言が交差しないので
`HasConflict` が false になり、LOD と Animator が同じバッチで並列実行され得る状態だった。

**M0 で修正済み**。4 システムに `Reads/Writes<SkinnedMeshRenderer>` と `Writes<Transform>` を追加し、
LateUpdate は `LOD → Animator → IK → Spring → Ragdoll` の直列になった。
競合から張られる辺は常に登録順（小さい添字 → 大きい添字）なので、閉路は構造的に増えない。

---

## 2. 目的

### 2.1 やること

1. **ロボット感** — 狭い可動域、硬いドライブ、重い慣性、そして**サーボのトルク上限**。
   「モーターが力負けして関節が back-drive する」画を出す
2. **汎用性** — 同じソルバのパラメータ違いで、ロボット / 人間 / 脱力死体を出せる
3. **世界との相互作用** — 坂で倒れる、壁に当たる、瓦礫を蹴散らす、押し返される
4. **HFF 型ラグドールへの到達可能性** — 掴んでぶら下がる、箱を押す、が後から載る土台

### 2.2 やらないこと

- 全身 IK / モーションマッチング（別系統。既存の `IKSolverComponent` の範囲）
- 筋骨格シミュレーション（腱・筋線維モデル）
- 布・ソフトボディ（`SpringBoneComponent` の範囲を超えない）
- 学習ベースのバランス制御（RL コントローラ）

---

## 3. 方針の決定

### 3.1 到達点は「接触と関節を同じ substep で解く」

HFF 型ラグドールの芯は「片手で縁にぶら下がる」「箱を押す」で、
このとき**接触が関節連鎖を通して全体重を支える**。持続的な両方向接触そのものなので、
接触を既存の PGS が全ステップで、関節を別ソルバが substep で解いていると必ず取り合いになる
（腕が伸びる or 接触がジッターする）。したがって最終的には

> **World を substep 化し、関節と接触を同じ substep の中で解く**

まで行く。ただし**接触の解法を書き換える必要はない**。
"Small Steps in Physics Simulation" (Macklin 2019) の主張は解法ではなく刻みの話で、
既存の PGS はそのまま substep の中で回せる。ここを取り違えるとリスクの見積もりが跳ね上がる。

一方、**ロボット感の要件は関節の性質だけで満たせる**ので、世界との相互作用より前に出せる。
だから段階を分ける（§7）。

### 3.2 質点系の延命 (旧案 A) は採らない

「今の質点系に角度制限と COM を足す」案は、剛体へ移る時点で全部捨てになる。
角度制限は相対回転を状態に持って初めて書けるもので、距離拘束での近似は移植できない。**却下。**

### 3.3 なぜ XPBD か

| 論点 | XPBD | 速度レベル PGS (既存) |
|---|---|---|
| 硬い拘束 | 小さい substep で安定。ドライブ剛性を上げられる | 反復数を増やしても質量比で発散する |
| パラメータ | compliance `α` [rad/(N·m)] という物理単位。刻み非依存 | ゲインが刻みと反復数に依存 |
| 定常たわみ | `α·τ`（刻みに依らない） | — |
| ワインドアップ | `λ` は substep 内でのみ累積。構造的に起きない | — |
| 既存コードとの距離 | 現行ラグドールと同じ「位置射影」の系統 | — |

**PID の I 項が要らなくなる**のが実務上いちばん大きい。
現行の「1 ステップで詰める割合 `k`」は定常たわみが `g·dt²/k` で刻みに依存し、
それを消すために I 項を足すとひるみ中（大きな誤差が 0.3 秒続く区間）でワインドアップする。
compliance ならたわみが `α·τ` で刻みに依らず、`λ` が substep ごとにリセットされるので
そもそも溜まらない。**PD + 重力補償すら不要になる。**

参照実装: Müller et al., *Detailed Rigid Body Simulation with Extended Position Based Dynamics* (2020)。
関節制限もドライブもこの論文の形をそのまま採る。

### 3.4 3 つの設計上の約束

これを守る限り、初期段階は最終形の**真部分集合**になり、捨てるコードが出ない。

1. **ソルバは `fbzz::physics` に置く**（`fbzz::scene` に private な物理を作らない）
2. **剛体は `physics::RigidBody` をそのまま使う**（ラグドール専用のボディ型を作らない）
3. **接触も最初から同じソルバの拘束として書く**（自前の水平床を作らない。
   現行の `groundHeight` 方式を持ち込むと、世界と噛み合わせる段階で全部捨てになる）

---

## 4. アーキテクチャ

### 4.1 レイヤーと依存方向

```
GreenWare (BossRagdollComponent)
      ↓ ScriptRagdollProxy
Engine/Scene   RagdollSystem      … スケルトン ↔ 剛体の橋渡し、捕獲、ブレンド、書き戻し
               RagdollComponent   … 設定と実行状態
      ↓
Physics        XPBDSolver         … substep ループ、拘束の解決
               RagdollJoint       … ボールソケット / swing-twist 制限 / 角度ドライブ
               RigidBody          … 既存。位置・姿勢・v・ω・invMass・対角 invInertia
      ↓
Math
```

既存の依存方向 (`Engine → Physics → Math`) を崩さない。
スケルトンを知っているのは Scene 側だけで、Physics は骨を知らない。

### 4.2 データ構造

**剛体**は既存の [`physics::RigidBody`](../../Projects/Physics/include/Physics/RigidBody.hpp) を使う。
必要な要素は揃っている。

| 必要なもの | 現状 |
|---|---|
| 角速度 `ω` | `GetAngularVelocity()` / `m_angularVelocity` |
| 逆慣性テンソル | `m_invInertiaDiag` + `ApplyInvInertia()` が `R·(diag⊙(Rᵀv))` を正しく計算する |
| 慣性の自動計算 | `SetInertiaFromCollider()` |
| トルク | `ApplyTorque()` |
| **点インパルス** | `ApplyImpulseAtPoint(J, worldPoint)` を追加済み (M4)。瓦礫へ反作用を返すのに使う |

**関節**は新規。実装は [`XPBDJoint`](../../Projects/Physics/include/Physics/XPBDJoint.hpp)
（可動域は `XPBDJointLimits`、ドライブは `XPBDJointDrive` に分けた）。関節フレームは
**X = ツイスト軸 (骨の向き)、Y/Z = スイング軸**に固定し、`Build()` が今の姿勢を「たわみ 0」に据える。
スイング制限は円錐ではなく**軸ごとの非対称な上下限 (角錐)** — 円錐は中立軸まわりに対称なので
「膝は前にだけ 140°」が書けないため。以下は設計時のスケッチ。

```cpp
struct RagdollJoint {
    RigidBody* parent = nullptr;
    RigidBody* child  = nullptr;

    // 各剛体ローカルでの関節位置。ボールソケットはこの 2 点を一致させる。
    math::Vector3 anchorParent, anchorChild;
    // 静止時の相対姿勢。制限もドライブもここからの差で測る。
    math::Quaternion restRelative;
    // ローカルのツイスト軸 (骨に沿った軸)。
    math::Vector3 twistAxis;

    // 可動域 [rad]。swing は楕円錐の 2 半角、twist は非対称に取れる。
    float swingLimitY = 0.6f, swingLimitZ = 0.6f;
    float twistMin = -0.3f, twistMax = 0.3f;

    // compliance。0 で剛。単位は位置 [m/N]、角度 [rad/(N·m)]。
    float socketCompliance = 0.0f;
    float limitCompliance  = 0.0f;
    float driveCompliance  = 1.0e-4f;
    float driveDamping     = 0.0f;
    // サーボのトルク上限 [N·m]。0 以下で無制限。
    float maxDriveTorque   = 0.0f;

    // ドライブの目標相対姿勢。Active では毎フレーム FK から取り直す。
    math::Quaternion driveTarget = math::Quaternion::Identity();

    // 解の状態。substep ごとにリセットする。
    float lambdaSocket = 0.0f, lambdaLimit = 0.0f, lambdaDrive = 0.0f;
};
```

### 4.3 substep ループ

```cpp
// h = dt / substeps。substeps は 8〜16 を想定 (現行の 4 より細かい)。
for (int s = 0; s < substeps; ++s) {
    for (Body& b : bodies) {
        b.prevPosition = b.position;
        b.prevRotation = b.rotation;
        b.velocity        += h * gravity * b.gravityScale;
        b.position        += h * b.velocity;
        b.angularVelocity += h * I⁻¹ * (τ_ext - ω × (I·ω));   // ジャイロ項を含む
        b.rotation         = Normalize(b.rotation + 0.5f * h * [ω, 0] * b.rotation);
    }

    for (Joint& j : joints) { j.ResetLambda(); j.SolvePositions(h); }
    for (Contact& c : contacts) c.SolvePositions(h);

    for (Body& b : bodies) {
        b.velocity        = (b.position - b.prevPosition) / h;
        b.angularVelocity = AngularVelocityFrom(b.prevRotation, b.rotation, h);
    }

    for (Joint& j : joints) j.SolveVelocities(h);      // 関節減衰・ドライブ減衰
    for (Contact& c : contacts) c.SolveVelocities(h);  // 反発・摩擦
}
```

現行ラグドールの `Integrate → SolveMuscles → SolveLinks` と同じ骨格で、
「位置を直接動かして、そこから速度を出し直す」構造は変わらない。

### 4.4 スケルトンとの橋渡し

Scene 側 (`RagdollSystem`) の責務。現行の構造をほぼそのまま使える。

| 段階 | 内容 |
|---|---|
| **構築** | プロファイルが指定する骨だけに剛体を作る。カプセルは骨の長さから自動生成し、半径はプロファイル |
| **捕獲** | FK 姿勢を剛体の位置・姿勢へ写す。速度は 0 から（現行と同じ理由 — 初速は呼び出し側が Push で明示する） |
| **駆動 (Active)** | 毎フレーム、各関節の `driveTarget` を FK の相対回転から取り直す。無負荷なら釣り合い点がクリップそのものになる |
| **書き戻し** | 剛体姿勢 → 骨のワールド姿勢 → 既存の `CommitBoneWorldPose` |
| **中間骨** | 剛体を持たない骨は、親剛体からの相対姿勢を FK のまま保つ (指・補助骨) |
| **ブレンド** | 既存の `weight` / `phase` / `blendIn` / `blendOut` をそのまま使う |

「毎フレーム目標を取り直す」は現行 Active の `RefreshRest` と同じ考え方で、
**目標が位置ではなく相対回転になる**点だけが違う。

### 4.4.1 根は繋ぎ止めが要る（当初の設計に抜けていた）

関節は**隣の骨との相対**しか拘束しない。根の骨は何にも繋がっていないので、Active で
走らせるとサーボが形を保ったまま全体が重力で落ちていく。質点系では「全質点を FK へ
寄せる」筋力が根も引いていたので問題にならず、この差に気付いていなかった。

倒れる数秒だけ走らせる分には見えないが、**立っている間ずっと走らせると胴が床下へ沈む**。
[`XPBDPoseAnchor`](../../Projects/Physics/include/Physics/XPBDPoseAnchor.hpp) が
根の剛体を FK の姿勢へ引き止める。完全固定にしないのは、固定すると押しても胴が動かず
「押されて沈む」が手足のたわみだけになるため ─ サーボと同じく compliance と力の上限を
持たせ、力も自重比 (`rootAnchor`) で持つ。押された瞬間は `impactSlack` が繋ぎ止めにも
掛かるので、胴が動いて戻る。**脱力 (Passive) では外す。**

### 4.5 既存 `RagdollComponent` の扱い

外向きの API (`ScriptRagdollProxy`) は変えない。
`Begin` / `BeginActive` / `End` / `Push` / `PushAt` / `IsStanding` / `GetDeviation` はそのまま使える。
内部の `RagdollParticle` / `RagdollLink` が `RagdollBodyRef` / `RagdollJoint` に置き換わる。

`braceStiffness`（筋交いの強さ）は角度制限に置き換わるので廃止。
`muscleStiffness` / `muscleFalloff` は compliance の逆数として意味が残るが、単位が変わるので改名する。

---

## 5. 拘束の仕様

### 5.1 XPBD の共通形

拘束 `C` を compliance `α` で解く。`α̃ = α / h²`。

```
位置拘束 (点 r1, r2 を方向 n に量 c だけ寄せる):
    w_i  = 1/m_i + (r_i × n)ᵀ I_i⁻¹ (r_i × n)
    Δλ   = (−c − α̃·λ) / (w1 + w2 + α̃)
    p    = Δλ · n
    x1  += p/m1,   q1 += ½·[I1⁻¹(r1 × p), 0]·q1
    x2  -= p/m2,   q2 -= ½·[I2⁻¹(r2 × p), 0]·q2

角度拘束 (軸 n、角 θ):
    w_i  = nᵀ I_i⁻¹ n
    Δλ   = (−θ − α̃·λ) / (w1 + w2 + α̃)
    p    = Δλ · n
    q1  += ½·[I1⁻¹p, 0]·q1,   q2 -= ½·[I2⁻¹p, 0]·q2
```

`λ` は substep のはじめに 0 へ戻す。溜まらないのでワインドアップしない (§3.3)。

### 5.2 ボールソケット

親と子のアンカー 2 点を一致させる位置拘束。`α = 0`（剛）。
既存の `HingeConstraint` がやっているのはこれだけで、
**軸を拘束していない**ことが現行ラグドールで剛体を使わなかった理由。

### 5.3 Swing / Twist 制限

相対姿勢 `q_rel = q_parent⁻¹ · q_child · restRelative⁻¹` を
ツイスト軸まわりの `q_twist` と、それに直交する `q_swing` に分解する。

- **Swing**: 楕円錐。2 半角 `(swingLimitY, swingLimitZ)` を超えた分だけ角度拘束で押し戻す
- **Twist**: `[twistMin, twistMax]` を超えた分だけ押し戻す。**非対称に取れることが重要**
  （膝も肘も片方向にしか曲がらない）

制限は**不等式拘束**なので、範囲内では何もしない (`Δλ` を計算しない)。
`limitCompliance` を 0 より大きくすると「止まるが少し食い込む」＝ ゴムの止まり方になる。
ロボットは 0 に近く、人間は少し持たせる。

### 5.4 角度ドライブとトルク上限

目標相対姿勢 `driveTarget` への角度拘束。compliance `driveCompliance` で柔らかさを決める。

```
θ_err = 2·Im(q_rel · driveTarget⁻¹)     // 小角近似での回転ベクトル
```

**トルク上限**は `λ` のクランプで入れる。関節が発生できるトルクは `τ = λ / h²` なので、

```
|λ| ≤ maxDriveTorque · h²
```

でクランプすれば、超えた瞬間に関節が目標へ追従できなくなり **back-drive する**。
これが「サーボが力負けする」画の正体で、ロボット感の中心。
HFF 側では同じ機構が「握力が足りず手が滑る」「腰が砕ける」になる。

`driveDamping` は速度パスで解く（位置パスに混ぜると刻みに依存する）。

### 5.5 接触

**自前の床を持たない**（§3.4-3）。段階に応じて接触の作り方を変える。

| 段階 | 相手 | 生成 |
|---|---|---|
| M2b-2 | 水平面 1 枚 | [`XPBDPlaneContact`](../../Projects/Physics/include/Physics/XPBDPlaneContact.hpp) をカプセルの両端に張る |
| M3 ✅ | 静的コライダ | 全身を包む球で `World::OverlapSphere` を **1 回**引き、AABB で絞ってから既存の `PhysicsSolver::NarrowPhase` へ丸投げする。返った `ContactPoint` を [`XPBDContact`](../../Projects/Physics/include/Physics/XPBDContact.hpp) に包んでソルバの transient 拘束にする |
| M4 ✅ | 動的剛体 | 同じ経路。解き方だけ «壁として解いて反作用を返す» に分かれる (§7) |
| M5 | 全部 | `World::Step` の substep ループが全接触を生成・解決 |

**接触の «検出» は 1 行も書いていない。** カプセル vs 三角メッシュも地形も、既存の
NarrowPhase がそのまま効く ─ Macklin (2019) の主張は解法ではなく刻みの話なので、
接触の作り方を変える理由が無い。

摩擦は Coulomb で、位置パスで静摩擦 (この substep で滑った距離を `|λ_t| ≤ μ|λ_n|`
の範囲で打ち消す)、速度パスで動摩擦。反発も速度パス。既存の `PhysicsMaterial` の
合成規則で骨側と世界側を混ぜる。

**Active の間は世界の接触を切ってある** (`RagdollComponent::contactWhileActive`、既定 false)。
立っている間 «足をどこに置くか» を決めているのはクリップで、床はその通りに踏まれている
前提で作ってある。そこへカプセルの半径ぶんの押し出しを重ねると、足が半径だけ浮いた所で
サーボと釣り合い、体が宙に浮く。世界と噛み合わせたいのは «崩れてから» なので、
支えている間は切る。自己衝突は姿勢がクリップの近くに居る限り新しく重ならないので残す。

**水平面 1 枚は抜け止めとして残してある** (`RagdollComponent::groundPlane`)。面は
«オーナーの足元» と «今のクリップの最下点» の低い方に置くので、床コライダーがある限り
世界側が先に受け止めて何もしない。床を置き忘れたシーンで «床下へ消えていく» のが
いちばん原因の分かりにくい壊れ方なので、そこだけを拾う。段差や多層の地形では切る。

### 5.5.1 1 剛体につき 3 形状

NarrowPhase が返す接触点は形状の組ごとに 1 個で、寝かせた胴が «1 点で床に触れている»
状態になり、その点を軸にくるくる回る。そこでカプセルに加えて**両端の球**も当てる。
球はカプセルの内側にあるのでカプセルより深い接触を報告することはなく、3 点で押さえられる。
自己衝突はカプセルどうしだけ ─ 端の球まで当てると同じ重なりを 9 通り報告してしまう。

### 5.6 自己衝突の除外

除外の規則は 2 つ。どちらも `RagdollRig` が持つ（`XPBDSolver` はラグドールを知らない）。

1. **関節グラフ上の距離** — 先祖を `selfSkip` 段まで遡って相手に当たれば除外する。
   1 で親子、2 で祖父–孫まで（首・肩は 2 段でも重なる）。既定は 2
2. **組んだ時点で重なっていた組** — 肩と胸のように、関節で繋がっていなくても
   元から重ねてある組がある。当てると起動した瞬間に押し合って自壊するので、
   `Build()` で AABB の重なりを記録し、その組は永久に外す

[`Layer`](../../Projects/Physics/include/Physics/Layer.hpp) は使わない ─
レイヤーは «誰と誰が» をシーン全体で決める仕組みで、骨ごとに違う除外は表現できない。

---

## 6. パラメータ設計

### 6.1 プロファイル

「どの骨に剛体を作り、どの関節にどの可動域を与えるか」はアセットではなくプロファイルで持つ。
骨名のパターンマッチ（`*Thigh*` → 股関節、`*Knee*|*Shin*` → 蝶番）で当てる。
最初は `MechProfile` / `HumanoidProfile` の 2 つを組み込みで持ち、
エディタから上書きできる形にする（オーサリング UI は M2 の後で足す。クリティカルパスに入れない）。

**当たらなかった骨は黙って通さない。** `Build()` が fallback に落ちた骨名を集め、
組み直したときに 1 度だけログへ名指しする（数は Inspector の Unmatched Bones）。
当たらなくても設定は返るので、外していても «なんとなく柔らかいラグドール» にしか
見えない ─ 骨格を差し替えるたびに起きる種類の失敗なので、症状ではなく原因を出す。

### 6.1.1 骨格に依らない量で書く

絶対値で書いた設定は、骨格を差し替えた瞬間に意味を失う。実際に Boss_01 で全部外した。

| 持ち方 | 単位 | なぜ |
|---|---|---|
| `density` | kg/m³ | 質量を直に書くと、節が長い骨格で «短い骨が重すぎる» |
| `radiusRatio` | 骨の長さ比 | 半径を直に書くと «長い骨が針金のように細い»。Boss_01 の脛は 2.7m あり、人型の想定で置いた 0.14m は当たり判定 (0.38m) の 3 分の 1 だった |
| `torqueScale` | 自重比 | トルクを直に書くと桁が合わない。人型の想定で置いた 14,000 N·m は、8 トンの Boss_01 の股関節には必要量の 1/3 しかなかった |
| `holdSag` | rad | compliance は [rad/(N·m)] なので質量に依存する。«上限を出し切るまでのたわみ角» なら依らない |

`torqueScale` の基準は「**その関節から先を、重力に対して真横へ伸ばした姿勢で支える**のに要るトルク」。
バインドポーズでは脚がまっすぐ下を向いていて重力に垂直な腕がほぼ 0 になるため、そちらを基準にすると
「必要トルク 0」と出てしまう。姿勢に依らず決まる「関節からの距離」を腕に採ると、倍率がそのまま
「どこまで傾けても耐えるか」になる。実トルクは `Build()` が部分木の質量から埋め、重力を変えれば追従する。

### 6.1.2 Boss_01 のリグとの対応

実リグは `Root → Body` の下に、突起 4 本と脚 4 本：

| 骨 | 扱い | 剛体 (骨 → 最初の子) |
|---|---|---|
| `RootNode` / `Boss_Armature` / `Root_Motion` / `Root` | `noBodyPatterns` | **なし**（枝は辿る）。Root → Body は 4.5m あり、拾うと胴から原点へ棒が 1 本できる |
| `Body` | `body` | Body → Head。半径 1.74m のほぼ球になり、当たり判定 (1.90m) と揃う |
| `Head` / `Rear` / `Ring` | — | **なし**（葉） |
| `Core` | `core` | Core → Muzzle。砲身 |
| `Yaw_XX` | `yaw` | Yaw → Thigh。脚の付け根の球 |
| `Thigh_XX` | `thigh` | Thigh → Shin。股関節 |
| `Shin_XX` | `shin` | Shin → Hock。膝。片方向の蝶番 |
| `Hock_XX` | `hock` | Hock → Foot。飛節。膝と同じ蝶番で**逆向き**。接地するのはこの節 |
| `Foot_XX` | — | **なし**（下が全部 exclude なので葉になる） |
| `Toe*` / `Heel*` | `excludePatterns` | **なし**（枝ごと落とす）。拾うと脚 1 本あたり剛体が 8 個増える |

**剛体 18 個 / 関節 17 本**（胴 1 + 砲身 1 + 脚 4 × 4）。総質量は約 14 トン。
実機の Inspector がこの数と大きく違うなら、`_$AssimpFbx$_` 分割ノードで骨が拾えていない。

### 6.1.3 可動域はクリップから測る

**可動域の上下限の符号は骨格依存。** 関節フレームの Z 軸は骨の向きから作るので、
どちらが «前» かは FBX のボーン軸で決まる。手で書くと必ず外す。

そこで `learnLimits`（既定 on）が、毎フレーム**ドライブ目標そのものを swing/twist へ
分解**して「クリップがこの関節に要求している角度」を測り、可動域をそこまで広げる。
広げるだけで狭めないので、**クリップが使う範囲は必ず可動域の内側**になる。
符号も、クリップが曲げる側だけが開くので自動で決まる。

これが無いと、プロファイルに書いた狭い可動域がクリップの動きより狭いときに
サーボが目標へ行けず、**「アニメーションが崩れる」**として現れる。崩しているのは
物理ではなく「物理が許していない」方で、原因の切り分けには `Limited`
（可動域に食い込んでいる関節の数）を見る。

| 症状 | 見る値 | 原因 |
|---|---|---|
| クリップどおりに動かない | **Limited > 0** | 可動域がクリップより狭い。`learnLimits` を入れる |
| 支え切れず垂れる・畳まれる | **Saturated > 0** | トルク不足。`torqueScale` / Muscle を上げる |
| 全体がじわじわ落ちる | Deviation が伸び続ける | 根の繋ぎ止め (`rootAnchor`) が弱い |
| 震える | — | `substeps` 不足、または可動域とサーボが押し合っている |

### 6.2 ロボット / 人間 / 死体

同じソルバのパラメータ違いで出し分ける。**ここが「汎用的なラグドール」の実質。**

| | ロボット (ボス) | 人間 (HFF) | 死体 (脱力) |
|---|---|---|---|
| swing 制限 | 狭い (15〜35°) | 広い (60〜110°) | 広い |
| twist 制限 | 極めて狭い (±5°) | 中 (±40°) | 中 |
| limitCompliance | ~0 (硬く止まる) | 少し持たせる | 少し持たせる |
| driveCompliance | 小 (硬い) | 中 | **∞ (ドライブ無効)** |
| driveDamping | 高 (カクッと止まる) | 中 | 0 |
| maxDriveTorque | 有限。**力負けが見える値** | 有限。低め | — |
| 質量 | 重い。慣性大 | 標準 | 標準 |

### 6.3 Passive / Active の位置づけ

現行の `RagdollMode` はそのまま残る。実装上の違いは**ドライブを有効にするかどうか**だけになる。

- **Passive** = `driveCompliance = ∞`（ドライブを解かない）。関節制限と接触だけで崩れる
- **Active** = 毎フレーム `driveTarget` を FK から取り直す

「崩落」も自然に書き直せる。現行の `collapseDistance`（一番離れた骨の距離）は物理的な意味がない代用品だが、
剛体になれば **COM が接地足の支持多角形から出たか**で判定できる。
質量分布が入るので COM が初めて意味を持つ（現行は `invMass` が全部 1.0）。

---

## 7. マイルストーン

| | 内容 | 動くようになるもの | 止められるか | 規模 |
|---|---|---|---|---|
| **M0** ✅ | `ComponentAccess` の宣言漏れを修正 (§1.3) | 挙動変化なし。LOD と Animator の並列実行を解消 | ○ | 完了 |
| **M1** ✅ | `XPBDSolver` 骨格。substep 積分 + 拘束インターフェース。ラグドールのアイランドだけ回す | — | × | 完了（ビルド未検証） |
| **M2a** ✅ | 関節 (`XPBDJoint`): ボールソケット / swing-twist 制限 / 角度ドライブ + トルク上限 | 関節単体としての可動域・力負け（テストで検証） | × | 完了（ビルド未検証） |
| **M2b-1** ✅ | `RagdollProfile` (骨名 → 太さ/密度/可動域/サーボ)、`RagdollRig` (骨の並び ↔ 剛体の往復) | 関節体としての組み立てと姿勢の往復（テストで検証） | × | 完了（ビルド未検証） |
| **M2b-2** ✅ | `RagdollSystem` の差し替え（Skeleton → 骨配列の変換、質点系の撤去） | **ロボット感・角度制限・ツイスト・慣性・力負けが画面に出る** | ○ | 完了（実機での調整は未) |
| **M3** ✅ | 接触を同じソルバの拘束として実装 (vs 静的ワールド) + 自己衝突 | 坂・段差・壁で倒れる。腕が胴を貫通しない | ○ | 完了（実機での調整は未) |
| **M4** ✅ | 動的剛体との接触。`ApplyImpulseAtPoint` 追加 | 瓦礫を蹴る・押される | ○ **GreenWare の完成形** | 完了（実機での調整は未) |
| **M5** | `World::Step` を substep 化し、関節と接触を同じ substep で解く | — (既存の統合) | ○ **HFF の土台** | 5 日 + 既存の再調整 |
| **M6** | 掴み拘束・プレイヤー入力・バランス制御 | HFF 型ラグドール | — | 内容次第 |

### M4 の «同じアイランド» をどう実装したか

M5 まで `World::Step` は substep 化されないので、**World が積分している剛体を XPBD の
substep の中で動かすと 1 フレームに 2 回進む**。そこで M4 では相手の扱いを 2 通りに分けた。

| 相手 | substep 内 | 反作用 |
|---|---|---|
| 自分の骨 | 双方を動かす (`solveOther = true`) | 拘束が両側へ配るので不要 |
| World の剛体 | **動かさない**。«今の姿勢の壁» として解く | 法線力積を溜め、フレーム末に `ApplyImpulseAtPoint` で 1 回返す |

これで «蹴る» (溜めた力積が瓦礫へ入る) と «弾かれる» (動いている瓦礫が壁として押し込む)
の両方が出る。**M5 で両者が同じ substep に入れば、この分岐ごと消える。**

### 受け入れ条件

- **M1**: 単体の剛体が自由落下し、床に相当する 1 枚の接触拘束で止まる。substep 数を変えても静止位置が変わらない
  → [`Projects/Tests/Physics/Auto/XPBDSolverTests.cpp`](../../Projects/Tests/Physics/Auto/XPBDSolverTests.cpp) で自動検証 (`FBZZTestsPhysicsAuto`)
- **M2a**: 関節 1 本の試験台で、(a) 可動域でちょうど止まる、(b) 非対称な可動域が書ける、
  (c) ドライブが重力に抗して目標を保つ、(d) トルク上限を下げると `m·g·L·cos θ = τ_max` の角度まで垂れる
  → [`XPBDJointTests.cpp`](../../Projects/Tests/Physics/Auto/XPBDJointTests.cpp)
- **M2b-1**: 骨の並びから剛体と関節を組み、(a) 捕獲 → 書き戻しで元の姿勢に戻る（組んだ時と違う姿勢でも）、
  (b) 落としても骨が伸びない、(c) 剛体を持たない葉が親に付いてくる、(d) ドライブがクリップの形を保つ
  → [`RagdollRigTests.cpp`](../../Projects/Tests/Engine/Auto/RagdollRigTests.cpp)
- **M2b-2**: ボスのスケルトンで、(a) 膝が逆に折れない、(b) `maxTorque` を下げると腰から崩れる、
  (c) 無負荷でクリップと一致する（`GetDeviation()` が 0 付近）、(d) 押すと沈んで戻る
  → 実装は入った。**シーン上での確認と数値の詰めは残っている**。
  診断は `BossRagdollComponent` の Debug グループ（Bodies / Saturated / Deviation）で見る ─
  Saturated が常に 0 なら Muscle が強すぎ、常に全部なら弱すぎる
- **M3**: 傾斜地形の上で倒すと、地形に沿って止まる。階段で足が沈まない
  → 世界のコライダーで止まること・繋がった骨が自己衝突しないことは
  [`RagdollRigTests.cpp`](../../Projects/Tests/Engine/Auto/RagdollRigTests.cpp) で自動検証。
  **傾斜と階段での見え方はシーン上で確認する**
- **M4**: 倒れたボスが近くの動的剛体を弾く。逆に弾かれる
  → 弾く側は `RagdollRigTests.cpp` で自動検証。弾かれる側は接触が «壁» として効くだけなので
  同じ経路。**手応えの調整はシーン上で**
- **M5**: 既存の CharacterController・瓦礫の挙動が M4 以前と同等（回帰確認）
- **M6**: 別途

---

## 8. 既存資産の扱い

| 資産 | 扱い |
|---|---|
| `ScriptRagdollProxy` の API | **そのまま**。`GetSaturatedJointCount()` (力負けしている関節の数) だけ追加した |
| `RagdollComponent` の phase / weight / blendIn / blendOut | **そのまま** |
| `RagdollStatus` (診断) | **そのまま**。項目を増やす |
| `CommitBoneWorldPose` / スキニング更新 | **そのまま** |
| 捕獲 (FK → 物理) の考え方 | **そのまま**。書き写す先が質点から剛体になる |
| `BossRagdollComponent` の Stagger / Topple | **そのまま**。パラメータの単位だけ変わる |
| `RagdollParticle` / `RagdollLink` | 撤去。`RagdollRig` (剛体 + `XPBDJoint`) へ |
| `braceStiffness` / `boneRadius` / `iterations` / `damping` | 撤去。太さと重さはプロファイル、`iterations` は `substeps`、`damping` は剛体の `linearDrag` / `angularDrag` へ |
| `groundHeight` / `groundThickness` / `SolveGround` | 撤去。`XPBDPlaneContact` へ (M3 で実接触に差し替え) |
| `muscleStiffness` / `muscleFalloff` / `muscleDamping` | `driveScale` / `driveFalloff` / `driveDamping` へ改名。**プロファイルの値への «倍率»** になり、`SetMuscle()` の引数の意味だけが変わる (シグネチャは同じ) |
| `collapseDistance` | 据え置き。COM ベースの判定は M3 以降 (`RagdollRig::CenterOfMass` は実装済み) |

---

## 9. リスクと検証

| リスク | 対処 |
|---|---|
| substep 数の増加によるコスト | **前提が変わった。** GreenWare のボスは `alwaysActive` で常時走らせるので、「倒れる数秒だけ」ではなくなった。剛体 18 個 × 12 substep が毎フレーム回る ─ `RagdollSystem` の `FBZZ_PROFILE_SCOPE` を Analysis で見て、重ければ `substeps` を落とす。Active はクリップに追従しているだけなので、倒れるときほどの刻みは要らないはず |
| M5 の既存挙動への波及 | M4 までで止められる設計にしてある。M5 は独立したブランチで、CharacterController と瓦礫の回帰を通してからマージ |
| 質量比による発散 | substep を細かくするのが唯一の対処。M1 の受け入れ条件に「substep 数を変えても静止位置が変わらない」を入れてある |
| プロファイルの当てが外れる (骨名が合わない) | **実際に外れた** (Boss_01 の `Body` と `Hock`)。fallback に落ちた骨名を `Build()` が集めてログへ名指しし、数を Inspector に出す。「剛体 0 個」も無言で通さない |
| オーサリングの手間 | カプセルは骨長から自動生成。手で詰めるのは可動域だけ。UI は M2 の後 |

デバッグ表示は `RagdollDebugPass` (Viewport の Overlays > Ragdoll、
operator `render.show_ragdoll`)。剛体のカプセルは水色、関節の可動域（角錐）は緑、
**トルク上限に張り付いている関節は赤**、接触点と押し返す向きは黄で描く。
「赤が出たら支え切れていない」だけ覚えれば読める配色にしてある。

数字だけの確認は `ScriptRagdollProxy` の `GetSaturatedJointCount()` / `GetContactCount()`
（GreenWare では `BossRagdollComponent` の Debug グループ）。
**倒れているのに Contacts が 0 なら、床に Collider が無いか Contact World が切れている。**

---

## 10. 未決事項

1. **プロファイルの持ち方** — 組み込みの 2 種で始めるが、最終的にアセット (`.ragdoll`) にするか
   `RagdollComponent` のシリアライズに埋めるか
2. **M5 で PGS を残すか XPBD へ寄せるか** — M3/M4 で「検出は既存の NarrowPhase、解決は
   XPBD」の形が動いたので、**接触も XPBD へ寄せる方に倒れた**。M5 で必要なのは
   `World::Step` の substep 化と、`XPBDContact` を World 側の剛体にも使うこと。
   CharacterController と瓦礫の回帰が通るかが判断材料になる
3. **バランス制御の形** — COM + 支持多角形までは決まっているが、
   「踏ん張って一歩出す」を入れるかは M6 の設計で
4. **HFF 側の入力設計** — 腕の目標をカーソルで動かすのか、掴み対象へ IK で伸ばすのか

---

## 付録 A. ノックバック (本設計とは独立)

「プレイヤーに斬られてボスが後退する」はラグドールの層ではなく**移動の層**の話で、
本設計と独立に入れられる。**保留中** — 下記のアニメーション側の問題を先に決める必要がある。

### A.1 移動そのものは簡単

ボスは既に物理剛体で、
[`MoveHorizontal` / `StopHorizontal`](../../GreenWare/Assets/Scripts/Combat/BossAiComponent.hpp)
が毎フレーム水平速度を**上書き**している。したがって剛体へインパルスを入れても次のフレームで消える。
`BossAiComponent` に減衰するノックバック速度を持たせ、この 2 関数が設定する速度へ上乗せすればよい。
壁で止まるのは剛体が接触を解くので自動で、**壁際へ追い込める**挙動もそのまま出る。

### A.2 だが足が動かない

[`BossAnimatorComponent::SampleLocomotion`](../../GreenWare/Assets/Scripts/Combat/BossAnimatorComponent.hpp) は

```cpp
const float speed = delta.Length() / dt;   // 符号なし
```

で移動量の**大きさ**だけを `Speed` パラメータへ流している。後ろへ押すと `Speed > 0` になり、
**前進の Walk_Crawl を再生しながら後ろへ滑る**。「後退」ではなく「滑走」に見える。

### A.3 取れる手

| | 内容 | コスト | 備考 |
|---|---|---|---|
| 1 | `SampleLocomotion` を前後の符号付きにし、Blend Tree の負側に Walk_Crawl の逆再生を置く | 中（Controller の編集） | 四足なら逆再生歩行が成立しやすい。クリップは増えない |
| 2 | 押された分だけ接地足を IK で置き直す（ステップ） | 大 | 最も正しい。M6 のバランス制御と地続きなので、そこまで待つ手もある |
| 3 | 後退させず「前進を打ち消す」。距離は 10cm 程度に抑える | 小 | 足の滑りが読めない範囲に収め、手応えは*止まること*で伝える |
| 4 | 大きい一撃だけ 1 歩下がり、その瞬間だけ短い後退クリップを流す | 中（クリップ 1 本） | 頻度が低いので破綻しない |

推奨は **3 を土台にして、大きい一撃だけ 1 か 4**。毎ヒットで数十 cm 下げると、
間合いで択を決めている AI（`chargeMinRange` 18 / `beamMinRange` 8 / `stompMaxRange` 6）が
斬るたびに揺れて攻撃選択が不安定になる。「押し込んで間合いを作る」をプレイヤーの手札にするなら、
常時ではなく大きい一撃へ集約したほうが読み合いとして立つ。**ゲームデザインの判断待ち。**
