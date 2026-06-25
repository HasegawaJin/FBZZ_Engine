// FBZZ Engine
// Cursor.hpp | fbzz::core
// ランタイムからマウスカーソル表示と拘束状態を制御する API
#pragma once

#ifdef _WIN32
#   ifdef fbzz_engine_EXPORTS
#       define FBZZ_ENGINE_API __declspec(dllexport)
#   else
#       define FBZZ_ENGINE_API __declspec(dllimport)
#   endif
#else
#   define FBZZ_ENGINE_API
#endif

namespace fbzz::core {

// CursorLockMode — Unity の CursorLockMode 相当の入力拘束モード。
// WHY: ScriptProxy が Win32 API を直接触ると DLL 境界と Editor 復元処理が散らばるため、
//      Engine 側の小さな状態管理 API に集約する。
enum class CursorLockMode {
    None,
    Confined,
    Locked
};

struct FBZZ_ENGINE_API Cursor {
    static void SetVisible(bool visible);
    static bool IsVisible();

    static void SetLockMode(CursorLockMode mode);
    static CursorLockMode GetLockMode();

    // 現在の LockMode を OS カーソルへ再適用する。ウィンドウ移動・リサイズ後に呼ぶ。
    static void ApplyLock();

    // Editor / PlayMode 終了時の安全復元。カーソルを必ず表示し、拘束を解除する。
    static void ResetForEditor();

private:
    static bool s_visible;
    static CursorLockMode s_lockMode;
};

} // namespace fbzz::core
