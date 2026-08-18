# 物理姿勢の描画補間

## 目的

固定 60 Hz の物理姿勢を可変リフレッシュの描画へ直接渡すと、動的剛体は 1 fixed step 単位で移動する。
追従カメラが可変フレームで平滑化すると、対象とカメラが異なる時間軸を参照し、距離の伸縮や画面上のジッターになる。

本設計はゲームロジック用の確定姿勢と描画専用姿勢を分離し、すべての動的剛体を共通経路で補間する。

## 姿勢の所有権

- `physics::RigidBody` は現在の確定物理姿勢を所有する。
- `RigidBodyComponent` は直前と現在の確定物理姿勢を保持する。
- `Transform::worldPosition/worldRotation/worldScale` はゲームロジックと物理同期が使う確定姿勢である。
- `Transform::presentationWorldPosition/presentationWorldRotation/presentationWorldScale` は描画と追従カメラだけが使う。
- presentation 姿勢を Physics やゲームロジックへ書き戻してはならない。

## フレーム順序

1. `ScriptSystem` が入力を収集する。
2. `TransformPrePhysics` が初回の物理姿勢履歴をワールド姿勢で初期化する。
3. `FixedScriptSystem` が速度・力・剛体回転を `fixedDeltaTime` で更新する。
4. `PhysicsSystem` が前回姿勢を保存し、`World::Step` 後の現在姿勢を確定する。
5. `SystemScheduler` が `physicsAlpha = accumulator / fixedDt` を公開する。
6. `TransformPresentationPostPhysics` が LateScript 用の補間姿勢を作る。
7. 追従カメラが対象の presentation 姿勢を参照する。
8. Animator、IK、Constraint、Camera、Particle の更新後、`TransformPresentationLateUpdate` が最終表示階層を作る。
9. RenderPass は `GetPresentationWorldMatrix()` だけを使用する。

## 補間式

```cpp
presentationPosition = Vector3::Lerp(previousPosition, currentPosition, physicsAlpha);
presentationRotation = Quaternion::Slerp(previousRotation, currentRotation, physicsAlpha);
```

この方式は 1 fixed step 分の表示遅延と引き換えに、描画フレーム間の連続性を保証する。
物理判定、Raycast、AI、ゲームスクリプトは遅延しない確定姿勢を使う。

## テレポート

動的剛体の `Transform` が最後の確定物理姿勢から明示的に変化していた場合、`PhysicsSystem` はテレポートとして扱う。
前回・現在姿勢を同じ値へリセットし、移動元から移動先まで補間して尾を引くことを防ぐ。

動的剛体は通常 Physics を正とする。毎 fixed step の無条件な `Transform → RigidBody` 再送は、
`OnFixedUpdate` の操作や姿勢履歴を巻き戻すため行わない。

## 親子階層

動的剛体のルートは物理姿勢をワールド空間で補間する。
子 GameObject は親の presentation 姿勢と自分のローカル TRS から presentation ワールド姿勢を再計算する。
これにより SkinnedMesh、Bone Attachment、Trail も同じ補間時間軸へ揃う。

## 禁止事項

- RenderPass から `GetWorldMatrix()` を使わない。
- 追従カメラから動的対象の `worldPosition` を直接追わない。
- 動的剛体の速度・力・回転を可変 `deltaTime` で更新しない。
- presentation 姿勢を SceneSerializer へ保存しない。
- ジッター対策としてカメラ軸ごとの個別補正を追加しない。

