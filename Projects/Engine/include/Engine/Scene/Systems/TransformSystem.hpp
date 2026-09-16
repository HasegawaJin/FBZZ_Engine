/// @file    TransformSystem.hpp
/// @brief   親子階層からワールド Transform を再計算する System 群。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// 各 Phase の親子関係を維持するため、必要なタイミングで実行する。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"
#include "Math/Quaternion.hpp"
#include "Math/Vector3.hpp"

namespace fbzz::scene {

// 共通実装を持つ基底クラス（直接登録しない）
class TransformSystem : public ISystem {
public:
    ComponentAccess GetAccess() const override;
    void Update(SystemContext& ctx) override;
};

// ──────────────────────────────────────────────────────────────────────────────
// Phase ごとのタグサブクラス（メタデータだけ異なる、実装は TransformSystem から継承）
// ──────────────────────────────────────────────────────────────────────────────

// Editor 停止中のみ動作。NavMeshBake が Transform を読む前に行列を更新する。
// WHY: Sim モードでは Physics 書き戻し後の Transform を翌フレーム先頭の NavMesh が
//      使うため Transform は PreScript より後に走る。Editor モードは Physics がないため
//      先頭で明示的に更新が必要。
class TransformEditorPreview final : public TransformSystem {
public:
    std::string_view Name()       const override { return "TransformSystem.EditorPreview"; }
    Phase            GetPhase()   const override { return Phase::PreScript; }
    RunMode          GetRunMode() const override { return RunMode::EditorOnly; }
};

class TransformPrePhysics final : public TransformSystem {
public:
    std::string_view Name()       const override { return "TransformSystem.PrePhysics"; }
    Phase            GetPhase()   const override { return Phase::PrePhysics; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
};

class TransformPostPhysics final : public TransformSystem {
public:
    std::string_view Name()       const override { return "TransformSystem.PostPhysics"; }
    Phase            GetPhase()   const override { return Phase::PostPhysics; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
};

class TransformLateUpdate final : public TransformSystem {
public:
    std::string_view Name()       const override { return "TransformSystem.LateUpdate"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
};

// ──────────────────────────────────────────────────────────────────────────────
// ユーティリティ関数
// ──────────────────────────────────────────────────────────────────────────────

class Scene;
class GameObject;

/// シーン内の全オブジェクトのワールド Transform を即時再計算する。
/// スケジューラを経由せず直接 BFS を走らせるため、シーンロード直後や Stop 復元後など
/// System 実行前にワールド値が必要な場合に使う。
void FlushWorldTransforms(Scene& scene);

/// 確定したワールド姿勢を GameObject へ書き、親から逆算した local も同時に更新する。
/// WHY local も要るか: world フィールドは local から再計算される派生値でしかない。
///   この後 FlushWorldTransforms が走った瞬間に
///   local から上書きされるため、local を直さない書き込みは 1 フレームも保たない。
void SetWorldPose(GameObject& go,
                  const math::Vector3& worldPosition,
                  const math::Quaternion& worldRotation,
                  const math::Vector3& worldScale);

} // namespace fbzz::scene
