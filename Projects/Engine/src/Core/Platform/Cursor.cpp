/// @file    Cursor.cpp
/// @brief   Win32 カーソル状態を Engine API として一元管理する。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#include <Engine/Core/Cursor.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/Input/Gamepad.hpp>
#include "CursorControlPolicy.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

/// @note Engine に画像デコーダーが無く、カーソル画像のためだけに Renderer 経由の texture ロードへ
/// @note 依存すると «core が上位を見る» ことになるため、ここで展開する。STB_IMAGE_STATIC で内部
/// @note リンケージに閉じ Editor 側の stb 実装と衝突させない (このファイルは CMake の unity build から除外)。
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>

namespace fbzz::core {

CursorPolicy Cursor::s_base{};
bool Cursor::s_osVisible = true;
bool Cursor::s_windowActive = true;
bool Cursor::s_suppressed = false;
bool Cursor::s_clipActive = false;
bool Cursor::s_hasClipRegion = false;
float Cursor::s_clipX = 0.0f;
float Cursor::s_clipY = 0.0f;
float Cursor::s_clipWidth = 0.0f;
float Cursor::s_clipHeight = 0.0f;
CursorShape Cursor::s_shape = CursorShape::Default;

namespace {

cursor_detail::CursorControlPolicy g_control;
CursorLockMode g_appliedLock = CursorLockMode::None;
bool g_targetAvailable = true;
bool g_recenterPending = true;
RECT g_appliedClip{};

/// @brief 積まれている要求。
/// @note Cursor は dllexport されたクラスで、std::vector/std::string を静的メンバーに置くと
/// @note «DLL と利用者で同じアロケーターを共有していること» が前提になるため、要求の実体は
/// @note この翻訳単位に閉じ、境界を越えるのは値のコピー (CursorRequestInfo) だけにする。
struct Request {
    CursorRequestId id = kInvalidCursorRequest;
    int             priority = 0;
    CursorPolicy    policy{};
    std::uint32_t   owner = 0;
    std::string     label;
};

std::vector<Request> g_requests;
CursorRequestId      g_nextId = 1;

/// @note 種類ごとのカーソル画像。未設定 (nullptr) なら OS の既定矢印を使う。
HCURSOR g_shapeCursors[kCursorShapeCount]{};
bool    g_shapeApplied = false;   ///< @note ウィンドウクラスの矢印を自前の絵で置き換えているか

std::size_t ShapeIndex(CursorShape shape)
{
    const auto index = static_cast<std::size_t>(shape);
    return index < kCursorShapeCount ? index : 0;
}

/// @brief 実効値を決めている要求。優先度が最大で、同値なら «後から積んだ方»。
/// @note 同じ強さの主張が 2 つ並ぶのは «今開いたダイアログ» と «その裏に残っている画面» の
/// @note 関係で、手前に出したものを見せるのが自然なため後勝ちにする。
const Request* WinningRequest()
{
    const Request* best = nullptr;
    for (const Request& r : g_requests)
        if (!best || r.priority >= best->priority) best = &r;
    return best;
}

/// @brief 拘束矩形をスクリーン座標で解く。
/// @return false ならウィンドウが無く、拘束できない。
/// @note Editor の Game View は Dock から引き剥がすと独立した OS ウィンドウになりメインウィンドウの
/// @note クライアント領域外へ出るため、指定矩形をウィンドウで挟まない。矩形は既に «見えている絵» そのもの。
bool ResolveClipRect(bool hasRegion, float rx, float ry, float rw, float rh, RECT& outRect)
{
    if (hasRegion) {
        if (!(rw >= 1.0f && rh >= 1.0f) || !std::isfinite(rx) || !std::isfinite(ry)
            || !std::isfinite(rw) || !std::isfinite(rh)) return false;
        const double left = rx, top = ry;
        const double right = left + rw, bottom = top + rh;
        if (left < std::numeric_limits<LONG>::min() || top < std::numeric_limits<LONG>::min()
            || right > std::numeric_limits<LONG>::max() || bottom > std::numeric_limits<LONG>::max()) return false;
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
    /// @note 既に外していれば触らない。ClipCursor(nullptr) は «誰の拘束でも» 外す API なので、
    /// @note 自分が張っていないときに毎フレーム呼ぶと他アプリの拘束まで剥がしてしまう。
    if (!clipActive) return;
    RECT current{};
    /// @note ClipCursor is shared with other applications; never remove a later owner's different rectangle.
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-clipcursor Cursor ownership responsibility.
    if (GetClipCursor(&current) && EqualRect(&current, &g_appliedClip)) ClipCursor(nullptr);
    clipActive = false;
    g_appliedLock = CursorLockMode::None;
}

/// @note PNG / TGA / BMP から HCURSOR を作る。失敗したら nullptr。
HCURSOR CreateCursorFromImage(const char* path, int hotspotX, int hotspotY)
{
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(path, &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return nullptr;
    }

    /// @note 32bit DIB は BGRA 並び。stb は RGBA で返すので R と B を入れ替える。
    BITMAPV5HEADER header{};
    header.bV5Size        = sizeof(BITMAPV5HEADER);
    header.bV5Width       = width;
    /// @note 負で «上から下» の並びになる
    header.bV5Height      = -height;
    header.bV5Planes      = 1;
    header.bV5BitCount    = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask     = 0x00FF0000;
    header.bV5GreenMask   = 0x0000FF00;
    header.bV5BlueMask    = 0x000000FF;
    header.bV5AlphaMask   = 0xFF000000;

    void* bits = nullptr;
    HDC screenDC = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screenDC, reinterpret_cast<BITMAPINFO*>(&header),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDC);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        stbi_image_free(pixels);
        return nullptr;
    }

