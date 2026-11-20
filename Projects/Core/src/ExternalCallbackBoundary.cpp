/// @file    ExternalCallbackBoundary.cpp
/// @brief   外部通知前の所有スレッド限定スコープを配送する。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#include <Core/ExternalCallbackBoundary.hpp>
#include <cassert>

namespace fbzz::core {
namespace {
thread_local ExternalCallbackScope* g_currentScope = nullptr;
}

ExternalCallbackScope::ExternalCallbackScope(Handler handler, void* context) noexcept
    : m_handler(handler), m_context(context), m_previous(g_currentScope)
{
    g_currentScope = this;
}

ExternalCallbackScope::~ExternalCallbackScope()
{
    assert(g_currentScope == this);
    g_currentScope = m_previous;
}

bool ExternalCallbackScope::NotifyOuterFirst() noexcept
{
    /// @note 外側が失敗しても、内側のハンドラーに状態を確定させる。
    const bool outerSucceeded = !m_previous || m_previous->NotifyOuterFirst();
    const bool currentSucceeded = !m_handler || m_handler(m_context);
    return outerSucceeded && currentSucceeded;
}

bool NotifyBeforeExternalCallback() noexcept
{
    return !g_currentScope || g_currentScope->NotifyOuterFirst();
}


}
