/// @file    SelectionPasses.hpp
/// @brief   Selection mask render pass declarations.
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Systems/RenderPasses/PassResources.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>
#include <span>
#include <unordered_set>

namespace fbzz::renderer { struct RenderSettings; }

namespace fbzz::scene {

struct RenderPassContext;
class GameObject;

/// @note A render pass reads one immutable selection list. Large selections share membership checks across all draws.
class RenderSelectionLookup {
public:
    explicit RenderSelectionLookup(std::span<const renderer::RenderSelectionID> selected) : m_selected(selected)
    {
        if (selected.size() <= 8) return;
        m_keys.reserve(selected.size());
        for (const auto& id : selected) m_keys.insert(Key(id.index, id.generation));
    }

    [[nodiscard]] bool Contains(EntityID id) const
    {
        if (!id.IsValid()) return false;
        if (!m_keys.empty()) return m_keys.contains(Key(id.index, id.generation));
        for (const auto& selected : m_selected)
            if (selected.index == id.index && selected.generation == id.generation) return true;
        return false;
    }

private:
    static uint64_t Key(uint32_t index, uint32_t generation) { return (uint64_t(generation) << 32) | index; }
    std::span<const renderer::RenderSelectionID> m_selected;
    std::unordered_set<uint64_t> m_keys;
};

/// @note PassResources を先頭で受ける理由: RT は «申告した名前» からしか引けない。ctx.handles を直接触れる限り、申告と束縛は別々に書き換えられてしまう。
FBZZ_ENGINE_API void ExecuteSelectionMaskPass(PassResources& res, RenderPassContext& ctx);
FBZZ_ENGINE_API void ExecuteSelectionOutlinePass(PassResources& res, RenderPassContext& ctx);

/// @note 選択輪郭を出す対象か。UI 要素の選択マスクも同じ判定で拾う。
[[nodiscard]] FBZZ_ENGINE_API bool IsSelectedForOutline(const GameObject& go,
                                        const renderer::RenderSettings& settings);

} /// @note namespace fbzz::scene
