/// @file    InspectorEditSession.cpp
/// @brief   Inspector の対象切替と入力 ID の遷移を共通化する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Editor/Util/InspectorEditSession.hpp>

namespace fbzz::editor {

InspectorEditTransition InspectorEditSession::Observe(scene::EntityID entity,
                                                       uint32_t activeBefore,
                                                       uint32_t activeAfter)
{
    if (!m_active && activeAfter != 0 && activeAfter != activeBefore) {
        m_entity = entity;
        m_activeId = activeAfter;
        m_active = true;
        return InspectorEditTransition::BEGIN;
    }
    if (m_active && m_entity != entity) {
        if (activeAfter != m_activeId) Cancel();
        return InspectorEditTransition::NONE;
    }
    if (!m_active || activeAfter == m_activeId) return InspectorEditTransition::NONE;
    return InspectorEditTransition::END;
}

} /// @note namespace fbzz::editor
