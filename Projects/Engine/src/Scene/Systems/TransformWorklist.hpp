/// @file    TransformWorklist.hpp
/// @brief   Build a callback-free parent-first transform worklist without enumerating child links.
/// @author  Hasegawa Jin
/// @date    2026-10-18
#pragma once
#include <Engine/Scene/Scene.hpp>
#include <vector>

namespace fbzz::scene::transform_detail {
/// @note 呼び出し間で作業領域の容量を再利用する。GameObject の非所有参照は走査終了時に消す。
class Worklist {
    struct Entry {
        GameObject* object;
        GameObject* parent;
        bool worldUpdated = false;
    };
    std::vector<Entry> m_entries;
    std::vector<std::size_t> m_entryIndexBySlot;
    std::vector<std::size_t> m_pendingAncestors;
public:
    void Collect(Scene& scene)
    {
        m_entries.clear();
        m_entryIndexBySlot.resize(scene.EntitySlotCount());
        for (GameObject& object : scene.GameObjects()) {
            m_entryIndexBySlot[object.GetID().index] = m_entries.size();
            m_entries.push_back({&object, object.GetParent()});
        }
    }

    /// @pre Collect 後に階層を編集せず、更新関数からコールバックやネストした走査を呼ばない。階層は非循環。
    /// @see Docs/design/script-transform-contract.md
    template<class Update> void Propagate(Update&& update)
    {
        for (std::size_t index = 0; index < m_entries.size(); ++index) {
            if (m_entries[index].worldUpdated) continue;
            m_pendingAncestors.clear();

            /// @note Scene の格納順に関わらず、親が確定する地点までさかのぼる。
            std::size_t ancestor = index;
            for (;;) {
                const auto& entry = m_entries[ancestor];
                if (entry.worldUpdated) break;
                m_pendingAncestors.push_back(ancestor);
                if (!entry.parent) break;
                ancestor = m_entryIndexBySlot[entry.parent->GetID().index];
            }

            /// @note 逆順に適用すると子は更新済みの親を読み、共有する祖先は一度だけ計算する。
            for (auto it = m_pendingAncestors.rbegin(); it != m_pendingAncestors.rend(); ++it) {
                auto& entry = m_entries[*it];
                update(*entry.object, entry.parent ? &entry.parent->transform : nullptr);
                entry.worldUpdated = true;
            }
        }
        m_pendingAncestors.clear();
        m_entries.clear();
    }
};
}
