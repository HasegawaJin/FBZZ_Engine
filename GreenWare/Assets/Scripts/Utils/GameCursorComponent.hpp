/// @file    GameCursorComponent.hpp
/// @brief   ゲーム内カーソル。マウスとパッドを 1 本のポインターへ畳む
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// @note UIButton / UISlider / UIToggle は Canvas 座標 1 つと押下状態しか見ないため、
///       ここを差し替えるだけでパッド対応が UI 側 0 行で済む。マウスとパッドは排他で、
///       両方を毎フレーム足すと直近に触った方だけがカーソルを動かす。置き方: Canvas の
///       子に UIImage を 1 つ作りこのスクリプトを付け、sortOrder を最前面にする。
#pragma once

#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <string>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class GameCursorComponent : public Script {
    FBZZ_SCRIPT(GameCursorComponent)
    FBZZ_REQUIRE_COMPONENT(UIImage)

public:
    FBZZ_GROUP("Pad")
    FBZZ_FIELD_RANGE(float, padSpeed, 1400.0f, "速さ", 100.0f, 4000.0f)
    FBZZ_TOOLTIP("スティックを倒しきったときのカーソル速度 (Canvas px/秒)")
    FBZZ_FIELD_RANGE(float, padDeadzone, 0.18f, "Deadzone", 0.0f, 0.6f)
    FBZZ_TOOLTIP("この値までの傾きは無視する。触れただけでカーソルが流れるのを防ぐ")
    FBZZ_FIELD_RANGE(float, padAccelPower, 2.0f, "Accel Curve", 1.0f, 4.0f)
    FBZZ_TOOLTIP("倒し量に掛ける指数。大きいほど「そっと動かす」がやりやすい")

    FBZZ_GROUP("Sprite")
    FBZZ_FIELD(Vector2, spriteOffset, (Vector2{ 10.0f, 10.0f }), "Offset")
    FBZZ_TOOLTIP("絵をポインターからずらす量 (Canvas px、右と下が +)。"
                 "矢印の先端のように、絵の中で実際に指している点が中心にない素材を合わせるのに使う。"
                 "既定値は pointer_c_shaded.png を 36px で置いたときの実測")
    FBZZ_FIELD(bool, useHardwareCursor, true, "OS Cursor For Mouse")
    FBZZ_TOOLTIP("マウス操作中は OS のカーソルを見せる (Project Settings の [cursor] に "
                 "Default の画像が要る)。この絵は 1 フレーム遅れないので、マウスでは常に "
                 "こちらが自然。パッド操作中は自動で下の UIImage へ切り替わる")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugDevice, "-", "Device")

    /// 直近に触った機器がパッドか。UI 側が表示を切り替えるのに使う。
    [[nodiscard]] static bool UsingPad() { return s_usingPad; }
    /// カーソルの現在位置 (Canvas 空間)。
    [[nodiscard]] static Vector2 Position() { return s_position; }

    /// カーソルが指している画面点を、カメラから depth [m] 前方の平面上のワールド座標へ起こす。
    /// カーソルの居ない画面やメインカメラの無いシーンでは false を返し、outWorld は触らない。
    ///
    /// @note 見えている Canvas の範囲は canvasWidth/canvasHeight ではなく基準解像度と
    ///       viewport の比 (Canvas Scaler) で決まる。同じ範囲で割らないと 16:9 以外の
    ///       窓でだけ 3D 側がカーソルの絵から静かにずれる。
    [[nodiscard]] static bool WorldPointAtDepth(Script& script, float depth, Vector3& outWorld);

    /// カーソルが指している画面点。左上 (0,0) 〜 右下 (1,1) の正規化座標。
    /// 「画面のどの領域を指しているか」で挙動を変えたい側が使う (電極の追従領域など)。
    ///
    /// @note 割る相手は canvasWidth ではなく «見えている Canvas の範囲» (WorldPointAtDepth の
    ///       @note 参照)。呼ぶ側がそれぞれ割ると、16:9 以外の窓でだけ判定が静かにずれる。
    [[nodiscard]] static bool NormalizedPoint(Script& script, Vector2& outNormalized);

    /// メインカメラ。解決結果を控えるので、毎フレーム呼んでも全 GameObject を舐め直さない。
    /// 奥行きを自分で決める側 (3D 側) が WorldPointAtDepth と同じカメラを見るための窓口。
    [[nodiscard]] static GameObject* MainCameraObject(Script& script);

    void OnStart() override;
    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override;
    void OnDestroy() override;

