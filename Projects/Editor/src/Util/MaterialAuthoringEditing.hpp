/// @file    MaterialAuthoringEditing.hpp
/// @brief   Editor drafts resolve their live asset/slot again before publication and persistence.
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once
#include <Editor/EditorContext.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Core/Logger.hpp>
#include <optional>
#include <string>
#include <utility>

namespace fbzz::editor::material_edit_detail {
using CommitResult = asset::detail::MaterialAuthoringCommitResult;

[[nodiscard]] inline std::optional<asset::MaterialAsset::AuthoringEdit> BeginAssetEdit(
    asset::AssetHandle<asset::MaterialAsset> handle)
{
    if (auto* source = asset::AssetManager::Get<asset::MaterialAsset>(handle))
        return source->BeginAuthoringEdit();
    return std::nullopt;
}

/// @note Only a freshly resolved live owner is borrowed; caller notification/persistence follows the actual commit result.
[[nodiscard]] inline CommitResult FinishAssetEdit(asset::AssetHandle<asset::MaterialAsset> handle,
    asset::MaterialAsset::AuthoringEdit& edit)
{
    const auto result = [&] {
        if (auto* live = asset::AssetManager::Get<asset::MaterialAsset>(handle)) return edit.Commit(*live);
        edit.Cancel();
        return CommitResult::CONFLICT;
    }();
    if (result == CommitResult::CONFLICT)
        FBZZ_LOG_WARN("Material authoring edit conflict; refreshing the Inspector on the next draw");
    return result;
}

/// @note The immutable values own their lifetime through file I/O or caller callbacks; no raw stored asset crosses that boundary.
[[nodiscard]] inline bool SaveAssetSnapshot(const std::string& path, asset::AssetHandle<asset::MaterialAsset> handle)
{
    const auto snapshot = asset::ReadMaterialAuthoring(handle);
    return snapshot && asset::SaveMaterialAssetToFile(path, snapshot->Data());
}

/// @note Undo intentionally restores a complete owned value; the raw live owner ends its scope before notification or persistence callbacks.
[[nodiscard]] inline bool ReplaceAssetAuthoring(asset::AssetHandle<asset::MaterialAsset> handle,
    const asset::MaterialAssetAuthoring& values)
{
    auto* live = asset::AssetManager::Get<asset::MaterialAsset>(handle);
    if (!live) return false;
    live->ReplaceAuthoring(values);
    return true;
}

/// @note Scene lifetime, GUID, numeric slot and immutable owner identity prevent callback edits from targeting a replacement owner.
class SlotTarget final {
public:
    SlotTarget(EditorContext& context, const scene::GameObject& owner, size_t slot)
        : SlotTarget(context, context.activeScene,
            context.activeScene ? context.activeScene->GetRenderSceneGeneration() : 0, owner.instanceId, slot) {}
    SlotTarget(EditorContext& context, scene::Scene* scene, uint64_t generation, std::string ownerId, size_t slot)
        : m_context(context), m_scene(scene), m_sceneGeneration(generation), m_ownerId(std::move(ownerId)), m_slot(slot)
    {
        if (auto* live = ResolveWithoutIdentity()) m_owner = live->ReadAuthoring();
    }
    [[nodiscard]] scene::MaterialSlot* Resolve() const
    {
        auto* live = ResolveWithoutIdentity();
        const auto current = live ? live->ReadAuthoring() : scene::MaterialSlot::AuthoringSnapshotPtr{};
        return m_owner && current && m_owner->OwnerToken() == current->OwnerToken() ? live : nullptr;
    }
    [[nodiscard]] CommitResult Finish(scene::MaterialSlot::AuthoringEdit& edit) const
    {
        const auto result = [&] {
            if (auto* live = Resolve()) return edit.Commit(*live);
            edit.Cancel();
            return CommitResult::CONFLICT;
        }();
        if (result == CommitResult::CONFLICT)
            FBZZ_LOG_WARN("Material slot edit conflict; refreshing the Inspector on the next draw");
        return result;
    }
private:
    scene::MaterialSlot* ResolveWithoutIdentity() const
    {
        if (!m_scene || m_context.activeScene != m_scene
            || m_context.activeScene->GetRenderSceneGeneration() != m_sceneGeneration) return nullptr;
        auto* owner = m_context.activeScene->FindByGuid(m_ownerId);
        auto* materials = owner ? owner->GetComponent<scene::MaterialComponent>() : nullptr;
        return materials && m_slot < materials->SlotCount() ? &materials->RawSlotAt(m_slot) : nullptr;
    }
    EditorContext& m_context;
    scene::Scene* m_scene = nullptr;
    uint64_t m_sceneGeneration = 0;
    std::string m_ownerId;
    size_t m_slot = 0;
    scene::MaterialSlot::AuthoringSnapshotPtr m_owner;
};

/// @note Explicit assignment preserves legacy same-path refresh, while every native invalidation reacquires the current slot owner.
[[nodiscard]] inline CommitResult AssignSlotReference(const SlotTarget& target, const std::string& path)
{
    auto* live = target.Resolve();
    if (!live) return CommitResult::CONFLICT;
    auto edit = live->BeginAuthoringEdit();
    edit.Draft().materialPath = path;
    edit.Draft().materialAsset = {};
    const auto result = target.Finish(edit);
    if (result != CommitResult::CONFLICT)
        if (auto* current = target.Resolve()) current->InvalidateResolvedMaterialReference();
    return result;
}

/// @note An accepted same-path field still retires resolved/native caches; only actual authoring publication asks the caller to mark dirty.
[[nodiscard]] inline CommitResult FinishSlotPathEdit(const SlotTarget& target,
    scene::MaterialSlot::AuthoringEdit& edit, bool fieldAccepted)
{
    if (fieldAccepted) edit.Draft().materialAsset = {};
    const auto result = target.Finish(edit);
    if (result == CommitResult::CONFLICT || (result == CommitResult::UNCHANGED && !fieldAccepted)) return result;
    if (auto* current = target.Resolve()) current->InvalidateResolvedMaterialReference();
    if (auto* current = target.Resolve()) {
        auto referenceEdit = current->BeginAuthoringEdit();
        const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(referenceEdit.Draft().materialPath);
        referenceEdit.Draft().materialAsset = handle;
        const auto referenceResult = target.Finish(referenceEdit);
        if (result == CommitResult::UNCHANGED) return referenceResult;
    }
    return result;
}
} /// @note namespace fbzz::editor::material_edit_detail
