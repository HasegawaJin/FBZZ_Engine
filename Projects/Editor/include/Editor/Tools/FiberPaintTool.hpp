/// @file    FiberPaintTool.hpp
/// @brief   表面の草ペイント、ストロークのプレビューと一括 Undo。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Engine/Asset/FiberPaintAsset.hpp>
#include <memory>
#include <string>
#include <vector>
struct ImVec2;
namespace fbzz::scene { class Scene; class GameObject; }
namespace fbzz::editor {
struct EditorContext;
struct OpResult;
struct FiberPaintEdit {
    std::string node, path;
    uint64_t lifetime = 0;
    std::shared_ptr<asset::FiberPaintAsset> before, after;
};
/// @note プレビューは未保存。確定した一括コマンドだけが AssetDirtyRegistry へ登録する。
struct FiberPaintTransaction {
    scene::Scene* owner = nullptr;
    uint64_t discardEpoch = 0;
    std::vector<FiberPaintEdit> edits;
};
struct FiberPaintTool {
    float radius = 2.0f, density = 1.0f, length = 1.0f, softness = 0.35f;
    int preset = 1, mode = 0;
    bool allRegistered = false, visibleOnly = true, dragging = false, erase = false, startEmpty = false;
    bool hasLastHit = false;
    math::Vector3 lastHit{};
    std::string lastNode;
    float lastMouseX = 0, lastMouseY = 0;
    int lastFrame = -1;
    size_t undoRevision = 0;
    uint64_t targetSignature = 0;
    FiberPaintTransaction transaction;
    std::vector<renderer::FiberPaintStroke> strokes;
    std::vector<std::string> targets;
    std::string diagnostic;
};
/// @note 選択対象は子孫を含む。allRegistered は有効な静的 Blade Fiber 全体を対象とする。
[[nodiscard]] std::vector<scene::GameObject*> FiberPaintTargets(EditorContext& ctx, bool allRegistered, bool requireFiber = true);
[[nodiscard]] bool PrepareFiberPaintEdits(EditorContext& ctx, const std::vector<renderer::FiberPaintStroke>& strokes,
    bool allRegistered, bool visibleOnly, std::vector<FiberPaintEdit>& out, std::string& reason);
/// @note preview は未保存。commit は既に適用済みの transaction を履歴へ渡す。
[[nodiscard]] bool ApplyFiberPaintPreview(EditorContext& ctx, const std::vector<FiberPaintEdit>& edits, std::string& reason);
void SynchronizeFiberPaintDrafts();
[[nodiscard]] scene::GameObject* FiberPaintRaycast(EditorContext& ctx, const math::Vector3& origin,
    const math::Vector3& direction, float& distance, uint32_t& triangle);
void CancelFiberPaintPreview(EditorContext& ctx, FiberPaintTransaction& transaction);
[[nodiscard]] OpResult CommitFiberPaintTransaction(EditorContext& ctx, const FiberPaintTransaction& transaction);
/// @return 同 frame の HUD が mouse を占有した場合 true。
bool DrawFiberPaintTool(EditorContext& ctx, FiberPaintTool& tool, const ImVec2& min,
    const ImVec2& size, bool hovered, bool editable);
} /// @note namespace fbzz::editor
