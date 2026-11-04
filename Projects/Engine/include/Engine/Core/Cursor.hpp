/// @file    Cursor.hpp
/// @brief   ランタイムからマウスカーソル表示・拘束・見た目を制御する API。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once
#include <Engine/Api.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fbzz::core {

/// @brief Unity の CursorLockMode 相当の入力拘束モード。
/// @note ScriptProxy が Win32 API を直接触ると DLL 境界と Editor 復元処理が散らばるため、Engine 側の小さな状態管理 API に集約する。
/// @note Locked は毎フレーム OS カーソルを中央へ戻し移動量だけを Input へ流すため «絶対座標» が消え、画面座標を読む自前カーソル描画は中央に貼り付く。ポインターを見せる作りや「画面から出さない」だけが目的なら Confined を選ぶ (絶対座標が生き残る)。
enum class CursorLockMode {
    None,
    Confined,
    Locked
};

FBZZ_ENGINE_API const char*    ToString(CursorLockMode mode);
FBZZ_ENGINE_API CursorLockMode CursorLockModeFromString(std::string_view text);

/// @brief カーソルの «絵» の種類。ProjectSettings が種類ごとに画像を持ち、ゲームは状況に応じて種類だけを切り替える。
/// @note 「押せる場所に来た」の表現はゲームの責務、どの絵を当てるかはプロジェクトの見た目の話。画像パスを直接切り替えると、素材を差し替えるたびにスクリプトを直すことになる。
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

/// @brief カーソルの拘束と表示。要求 1 件ぶんの内容でもあり、実効状態の型でもある。
struct CursorPolicy {
    CursorLockMode lockMode = CursorLockMode::None;
    bool           visible  = true;

    /// @brief OS カーソルを «プレイヤーから取り上げている» か。Editor が Escape を横取りしてよいかの判断に使う。
    [[nodiscard]] bool CapturesCursor() const
    {
        return lockMode != CursorLockMode::None || !visible;
    }
};

/// @brief 要求の強さ。同じ値なら «後から積んだ方» が勝つ。
/// @note 視点操作とメニューが同居したときどちらが勝つべきかはゲームごとに変わらないため、名前を付ける。数値を直接書かせると「なぜ 50 なのか」が残らない。
namespace CursorPriority {
inline constexpr int Camera   = 0;    ///< @note 視点操作。ゲームの地の状態
inline constexpr int Gameplay = 100;  ///< @note 照準・ドラッグなど遊びの都合
inline constexpr int UI       = 200;  ///< @note ゲーム内カーソル・メニュー
inline constexpr int Modal    = 300;  ///< @note ポーズ・ダイアログ。何より優先する
} /// @note namespace CursorPriority

using CursorRequestId = std::uint32_t;
inline constexpr CursorRequestId kInvalidCursorRequest = 0;

/// @brief 積まれている要求 1 件のスナップショット (Editor の表示用)。
/// @note DLL 境界を越えて std::string や内部ポインターを返すと、Engine 側が要求を畳んだ瞬間に Editor の手元がぶら下がるため、文字列は実体で持つ。
struct CursorRequestInfo {
    CursorRequestId id       = kInvalidCursorRequest;
    int             priority = 0;
    CursorPolicy    policy{};
    std::uint32_t   owner    = 0;      ///< @note 要求元の EntityID の生値 (0 = 無所属)
    char            label[32]{};       ///< @note 要求元の名前 (スクリプトの型名など)
    bool            active   = false;  ///< @note 今この要求が実効値を決めているか
};

/// @brief Input ownership is independent of cursor visibility and movement restriction.
enum class CursorInputOwner { HOST = 0, GAME = 1 };
enum class CursorSuspendReason {
    NONE = 0, NOT_PLAYING, RELEASED, WINDOW_INACTIVE, GAME_VIEW_UNFOCUSED,
    PAUSED, HOST_OVERRIDE, TARGET_UNAVAILABLE, WINDOW_MOVING
};
FBZZ_ENGINE_API const char* ToString(CursorInputOwner owner);
FBZZ_ENGINE_API const char* ToString(CursorSuspendReason reason);

/// @brief Requested game settings and cursor state currently applied by the host.
/// @note Input owner GAME includes visible, unrestricted pointer interaction.
struct CursorState {
    CursorPolicy requested{};
    CursorPolicy effective{};
    CursorInputOwner inputOwner = CursorInputOwner::HOST;
    CursorSuspendReason suspendedReason = CursorSuspendReason::NOT_PLAYING;
    bool windowActive = false;
};