    auto* dst = static_cast<std::uint8_t*>(bits);
    const std::size_t texels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < texels; ++i) {
        dst[i * 4 + 0] = pixels[i * 4 + 2];
        dst[i * 4 + 1] = pixels[i * 4 + 1];
        dst[i * 4 + 2] = pixels[i * 4 + 0];
        dst[i * 4 + 3] = pixels[i * 4 + 3];
    }
    stbi_image_free(pixels);

    /// @note ICONINFO はカラーとマスクの両方を要求する。アルファ付き 32bit では
    /// @note マスクは使われないが、渡さないと作成そのものが失敗する。
    HBITMAP mask = CreateBitmap(width, height, 1, 1, nullptr);
    if (!mask) {
        DeleteObject(color);
        return nullptr;
    }

    ICONINFO info{};
    /// @note FALSE = カーソル (ホットスポットが効く)
    info.fIcon    = FALSE;
    info.xHotspot = static_cast<DWORD>(std::clamp(hotspotX, 0, width  - 1));
    info.yHotspot = static_cast<DWORD>(std::clamp(hotspotY, 0, height - 1));
    info.hbmMask  = mask;
    info.hbmColor = color;

    HCURSOR cursor = static_cast<HCURSOR>(CreateIconIndirect(&info));
    DeleteObject(mask);
    DeleteObject(color);
    return cursor;
}

} /// @note namespace

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

const char* ToString(CursorShape shape)
{
    switch (shape) {
    case CursorShape::Clickable: return "Clickable";
    case CursorShape::Grab:      return "Grab";
    case CursorShape::Text:      return "Text";
    case CursorShape::Aim:       return "Aim";
    case CursorShape::Busy:      return "Busy";
    default:                     return "Default";
    }
}

CursorShape CursorShapeFromString(std::string_view text)
{
    for (std::size_t i = 0; i < kCursorShapeCount; ++i) {
        const auto shape = static_cast<CursorShape>(i);
        if (text == ToString(shape)) return shape;
    }
    return CursorShape::Default;
}

void Cursor::SetVisible(bool visible)
{
    s_base.visible = visible;
    Refresh();
}

bool Cursor::Effective()
{
    return g_control.OwnsGameInput() && s_windowActive && !s_suppressed;
}

CursorPolicy Cursor::GetRequestedPolicy()
{
    if (const Request* winner = WinningRequest()) return winner->policy;
    return s_base;
}

CursorPolicy Cursor::GetEffectivePolicy()
{
    return GetRequestedPolicy();
}

CursorState Cursor::GetState()
{
    auto state = g_control.Resolve(GetRequestedPolicy(), s_suppressed, g_targetAvailable);
    state.effective = {g_appliedLock, s_osVisible};
    return state;
}

bool Cursor::IsGameInputOwned() { return g_control.OwnsGameInput(); }
bool Cursor::CanReadGameInput() { return g_control.OwnsGameInput(); }

bool Cursor::IsPhysicalInputAllowed() { return g_control.IsPhysicalInputAllowed(); }

