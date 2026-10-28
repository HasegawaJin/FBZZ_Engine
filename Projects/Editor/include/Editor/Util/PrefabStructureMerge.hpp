/// @file    PrefabStructureMerge.hpp
/// @brief   Reviewed three-way Prefab hierarchy updates with checked Undo and Redo.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <toml++/toml.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene { class Scene; }
namespace fbzz::editor {
struct EditorContext;
class ICommand;
class OperatorRegistry;

struct PrefabStructureChange {
    std::string sourceId;
    std::string action;
    std::string beforeParent;
    std::string afterParent;
};

struct PrefabStructureConflict {
    std::string sourceId;
    std::string path;
    std::string reason;
};

struct PrefabStructureMergeResult {
    toml::table document;
    std::vector<PrefabStructureChange> changes;
    std::vector<PrefabStructureConflict> conflicts;
    [[nodiscard]] bool CanApply() const { return conflicts.empty() && !document.empty(); }
};

/// @brief Merge source-ID keyed baseline, instance and current template documents without mutating a Scene.
/// @note Instance-only IDs must be disjoint from template IDs. All parents are IDs, with exactly one unchanged root.
/// @note Local property overrides win; delete/edit, divergent reparent, orphan, cycle and nested boundaries are conflicts.
/// @see Docs/design/prefab-structural-merge.md
[[nodiscard]] PrefabStructureMergeResult MergePrefabStructure(
    const toml::table& baseline, const toml::table& instance, const toml::table& incoming);

struct PrefabStructureMergePlan {
    std::string rootGuid;
    std::string assetReference;
    std::string diskPath;
    std::string projectRoot;
    std::string sourceText;
    std::string sceneBefore;
    toml::table instanceDocument;
    PrefabStructureMergeResult merge;
    std::unordered_map<std::string, std::string> preserveGuids;
    std::unordered_map<std::string, std::string> originalSourceIds;
};

/// @note Captures a passive snapshot; no save callback or asset write is performed. False still returns conflict diagnostics.
bool PreviewPrefabStructureMerge(scene::Scene& scene, scene::EntityID root,
                                 const std::string& projectRoot, PrefabStructureMergePlan& out);

/// @note Revalidates the exact Scene, source bytes and reviewed merge before using the existing safe Prefab replacement.
/// @note Low-level caller owns dirty state and Undo; editor users use MakePrefabStructureMergeCommand.
bool ApplyPrefabStructureMerge(scene::Scene& scene, const PrefabStructureMergePlan& plan,
                               std::vector<scene::EntityID>& outRoots, std::string& reason);

/// @brief Create an unexecuted checked command for UndoStack::Execute or an Operator result.
/// @note Existing Scene snapshots restore deleted objects and external references together. Stale history never overwrites edits.
[[nodiscard]] std::unique_ptr<ICommand> MakePrefabStructureMergeCommand(
    EditorContext& context, const PrefabStructureMergePlan& plan);

/// @return True when Apply replaced objects and the caller must stop drawing cached GameObject pointers this frame.
bool DrawPrefabStructureMergeReview(EditorContext& context, scene::EntityID root);

/// @brief Register query preview and explicit receipt-bound Apply through the existing operator registry.
void RegisterPrefabStructureMergeOperators(OperatorRegistry& registry);
} /// @note namespace fbzz::editor