struct Cursor {
    /// @note Legacy IsVisible/GetLockMode/GetEffectivePolicy return the winning requested settings.
    /// @see https://docs.unity3d.com/ScriptReference/Cursor-visible.html Requested visibility and locked effective visibility.
    [[nodiscard]] FBZZ_ENGINE_API static CursorPolicy GetRequestedPolicy();
    [[nodiscard]] FBZZ_ENGINE_API static CursorState GetState();

    /// @brief Explicit host handoff; focus return alone never reacquires game input.
    /// @note Consumes controls already held by the host until their individual release; raw Editor UI input is preserved.
    /// @return False while the application is inactive.
    FBZZ_ENGINE_API static bool AcquireGameInput();
    FBZZ_ENGINE_API static void ReleaseGameInput(CursorSuspendReason reason = CursorSuspendReason::RELEASED);
    [[nodiscard]] FBZZ_ENGINE_API static bool IsGameInputOwned();
    [[nodiscard]] FBZZ_ENGINE_API static bool CanReadGameInput();

    /// @name 基底の要求
    /// @note 誰も Push していないときの状態。Unity の Cursor.visible / lockState 相当で、「1 画面に主張者が 1 つしか居ない」単純なゲームはこれだけで足りる。
    /// @{
    FBZZ_ENGINE_API static void SetVisible(bool visible);
    FBZZ_ENGINE_API static bool IsVisible();

    FBZZ_ENGINE_API static void SetLockMode(CursorLockMode mode);
    FBZZ_ENGINE_API static CursorLockMode GetLockMode();

    FBZZ_ENGINE_API static void Apply(const CursorPolicy& policy);
    /// @}

    /// @name 要求スタック
    /// @note 「今このスクリプトが居る間だけ、この状態にしたい」を積む。
    /// @note ポーズメニューが閉じるとき «元が何だったか» を覚えているのはメニュー側になり、画面が増えるほど復元の組み合わせが増えて 1 つ忘れると戻らない。積んで畳む形にすると、戻す先は常に «残っている要求のうち最も強いもの» で済む。
    /// @{

    /// @param owner EntityID の生値 (省略可)。
    /// @param label 要求元の名前 (省略可)。Editor の Game View オーバーレイが表示する。
    [[nodiscard]] FBZZ_ENGINE_API static CursorRequestId Push(const CursorPolicy& policy,
                                              int                 priority = CursorPriority::Gameplay,
                                              std::uint32_t       owner    = 0,
                                              const char*         label    = nullptr);
    /// @brief 積んだままの要求の中身を差し替える。
    /// @note Release → Push だと «同値なら後勝ち» の順序が変わるため、状態だけを変えたい側はこちらを使う。
    FBZZ_ENGINE_API static void UpdateRequest(CursorRequestId id, const CursorPolicy& policy);
    FBZZ_ENGINE_API static void Release(CursorRequestId id);
    /// @return ClearRequests / Release 後は false。優先順位の勝者かとは区別する。
    [[nodiscard]] FBZZ_ENGINE_API static bool IsRequestActive(CursorRequestId id);
    FBZZ_ENGINE_API static void ReleaseByOwner(std::uint32_t owner);

    /// @brief 全ての要求を畳み、基底も既定 (拘束なし・表示) へ戻す。シーン遷移と Play 終了で呼ぶ。
    /// @note 遷移先に主張者が居なければ前の画面の拘束が «誰も外さない» 状態で残るため、画面をまたいで持ち越してよいカーソル状態は無いと決めておく。
    FBZZ_ENGINE_API static void ClearRequests();

    [[nodiscard]] FBZZ_ENGINE_API static std::size_t GetRequestCount();
    /// @brief index は «優先度の高い順»。実効値を決めている要求が先頭に来る。
    FBZZ_ENGINE_API static bool GetRequest(std::size_t index, CursorRequestInfo& out);

    /// @brief 実効値 (要求スタックの勝者。無ければ基底)。IsVisible / GetLockMode と同じ出所。
    /// @deprecated Use GetRequestedPolicy for game intent or GetState for effective OS state. Kept with its original meaning.
    [[nodiscard]] FBZZ_ENGINE_API static CursorPolicy GetEffectivePolicy();
    [[nodiscard]] FBZZ_ENGINE_API static CursorPolicy GetBasePolicy();
    /// @}