void Cursor::SetPhysicalInputAllowed(bool allowed)
{
    const bool wasOwned = g_control.OwnsGameInput();
    g_control.SetPhysicalInputAllowed(allowed);
    if (wasOwned && !allowed) {
        input::Input::CancelGameInput();
        input::InputActionMap::ResetRuntime();
        input::Gamepad::StopAllVibration();
    }
    g_recenterPending = true;
    Refresh();
}

bool Cursor::AcquireGameInput()
{
    if (g_control.OwnsGameInput()) return true;
    if (!g_control.Acquire()) return false;
    input::Input::ConsumeGameInput();
    input::InputActionMap::ResetRuntime();
    g_recenterPending = true;
    Refresh();
    return true;
}

void Cursor::ReleaseGameInput(CursorSuspendReason reason)
{
    const bool wasOwned = g_control.OwnsGameInput();
    g_control.Release(reason);
    if (wasOwned) {
        input::Input::CancelGameInput();
        input::InputActionMap::ResetRuntime();
        input::Gamepad::StopAllVibration();
    }
    g_recenterPending = true;
    Refresh();
}

const char* ToString(CursorInputOwner owner)
{
    return owner == CursorInputOwner::GAME ? "Game" : "Host";
}

const char* ToString(CursorSuspendReason reason)
{
    switch (reason) {
    case CursorSuspendReason::NONE: return "None";
    case CursorSuspendReason::NOT_PLAYING: return "Not playing";
    case CursorSuspendReason::RELEASED: return "Released";
    case CursorSuspendReason::WINDOW_INACTIVE: return "Window focus lost; click to reacquire";
    case CursorSuspendReason::GAME_VIEW_UNFOCUSED: return "Game View unfocused";
    case CursorSuspendReason::PAUSED: return "Paused";
    case CursorSuspendReason::HOST_OVERRIDE: return "Cursor override: Free";
    case CursorSuspendReason::TARGET_UNAVAILABLE: return "Game target unavailable";
    case CursorSuspendReason::WINDOW_MOVING: return "Window moving";
    default: return "Unknown";
    }
}

CursorPolicy Cursor::GetBasePolicy()
{
    return s_base;
}

void Cursor::Refresh()
{
    ApplyLock();
    ApplyVisibility();
    ApplyShape();
}

void Cursor::ApplyVisibility(bool force)
{
    /// @note 効かせない状況では隠したままにしない。ShowCursor はスレッド単位なので実害は薄いが、
    /// @note 復帰失敗でカーソルが消えたまま残る事故を構造的に潰しておく。
    const bool wanted = g_control.Resolve(GetRequestedPolicy(), s_suppressed, g_targetAvailable).effective.visible;
    if (!force && wanted == s_osVisible) return;
    s_osVisible = wanted;

    /// @note ShowCursor は内部カウンタ式の API なので目的の表示状態になるまで補正する。Editor と
    /// @note GameView の両方がカーソルを触るため、1 回呼ぶだけでは実表示状態とずれることがある。
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
    return GetEffectivePolicy().visible;
}

void Cursor::SetLockMode(CursorLockMode mode)
{
    if (s_base.lockMode != mode) g_recenterPending = true;
    s_base.lockMode = mode;
    Refresh();
}

CursorLockMode Cursor::GetLockMode()
{
    return GetEffectivePolicy().lockMode;
}

void Cursor::Apply(const CursorPolicy& policy)
{
    if (s_base.lockMode != policy.lockMode) g_recenterPending = true;
    s_base = policy;
    Refresh();
}

CursorRequestId Cursor::Push(const CursorPolicy& policy, int priority,
                             std::uint32_t owner, const char* label)
{
    assert(g_nextId != kInvalidCursorRequest && "Cursor request identity exhausted");
    if (g_nextId == kInvalidCursorRequest) std::abort();
    Request request{};
    request.id       = g_nextId++;
    request.priority = priority;
    request.policy   = policy;
    request.owner    = owner;
    request.label    = label ? label : "";
    g_requests.push_back(std::move(request));
    Refresh();
    return g_requests.back().id;
}

void Cursor::UpdateRequest(CursorRequestId id, const CursorPolicy& policy)
{
    if (id == kInvalidCursorRequest) return;
    for (Request& r : g_requests) {
        if (r.id != id) continue;
        if (r.policy.lockMode == policy.lockMode && r.policy.visible == policy.visible) return;
        r.policy = policy;
        Refresh();
        return;
    }
}

