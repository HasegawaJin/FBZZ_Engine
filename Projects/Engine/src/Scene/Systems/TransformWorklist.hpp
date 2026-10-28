/// @file    TransformWorklist.hpp
/// @brief   Build a callback-free parent-first transform worklist without enumerating child links.
/// @author  Hasegawa Jin
/// @date    2026-10-18
#pragma once
#include <Engine/Scene/Scene.hpp>
#include <vector>

namespace fbzz::scene::transform_detail {
/// @note Owns only scratch capacity between calls; object references never outlive a traversal.
class Worklist {
    struct Entry { GameObject* object; GameObject* parent; bool complete = false; };
    std::vector<Entry> m_entries;
    std::vector<std::size_t> m_slots, m_ancestors;
public:
    void Collect(Scene& scene)
    {
        m_entries.clear();
        m_slots.resize(scene.EntitySlotCount());
        for (GameObject& object : scene.GameObjects()) {
            m_slots[object.GetID().index] = m_entries.size();
            m_entries.push_back({&object, object.GetParent()});
        }
    }

    /// @pre No callbacks, hierarchy edits or nested traversal occur; the hierarchy is acyclic.
    template<class Update> void Propagate(Update&& update)
    {
        for (std::size_t index = 0; index < m_entries.size(); ++index) {
            if (m_entries[index].complete) continue;
            m_ancestors.clear();
            std::size_t current = index;
            for (;;) {
                const auto& entry = m_entries[current];
                if (entry.complete) break;
                m_ancestors.push_back(current);
                if (!entry.parent) break;
                current = m_slots[entry.parent->GetID().index];
            }
            for (auto it = m_ancestors.rbegin(); it != m_ancestors.rend(); ++it) {
                auto& entry = m_entries[*it];
                update(*entry.object, entry.parent ? &entry.parent->transform : nullptr);
                entry.complete = true;
            }
        }
        m_ancestors.clear();
        m_entries.clear();
    }
};
} /// @note namespace fbzz::scene::transform_detail
