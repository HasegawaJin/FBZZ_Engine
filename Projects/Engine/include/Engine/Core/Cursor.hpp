/// @file    Cursor.hpp
/// @brief   ランタイムからマウスカーソル表示・拘束・見た目を制御する API。
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

#include <cstddef>
#include <cstdint>
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

// カーソルの «絵» の種類。ProjectSettings が種類ごとに画像を持ち、ゲームは
// 状況に応じて種類だけを切り替える。
// WHY 画像パスを直接切り替えないか: 「押せる場所に来た」を表現したいのはゲームで、
//     どの絵を当てるかはプロジェクトの見た目の話。両者を同じ引数に載せると、
//     カーソル素材を差し替えるたびにスクリプトを直すことになる。
enum class CursorShape {
    Default,
    Clickable,
    Grab,
    Text,
    Aim,
    Busy
};
inline constexpr std::size_t kCursorShapeCount = 6;

FBZZ_ENGINE_API const char* ToString(CursorShape shape);
FBZZ_ENGINE_API CursorShape CursorShapeFromString(std::string_view text);

// カーソルの拘束と表示。要求 1 件ぶんの内容でもあり、実効状態の型でもある。
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

// 要求の強さ。同じ値なら «後から積んだ方» が勝つ。
// WHY 名前を付けるか: 視点操作とメニューが同居したとき、どちらが勝つべきかは
//     ゲームごとに変わらない。数値をスクリプトへ書かせると「なぜ 50 なのか」が
//     どこにも残らない。
namespace CursorPriority {
inline constexpr int Camera   = 0;    // 視点操作。ゲームの地の状態
inline constexpr int Gameplay = 100;  // 照準・ドラッグなど遊びの都合
inline constexpr int UI       = 200;  // ゲーム内カーソル・メニュー
inline constexpr int Modal    = 300;  // ポーズ・ダイアログ。何より優先する
} // namespace CursorPriority

using CursorRequestId = std::uint32_t;
inline constexpr CursorRequestId kInvalidCursorRequest = 0;

// 積まれている要求 1 件のスナップショット (Editor の表示用)。
// WHY 文字列を実体で持つか: DLL 境界を越えて std::string や内部ポインターを
//     返すと、Engine 側が要求を畳んだ瞬間に Editor の手元がぶら下がる。
struct CursorRequestInfo {
    CursorRequestId id       = kInvalidCursorRequest;
    int             priority = 0;
    CursorPolicy    policy{};
    std::uint32_t   owner    = 0;      // 要求元の EntityID の生値 (0 = 無所属)
    char            label[32]{};       // 要求元の名前 (スクリプトの型名など)
    bool            active   = false;  // 今この要求が実効値を決めているか
};

struct FBZZ_ENGINE_API Cursor {
    // ── 基底の要求 ──────────────────────────────────────────────────────────
    // 誰も Push していないときの状態。Unity の Cursor.visible / lockState 相当で、
    // 「1 画面に主張者が 1 つしか居ない」単純なゲームはこれだけで足りる。
    static void SetVisible(bool visible);
    static bool IsVisible();

    static void SetLockMode(CursorLockMode mode);
    static CursorLockMode GetLockMode();

    static void Apply(const CursorPolicy& policy);

    // ── 要求スタック ────────────────────────────────────────────────────────
    // 「今このスクリプトが居る間だけ、この状態にしたい」を積む。
    //
    // WHY 基底の書き換えでは足りないか: ポーズメニューがカーソルを出したあと、
    //     閉じたときに «元が何だったか» を覚えているのはメニュー側になる。画面が
    //     増えるほど復元の組み合わせが増え、1 つ忘れるとカーソルが戻らない。
    //     積んで畳む形にすると、戻す先は常に «残っている要求のうち最も強いもの» で、
    //     覚えておく必要が無くなる。
    //
    // owner には EntityID の生値を、label には要求元の名前を渡す (どちらも省略可)。
    // Editor の Game View オーバーレイが「今カーソルを持っているのは誰か」を出す。
    [[nodiscard]] static CursorRequestId Push(const CursorPolicy& policy,
                                              int                 priority = CursorPriority::Gameplay,
                                              std::uint32_t       owner    = 0,
                                              const char*         label    = nullptr);
    // 積んだままの要求の中身を差し替える。Release → Push だと «同値なら後勝ち» の
    // 順序が変わるため、状態だけを変えたい側はこちらを使う。
    static void UpdateRequest(CursorRequestId id, const CursorPolicy& policy);
    static void Release(CursorRequestId id);
    static void ReleaseByOwner(std::uint32_t owner);

    // 全ての要求を畳み、基底も既定 (拘束なし・表示) へ戻す。シーン遷移と Play 終了で呼ぶ。
    // WHY 基底まで戻すか: 前の画面が SetLockMode で取った拘束は、遷移先に主張者が
    //     居なければ «誰も外さない» 状態で残る。画面をまたいで持ち越してよい
    //     カーソル状態は無い、と決めておく方が事故が少ない。
    static void ClearRequests();

    [[nodiscard]] static std::size_t GetRequestCount();
    // index は «優先度の高い順»。実効値を決めている要求が先頭に来る。
    static bool GetRequest(std::size_t index, CursorRequestInfo& out);

    // 実効値 (要求スタックの勝者。無ければ基底)。IsVisible / GetLockMode と同じ出所。
    [[nodiscard]] static CursorPolicy GetEffectivePolicy();
    [[nodiscard]] static CursorPolicy GetBasePolicy();

    // ── 見た目 ──────────────────────────────────────────────────────────────
    // 種類ごとの画像を差し込む。path は絶対パス、hotspot は画像左上からの画素。
    // 失敗しても既定の矢印が残るだけで、拘束や表示には影響しない。
    static bool SetShapeImage(CursorShape shape, const char* path,
                              float hotspotX, float hotspotY);
    static void ClearShapeImages();
    [[nodiscard]] static bool HasShapeImage(CursorShape shape);

    static void SetShape(CursorShape shape);
    [[nodiscard]] static CursorShape GetShape();

    // ── OS への反映 ─────────────────────────────────────────────────────────
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
    //     何を復元すればよいか分からなくなる。要求はスタックが持ち続け、
    //     ここは «今それを OS へ流すか» だけを切り替える。
    static void SetSuppressed(bool suppressed);
    static bool IsSuppressed();

    // 現在の LockMode を OS カーソルへ再適用する。ウィンドウ移動・リサイズ後に呼ぶ。
    static void ApplyLock();

    // Editor / PlayMode 終了時の安全復元。カーソルを必ず表示し、拘束と絵を元へ戻す。
    static void ResetForEditor();

private:
    // 実効値が変わったかもしれないときに呼ぶ。表示・拘束・絵をまとめて追従させる。
    static void Refresh();

    // force: 追跡している OS 状態を信用せず、必ず ShowCursor のカウンタを補正し直す。
    static void ApplyVisibility(bool force = false);

    // 現在の CursorShape に対応する HCURSOR をウィンドウへ適用する。
    static void ApplyShape();

    // OS へ効かせてよいか。ウィンドウが非アクティブでも Editor が取り上げていても効かせない。
    [[nodiscard]] static bool Effective();

    static CursorPolicy s_base;
    static bool s_osVisible;
    static bool s_windowActive;
    static bool s_suppressed;
    static bool s_clipActive;
    static bool s_hasClipRegion;
    static float s_clipX;
    static float s_clipY;
    static float s_clipWidth;
    static float s_clipHeight;
    static CursorShape s_shape;
};

} // namespace fbzz::core