private:
    /// 画面 UI を載せている Canvas。MainCameraObject と同じく解決結果を控える。
    [[nodiscard]] static GameObject* ResolveCanvas(Script& script);

    /// マウスが動いたとみなす距離。手の震えで機器が揺れないだけの幅。
    static constexpr float kMouseWake = 2.0f;

    /// 直近に解決したカメラと Canvas。
    /// @note `scene.GetMainCameraObject()` も `FindObjectOfType<UICanvas>()` も全 GameObject を
    ///       走査する O(オブジェクト数) の実装で、毎フレーム複数の呼び出し元から呼ばれると
    ///       同じ走査を繰り返す。使う前に対象が今も有効か確かめ、外れていれば引き直す
    ///       (控えは «当たれば速い» だけの役、正しさは毎回の確認側が持つ)。
    static inline EntityID s_cameraId = EntityID::INVALID;
    static inline EntityID s_canvasId = EntityID::INVALID;

    static inline bool    s_usingPad = false;
    static inline Vector2 s_position{};
    /// このフレームのカーソル位置が有効か。静的なので、カーソルの居ない画面へ遷移した後も
    /// s_position には前の画面の値が残る。それを «今どこを指しているか» として読ませない。
    static inline bool    s_live = false;

    Vector2 m_lastMouse{};
    Vector2 m_canvasSize{ 1920.0f, 1080.0f };
    Vector2 m_size{ 24.0f, 24.0f };
    /// このカーソルが生きている間だけの拘束要求。ポーズ等がこの上に重なっても、
    /// 閉じた時点でここへ戻る。
    CursorRequest m_cursor;
    /// OS のカーソルを見せてよいか (画像が設定されているときだけ)。
    bool m_hardwareAvailable = false;
};

FBZZ_REFLECT(GameCursorComponent)

inline void GameCursorComponent::OnStart()
{
    if (GameObject* canvasGO = scene.FindObjectOfType<UICanvas>())
        if (auto* canvas = canvasGO->GetComponent<UICanvas>())
            m_canvasSize = { canvas->canvasWidth, canvas->canvasHeight };

    /// @note 見た目の寸法は transform.scale が正本。中心をポインターへ合わせるのに使う。
    if (GameObject* self = scene.Self()) {
        const Vector3 scale = self->transform.scale;
        if (scale.x > 0.0f && scale.y > 0.0f) m_size = { scale.x, scale.y };
    }

    /// @note 画面中央から始める。0,0 だと左上の隅に何か置いてある画面で誤爆する。
    s_position   = { m_canvasSize.x * 0.5f, m_canvasSize.y * 0.5f };
    m_lastMouse  = ui.GetCanvasMousePosition();
    s_usingPad   = false;
}

/// @note ポーズ画面のカーソルは «開いている間だけ» 居る。OnStart で要求を積むと、閉じて
///       畳んでも要求が残り、視点操作の Locked を UI の Confined が覆い続けて
///       «閉じた後だけマウスで振り向けない» になる。有効/無効の出し入れで枠の開閉に合わせる。
inline void GameCursorComponent::OnEnable()
{
    /// @note Locked は毎フレーム OS カーソルを中央へ戻すため、絶対座標で動くこの実装とは
    ///       噛み合わない。Confined なら座標は生きたまま画面外への逸出だけを止められる。
    ///       表示は OnUpdate が機器ごとに切り替え、両方は出さない (二重に見える上、片方だけ
    ///       隠すと別モニターへ出た先で OS の矢印が戻る)。
    m_hardwareAvailable = useHardwareCursor && cursor.HasShapeImage(CursorShape::Default);
    m_cursor = cursor.Push(CursorLockMode::Confined, m_hardwareAvailable, CursorPriority::UI);
}

