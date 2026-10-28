/// @file    InspectorEditSession.hpp
/// @brief   Inspector の入力開始・継続・終了を同じ規則で判定する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <cstdint>

namespace fbzz::editor {

enum class InspectorEditTransition : uint8_t { NONE, BEGIN, END };

/// @note UI スレッド専用。異なる対象を描画しても入力 ID が継続中なら元の操作を保持する。
class InspectorEditSession {
public:
    [[nodiscard]] bool Active() const { return m_active; }
    void Cancel() { m_active = false; }
    /// @note END の確定後は呼び出し側が Cancel する。保存値の寿命は呼び出し側が管理する。
    [[nodiscard]] InspectorEditTransition Observe(scene::EntityID entity,
                                                  uint32_t activeBefore,
                                                  uint32_t activeAfter);
private:
    scene::EntityID m_entity;
    uint32_t m_activeId = 0;
    bool m_active = false;
};

} /// @note namespace fbzz::editor
