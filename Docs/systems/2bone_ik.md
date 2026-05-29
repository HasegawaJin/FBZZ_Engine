# 2-Bone IK システム設計

## 目的

スケルトンアニメーションに対して解析的 2-Bone IK を後処理として適用する。
主なユースケースは腕（肩→肘→手首）と脚（腿→膝→足首）のリーチ動作。

AnimatorSystem が FK ポーズを確定した後、IKSystem がそのポーズを補正する形で組み込む。

---

## アルゴリズム概要

### 入力

| 変数 | 説明 |
|------|------|
| `pA` | ルートジョイント（肩/腿）のワールド位置 |
| `pB` | 中間ジョイント（肘/膝）のワールド位置 |
| `pC` | 先端ジョイント（手首/足首）のワールド位置 |
| `pT` | IK ターゲットのワールド位置 |
| `pP` | ポールベクターのワールド位置（省略可） |
| `LA` | ルート→中間の骨長 `|pB - pA|` |
| `LB` | 中間→先端の骨長 `|pC - pB|` |

### ステップ 1 — 到達距離のクランプ

```
D     = |pT - pA|
D_max = LA + LB - ε    // 完全伸展
D_min = |LA - LB| + ε  // 完全折り畳み
D     = clamp(D, D_min, D_max)
```

### ステップ 2 — 余弦定理で中間ジョイントの角度を求める

```
cos_θA = (LA² + D² - LB²) / (2 · LA · D)
θA     = acos(clamp(cos_θA, -1, 1))
```

`θA` は「pA から pB への方向」と「pA から pT への方向」の成す角。

### ステップ 3 — IK 平面と折り曲げ方向の決定

**ポールベクターなし**
```
axis_AT   = normalize(pT - pA)
bend_raw  = pB - pA - dot(pB - pA, axis_AT) · axis_AT  // axis_AT に垂直な成分
bend_dir  = normalize(bend_raw)
// |bend_raw| < ε なら axis_AT に直交する任意軸を選ぶ
```

**ポールベクターあり**
```
axis_AT   = normalize(pT - pA)
pole_raw  = pP - pA - dot(pP - pA, axis_AT) · axis_AT
bend_dir  = normalize(pole_raw)
```

ポールベクターにより「肘/膝がどちら方向に出るか」を制御できる。

### ステップ 4 — 新しい中間ジョイント位置

```
pB_new = pA + LA · (cos(θA) · axis_AT + sin(θA) · bend_dir)
```

### ステップ 5 — デルタ回転の計算

```cpp
// ルートジョイント
Vector3 dir_old_A = normalize(pB - pA);
Vector3 dir_new_A = normalize(pB_new - pA);
Quaternion rot_delta_A = Quaternion::FromToRotation(dir_old_A, dir_new_A);
Quaternion rot_new_A = rot_delta_A * rot_current_A;

// 中間ジョイント
Vector3 dir_old_B = normalize(pC - pB);
Vector3 dir_new_B = normalize(pT - pB_new);
Quaternion rot_delta_B = Quaternion::FromToRotation(dir_old_B, dir_new_B);
Quaternion rot_new_B = rot_delta_B * rot_current_B;
```

`FromToRotation` は `Vector3::Cross` と `Quaternion::FromAxisAngle` で実装する（既存 Math モジュールで完結）。

### ステップ 6 — nodeGlobalTransforms の更新

```cpp
// ルートの globalTransform を新回転で上書き
nodeGlobalTransforms[nodeA] = parentGlobal * Matrix4::TRS(localPos_A, rot_new_A, localScale_A);
// 中間は pA の子なので連鎖更新
nodeGlobalTransforms[nodeB] = nodeGlobalTransforms[nodeA] * Matrix4::TRS(localPos_B, rot_new_B, localScale_B);
// 先端も連鎖
nodeGlobalTransforms[nodeC] = nodeGlobalTransforms[nodeB] * Matrix4::TRS(localPos_C, localRot_C, localScale_C);
```

### ステップ 7 — ブレンド (IK weight)

