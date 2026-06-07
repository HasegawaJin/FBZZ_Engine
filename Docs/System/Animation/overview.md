# Animation システム 設計 — 全体概要

## 対象コンポーネント
`AnimatorComponent` / `BoneComponent` / `IKSolverComponent`
システム: `AnimatorSystem` / `IKSystem`

---

## 実装済み機能

### AnimatorComponent / AnimatorSystem

| 機能 | 実装箇所 |
|---|---|
| レガシー単クリップ再生（clipIndex / clipName / time） | `AnimatorSystem.cpp::RunLegacyAnimatorPath` |
| ステートマシン再生（states / parameters / transitions） | `AnimatorSystem.cpp::RunStateMachineAnimatorPath` |
| クロスフェード遷移（TRS レベル Lerp/Slerp） | `AnimatorSystem.cpp::EvaluateBlendedNodeRecursive` |
| パラメーター型（Float / Int / Bool / Trigger） | `AnimatorComponent.hpp`、`CheckCondition` |
| 遷移条件評価（Greater/Less/Equal/NotEqual/True/False + AND 結合） | `AnimatorSystem.cpp::EvaluateTransition` |
| hasExitTime / exitTime | `AnimationTransition::hasExitTime` |
| Trigger の自動消費 | `AnimatorSystem.cpp::ConsumeTriggers` |
| ステート別再生速度 | `AnimationState::speed` |
| ステート別 IK Weight | `AnimationState::ikWeight` |
| GetNormalizedTime() / IsInState() | `AnimatorComponent.hpp` |
| Script API（SetFloat/SetInt/SetBool/SetTrigger/IsInState） | `ScriptAnimatorProxy.hpp` |
| クリップ名 Fuzzy 解決（完全一致→大文字小文字無視部分一致） | `AnimatorSystem.cpp::FindClipByName` |
| FBX ノード名正規化（namespace / $AssimpFbx$ / パス区切り除去） | `AnimatorSystem.cpp::FindTrack` |
| ボーン階層の自動生成・再利用 | `EnsureBoneObject`, `EnsureBoneHierarchy` |
| FK ワールド変換の伝播 | `AnimatorSystem.cpp::PropagateBoneTransforms` |
| スキニング行列の再構築と GPU 転送 | `RebuildSkinningFromBoneTransforms` |
| アセット非同期ロード対応（FlushGeneration 再試行） | `AnimatorSystem.cpp` の needsRetry ロジック |

### IKSolverComponent / IKSystem

| 機能 | 実装箇所 |
|---|---|
| 解析的 2-Bone IK（cosine law） | `IKSystem.cpp` メインソルブ |
| Pole Vector による膝方向制御 | `IKSystem.cpp`（bendDir の Pole 分岐） |
| IK Weight ブレンド（FK ↔ IK Slerp/Lerp） | `chain.weight * stateIKWeight` |
| Soft IK（Blender 準拠の指数減衰） | `IKSystem.cpp::ApplySoftIK` |
| maxExtension によるゴール距離ハードクランプ | `IKSystem.cpp`（D_max 計算） |
| 地面スナップ レイキャスト（脚長比率ベースの動的レイ高さ） | `IKSystem.cpp::QueryGroundHit` |
| スイング相判定（FK 足が地面より高ければスナップ無効） | `IKSystem.cpp`（hasGroundHit 条件） |
| 足首傾き補正（地面法線 FromToRotation） | `IKSystem.cpp`（rotC_final の groundAlign） |
| 骨盤高さ補正 pre-pass（両脚の平均オフセット → hip 先行適用） | `IKSystem.cpp`（Hip pre-pass ブロック） |
| GUID + 名前フォールバックによるターゲット解決設計 | `IKSolverComponent.hpp`（targetGuid/targetName フィールド） |

---

## 不足機能・バグ

### 重要度：高

#### 1. `AnimatorComponent::loop` の宣言が存在しないバグ

`Reflect()` 内で `r.Field("loop", loop)` を呼んでいるが、構造体本体に `bool loop` の宣言がない。
コンパイルエラーまたは未定義動作につながる。

**修正方針：** `AnimatorComponent` 構造体に `bool loop = true;` を追加する。

---

#### 2. `EvaluateBlendedNodeRecursive` の呼び出しに `weight` 引数が渡されていないバグ

