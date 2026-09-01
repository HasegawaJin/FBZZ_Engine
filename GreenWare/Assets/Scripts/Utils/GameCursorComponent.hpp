/// @file    GameCursorComponent.hpp
/// @brief   ゲーム内カーソル。マウスとパッドを 1 本のポインターへ畳む
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// WHY カーソルを 1 つ挟むか:
///   UIButton / UISlider / UIToggle の判定は「Canvas 空間の座標 1 つと押下状態」
///   しか見ていない。ここを差し替えれば、パッド対応をウィジェット側に 1 行も
///   書かずに済む。画面ごとに index 管理を書くのと違って、UI を増やしても
///   このスクリプトは変わらない。
///
/// WHY マウスとパッドを排他にするか:
///   両方を毎フレーム足すと、パッドで寄せたカーソルが「動いていないマウス」の
///   位置へ引き戻される。直近に触った方だけがカーソルを動かす。
///
/// 置き方: Canvas の子に UIImage を 1 つ作り、このスクリプトを付ける。
///         sortOrder は最前面 (他の UI より大きい値) にすること。
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
    FBZZ_FIELD_RANGE(float, padSpeed, 1400.0f, "Speed", 100.0f, 4000.0f)
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

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugDevice, "-", "Device")

    /// 直近に触った機器がパッドか。UI 側が表示を切り替えるのに使う。
    [[nodiscard]] static bool UsingPad() { return s_usingPad; }
    /// カーソルの現在位置 (Canvas 空間)。
    [[nodiscard]] static Vector2 Position() { return s_position; }

    /// カーソルが指している画面点を、カメラから depth [m] 前方の平面上のワールド座標へ起こす。
    /// カーソルの居ない画面やメインカメラの無いシーンでは false を返し、outWorld は触らない。
    ///
    /// WHY Canvas の幅で割るだけでは足りないか:
    ///   画面に見えている Canvas の範囲は canvasWidth / canvasHeight ではなく、
    ///   基準解像度と viewport の比 (Canvas Scaler) で決まる。UI 自身もその範囲で
    ///   正規化して描かれるので、同じ範囲で割らないと 16:9 以外の窓でだけ
    ///   3D 側がカーソルの絵から静かにずれる。
    [[nodiscard]] static bool WorldPointAtDepth(Script& script, float depth, Vector3& outWorld);

    /// カーソルが指している画面点。左上 (0,0) 〜 右下 (1,1) の正規化座標。
    /// 「画面のどの領域を指しているか」で挙動を変えたい側が使う (電極の追従領域など)。
    ///
    /// WHY canvasWidth で割った値を各所で作らないか:
    ///   割る相手は canvasWidth ではなく «見えている Canvas の範囲» で、これは
    ///   Canvas Scaler と viewport の比で決まる (WorldPointAtDepth の WHY を参照)。
    ///   呼ぶ側がそれぞれ割ると、16:9 以外の窓でだけ判定が静かにずれる。
    [[nodiscard]] static bool NormalizedPoint(Script& script, Vector2& outNormalized);

    /// メインカメラ。解決結果を控えるので、毎フレーム呼んでも全 GameObject を舐め直さない。
    /// 奥行きを自分で決める側 (3D 側) が WorldPointAtDepth と同じカメラを見るための窓口。
    [[nodiscard]] static GameObject* MainCameraObject(Script& script);

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;

private:
    /// 画面に見えている Canvas の範囲 [px]。
    /// UISystem::ResolveScreenSpaceCanvasArea と同じ値を viewport 寸法なしで解いた形で、
    /// 倍率の式で viewport が約分され、基準解像度とアスペクト比だけが残る。
    [[nodiscard]] static Vector2 VisibleCanvasSize(const UICanvas& canvas, float aspect);
    /// 画面 UI を載せている Canvas。MainCameraObject と同じく解決結果を控える。
    [[nodiscard]] static UICanvas* ResolveCanvas(Script& script);

    /// マウスが動いたとみなす距離。手の震えで機器が揺れないだけの幅。
    static constexpr float kMouseWake = 2.0f;

    // 直近に解決したカメラと Canvas。
    //
    // WHY 控えるか: scene.GetMainCameraObject() も FindObjectOfType<UICanvas>() も
    //     «全 GameObject を走査してコンポーネントを引く» 実装で、O(オブジェクト数) が
    //     毎回かかる。電極のように «毎フレーム・複数の呼び出し元» から呼ばれると、
    //     1 フレームに同じ走査を何度も繰り返すことになる。
    // WHY 別シーンの ID が残っても壊れないか: 使う前に «そのコンポーネントが今も居るか»
    //     を確かめ、外れていれば引き直す。控えは «当たれば速い» だけの役で、
    //     正しさは毎回の確認側が持つ。
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
};

FBZZ_REFLECT(GameCursorComponent)