```cpp
// 位置ベースのブレンドで最終的な nodeGlobalTransforms を補間する場合
pB_final = Vector3::Lerp(pB_fk, pB_new, weight);
// 回転ベースのブレンド
rot_final_A = Quaternion::Slerp(rot_fk_A, rot_new_A, weight);
```

weight = 0 で完全 FK、weight = 1 で完全 IK。

### ステップ 8 — boneMatrices の再計算

```cpp
for (int i = 0; i < skeleton.bones.size(); ++i) {
    const auto& bone = skeleton.bones[i];
    boneMatrices[i] = skeleton.rootInverseTransform
                    * nodeGlobalTransforms[bone.nodeIndex]
                    * bone.offsetMatrix;
}
```

IKSystem が触るのは連鎖内の 3 ノードだけなので、全ノードではなく差分のみ再計算すれば十分。

---

## コンポーネント設計

### IKChain

```cpp
struct IKChain {
    std::string rootBoneName;   // "UpperArm_L"
    std::string midBoneName;    // "ForeArm_L"
    std::string tipBoneName;    // "Hand_L"

    EntityID    targetEntity;   // IK ターゲットの GameObject
    EntityID    poleEntity;     // ポールベクター GameObject（省略時は EntityID{} ）

    float       weight      = 1.0f;   // 0〜1
    bool        enabled     = true;
};
```

### IKSolverComponent

```cpp
struct IKSolverComponent {
    std::vector<IKChain> chains;
};
```

1 つの SkinnedMeshRenderer に複数チェーン（例: 左腕 + 右腕 + 左脚 + 右脚）を持てる。

**ComponentRegistry.hpp** の `ComponentList` に追加する：

```cpp
using ComponentList = std::tuple<
    // ... 既存 ...
    IKSolverComponent,
>;
```

---

## System 設計

### シグネチャ

```cpp
// IKSystem.hpp
void IKSystem(Scene& scene, float dt);
```

### 処理フロー

```
AnimatorSystem (FK ポーズ確定 + nodeGlobalTransforms 更新)
       ↓
IKSystem       (nodeGlobalTransforms + boneMatrices を IK で補正)
       ↓
SkinnedMeshRenderSystem (boneMatrices を GPU へ転送)
```

### 実装スケッチ

```cpp
void IKSystem(Scene& scene, float dt) {
    for (auto& go : scene.GameObjects()) {
        auto* ik       = go.GetComponent<IKSolverComponent>();
        auto* animator = go.GetComponent<AnimatorComponent>();
        auto* smr      = go.GetComponent<SkinnedMeshRenderer>();
        if (!ik || !animator || !smr) continue;

        const auto& skeleton = smr->model->skeleton;

        for (auto& chain : ik->chains) {
            if (!chain.enabled || chain.weight <= 0.0f) continue;

            // ノードインデックスの解決
            auto itA = skeleton.nodeMap.find(chain.rootBoneName);
            auto itB = skeleton.nodeMap.find(chain.midBoneName);
            auto itC = skeleton.nodeMap.find(chain.tipBoneName);
            assert(itA != skeleton.nodeMap.end());
            assert(itB != skeleton.nodeMap.end());
            assert(itC != skeleton.nodeMap.end());
            int nodeA = itA->second, nodeB = itB->second, nodeC = itC->second;

            // ターゲット位置の取得
            auto* targetGO = scene.FindGameObjectByID(chain.targetEntity);
            if (!targetGO) continue;
            Vector3 pT = targetGO->GetComponent<Transform>()->position;

            // FK ポーズから現在位置を抽出
            Vector3 pA = ExtractPosition(animator->nodeGlobalTransforms[nodeA]);
            Vector3 pB = ExtractPosition(animator->nodeGlobalTransforms[nodeB]);
            Vector3 pC = ExtractPosition(animator->nodeGlobalTransforms[nodeC]);

            // ポールベクター
            Vector3 pP = {};
            bool hasPole = chain.poleEntity.IsValid();
            if (hasPole) {
                auto* poleGO = scene.FindGameObjectByID(chain.poleEntity);
                if (poleGO) pP = poleGO->GetComponent<Transform>()->position;
                else hasPole = false;
            }

            // IK ソルブ
            SolveTwoBoneIK(skeleton, animator->nodeGlobalTransforms,
                           nodeA, nodeB, nodeC, pA, pB, pC, pT,
                           pP, hasPole, chain.weight);

            // boneMatrices の差分再計算 (3 ノード分)
            RecalcBoneMatrices(skeleton, animator->nodeGlobalTransforms,
                               animator->boneMatrices,
                               { nodeA, nodeB, nodeC });
        }
    }
}
```