`RunStateMachineAnimatorPath` からの呼び出しで `w`（blendWeight）が渡されておらず、
`skeleton.rootNodeIndex`（整数）が float の weight 位置にキャストされる。
結果として weight ≈ 0 で常に現ステートのポーズが使われ、**クロスフェードが機能しない**。

```cpp
// 問題箇所
EvaluateBlendedNodeRecursive(skeleton,
    *curClip, ticksA,
    *nextClip, ticksB,
    // ← w が抜けている
    skeleton.rootNodeIndex,  // weight 位置に来てしまっている
    ...);
```

**修正方針：** 呼び出しに `w,` を第 6 引数として挿入し以降をシフトする。

---

#### 3. IK ターゲット / Pole の GUID→EntityID 解決が未実装

`IKSolverComponent.hpp` に `targetGuid`, `targetName`, `poleGuid`, `poleName` が設計されているが、
`IKSystem.cpp` では `chain.targetEntity`（EntityID）を直接参照するだけで GUID / 名前からの解決コードがない。
**シーン再ロード後に EntityID が変わるため、ターゲットが常に INVALID になる**。

**修正方針：** IKSystem の冒頭またはシーンロード後フックで GUID → 名前 → EntityID の解決パスを実装する。

---

### 重要度：中

#### 4. `IKSolverComponent::Reflect()` が `chains` を反映していない

```cpp
void Reflect(IReflector& r) {
    r.Field("enabled", enabled);
    r.Field("hipBoneName", hipBoneName);
    // chains は Reflect されていない
}
```

IK チェーン設定がシーンファイルに保存されない。
Inspector 側でカスタム描画する設計だが、シリアライズ側が未実装の可能性がある。

**修正方針：** SceneSerializer で `chains` の読み書きが実装されているか確認し、未実装なら追加する。

---

#### 5. `AnimationState` に `bool loop` フィールドがない

ステートマシンパスで `curSt->loop` を参照しているが、`AnimationState` 構造体に `bool loop` が宣言されていない。
ステートごとのループ ON/OFF を制御できない。

**修正方針：** `AnimationState` に `bool loop = true;` を追加する。

---

#### 6. `ScriptAnimatorProxy` に Get 系 API がない

`SetFloat` / `SetInt` / `SetBool` / `SetTrigger` はあるが、`GetFloat` / `GetInt` / `GetBool` / `GetNormalizedTime` がなく、スクリプトからパラメーターを読み出せない。

**修正方針：** `ScriptAnimatorProxy` に Get 系メソッドを追加する。

---

#### 7. 骨盤高さ補正の上限が `0.4f` にハードコードされている

```cpp
const float maxHipMove = avgLegLen * 0.4f;  // 定数
```

モデルやゲームプレイごとに調整できない。

**修正方針：** `IKSolverComponent` に `float hipMaxOffsetRatio = 0.4f;` を追加してパラメーター化する。

---

### 重要度：低

| 項目 | 説明 | 修正方針 |
|---|---|---|
| **FindTrack の線形探索** | 毎フレームの `clip.tracks` 線形探索がボーン数増加で O(N²) になる | クリップ初回使用時に `nodeName → track インデックス` の `unordered_map` をキャッシュ |
| **パラメーター名不一致のサイレント失敗** | SetFloat/SetTrigger 等で名前が見つからなくてもログが出ない | 名前不一致時に `FBZZ_LOG_WARN` を追加 |
| **`IKChain::Reflect` がない** | 他コンポーネントとの一貫性がない | `IKChain` に最低限の `Reflect` を追加するか、カスタムシリアライズの意図をコメントで明示 |

---

## ファイル配置

```
Projects/Engine/
  include/Engine/Scene/
    Components/
      AnimatorComponent.hpp       ← ステートマシン定義・パラメーター
      BoneComponent.hpp           ← ボーン参照保持
      IKSolverComponent.hpp       ← IKChain 定義・地面スナップパラメーター
    ScriptProxy/
      ScriptAnimatorProxy.hpp     ← Script API（Get 系追加が必要）
    Systems/
      AnimatorSystem.hpp / .cpp   ← FK・クロスフェード・スキニング
      IKSystem.hpp / .cpp         ← 2-Bone IK・Soft IK・地面スナップ

Docs/System/Animation/
  overview.md                     ← このファイル
```

---

## 隣接ドキュメント

- [../Physics/overview.md](../Physics/overview.md) — IK の地面スナップが依存する Raycast
- [../MeshTrail/overview.md](../MeshTrail/overview.md) — AnimatorComponent の boneMatrices を参照する残像システム
