/// @file    TransformSystem.hpp
/// @brief   親子階層からワールド Transform を再計算する System 群。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"
#include "Math/Quaternion.hpp"
#include "Math/Vector3.hpp"

namespace fbzz::scene {

/// @note Phase 別の派生クラスを登録する。
class TransformSystem : public ISystem {
public:
    ComponentAccess GetAccess() const override;
    void Update(SystemContext& ctx) override;
};

/// @note Editor 停止中は Physics の書き戻しが無いため、NavMeshBake より前に姿勢を確定する。
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

/// @name 姿勢の同期

class Scene;
class GameObject;

/// @brief シーン内の全オブジェクトのワールド姿勢をローカル値から即時再計算する。
void FlushWorldTransforms(Scene& scene);

/// @brief 祖先のローカル値を解決してから、対象と全子孫のワールド姿勢を即時再計算する。
/// @note 非アクティブな子孫も含む。祖先の別の子孫と他のルートは更新しない。
/// @see Docs/design/script-transform-contract.md
void FlushWorldTransforms(GameObject& root);

/// @brief ワールド姿勢を親から逆算したローカル値と対で書く。
/// @pre 親のワールド姿勢は呼び出し前に確定済みであること。
/// @note world は local の派生値。親のゼロスケール軸は local をゼロへ倒し、子孫の同期は呼び出し側で行う。
void SetWorldPose(GameObject& go,
                  const math::Vector3& worldPosition,
                  const math::Quaternion& worldRotation,
                  const math::Vector3& worldScale);

} /// @note namespace fbzz::scene
