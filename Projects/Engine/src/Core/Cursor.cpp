/// @file    Cursor.cpp
/// @brief   Win32 カーソル状態を Engine API として一元管理する。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#define NOMINMAX
#include <Engine/Core/Cursor.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Input/Input.hpp>

#include <Windows.h>

namespace fbzz::core {

bool Cursor::s_visible = true;
CursorLockMode Cursor::s_lockMode = CursorLockMode::None;

void Cursor::SetVisible(bool visible)
{
    s_visible = visible;

    // ShowCursor は内部カウンタ式の API なので、目的の表示状態になるまで補正する。
    // WHY: Editor と GameView の両方がカーソルを触る可能性があるため、1 回呼ぶだけでは
    //      実表示状態と Engine の要求状態がずれることがある。
    int count = ShowCursor(visible ? TRUE : FALSE);
    if (visible) {
        while (count < 0)
            count = ShowCursor(TRUE);
    } else {
        while (count >= 0)
            count = ShowCursor(FALSE);
    }
}

bool Cursor::IsVisible()
{
    return s_visible;
}

void Cursor::SetLockMode(CursorLockMode mode)
{
    s_lockMode = mode;
    ApplyLock();
}

CursorLockMode Cursor::GetLockMode()
{
    return s_lockMode;
}

void Cursor::ApplyLock()
{
    if (s_lockMode == CursorLockMode::None) {
        ClipCursor(nullptr);
        return;
    }

    HWND hwnd = Application::Get().GetWindow().GetHandle();
    if (!hwnd) {
        ClipCursor(nullptr);
        return;
    }

    RECT rect{};
    GetClientRect(hwnd, &rect);
    POINT topLeft{ rect.left, rect.top };
    POINT bottomRight{ rect.right, rect.bottom };
    ClientToScreen(hwnd, &topLeft);
    ClientToScreen(hwnd, &bottomRight);

    RECT clipRect{ topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
    ClipCursor(&clipRect);

    if (s_lockMode == CursorLockMode::Locked) {
        const int centerX = (clipRect.left + clipRect.right) / 2;
        const int centerY = (clipRect.top + clipRect.bottom) / 2;
        POINT cursorPos{};
        if (GetCursorPos(&cursorPos)) {
            input::Input::OverrideMouseDelta({
                static_cast<float>(cursorPos.x - centerX),
                static_cast<float>(cursorPos.y - centerY)
            });
        }
        SetCursorPos(centerX, centerY);
    }
}

void Cursor::ResetForEditor()
{
    SetLockMode(CursorLockMode::None);
    SetVisible(true);
}

} // namespace fbzz::core
