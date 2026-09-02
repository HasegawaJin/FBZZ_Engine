# Active Ragdoll — XPBD 関節体への移行設計

- Status: Draft
- Author: Hasegawa Jin
- Date: 2026-09-02
- Scope: `fbzz::physics` (ソルバ・拘束), `fbzz::scene` (RagdollSystem / RagdollComponent), GreenWare (ボスの被弾反応)

骨を質点として落とす現行ラグドールを、**剛体と関節で組む XPBD ベースの関節体**へ置き換える。
狙いは 2 つ。GreenWare のボスに「機械が力負けする」画を出すことと、
後から Human Fall Flat 型のプレイヤー操作ラグドールを**同じソルバのパラメータ違い**で作れる形にすること。

---

## 1. 現状と課題

### 1.1 今あるもの

[`RagdollComponent`](../../Projects/Engine/include/Engine/Scene/Components/RagdollComponent.hpp) /
[`RagdollSystem`](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp) は、
骨 1 本を質点 1 個とする Verlet + 距離拘束で組んである。

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
| **点インパルス** | **無い**。`ApplyImpulse()` は重心のみ。`ApplyImpulseAtPoint(J, r)` を追加する (10 行) |

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
| M3 | 静的コライダ | ラグドールのカプセルで `World` の BVH を問い合わせ、接触を拘束として生成 |
| M4 | 動的剛体 | 同じアイランドに入れて双方向に解く |
| M5 | 全部 | `World::Step` の substep ループが全接触を生成・解決 |

摩擦は Coulomb で、位置パスで静摩擦、速度パスで動摩擦。既存の `PhysicsMaterial` を使う。

### 5.6 自己衝突の除外

親子関係にある剛体どうしは除外する（関節で繋がっているので必ず重なる）。
祖父–孫までを除外するかはプロファイルで持つ（首・肩は 2 段でも重なる）。
[`Layer`](../../Projects/Physics/include/Physics/Layer.hpp) とは別に、
関節グラフ上の距離で判定するフィルタを `XPBDSolver` に持たせる。

---

## 6. パラメータ設計

### 6.1 プロファイル

「どの骨に剛体を作り、どの関節にどの可動域を与えるか」はアセットではなくプロファイルで持つ。
骨名のパターンマッチ（`*Thigh*` → 股関節、`*Knee*|*Shin*` → 蝶番）で当てる。
最初は `MechProfile` / `HumanoidProfile` の 2 つを組み込みで持ち、
エディタから上書きできる形にする（オーサリング UI は M2 の後で足す。クリティカルパスに入れない）。

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
| **M2b-2** | `RagdollSystem` の差し替え（Skeleton → 骨配列の変換、質点系の撤去） | **ロボット感・角度制限・ツイスト・慣性・力負けが画面に出る** | ○ | 2 日 |
| **M3** | 接触を同じソルバの拘束として実装 (vs 静的ワールド) | 坂・段差・壁で倒れる | ○ | 3 日 |
| **M4** | 動的剛体を同じアイランドへ。`ApplyImpulseAtPoint` 追加 | 瓦礫を蹴る・押される | ○ **GreenWare の完成形** | 3 日 |
| **M5** | `World::Step` を substep 化し、関節と接触を同じ substep で解く | — (既存の統合) | ○ **HFF の土台** | 5 日 + 既存の再調整 |
| **M6** | 掴み拘束・プレイヤー入力・バランス制御 | HFF 型ラグドール | — | 内容次第 |

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
- **M3**: 傾斜地形の上で倒すと、地形に沿って止まる。階段で足が沈まない
- **M4**: 倒れたボスが近くの動的剛体を弾く。逆に弾かれる
- **M5**: 既存の CharacterController・瓦礫の挙動が M4 以前と同等（回帰確認）
- **M6**: 別途

---

## 8. 既存資産の扱い

| 資産 | 扱い |
|---|---|
| `ScriptRagdollProxy` の API | **そのまま**。呼び出し側 (GreenWare) は変更なし |
| `RagdollComponent` の phase / weight / blendIn / blendOut | **そのまま** |
| `RagdollStatus` (診断) | **そのまま**。項目を増やす |
| `CommitBoneWorldPose` / スキニング更新 | **そのまま** |
| 捕獲 (FK → 物理) の考え方 | **そのまま**。書き写す先が質点から剛体になる |
| `BossRagdollComponent` の Stagger / Topple | **そのまま**。パラメータの単位だけ変わる |
| `RagdollParticle` / `RagdollLink` | 置き換え |
| `braceStiffness` | 廃止（角度制限へ） |
| `groundHeight` / `groundThickness` / `SolveGround` | 廃止（接触拘束へ） |
| `muscleStiffness` / `muscleFalloff` / `muscleDamping` | compliance へ改名・単位変更 |
| `collapseDistance` | COM ベースの判定へ置き換え |

---

## 9. リスクと検証

| リスク | 対処 |
|---|---|
| substep 数の増加によるコスト | ラグドールは常時ではなく「倒れる数秒 / ひるむ 0.3 秒」しか走らない。M5 で World 全体を substep 化するときに初めて全体コストになるので、そこでプロファイルを取る |
| M5 の既存挙動への波及 | M4 までで止められる設計にしてある。M5 は独立したブランチで、CharacterController と瓦礫の回帰を通してからマージ |
| 質量比による発散 | substep を細かくするのが唯一の対処。M1 の受け入れ条件に「substep 数を変えても静止位置が変わらない」を入れてある |
| プロファイルの当てが外れる (骨名が合わない) | 現行と同じく `RagdollStatus` で名指しする。「剛体 0 個」を無言で通さない |
| オーサリングの手間 | カプセルは骨長から自動生成。手で詰めるのは可動域だけ。UI は M2 の後 |

デバッグ表示は既存の `ConstraintDebugGeometry` / `ColliderDebugGeometry` を使う。
関節の可動域（錐）と、ドライブが飽和している関節の色分けは M2 の一部として入れる
（「どの関節が力負けしたか」が見えないと、ロボット感の調整ができない）。

---

## 10. 未決事項

1. **プロファイルの持ち方** — 組み込みの 2 種で始めるが、最終的にアセット (`.ragdoll`) にするか
   `RagdollComponent` のシリアライズに埋めるか
2. **M5 で PGS を残すか XPBD へ寄せるか** — 接触は PGS のまま substep 内で回すのが低リスクだが、
   関節と接触で解法が違うままになる。M4 の結果を見て決める
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
