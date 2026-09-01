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
bool Cursor::s_osVisible = true;
bool Cursor::s_windowActive = true;
bool Cursor::s_suppressed = false;
bool Cursor::s_clipActive = false;
bool Cursor::s_hasClipRegion = false;
float Cursor::s_clipX = 0.0f;
float Cursor::s_clipY = 0.0f;
float Cursor::s_clipWidth = 0.0f;
float Cursor::s_clipHeight = 0.0f;

namespace {

// 拘束矩形をスクリーン座標で解く。false ならウィンドウが無く、拘束できない。
//
// WHY 指定矩形をウィンドウで挟まないか: Editor の Game View は Dock から引き剥がすと
//     独立した OS ウィンドウになり、メインウィンドウのクライアント領域の外へ出る。
//     挟むと «ゲーム画面が居ない矩形» へ潰れてしまう。矩形は既に «見えている絵» そのもの。
bool ResolveClipRect(bool hasRegion, float rx, float ry, float rw, float rh, RECT& outRect)
{
    if (hasRegion && rw >= 1.0f && rh >= 1.0f) {
        outRect = { static_cast<LONG>(rx),
                    static_cast<LONG>(ry),
                    static_cast<LONG>(rx + rw),
                    static_cast<LONG>(ry + rh) };
        return true;
    }

    HWND hwnd = Application::Get().GetWindow().GetHandle();
    if (!hwnd) return false;

    RECT client{};
    GetClientRect(hwnd, &client);
    POINT topLeft{ client.left, client.top };
    POINT bottomRight{ client.right, client.bottom };
    ClientToScreen(hwnd, &topLeft);
    ClientToScreen(hwnd, &bottomRight);
    if (bottomRight.x <= topLeft.x || bottomRight.y <= topLeft.y) return false;

    outRect = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
    return true;
}

void ReleaseClip(bool& clipActive)
{
    // 既に外していれば触らない。ClipCursor(nullptr) は «誰の拘束でも» 外す API なので、
    // 自分が張っていないときに毎フレーム呼ぶと他アプリの拘束まで剥がしてしまう。
    if (!clipActive) return;
    ClipCursor(nullptr);
    clipActive = false;
}

} // namespace

const char* ToString(CursorLockMode mode)
{
    switch (mode) {
    case CursorLockMode::Confined: return "Confined";
    case CursorLockMode::Locked:   return "Locked";
    default:                       return "None";
    }
}

CursorLockMode CursorLockModeFromString(std::string_view text)
{
    if (text == "Confined" || text == "confined") return CursorLockMode::Confined;
    if (text == "Locked"   || text == "locked")   return CursorLockMode::Locked;
    return CursorLockMode::None;
}

void Cursor::SetVisible(bool visible)
{
    s_visible = visible;
    ApplyVisibility();
}

bool Cursor::Effective()
{
    return s_windowActive && !s_suppressed;
}

void Cursor::ApplyVisibility(bool force)
{
    // 効かせない状況では隠したままにしない。ShowCursor はスレッド単位なので実害は薄いが、
    // 復帰失敗でカーソルが消えたまま残る事故を構造的に潰しておく。
    const bool wanted = s_visible || !Effective();
    if (!force && wanted == s_osVisible) return;
    s_osVisible = wanted;

    // ShowCursor は内部カウンタ式の API なので、目的の表示状態になるまで補正する。
    // WHY: Editor と GameView の両方がカーソルを触る可能性があるため、1 回呼ぶだけでは
    //      実表示状態と Engine の要求状態がずれることがある。
    int count = ShowCursor(wanted ? TRUE : FALSE);
    if (wanted) {
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

void Cursor::Apply(const CursorPolicy& policy)
{
    SetVisible(policy.visible);
    SetLockMode(policy.lockMode);
}

void Cursor::SetClipRegion(float x, float y, float width, float height)
{
    s_hasClipRegion = true;
    s_clipX      = x;
    s_clipY      = y;
    s_clipWidth  = width;
    s_clipHeight = height;
}

void Cursor::ClearClipRegion()
{
    s_hasClipRegion = false;
}

void Cursor::SetWindowActive(bool active)
{
    if (s_windowActive == active) return;
    s_windowActive = active;
    ApplyVisibility();
    ApplyLock();
}

void Cursor::SetSuppressed(bool suppressed)
{
    if (s_suppressed == suppressed) return;
    s_suppressed = suppressed;
    ApplyVisibility();
    ApplyLock();
}

bool Cursor::IsSuppressed()
{
    return s_suppressed;
}

void Cursor::ApplyLock()
{
    if (!Effective() || s_lockMode == CursorLockMode::None) {
        ReleaseClip(s_clipActive);
        return;
    }

    RECT clipRect{};
    if (!ResolveClipRect(s_hasClipRegion, s_clipX, s_clipY, s_clipWidth, s_clipHeight, clipRect)) {
        ReleaseClip(s_clipActive);
        return;
    }

    ClipCursor(&clipRect);
    s_clipActive = true;

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
    // 安全復元なので、追跡している状態が実際とずれていても必ず表示へ戻す。
    // ここが «カーソルが消えたまま帰ってこない» の最後の砦。要求そのものを畳むので、
    // 一時的な取り上げ (SetSuppressed) とは別物。Play の終了時にだけ呼ぶこと。
    s_suppressed = false;
    SetLockMode(CursorLockMode::None);
    ClearClipRegion();
    s_visible = true;
    ApplyVisibility(true);
}

} // namespace fbzz::core