---

## ヘルパー関数

```cpp
// 2 ベクタ間の最短回転クォータニオン
Quaternion FromToRotation(const Vector3& from, const Vector3& to);

// Matrix4 から位置を取り出す
Vector3 ExtractPosition(const Matrix4& m);

// Matrix4 から回転クォータニオンを取り出す (スケール除去後に抽出)
Quaternion ExtractRotation(const Matrix4& m);

// 指定ノードの localRotation を上書きして globalTransform を再計算
void SetLocalRotationAndPropagate(
    const Skeleton& skeleton,
    std::vector<Matrix4>& nodeGlobalTransforms,
    int nodeIndex,
    const Quaternion& newLocalRot);
```

`FromToRotation` の実装：

```cpp
Quaternion FromToRotation(const Vector3& from, const Vector3& to) {
    Vector3 f = from.Normalized();
    Vector3 t = to.Normalized();
    float d = Vector3::Dot(f, t);
    if (d >= 1.0f - 1e-6f) return Quaternion::Identity();
    if (d <= -1.0f + 1e-6f) {
        // 180° 回転: f に直交する任意軸
        Vector3 axis = Vector3::Cross(Vector3::RIGHT, f);
        if (axis.Length() < 1e-6f) axis = Vector3::Cross(Vector3::UP, f);
        return Quaternion::FromAxisAngle(axis.Normalized(), math::PI);
    }
    Vector3 axis = Vector3::Cross(f, t);
    float   w    = 1.0f + d;
    return Quaternion{ axis.x, axis.y, axis.z, w }.Normalized();
}
```

---

## エラーハンドリング方針

[conventions/error_handling.md](../conventions/error_handling.md) に準拠する。

| 状況 | 対応 |
|------|------|
| `rootBoneName` が Skeleton に存在しない | `assert` (設定ミスはバグ扱い) |
| `targetEntity` が無効 | チェーンをスキップ（ログ出力なし） |
| ターゲットが到達範囲外 | クランプして継続（正常ケース） |
| `weight == 0` | 早期リターン（処理スキップ） |

---

## ファイル配置

```
Projects/Engine/include/Scene/Components/IKSolverComponent.hpp
Projects/Engine/include/Scene/Systems/IKSystem.hpp
Projects/Engine/src/Scene/Systems/IKSystem.cpp
```

---

## 使用例

```cpp
// キャラクターのセットアップ
auto& go = scene.CreateGameObject("Player");
go.AddComponent<SkinnedMeshRenderer>(...);
go.AddComponent<AnimatorComponent>(...);

auto& ik = go.AddComponent<IKSolverComponent>();
ik.chains.push_back(IKChain{
    .rootBoneName = "UpperArm_R",
    .midBoneName  = "ForeArm_R",
    .tipBoneName  = "Hand_R",
    .targetEntity = handTargetEntity,
    .poleEntity   = elbowPoleEntity,
    .weight       = 1.0f,
});

// メインループ
AnimatorSystem(scene, resources, dt);
IKSystem(scene, dt);              // FK の後に呼ぶ
SkinnedMeshRenderSystem(scene, renderer);
```

---

## 今後の拡張

| 優先度 | 内容 |
|--------|------|
| 高 | ターゲット姿勢のブレンドカーブ（weight アニメーション） |
| 中 | 先端ジョイントの回転保持（Hand の向きを維持） |
| 中 | チェーン長 3 以上の FABRIK への移行 |
| 低 | デバッグ描画（IK チェーンをワイヤー表示） |
