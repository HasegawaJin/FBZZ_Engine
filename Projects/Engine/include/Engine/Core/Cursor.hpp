/// @file    Cursor.hpp
/// @brief   ランタイムからマウスカーソル表示と拘束状態を制御する API。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once

#ifdef _WIN32
#   ifdef FBZZEngine_EXPORTS
#       define FBZZ_ENGINE_API __declspec(dllexport)
#   else
#       define FBZZ_ENGINE_API __declspec(dllimport)
#   endif
#else
#   define FBZZ_ENGINE_API
#endif

#include <string_view>

namespace fbzz::core {

// CursorLockMode — Unity の CursorLockMode 相当の入力拘束モード。
// WHY: ScriptProxy が Win32 API を直接触ると DLL 境界と Editor 復元処理が散らばるため、
//      Engine 側の小さな状態管理 API に集約する。
//
// WHY Confined と Locked を混同してはいけないか:
//   Locked は毎フレーム OS カーソルを中央へ戻し、移動量だけを Input へ流す。マウスの
//   «絶対座標» は消えるので、画面座標を読んで自前のカーソルを描くゲームは中央に貼り付く。
//   ポインターを見せる作りなら Confined を選ぶこと。拘束の目的が「画面から出さない」
//   だけなら Confined で足り、絶対座標も生き残る。
enum class CursorLockMode {
    None,
    Confined,
    Locked
};

FBZZ_ENGINE_API const char*    ToString(CursorLockMode mode);
FBZZ_ENGINE_API CursorLockMode CursorLockModeFromString(std::string_view text);

// カーソルの初期状態。ProjectSettings の [cursor] として保存され、Standalone は起動時、
// Editor は Play 開始時に適用する。実行中はスクリプトが自由に上書きしてよい。
struct CursorPolicy {
    CursorLockMode lockMode = CursorLockMode::None;
    bool           visible  = true;

    // OS カーソルを «プレイヤーから取り上げている» か。Editor が Escape を横取りして
    // よいかの判断に使う。
    [[nodiscard]] bool CapturesCursor() const
    {
        return lockMode != CursorLockMode::None || !visible;
    }
};

struct FBZZ_ENGINE_API Cursor {
    static void SetVisible(bool visible);
    static bool IsVisible();

    static void SetLockMode(CursorLockMode mode);
    static CursorLockMode GetLockMode();

    static void Apply(const CursorPolicy& policy);

    // 拘束範囲をスクリーン (デスクトップ) 座標の矩形で指定する。
    // 未指定ならウィンドウのクライアント領域全体を使う (Standalone はこちら)。
    // WHY: Editor では «見えているゲーム画面» は Game View パネルの矩形でしかない。
    //      ウィンドウ全体へ拘束すると、Confined の範囲も Locked の中心も、
    //      UISystem が座標を解く viewport とずれる。
    static void SetClipRegion(float x, float y, float width, float height);
    static void ClearClipRegion();

    // アプリのアクティブ状態。Window の WM_ACTIVATEAPP から呼ぶ。
    // WHY: 非アクティブ中に拘束を残すと Alt+Tab した先でカーソルが動かせない。
    //      要求状態は保持したまま OS 拘束だけ外し、復帰時に自動で張り直す。
    static void SetWindowActive(bool active);

    // ゲームの要求を «一時的に OS へ効かせない» 状態にする (Editor 専用)。
    // WHY 要求そのものを None へ落とさないか: Game View からフォーカスが外れた・Escape で
    //     解放した、といった «Editor の都合» でゲームの要求を消してしまうと、戻ってきたときに
    //     何を復元すればよいか分からなくなる。要求は SetLockMode/SetVisible が持ち続け、
    //     ここは «今それを OS へ流すか» だけを切り替える。
    static void SetSuppressed(bool suppressed);
    static bool IsSuppressed();

    // 現在の LockMode を OS カーソルへ再適用する。ウィンドウ移動・リサイズ後に呼ぶ。
    static void ApplyLock();

    // Editor / PlayMode 終了時の安全復元。カーソルを必ず表示し、拘束を解除する。
    static void ResetForEditor();

private:
    // force: 追跡している OS 状態を信用せず、必ず ShowCursor のカウンタを補正し直す。
    static void ApplyVisibility(bool force = false);

    // OS へ効かせてよいか。ウィンドウが非アクティブでも Editor が取り上げていても効かせない。
    [[nodiscard]] static bool Effective();

    static bool s_visible;
    static CursorLockMode s_lockMode;
    static bool s_osVisible;
    static bool s_windowActive;
    static bool s_suppressed;
    static bool s_clipActive;
    static bool s_hasClipRegion;
    static float s_clipX;
    static float s_clipY;
    static float s_clipWidth;
    static float s_clipHeight;
};

} // namespace fbzz::core