inline void GameCursorComponent::OnStart()
{
    if (GameObject* canvasGO = scene.FindObjectOfType<UICanvas>())
        if (auto* canvas = canvasGO->GetComponent<UICanvas>())
            m_canvasSize = { canvas->canvasWidth, canvas->canvasHeight };

    // 見た目の寸法は transform.scale が正本。中心をポインターへ合わせるのに使う。
    if (GameObject* self = scene.Self()) {
        const Vector3 scale = self->transform.scale;
        if (scale.x > 0.0f && scale.y > 0.0f) m_size = { scale.x, scale.y };
    }

    // 画面中央から始める。0,0 だと左上の隅に何か置いてある画面で誤爆する。
    s_position   = { m_canvasSize.x * 0.5f, m_canvasSize.y * 0.5f };
    m_lastMouse  = ui.GetCanvasMousePosition();
    s_usingPad   = false;

    // このカーソルはマウスの «絶対座標» で動く。Locked は毎フレーム OS カーソルを中央へ
    // 戻すモードなので、そのままだと絵が画面中央に貼り付いてマウスで動かせなくなる。
    // Confined なら座標は生きたまま、ポインターがゲーム画面の外 (別モニター) へ
    // 出て行くことだけを止められる。
    //
    // WHY OS カーソルを隠すか: 隠さないと矢印が 2 つ見える。Confined と対で使うこと。
    //     非表示だけだと、別モニターへ出た先で OS の矢印が戻ってくる。
    cursor.SetLockMode(CursorLockMode::Confined);
    cursor.SetVisible(false);
}

inline void GameCursorComponent::OnDestroy()
{
    // 次のシーンへ古い座標を持ち込まない。UIPointer 自体も 1 フレームで失効するが、
    // 明示的に取り下げておく方が「誰が持っているか」を追いやすい。
    ui.ClearPointer();
    s_live     = false;
    s_cameraId = EntityID::INVALID;
    s_canvasId = EntityID::INVALID;
}

inline void GameCursorComponent::OnUpdate()
{
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);

    // ── マウス ────────────────────────────────────────────────────────────
    // GetCanvasMousePosition は常に OS のマウスを返す (自分の出力ではない)。
    const Vector2 mouse = ui.GetCanvasMousePosition();
    const float moved = std::abs(mouse.x - m_lastMouse.x) + std::abs(mouse.y - m_lastMouse.y);
    if (moved >= kMouseWake) {
        s_position  = mouse;
        s_usingPad  = false;
        m_lastMouse = mouse;
    }

    // ── キーボード ────────────────────────────────────────────────────────
    // WHY 矢印でもカーソルを動かすか: 経路をカーソル 1 本に寄せた以上、
    //     マウスを持たない人が UI を触れなくなる。方向キーはパッドと同じ速度で扱う。
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

    // ── パッド ────────────────────────────────────────────────────────────
    if (input.IsPadConnected()) {
        float sx = input.GetPadAxis(fbzz::input::GamepadAxis::LEFT_STICK_X);
        float sy = input.GetPadAxis(fbzz::input::GamepadAxis::LEFT_STICK_Y);
        // 十字キーも同じ速度で効かせる。細かく詰めたいときに使う。
        if (input.GetPadButton(fbzz::input::GamepadButton::DPAD_LEFT))  sx = -1.0f;
        if (input.GetPadButton(fbzz::input::GamepadButton::DPAD_RIGHT)) sx = +1.0f;
        if (input.GetPadButton(fbzz::input::GamepadButton::DPAD_UP))    sy = +1.0f;
        if (input.GetPadButton(fbzz::input::GamepadButton::DPAD_DOWN))  sy = -1.0f;

        // 半径でデッドゾーンを取る。軸ごとに切ると斜めの感度が方向で変わる。
        const float length = std::sqrt(sx * sx + sy * sy);
        if (length > padDeadzone) {
            // デッドゾーンの外側を 0..1 へ引き伸ばしてから曲線を掛ける。
            // WHY: 生の傾きに指数を掛けると、デッドゾーンの縁で速度が飛ぶ。
            const float t = std::clamp((length - padDeadzone) / (1.0f - padDeadzone), 0.0f, 1.0f);
            const float speed = std::pow(t, padAccelPower) * padSpeed * dt;
            s_position.x += (sx / length) * speed;
            s_position.y -= (sy / length) * speed;   // スティックは上が +、画面は下が +
            s_usingPad = true;
        }
    }

    s_position.x = std::clamp(s_position.x, 0.0f, m_canvasSize.x);
    s_position.y = std::clamp(s_position.y, 0.0f, m_canvasSize.y);
    s_live = true;

    // ── 押下 ──────────────────────────────────────────────────────────────
    const bool pressed = input.MouseButton(MouseBtn::Left)
                      || input.GetKey(fbzz::input::KeyCode::ENTER)
                      || input.GetKey(fbzz::input::KeyCode::SPACE)
                      || (input.IsPadConnected()
                          && input.GetPadButton(fbzz::input::GamepadButton::A));

    // UI のヒット判定はここで差し替わる。毎フレーム宣言しないと OS のマウスへ戻る。
    ui.SetPointer(s_position, pressed);

    // 見た目。矩形の中心をポインターへ合わせ、そこから spriteOffset だけずらす。
    //
    // WHY anchor/pivot を読むか: transform.position は「基準点からのずれ」でしかなく、
    //     矩形の左上は ResolveUIRect が anchor と pivot から決める。半分引くだけだと
    //     Inspector で pivot を中央にした瞬間に絵が半分ぶんずれ、offset で詰めた値が
    //     意味を失う。ここは常に逆算しておき、offset は素材のずれだけを担当させる。
    if (GameObject* self = scene.Self()) {
        UIAnchor anchoring{};
        if (const auto* image = self->GetComponent<UIImage>()) anchoring = image->anchoring;

        self->transform.position = {
            s_position.x - m_canvasSize.x * anchoring.anchor.x
                + m_size.x * (anchoring.pivot.x - 0.5f) + spriteOffset.x,
            s_position.y - m_canvasSize.y * anchoring.anchor.y
                + m_size.y * (anchoring.pivot.y - 0.5f) + spriteOffset.y,
            0.0f,
        };
    }
    debugDevice = s_usingPad ? "Gamepad" : "Mouse";
}

