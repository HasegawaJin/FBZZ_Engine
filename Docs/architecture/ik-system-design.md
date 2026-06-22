# IK System 設計改善 — FullBodyIK に向けたアーキテクチャ

## 概要

アニメーション後段の IK 処理を、単一の `IKSystem` が複数 Solver タイプを順序付きで実行できる
拡張可能な設計に切り替える。FootIKSystem / FootIKComponent を廃止し、FullBodyIK に向けた
チェーン間依存 (足→腰→脊椎→手) の基盤を整える。

---

## 現状の問題

| 問題 | 詳細 |
|------|------|
| 型拡張性なし | `IKChain` は Root/Mid/Tip 固定の 2-Bone 専用。LookAt / Spine / Hand を追加する口がない |
| 二重システム | `FootIKSystem` が `IKSystem` と別系統で存在し、実行順を `Before<IKSystem>` で外部管理している |
| 膝が伸縮する | `FootIKSystem` は足の Y だけ平行移動するだけで 2-Bone IK を解かない |
| FBIK 依存順なし | 足→腰→脊椎→手 という依存関係を表現するメカニズムがない |

---

## アーキテクチャ設計

### 1. `IKSolverType` enum

```cpp
// IKSolverComponent.hpp
enum class IKSolverType : uint8_t {
    TwoBone   = 0,   // 解析的 2-Bone IK (現行・デフォルト)
    FootPlace = 1,   // 地形スナップ + 両足 2-Bone IK + ヒップ補正
    LookAt    = 2,   // 角度制限・速度平滑化付き 1 ボーン回転追従
    Spine     = 3,   // 可変長多ボーン FABRIK
};
```

新タイプ追加時はここに値を追加し、`IKSystem.cpp` に対応 Solver 関数を実装するだけで拡張できる。

---

### 2. `IKChain` の再構成

#### 変更前後

| フィールド | 旧 | 新 |
|-----------|----|----|
| ボーン名 | `rootBoneName` / `midBoneName` / `tipBoneName` (3個固定) | `std::vector<std::string> boneNames` (可変長) |
| Solver 種別 | なし (2-Bone 固定) | `IKSolverType type = TwoBone` |
| 実行優先度 | なし | `int order = 0` (低い値が先) |
| FootPlace パラメータ | FootIKComponent に別存在 | IKChain に統合 |
| スムーズ状態 | `FootIKComponent::leftSmoothedY` 等 | `IKChain::smoothedLeft/Right/Hip` |

#### `boneNames` のインデックス意味 (type ごと)

| type | [0] | [1] | [2] | [3] |
|------|-----|-----|-----|-----|
| TwoBone | Root | Mid | Tip | — |
| FootPlace | *(未使用: ヒューマノイド標準名を自動使用)* | — | — | — |
| LookAt | 対象ボーン | — | — | — |
| Spine | Base | … | … | Tip |

#### 新 `IKChain` の主要フィールド

```cpp
struct IKChain {
    IKSolverType type    = IKSolverType::TwoBone;
    bool         enabled = true;
    float        weight  = 1.0f;
    int          order   = 0;   // 実行優先度。FootPlace=0, Spine=10, LookAt=20 推奨

    // 共通: ボーン名 / ターゲット参照 / IK パラメータ
    std::vector<std::string> boneNames;
    EntityID      targetEntity, poleEntity;
    std::string   targetName, targetGuid, poleName, poleGuid;
    math::Vector3 targetOffset;
    float         maxExtension, softness;
    bool          autoPole;
    math::Vector3 autoPoleLocalDirection;

    // FootPlace 専用
    bool          useAnimatorIKWeight;
    float         rayUpRatio, rayDownRatio, footSurfaceOffset;
    float         correctionDeadZone, maxCorrection, smoothTime;
    math::Vector3 footNormalAxis;
    bool          adjustHip;
    std::string   hipBoneName;

    // LookAt 専用
    math::Vector3 lookAtAxis, lookAtUpAxis;
    float         lookAtClampAngle, lookAtSpeed;

    // ランタイム状態 (Reflect 対象外、FootPlace 用)
    float smoothedLeft, smoothedRight, smoothedHip;
};
```

#### FootPlace は「両足 1 チェーン」設計

左右の脚をひとつの `IKChain(type=FootPlace)` で処理する。
現行 `FootIKSystem` と同じ粒度であり、左右の補正量を同一フレームで保持できるため
ヒップ補正の協調が自然に機能する。

---

### 3. `IKBodyState` — チェーン間共有コンテキスト