void Cursor::Release(CursorRequestId id)
{
    if (id == kInvalidCursorRequest) return;
    const auto it = std::find_if(g_requests.begin(), g_requests.end(),
                                 [id](const Request& r) { return r.id == id; });
    if (it == g_requests.end()) return;
    g_requests.erase(it);
    Refresh();
}

void Cursor::ReleaseByOwner(std::uint32_t owner)
{
    if (owner == 0) return;
    const auto removed = std::remove_if(g_requests.begin(), g_requests.end(),
                                        [owner](const Request& r) { return r.owner == owner; });
    if (removed == g_requests.end()) return;
    g_requests.erase(removed, g_requests.end());
    Refresh();
}

bool Cursor::IsRequestActive(CursorRequestId id)
{
    return id != kInvalidCursorRequest && std::any_of(g_requests.begin(), g_requests.end(),
        [id](const Request& request) { return request.id == id; });
}

void Cursor::ClearRequests()
{
    g_requests.clear();
    s_base = CursorPolicy{};
    s_shape = CursorShape::Default;
    Refresh();
}

std::size_t Cursor::GetRequestCount()
{
    return g_requests.size();
}

bool Cursor::GetRequest(std::size_t index, CursorRequestInfo& out)
{
    if (index >= g_requests.size()) return false;

    /// @note 優先度の高い順・同値なら後から積んだ順。実効値を決めている要求が必ず先頭に来る。
    std::vector<const Request*> sorted;
    sorted.reserve(g_requests.size());
    for (const Request& r : g_requests) sorted.push_back(&r);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Request* a, const Request* b) {
                         if (a->priority != b->priority) return a->priority > b->priority;
                         return a->id > b->id;
                     });

    const Request& r = *sorted[index];
    const Request* winner = WinningRequest();

    out          = CursorRequestInfo{};
    out.id       = r.id;
    out.priority = r.priority;
    out.policy   = r.policy;
    out.owner    = r.owner;
    out.active   = winner && winner->id == r.id;
    std::snprintf(out.label, sizeof(out.label), "%s", r.label.c_str());
    return true;
}

bool Cursor::SetShapeImage(CursorShape shape, const char* path, float hotspotX, float hotspotY)
{
    const std::size_t index = ShapeIndex(shape);
    HCURSOR created = (path && *path)
        ? CreateCursorFromImage(path, static_cast<int>(hotspotX), static_cast<int>(hotspotY))
        : nullptr;
    if (path && *path && !created) {
        FBZZ_LOG_WARN("Cursor: failed to load cursor image '%s' for shape %s.",
                      path, ToString(shape));
        return false;
    }

    if (g_shapeCursors[index]) DestroyIcon(static_cast<HICON>(g_shapeCursors[index]));
    g_shapeCursors[index] = created;
    ApplyShape();
    return created != nullptr;
}

void Cursor::ClearShapeImages()
{
    for (HCURSOR& cursor : g_shapeCursors) {
        if (!cursor) continue;
        DestroyIcon(static_cast<HICON>(cursor));
        cursor = nullptr;
    }
    ApplyShape();
}

bool Cursor::HasShapeImage(CursorShape shape)
{
    return g_shapeCursors[ShapeIndex(shape)] != nullptr;
}

void Cursor::SetShape(CursorShape shape)
{
    if (s_shape == shape) return;
    s_shape = shape;
    ApplyShape();
}

CursorShape Cursor::GetShape()
{
    return s_shape;
}

void Cursor::ApplyShape()
{
    /// @note 差し替え先はウィンドウクラスのカーソルで、Editor では «エディタの窓全体» が対象になる。
    /// @note Play を抜けた後もゲームの絵が残ると、パネル上でもゲームのカーソルが出続けるため、
    /// @note 効かせない間は矢印へ戻す。
    HCURSOR wanted = Effective() ? g_shapeCursors[ShapeIndex(s_shape)] : nullptr;

    /// @note 絵を一度も差し込んでいないなら触るものが無い。ここで抜けることで、
    /// @note カーソル画像を使わないゲームとテストは Window にも触らずに済む。
    if (!wanted && !g_shapeApplied) return;

    HWND hwnd = Application::Get().GetWindow().GetHandle();
    if (!hwnd) return;

    if (!wanted) {
        g_shapeApplied = false;
        wanted = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    } else {
        g_shapeApplied = true;
    }

    SetClassLongPtrW(hwnd, GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(wanted));

    /// @note クラスのカーソルは «次に WM_SETCURSOR が来たとき» に効く。マウスが止まっている間も
    /// @note 切り替わって見えるよう、カーソルがこのウィンドウの上に居るなら即座に差し替える。
    POINT screen{};
    if (GetCursorPos(&screen)) {
        HWND under = WindowFromPoint(screen);
        if (under && GetAncestor(under, GA_ROOT) == hwnd) SetCursor(wanted);
    }
}