inline void GameCursorComponent::OnDisable()
{
    m_cursor.Release();
    /// @note ポインターの差し替えも下ろす。1 フレームで失効する作りだが、UI が «OS のマウス»
    ///       へ戻る瞬間をここで揃えておくと、閉じた後に前の座標で 1 コマ当たるのを防げる。
    ui.ClearPointer();
    s_live = false;
}

inline void GameCursorComponent::OnDestroy()
{
    /// @note 次のシーンへ古い座標を持ち込まない。UIPointer 自体も 1 フレームで失効するが、
    ///       明示的に取り下げておく方が「誰が持っているか」を追いやすい。
    ui.ClearPointer();
    s_live     = false;
    s_cameraId = EntityID::INVALID;
    s_canvasId = EntityID::INVALID;
}

inline void GameCursorComponent::OnUpdate()
{
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);

    /// @name マウス
    /// @note GetCanvasMousePosition は常に OS のマウスを返す (自分の出力ではない)。
    const Vector2 mouse = ui.GetCanvasMousePosition();
    const float moved = std::abs(mouse.x - m_lastMouse.x) + std::abs(mouse.y - m_lastMouse.y);
    if (moved >= kMouseWake) {
        s_position  = mouse;
        s_usingPad  = false;
        m_lastMouse = mouse;
    }

    /// @name キーボード
    /// @note 経路をカーソル 1 本に寄せた以上、方向キーでも動かせないとマウスを持たない人が
    ///       UI を触れなくなる。パッドと同じ速度で扱う。
    {
        float kx = 0.0f, ky = 0.0f;
        if (input.GetKey(fbzz::input::KeyCode::LEFT)  || input.GetKey(fbzz::input::KeyCode::A)) kx -= 1.0f;
        if (input.GetKey(fbzz::input::KeyCode::RIGHT) || input.GetKey(fbzz::input::KeyCode::D)) kx += 1.0f;
        if (input.GetKey(fbzz::input::KeyCode::UP)    || input.GetKey(fbzz::input::KeyCode::W)) ky -= 1.0f;
        if (input.GetKey(fbzz::input::KeyCode::DOWN)  || input.GetKey(fbzz::input::KeyCode::S)) ky += 1.0f;
        if (kx != 0.0f || ky != 0.0f) {
            const float length = std::sqrt(kx * kx + ky * ky);
            const float speed = padSpeed * dt;
            s_position.x += (kx / length) * speed;
            s_position.y += (ky / length) * speed;
            s_usingPad = false;
        }
    }

    /// @name パッド
    if (input.IsPadConnected()) {
        float sx = input.GetPadAxis(fbzz::input::GamepadAxis::LEFT_STICK_X);
        float sy = input.GetPadAxis(fbzz::input::GamepadAxis::LEFT_STICK_Y);
        /// @note 十字キーも同じ速度で効かせる。細かく詰めたいときに使う。
        if (input.GetPadButton(fbzz::input::GamepadButton::DPAD_LEFT))  sx = -1.0f;
        if (input.GetPadButton(fbzz::input::GamepadButton::DPAD_RIGHT)) sx = +1.0f;
        if (input.GetPadButton(fbzz::input::GamepadButton::DPAD_UP))    sy = +1.0f;
        if (input.GetPadButton(fbzz::input::GamepadButton::DPAD_DOWN))  sy = -1.0f;

        /// @note 半径でデッドゾーンを取る。軸ごとに切ると斜めの感度が方向で変わる。
        const float length = std::sqrt(sx * sx + sy * sy);
        if (length > padDeadzone) {
            /// @note デッドゾーンの外側を 0..1 へ引き伸ばしてから曲線を掛ける。生の傾きに
            ///       指数を掛けると、デッドゾーンの縁で速度が飛ぶ。
            const float t = std::clamp((length - padDeadzone) / (1.0f - padDeadzone), 0.0f, 1.0f);
            const float speed = std::pow(t, padAccelPower) * padSpeed * dt;
            s_position.x += (sx / length) * speed;
            /// @note スティックは上が +、画面は下が +
            s_position.y -= (sy / length) * speed;
            s_usingPad = true;
        }
    }

    s_position.x = std::clamp(s_position.x, 0.0f, m_canvasSize.x);
    s_position.y = std::clamp(s_position.y, 0.0f, m_canvasSize.y);
    s_live = true;

    /// @name 押下
    const bool pressed = input.MouseButton(MouseBtn::Left)
                      || input.GetKey(fbzz::input::KeyCode::ENTER)
                      || input.GetKey(fbzz::input::KeyCode::SPACE)
                      || (input.IsPadConnected()
                          && input.GetPadButton(fbzz::input::GamepadButton::A));

    /// @note UI のヒット判定はここで差し替わる。毎フレーム宣言しないと OS のマウスへ戻る。
    ui.SetPointer(s_position, pressed);

    /// @name どちらのカーソルを見せるか
    /// @note マウスは OS のカーソルに任せる方が良い。こちらの絵は «前フレームの位置» を
    ///       描くので、速く振ると必ず遅れて見える。パッドはそもそも OS カーソルを動かせない
    ///       ので、こちらの絵しか選べない。
    const bool showOsCursor = m_hardwareAvailable && !s_usingPad;
    m_cursor.Set(CursorLockMode::Confined, showOsCursor);

    /// @note 見た目。矩形の中心をポインターへ合わせ spriteOffset だけずらす。
    ///       transform.position は基準点からのずれでしかなく、矩形の左上は ResolveUIRect が
    ///       anchor と pivot から決めるため、Inspector で pivot を変えても offset の意味が
    ///       変わらないよう常に逆算する。offset は素材のずれだけを担当する。
    if (GameObject* self = scene.Self()) {
        UIAnchor anchoring{};
        if (auto* image = self->GetComponent<UIImage>()) {
            anchoring = image->anchoring;
            image->enabled = !showOsCursor;
        }

        self->transform.position = {
            s_position.x - m_canvasSize.x * anchoring.anchor.x
                + m_size.x * (anchoring.pivot.x - 0.5f) + spriteOffset.x,
            s_position.y - m_canvasSize.y * anchoring.anchor.y
                + m_size.y * (anchoring.pivot.y - 0.5f) + spriteOffset.y,
            0.0f,
        };
    }
    debugDevice = s_usingPad ? "Gamepad" : (showOsCursor ? "Mouse (OS cursor)" : "Mouse");
}