```cpp
// IKSystem.cpp anonymous namespace
// エンティティ 1 体あたり毎フレームリセットされるチェーン間共有状態。
// WHY: FBIK では足→腰→脊椎という依存順があり、上流チェーンの計算結果を
//      下流チェーンに伝達する場として使う。
struct IKBodyState {
    math::Vector3 hipDisplacement = math::Vector3::ZERO;
    // 将来: shoulderDisplacement, spineOffset, ...
};
```

`FootPlace` が `hipDisplacement` を書き込み → `Spine` IK がそれを読んで脊椎を補正できる
(将来の FullBodyIK の依存チェーン)。

---

### 4. `IKSystem` — フリー関数ディスパッチ

新タイプ追加 = 関数を 1 つ追加するだけ。`IKSystem::Update` は変更不要。

```cpp
// IKSystem.cpp anonymous namespace
bool SolveTwoBone  (IKChain&, Scene&, Skeleton&, SMR&, AnimatorComponent&,
                    const Matrix4& ownerInv, float stateWeight, IKBodyState&);
bool SolveFootPlace(IKChain&, Scene&, physics::World&, Skeleton&, SMR&, AnimatorComponent&,
                    const Matrix4& ownerInv, float stateWeight, float dt, IKBodyState&);
bool SolveLookAt   (IKChain&, ...);  // 軸追従 + Clamp + 時間平滑化
bool SolveSpine    (IKChain&, ...);  // 可変長 FABRIK

// IKSystem::Update 内
for (EntityID id : scene.GetEntities<IKSolverComponent>()) {
    IKBodyState bodyState;
    auto sortedChains = /* chains sorted by order */;
    for (auto* chain : sortedChains) {
        switch (chain->type) {
            case TwoBone:   anyMod |= SolveTwoBone  (*chain, ...); break;
            case FootPlace: anyMod |= SolveFootPlace (*chain, ...); break;
            case LookAt:    anyMod |= SolveLookAt    (*chain, ...); break;
            case Spine:     anyMod |= SolveSpine     (*chain, ...); break;
        }
    }
    if (anyMod) UploadBoneMatrices(...);
}
```

---

### 5. `SolveFootPlace` の処理フロー

FootIKSystem の全ロジックを移植し、足首 Y 移動を完全な 2-Bone IK に昇格させる。

```
Step 1  左右の脚ノード解決
        LeftUpLeg / LeftLeg / LeftFoot / LeftToeBase
        RightUpLeg / RightLeg / RightFoot / RightToeBase

Step 2  地形レイキャスト → 生補正量 (leftRaw, rightRaw) を取得
        接地の上下両方向を [-maxCorrection, +maxCorrection] で取得
        デッドゾーン内でも「接地中」として保持し、膝 Solver を止めない

Step 3  指数平滑化
        alpha = 1 - exp(-dt / smoothTime)
        smoothedLeft  = Lerp(smoothedLeft,  leftRaw  * effectiveWeight, alpha)
        smoothedRight = Lerp(smoothedRight, rightRaw * effectiveWeight, alpha)

Step 4  ヒップ補正 (足だけ動かすと下腿が伸縮するため、先に腰を下げる)
        bendReserve = averageLegLength * (1 - maxExtension)
        hipTarget = min(0, smoothedLeft, smoothedRight) - bendReserve * effectiveWeight
        smoothedHip = Lerp(smoothedHip, hipTarget, alpha)
        ApplyNodeAndDescendantsOffset(hipNode, {0, smoothedHip, 0})
        bodyState.hipDisplacement.y = smoothedHip   ← 他チェーンへ伝播

Step 5  各脚の 2-Bone IK (← ここが旧システムとの最大の差分)
        root   = UpLeg.fkWorldPos + hipDelta   ← ヒップ補正後の正しい根元位置
        target = {foot.fk.x, foot.fk.y + smoothedFoot, foot.fk.z}
        bend   = autoPoleLocalDirection (Owner ローカル) を優先し、未指定時は FK 膝方向を維持
        接地中は高さ補正 0 でも解き、maxExtension 分の膝曲げを保証
        → コサイン定理で midBone (膝) 位置を解く
        → rotA_ik, rotB_ik を適用

Step 6  TranslateDescendantsKeepFkRotation でつま先 FK 維持
```

**旧システムとの差分と効果:**

| | FootIKSystem (旧) | SolveFootPlace (新) |
|-|-------------------|--------------------|
| 足首補正 | Y 平行移動のみ | 2-Bone IK で膝も解く |
| 膝の動き | 下腿が不自然に伸縮 | 正しく曲がる |
| 根元位置 | UpLeg は FK のまま | UpLeg = FK + hipDelta を IK 根元に使う |

---

### 6. `GetAccess` の更新

