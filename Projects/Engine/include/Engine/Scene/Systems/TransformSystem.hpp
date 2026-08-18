// FBZZ Engine
// TransformSystem.hpp | fbzz::scene
// 親子階層からワールド Transform を再計算する System 群。
// 1 フレームに 4 回、別の Phase と RunMode で実行されるため、タグサブクラスで型 ID を分ける。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

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

// Editor 停止中のみ動作。FoliageBake/NavMeshBake が Transform を読む前に行列を更新する。
// WHY: Sim モードでは Physics 書き戻し後の Transform を翌フレーム先頭の Foliage/NavMesh が
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

// 物理確定直後に表示姿勢を作り、LateScript の追従カメラから参照できるようにする。
class TransformPresentationPostPhysics final : public ISystem {
public:
    std::string_view Name()       const override { return "TransformPresentation.PostPhysics"; }
    Phase            GetPhase()   const override { return Phase::PostPhysics; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

class TransformLateUpdate final : public TransformSystem {
public:
    std::string_view Name()       const override { return "TransformSystem.LateUpdate"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
};

// Animator / IK / Constraint / Camera の最終結果を表示階層へ反映してから Render へ渡す。
class TransformPresentationLateUpdate final : public ISystem {
public:
    std::string_view Name()       const override { return "TransformPresentation.LateUpdate"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

// ──────────────────────────────────────────────────────────────────────────────
// ユーティリティ関数
// ──────────────────────────────────────────────────────────────────────────────

class Scene;

/// シーン内の全オブジェクトのワールド Transform を即時再計算する。
/// スケジューラを経由せず直接 BFS を走らせるため、シーンロード直後や Stop 復元後など
/// System 実行前にワールド値が必要な場合に使う。
void FlushWorldTransforms(Scene& scene);

} // namespace fbzz::scene