inline GameObject* GameCursorComponent::MainCameraObject(Script& script)
{
    if (s_cameraId.IsValid()) {
        if (GameObject* cached = script.scene.GetGameObject(s_cameraId)) {
            const auto* camera = cached->GetComponent<CameraComponent>();
            if (camera && camera->enabled && camera->isMain && cached->activeInHierarchy()) return cached;
        }
    }
    GameObject* found = script.scene.GetMainCameraObject();
    s_cameraId = found ? found->GetID() : EntityID::INVALID;
    return found;
}

inline GameObject* GameCursorComponent::ResolveCanvas(Script& script)
{
    if (s_canvasId.IsValid()) {
        if (GameObject* cached = script.scene.GetGameObject(s_canvasId))
            if (cached->GetComponent<UICanvas>()) return cached;
    }
    GameObject* found = script.scene.FindObjectOfType<UICanvas>();
    s_canvasId = found ? found->GetID() : EntityID::INVALID;
    return found;
}

inline bool GameCursorComponent::NormalizedPoint(Script& script, Vector2& outNormalized)
{
    if (!s_live) return false;

    Vector2 visible{ 1920.0f, 1080.0f };
    if (GameObject* canvasObject = ResolveCanvas(script))
        if (!script.ui.TryGetCanvasSize(visible, canvasObject)) return false;

    outNormalized = { s_position.x / visible.x, s_position.y / visible.y };
    return true;
}

inline bool GameCursorComponent::WorldPointAtDepth(Script& script, float depth, Vector3& outWorld)
{
    Vector2 normalized{};
    if (!NormalizedPoint(script, normalized)) return false;

    GameObject* cameraObject = MainCameraObject(script);
    return cameraObject && script.camera.TryViewportToWorldPoint(normalized, depth, outWorld, cameraObject);
}

} // namespace sandbox