    /// @name 見た目
    /// @{
    /// @param path 絶対パス。
    /// @param hotspotX 画像左上からの画素。
    /// @param hotspotY 画像左上からの画素。
    /// @note 失敗しても既定の矢印が残るだけで、拘束や表示には影響しない。
    FBZZ_ENGINE_API static bool SetShapeImage(CursorShape shape, const char* path,
                              float hotspotX, float hotspotY);
    FBZZ_ENGINE_API static void ClearShapeImages();
    [[nodiscard]] FBZZ_ENGINE_API static bool HasShapeImage(CursorShape shape);

    FBZZ_ENGINE_API static void SetShape(CursorShape shape);
    [[nodiscard]] FBZZ_ENGINE_API static CursorShape GetShape();
    /// @}

    /// @name OS への反映
    /// @{
    /// @brief 拘束範囲をスクリーン (デスクトップ) 座標の矩形で指定する。未指定ならウィンドウのクライアント領域全体を使う (Standalone はこちら)。
    /// @note Editor では «見えているゲーム画面» は Game View パネルの矩形でしかなく、ウィンドウ全体へ拘束すると Confined の範囲も Locked の中心も UISystem が座標を解く viewport とずれる。
    FBZZ_ENGINE_API static void SetClipRegion(float x, float y, float width, float height);
    FBZZ_ENGINE_API static void ClearClipRegion();

    /// @brief アプリのアクティブ状態。Window の WM_ACTIVATEAPP から呼ぶ。
    /// @note 非アクティブ中に拘束を残すと Alt+Tab した先でカーソルが動かせないため、要求状態は保持したまま OS 拘束だけ外し復帰時に自動で張り直す。
    FBZZ_ENGINE_API static void SetWindowActive(bool active);

    /// @brief ゲームの要求を «一時的に OS へ効かせない» 状態にする (Editor 専用)。
    /// @note Game View からフォーカスが外れた・Escape で解放した、といった «Editor の都合» でゲームの要求そのものを None へ落とすと、戻ってきたときに何を復元すればよいか分からなくなる。要求はスタックが持ち続け、ここは «今それを OS へ流すか» だけを切り替える。
    FBZZ_ENGINE_API static void SetSuppressed(bool suppressed);
    FBZZ_ENGINE_API static bool IsSuppressed();

    /// @brief 現在の LockMode を OS カーソルへ再適用する。ウィンドウ移動・リサイズ後に呼ぶ。
    FBZZ_ENGINE_API static void ApplyLock();

    /// @brief Editor / PlayMode 終了時の安全復元。カーソルを必ず表示し、拘束と絵を元へ戻す。
    FBZZ_ENGINE_API static void ResetForEditor();
    /// @}

private:
    /// @brief 実効値が変わったかもしれないときに呼ぶ。表示・拘束・絵をまとめて追従させる。
    FBZZ_ENGINE_API static void Refresh();

    /// @param force 追跡している OS 状態を信用せず、必ず ShowCursor のカウンタを補正し直す。
    FBZZ_ENGINE_API static void ApplyVisibility(bool force = false);

    /// @brief 現在の CursorShape に対応する HCURSOR をウィンドウへ適用する。
    FBZZ_ENGINE_API static void ApplyShape();

    /// @brief OS へ効かせてよいか。ウィンドウが非アクティブでも Editor が取り上げていても効かせない。
    [[nodiscard]] FBZZ_ENGINE_API static bool Effective();

    FBZZ_ENGINE_API static CursorPolicy s_base;
    FBZZ_ENGINE_API static bool s_osVisible;
    FBZZ_ENGINE_API static bool s_windowActive;
    FBZZ_ENGINE_API static bool s_suppressed;
    FBZZ_ENGINE_API static bool s_clipActive;
    FBZZ_ENGINE_API static bool s_hasClipRegion;
    FBZZ_ENGINE_API static float s_clipX;
    FBZZ_ENGINE_API static float s_clipY;
    FBZZ_ENGINE_API static float s_clipWidth;
    FBZZ_ENGINE_API static float s_clipHeight;
    FBZZ_ENGINE_API static CursorShape s_shape;
};

} /// @note namespace fbzz::core
