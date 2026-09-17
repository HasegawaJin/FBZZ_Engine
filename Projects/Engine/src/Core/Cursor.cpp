/// @file    Cursor.cpp
/// @brief   Win32 カーソル状態を Engine API として一元管理する。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#include <Engine/Core/Cursor.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

/// @note Engine に画像デコーダーが無く、カーソル画像のためだけに Renderer 経由の texture ロードへ
///       依存すると «core が上位を見る» ことになるため、ここで展開する。STB_IMAGE_STATIC で内部
///       リンケージに閉じ Editor 側の stb 実装と衝突させない (このファイルは CMake の unity build から除外)。
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

/// @brief 積まれている要求。
/// @note Cursor は dllexport されたクラスで、std::vector/std::string を静的メンバーに置くと
///       «DLL と利用者で同じアロケーターを共有していること» が前提になるため、要求の実体は
///       この翻訳単位に閉じ、境界を越えるのは値のコピー (CursorRequestInfo) だけにする。
struct Request {
    CursorRequestId id = kInvalidCursorRequest;
    int             priority = 0;
    CursorPolicy    policy{};
    std::uint32_t   owner = 0;
    std::string     label;
};

std::vector<Request> g_requests;
CursorRequestId      g_nextId = 1;

/// 種類ごとのカーソル画像。未設定 (nullptr) なら OS の既定矢印を使う。
HCURSOR g_shapeCursors[kCursorShapeCount]{};
bool    g_shapeApplied = false;   ///< ウィンドウクラスの矢印を自前の絵で置き換えているか

std::size_t ShapeIndex(CursorShape shape)
{
    const auto index = static_cast<std::size_t>(shape);
    return index < kCursorShapeCount ? index : 0;
}

/// @brief 実効値を決めている要求。優先度が最大で、同値なら «後から積んだ方»。
/// @note 同じ強さの主張が 2 つ並ぶのは «今開いたダイアログ» と «その裏に残っている画面» の
///       関係で、手前に出したものを見せるのが自然なため後勝ちにする。
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
///       クライアント領域外へ出るため、指定矩形をウィンドウで挟まない。矩形は既に «見えている絵» そのもの。
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
    /// @note 既に外していれば触らない。ClipCursor(nullptr) は «誰の拘束でも» 外す API なので、
    ///       自分が張っていないときに毎フレーム呼ぶと他アプリの拘束まで剥がしてしまう。
    if (!clipActive) return;
    ClipCursor(nullptr);
    clipActive = false;
}

/// PNG / TGA / BMP から HCURSOR を作る。失敗したら nullptr。
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
    ///       マスクは使われないが、渡さないと作成そのものが失敗する。
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
    return s_windowActive && !s_suppressed;
}

CursorPolicy Cursor::GetEffectivePolicy()
{
    if (const Request* winner = WinningRequest()) return winner->policy;
    return s_base;
}

CursorPolicy Cursor::GetBasePolicy()
{
    return s_base;
}

void Cursor::Refresh()
{
    ApplyVisibility();
    ApplyLock();
    ApplyShape();
}

void Cursor::ApplyVisibility(bool force)
{
    /// @note 効かせない状況では隠したままにしない。ShowCursor はスレッド単位なので実害は薄いが、
    ///       復帰失敗でカーソルが消えたまま残る事故を構造的に潰しておく。
    const bool wanted = GetEffectivePolicy().visible || !Effective();
    if (!force && wanted == s_osVisible) return;
    s_osVisible = wanted;

    /// @note ShowCursor は内部カウンタ式の API なので目的の表示状態になるまで補正する。Editor と
    ///       GameView の両方がカーソルを触るため、1 回呼ぶだけでは実表示状態とずれることがある。
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
    s_base.lockMode = mode;
    Refresh();
}

CursorLockMode Cursor::GetLockMode()
{
    return GetEffectivePolicy().lockMode;
}

void Cursor::Apply(const CursorPolicy& policy)
{
    s_base = policy;
    Refresh();
}

CursorRequestId Cursor::Push(const CursorPolicy& policy, int priority,
                             std::uint32_t owner, const char* label)
{
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
    ///       Play を抜けた後もゲームの絵が残ると、パネル上でもゲームのカーソルが出続けるため、
    ///       効かせない間は矢印へ戻す。
    HCURSOR wanted = Effective() ? g_shapeCursors[ShapeIndex(s_shape)] : nullptr;

    /// @note 絵を一度も差し込んでいないなら触るものが無い。ここで抜けることで、
    ///       カーソル画像を使わないゲームとテストは Window にも触らずに済む。
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
    ///       切り替わって見えるよう、カーソルがこのウィンドウの上に居るなら即座に差し替える。
    POINT screen{};
    if (GetCursorPos(&screen)) {
        HWND under = WindowFromPoint(screen);
        if (under && GetAncestor(under, GA_ROOT) == hwnd) SetCursor(wanted);
    }
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
    Refresh();
}

void Cursor::SetSuppressed(bool suppressed)
{
    if (s_suppressed == suppressed) return;
    s_suppressed = suppressed;
    Refresh();
}

bool Cursor::IsSuppressed()
{
    return s_suppressed;
}

void Cursor::ApplyLock()
{
    const CursorLockMode lockMode = GetEffectivePolicy().lockMode;
    if (!Effective() || lockMode == CursorLockMode::None) {
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

    if (lockMode == CursorLockMode::Locked) {
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
    /// @note 安全復元なので、追跡している状態が実際とずれていても必ず表示へ戻す。
    ///       ここが «カーソルが消えたまま帰ってこない» の最後の砦。要求そのものを畳むので、
    ///       一時的な取り上げ (SetSuppressed) とは別物。Play の終了時にだけ呼ぶこと。
    s_suppressed = false;
    g_requests.clear();
    s_base = CursorPolicy{};
    s_shape = CursorShape::Default;
    ClearShapeImages();
    ClearClipRegion();
    ApplyLock();
    ApplyVisibility(true);
}

} // namespace fbzz::core