```cpp
ComponentAccess IKSystem::GetAccess() const {
    return ComponentAccess{}
        .Writes<IKSolverComponent>()        // smoothedLeft/Right/Hip を毎フレーム書き換える
        .Writes<AnimatorComponent, BoneComponent>();
}
```

実行順:
```cpp
OrderingHints IKSystem::GetOrder() const {
    return OrderingHints{}.After<AnimatorSystem>();
    // FootIKSystem が消えるので Before<IKSystem> も不要になる
}
```

---

## FootIKSystem / FootIKComponent 廃止

### 廃止するファイル

- `Engine/include/Engine/Scene/Components/FootIKComponent.hpp`
- `Engine/include/Engine/Scene/Systems/FootIKSystem.hpp`
- `Engine/src/Scene/Systems/FootIKSystem.cpp`

### 各ファイルの対応

| ファイル | 変更内容 |
|---------|---------|
| `SceneManager.cpp` | `AddSystem<FootIKSystem>()` 削除 |
| `ComponentRegistry.hpp` | `FootIKComponent` のインクルード・tuple 登録を削除 |
| `SceneSerializer.cpp` | FootIKComponent 形式を削除し、IKSolverComponent の type/order/boneNames と型別設定を保存・読込 |
| `InspectorAnimation.cpp` | Foot IK セクションを削除、IK Solver に FootPlace UI を追加 |
| `InspectorCommon.hpp` | AddComponent メニューの "Foot IK" エントリ削除 |

---

## 後方互換の保証

| ケース | 対処 |
|--------|------|
旧 `rootBone/midBone/tipBone` および `FootIKComponent` 形式は読み込まない。
移行対象シーンは `IKSolverComponent.chains` の新形式へ一括変換する。

---

## 変更ファイル一覧

| ファイル | 変更種別 | 主な内容 |
|---------|---------|---------|
| `Engine/include/Engine/Scene/Components/IKSolverComponent.hpp` | 改修 | IKSolverType enum 追加、IKChain 再構成 |
| `Engine/src/Scene/Systems/IKSystem.cpp` | 改修 | Solver フリー関数ディスパッチ、SolveFootPlace 追加 |
| `Engine/include/Engine/Scene/Systems/IKSystem.hpp` | 微修正 | GetAccess 更新 |
| `Engine/src/Scene/Systems/FootIKSystem.cpp` | **削除** | |
| `Engine/include/Engine/Scene/Systems/FootIKSystem.hpp` | **削除** | |
| `Engine/include/Engine/Scene/Components/FootIKComponent.hpp` | **削除** | |
| `Engine/src/Scene/SceneManager.cpp` | 微修正 | FootIKSystem 登録削除 |
| `Engine/include/Engine/Scene/ComponentRegistry.hpp` | 微修正 | FootIKComponent 削除 |
| `Engine/src/Scene/SceneSerializer.cpp` | 改修 | type/order/boneNames と型別設定を保存・読込。旧形式は廃止 |
| `Editor/src/Panels/Inspector/InspectorAnimation.cpp` | 改修 | type コンボボックス、FootPlace 専用フィールド表示 |
| `Editor/src/Panels/Inspector/InspectorCommon.hpp` | 微修正 | FootIK AddComponent エントリ削除 |
| `Assets/scenes/Main.scene` | 変換 | FootIKComponent → IKSolverComponent + FootPlace チェーン |

---

## 検証方法

1. ビルドが通ること (FootIKSystem 削除後のコンパイルエラーなし)
2. Main.scene を開き、Player に `IKSolverComponent` (FootPlace チェーン) が存在すること
3. 地形上を歩く → **膝が正しく曲がること** (旧: 下腿が伸縮していた)
4. Jump / Fall 中 → 補正が 0 に平滑減衰すること
5. 既存の TwoBone `IKSolverComponent` が引き続き動作すること
6. LookAt の対象ボーンが Clamp Angle 内でターゲットを追従し、子ボーンも剛体追従すること
7. Spine の連続ボーン列が節長を維持したまま FABRIK ターゲットへ収束すること

---

## IK Solver 実装状況

```
Solver                  状態・責務
─────────────────────    ─────────────────────────────────────
TwoBone       ✓ 実装済み
FootPlace     ✓ 実装済み
AimAt         ✓ 実装済み
FABRIK        ✓ 実装済み
HandPlace     ✓ 実装済み  手首位置・回転オフセット・指階層追従
FullBodyBiped ✓ 実装済み  IKBodyState 上で部位別 Solver を反復収束
```

FullBodyBiped は位置エフェクターの最大残差を毎反復後に測定し、`fullBodyTolerance` 以下で
早期終了する。Inspector には使用反復数・最終残差・収束可否を表示し、到達不能なターゲットや
不完全なリグ設定を実行時に判別できるようにする。
