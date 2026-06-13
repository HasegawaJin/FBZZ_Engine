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
| クロスフェード遷移（TRS レベル加重合成） | `AnimatorSystem.cpp::EvaluateNBlendedNodeRecursive` |
| パラメーター型（Float / Int / Bool / Trigger） | `AnimatorComponent.hpp`、`CheckCondition` |
| 遷移条件評価（Greater/Less/Equal/NotEqual/True/False + AND 結合） | `AnimatorSystem.cpp::EvaluateTransition` |
| hasExitTime / exitTime | `AnimationTransition::hasExitTime` |
| Trigger の自動消費 | `AnimatorSystem.cpp::ConsumeTriggers` |
| ステート別再生速度 | `AnimationState::speed` |
| ステート別 IK Weight | `AnimationState::ikWeight` |
| GetNormalizedTime() / IsInState() | `AnimatorComponent.hpp` |
| Script API（Set/Get パラメーター、GetNormalizedTime、IsInState） | `ScriptAnimatorProxy.hpp` |
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
| 骨盤高さ補正 pre-pass（両脚の平均オフセット → hip 先行適用） | `IKSystem.cpp`（`hipMaxOffsetRatio` で上限調整） |
| GUID + 名前フォールバックによるターゲット解決設計 | `IKSolverComponent.hpp`（targetGuid/targetName フィールド） |

---

## 修正済みの不具合

以下は修正前の症状と対応内容を、回帰確認のために記録したもの。

### 重要度：高

#### 1. 【修正済み】`AnimatorComponent::loop` の宣言不足

**修正前：** `Reflect()` 内で `r.Field("loop", loop)` を呼んでいたが、
構造体本体に `bool loop` の宣言がなく、コンパイルエラーの原因になっていた。

**対応：** `AnimatorComponent` に `bool loop = true;` を追加済み。

---

#### 2. 【修正済み】旧`EvaluateBlendedNodeRecursive` の `weight` 引数不足

**修正前：** `RunStateMachineAnimatorPath` からの呼び出しで `w`（blendWeight）が渡されておらず、
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

**対応：** 呼び出しの第 6 引数に `w` を渡すよう修正済み。

---

#### 3. 【修正済み】IK ターゲット / Pole の GUID→EntityID 解決

**修正前：** `IKSolverComponent.hpp` に `targetGuid`, `targetName`, `poleGuid`, `poleName` が設計されていたが、
`IKSystem.cpp` では `chain.targetEntity`（EntityID）を直接参照するだけで GUID / 名前からの解決コードがない。
**シーン再ロード後に EntityID が変わるため、ターゲットが常に INVALID になる**。

**対応：** SceneSerializer のロード後 Pass で GUID → 名前 → EntityID の順に解決するよう修正済み。

---

### 重要度：中

#### 4. 【修正済み】`IKSolverComponent::chains` のシリアライズ確認

**修正前の確認対象：**
```cpp
void Reflect(IReflector& r) {
    r.Field("enabled", enabled);
    r.Field("hipBoneName", hipBoneName);
    // chains は Reflect されていない
}
```

`chains` は Reflection の対象外であるため、シリアライズ側が未実装の場合は
IK チェーン設定がシーンファイルに保存されない状態だった。

**対応：** `chains` は SceneSerializer で明示的に読み書きする。Inspector のカスタム描画設計は維持する。

---

#### 5. 【修正済み】`AnimationState::loop` の宣言不足

**修正前：** ステートマシンパスで `curSt->loop` を参照していたが、
`AnimationState` にフィールドがなく、ステートごとのループを制御できなかった。

**対応：** `AnimationState` に `bool loop = true;` を追加済み。

---

#### 6. 【修正済み】`ScriptAnimatorProxy` の Get 系 API

**修正前：** Set 系 API のみで、スクリプトからパラメーターや正規化再生時間を読み出せなかった。

**対応：** `GetFloat` / `GetInt` / `GetBool` / `GetNormalizedTime` / `GetCurrentState` を追加済み。

---

#### 7. 【修正済み】骨盤高さ補正上限のパラメーター化

**修正前：**
```cpp
const float maxHipMove = avgLegLen * 0.4f;  // 定数
```

モデルやゲームプレイごとに調整できない。

**対応：** `IKSolverComponent::hipMaxOffsetRatio` を追加し、シリアライズと Inspector 編集に対応済み。

## 改善候補

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

- [blend_tree.md](blend_tree.md) — BlendTree (1D / 2D) と AnyState 遷移の設計
- [../Physics/overview.md](../Physics/overview.md) — IK の地面スナップが依存する Raycast
- [../MeshTrail/overview.md](../MeshTrail/overview.md) — AnimatorComponent の boneMatrices を参照する残像システム