inline Vector2 GameCursorComponent::VisibleCanvasSize(const UICanvas& canvas, float aspect)
{
    if (canvas.scaleMode != UICanvasScaleMode::ScaleWithScreenSize)
        return { canvas.canvasWidth, canvas.canvasHeight };

    const float match = Clamp01(canvas.matchWidthOrHeight);
    const float refW  = (std::max)(1.0f, canvas.referenceWidth);
    const float refH  = (std::max)(1.0f, canvas.referenceHeight);
    const float width = std::pow(refW, 1.0f - match) * std::pow(refH * aspect, match);
    return { width, width / aspect };
}

inline GameObject* GameCursorComponent::MainCameraObject(Script& script)
{
    if (s_cameraId.IsValid()) {
        if (GameObject* cached = script.scene.GetGameObject(s_cameraId)) {
            const auto* camera = cached->GetComponent<CameraComponent>();
            if (camera && camera->enabled && camera->isMain) return cached;
        }
    }
    GameObject* found = script.scene.GetMainCameraObject();
    s_cameraId = found ? found->GetID() : EntityID::INVALID;
    return found;
}

inline UICanvas* GameCursorComponent::ResolveCanvas(Script& script)
{
    if (s_canvasId.IsValid()) {
        if (GameObject* cached = script.scene.GetGameObject(s_canvasId))
            if (auto* canvas = cached->GetComponent<UICanvas>()) return canvas;
    }
    GameObject* found = script.scene.FindObjectOfType<UICanvas>();
    s_canvasId = found ? found->GetID() : EntityID::INVALID;
    return found ? found->GetComponent<UICanvas>() : nullptr;
}

inline bool GameCursorComponent::NormalizedPoint(Script& script, Vector2& outNormalized)
{
    if (!s_live) return false;

    GameObject* cameraObject = MainCameraObject(script);
    if (!cameraObject) return false;
    const auto* camera = cameraObject->GetComponent<CameraComponent>();
    if (!camera) return false;

    // aspectRatio は毎フレーム実 viewport から書き戻される (SceneUtils::ResolveGameCamera)。
    // Editor の Game ビューのように viewport ≠ ウィンドウの場面でも、ここだけ見れば合う。
    const float aspect = (std::max)(camera->aspectRatio, 0.0001f);

    Vector2 visible{ 1920.0f, 1080.0f };
    if (const UICanvas* canvas = ResolveCanvas(script))
        visible = VisibleCanvasSize(*canvas, aspect);

    outNormalized = { s_position.x / visible.x, s_position.y / visible.y };
    return true;
}

inline bool GameCursorComponent::WorldPointAtDepth(Script& script, float depth, Vector3& outWorld)
{
    Vector2 normalized{};
    if (!NormalizedPoint(script, normalized)) return false;

    // NormalizedPoint が通った時点でカメラは居る。控えが効くので引き直しても走査は起きない。
    GameObject* cameraObject = MainCameraObject(script);
    const auto* camera = cameraObject ? cameraObject->GetComponent<CameraComponent>() : nullptr;
    if (!camera) return false;

    const float ndcX = normalized.x * 2.0f - 1.0f;
    const float ndcY = 1.0f - normalized.y * 2.0f;

    const float halfHeight = std::tan(ToRad(camera->fovY) * 0.5f) * depth;
    const float halfWidth  = halfHeight * (std::max)(camera->aspectRatio, 0.0001f);

    const Transform& view = cameraObject->transform;
    outWorld = view.worldPosition
             + view.forward * depth
             + view.right   * (ndcX * halfWidth)
             + view.up      * (ndcY * halfHeight);
    return true;
}

} // namespace sandbox
