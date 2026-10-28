/// @file    ControlPointEditing.hpp
/// @brief   共通配置点の候補移動とviewport操作を確定済みSceneから分離する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>
#include <memory>
#include <vector>

namespace fbzz::scene { class Scene; class GameObject; }

struct ImVec2;

namespace fbzz::editor {
struct EditorContext;
class OperatorRegistry;

[[nodiscard]] bool IsControlPointAuthoringAllowed(const EditorContext& ctx);
[[nodiscard]] bool HasTargetPointSelection(const EditorContext& ctx);
/// @note 非永続の編集guard。GUID、Entity/Component寿命と参照先の入力姿勢を含み、Bake署名には使わない。
[[nodiscard]] uint64_t ControlPointOwnerReferenceSignature(scene::Scene& scene, scene::EntityID owner);
void RegisterTargetPointMoveOperator(OperatorRegistry& registry);
/// @return 操作を実行したframeはtrue。呼び出し側は生成によるpointer移動を避けてInspector描画を終了する。
[[nodiscard]] bool DrawControlPointInspector(EditorContext& ctx, scene::GameObject& object);

struct ControlPointPreview {
    scene::EntityID entity{};
    math::Vector3 worldPosition{};
    math::Vector3 worldDelta{};
};

/// @note 候補はSceneを書き換えず、確定時だけ登録済みMutationへ渡す。Component pointerは保持しない。
/// @see Docs/design/control-point-authoring.md
class ControlPointMoveSession {
public:
    ControlPointMoveSession();
    ~ControlPointMoveSession();
    ControlPointMoveSession(ControlPointMoveSession&&) noexcept;
    ControlPointMoveSession& operator=(ControlPointMoveSession&&) noexcept;
    ControlPointMoveSession(const ControlPointMoveSession&) = delete;
    ControlPointMoveSession& operator=(const ControlPointMoveSession&) = delete;
    [[nodiscard]] bool Begin(EditorContext& ctx);
    [[nodiscard]] bool Validate(EditorContext& ctx) const;
    /// @note 開始姿勢に対するworld deltaを親の符号付きTRSへ戻し、絶対world位置の差分を作らない。
    [[nodiscard]] bool SetWorldDelta(EditorContext& ctx, const math::Vector3& delta);
    [[nodiscard]] bool Commit(EditorContext& ctx);
    void Cancel();
    [[nodiscard]] bool Active() const;
    [[nodiscard]] math::Vector3 Pivot() const;
    [[nodiscard]] const std::vector<ControlPointPreview>& Preview() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// @note 状態はviewport個体に所有させ、非表示のframe断絶や別viewportのImGuizmo IDを混同しない。
class ControlPointViewportTool {
public:
    ControlPointViewportTool();
    ~ControlPointViewportTool();
    ControlPointViewportTool(ControlPointViewportTool&&) noexcept;
    ControlPointViewportTool& operator=(ControlPointViewportTool&&) noexcept;
    ControlPointViewportTool(const ControlPointViewportTool&) = delete;
    ControlPointViewportTool& operator=(const ControlPointViewportTool&) = delete;
    /// @return 点選択またはハンドルがmouseを取得した場合true。通常GO操作は同frameに実行しない。
    [[nodiscard]] bool Update(EditorContext& ctx, const ImVec2& min, const ImVec2& size,
                              bool editable, bool blocked, bool hovered);
    void Cancel();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} /// @note namespace fbzz::editor