void Cursor::SetClipRegion(float x, float y, float width, float height)
{
    if (!s_hasClipRegion || s_clipX != x || s_clipY != y || s_clipWidth != width || s_clipHeight != height)
        g_recenterPending = true;
    s_hasClipRegion = true;
    s_clipX      = x;
    s_clipY      = y;
    s_clipWidth  = width;
    s_clipHeight = height;
}

void Cursor::ClearClipRegion()
{
    if (s_hasClipRegion) g_recenterPending = true;
    s_hasClipRegion = false;
}

void Cursor::SetWindowActive(bool active)
{
    if (s_windowActive == active) return;
    s_windowActive = active;
    g_control.SetWindowActive(active);
    if (!active) {
        input::Input::ResetNativeState();
        input::InputActionMap::ResetRuntime();
        input::Gamepad::StopAllVibration();
    }
    g_recenterPending = true;
    Refresh();
}

void Cursor::SetSuppressed(bool suppressed)
{
    if (s_suppressed == suppressed) return;
    s_suppressed = suppressed;
    g_recenterPending = true;
    Refresh();
}

bool Cursor::IsSuppressed()
{
    return s_suppressed;
}

void Cursor::ApplyLock()
{
    const CursorLockMode lockMode = GetRequestedPolicy().lockMode;
    g_targetAvailable = true;
    if (!Effective() || lockMode == CursorLockMode::None) {
        ReleaseClip(s_clipActive);
        g_appliedLock = CursorLockMode::None;
        g_recenterPending = true;
        ApplyVisibility();
        return;
    }

    RECT clipRect{};
    if (!ResolveClipRect(s_hasClipRegion, s_clipX, s_clipY, s_clipWidth, s_clipHeight, clipRect)) {
        ReleaseClip(s_clipActive);
        g_targetAvailable = false;
        g_recenterPending = true;
        ApplyVisibility();
        return;
    }

    const bool modeChanged = g_appliedLock != lockMode || !EqualRect(&clipRect, &g_appliedClip);
    if (!ClipCursor(&clipRect)) {
        ReleaseClip(s_clipActive);
        g_targetAvailable = false;
        g_recenterPending = true;
        ApplyVisibility();
        return;
    }
    g_appliedClip = clipRect;
    s_clipActive = true;
    g_appliedLock = lockMode;

    if (lockMode == CursorLockMode::Locked) {
        const int centerX = (clipRect.left + clipRect.right) / 2;
        const int centerY = (clipRect.top + clipRect.bottom) / 2;
        POINT cursorPos{};
        if (!g_recenterPending && !modeChanged && GetCursorPos(&cursorPos)) {
            input::Input::OverrideMouseDelta({
                static_cast<float>(cursorPos.x - centerX),
                static_cast<float>(cursorPos.y - centerY)
            });
        }
        else input::Input::OverrideMouseDelta({});
        if (!SetCursorPos(centerX, centerY)) {
            ReleaseClip(s_clipActive);
            g_targetAvailable = false;
            g_recenterPending = true;
            ApplyVisibility();
            return;
        }
        g_recenterPending = false;
    }
    ApplyVisibility();
}

void Cursor::ResetForEditor()
{
    /// @note 安全復元なので、追跡している状態が実際とずれていても必ず表示へ戻す。
    /// @note ここが «カーソルが消えたまま帰ってこない» の最後の砦。要求そのものを畳むので、
    /// @note 一時的な取り上げ (SetSuppressed) とは別物。Play の終了時にだけ呼ぶこと。
    g_control.Release(CursorSuspendReason::NOT_PLAYING);
    input::Input::CancelGameInput();
    input::InputActionMap::ResetRuntime();
    input::Gamepad::StopAllVibration();
    g_recenterPending = true;
    s_suppressed = false;
    g_requests.clear();
    s_base = CursorPolicy{};
    s_shape = CursorShape::Default;
    ClearShapeImages();
    ClearClipRegion();
    ApplyLock();
    ApplyVisibility(true);
}

} /// @note namespace fbzz::core
