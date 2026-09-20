/// @file    AimMarkerComponent.hpp
/// @brief   ビームの手前に居る 1 体を WorldSpace UI の四隅ブラケットで示す
/// @author  Hasegawa Jin
/// @date    2026-08-22

#pragma once

#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class AimMarkerComponent : public Script {
    FBZZ_SCRIPT(AimMarkerComponent)

public:
    FBZZ_FIELD(bool, showAimMarker, false, "AIM枠を表示")
    FBZZ_GROUP("配置")
    FBZZ_FIELD_RANGE(float, padding, 0.25f, "Padding", 0.0f, 3.0f)
    FBZZ_TOOLTIP("体の外周から枠までのワールド距離。体にぴったり付けると輪郭に紛れる")
    FBZZ_FIELD_RANGE(float, cornerRatio, 0.34f, "Corner Ratio", 0.05f, 0.5f)
    FBZZ_TOOLTIP("角のブラケットが辺のどれだけを占めるか。0.5 で閉じた四角形になる")
    FBZZ_FIELD_RANGE(float, lineWidth, 0.06f, "線の幅", 0.005f, 0.5f)
    FBZZ_FIELD_RANGE(float, minSize, 0.6f, "Min Size", 0.1f, 5.0f)
    FBZZ_TOOLTIP("枠の最小の一辺。小さい敵でも読める大きさを保証する")
    FBZZ_FIELD_RANGE(float, fallbackBodySize, 1.6f, "Fallback Body Size", 0.1f, 10.0f)
    FBZZ_TOOLTIP("コライダーが無く寸法を測れない対象に使う一辺の長さ")

    FBZZ_GROUP("Color")
    FBZZ_FIELD_COLOR(markerColor, (Vector4{ 0.20f, 1.00f, 0.45f, 1.00f }), "Marker Color")

    FBZZ_GROUP("Acquire")
    /// @note 乗り移った瞬間だけ lockScale 倍に出して縮める。位置の移動だけでは乗り換えに気付けない。
    FBZZ_FIELD_RANGE(float, lockScale, 1.9f, "Acquire Scale", 1.0f, 4.0f)
    FBZZ_TOOLTIP("線に入った瞬間の枠の倍率。1.0 で演出なし")
    FBZZ_FIELD_RANGE(float, lockSeconds, 0.16f, "Lock Seconds", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, pulseDepth, 0.06f, "脈動の深さ", 0.0f, 0.5f)
    FBZZ_TOOLTIP("追従中の呼吸。0 で止まった枠になる")
    FBZZ_FIELD_RANGE(float, pulseHz, 1.4f, "脈動の周波数 [Hz]", 0.0f, 8.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugTargetName, "", "対象")

    /// PlayerComponent が内部モジュールとして持つときに、同じ PlayerAimComponent を渡す。
    void SetAimComponent(PlayerAimComponent* aim) { m_aimOverride = aim; }

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override;

private:
    /// Canvas のピクセル座標系とワールド単位の換算比。UICanvas::worldScale の逆数。
    static constexpr float kPixelsPerUnit = 100.0f;
    /// 角 4 つ × (横棒 + 縦棒)。
    static constexpr int kBarCount = 8;

    static constexpr const char* kCanvasName = "AimMarker";

    [[nodiscard]] PlayerAimComponent* Aim() const;
    [[nodiscard]] std::string CanvasName() const;
    [[nodiscard]] Vector4 CurrentColor(GameObject& target) const;
    /// 枠の一辺 (ワールド) を対象の体から決める。
    [[nodiscard]] float FrameSize(GameObject& target) const;

    /// 生成済みの枠を拾い直せたら true。スクリプト DLL のリロード対策。
    bool Adopt();
    void Build();
    /// 一辺 size (ピクセル) の四隅ブラケットとして 8 本を並べる。
    void LayoutBars(float size);

    PlayerAimComponent* m_aimOverride = nullptr;

    EntityRef m_canvas;
    std::array<EntityRef, kBarCount> m_bars{};

    /// 乗り換え検出。EntityRef ではなく素の ID で持つ (解決は要らず比較しかしない)。
    EntityID m_lastTarget{};
    float    m_lockRemaining = 0.0f;
};

FBZZ_REFLECT(AimMarkerComponent)


inline PlayerAimComponent* AimMarkerComponent::Aim() const
{
    return m_aimOverride ? m_aimOverride : scene.GetScript<PlayerAimComponent>();
}

inline std::string AimMarkerComponent::CanvasName() const
{
    /// @note 枠はルートに置くため、同名だと 2 人目のプレイヤー (デバッグ複製含む) が
    ///       1 人目の枠を拾い直してしまう。持ち主ごとに名前を分ける。
    GameObject* owner = scene.Self();
    return std::string(kCanvasName) + "_" + (owner ? owner->instanceId : std::string{});
}

inline void AimMarkerComponent::OnStart()
{
    m_lastTarget    = {};
    m_lockRemaining = 0.0f;
    debugTargetName.clear();

    /// @note スクリプト DLL リロードで Script は作り直され EntityRef は空へ戻るが、
    ///       UI の GameObject は Scene 側に残る。無条件に組み直すとリロードのたびに枠が増える。
    if (!Adopt()) Build();
}

inline bool AimMarkerComponent::Adopt()
{
    GameObject* canvasObject = scene.Find(CanvasName(), true);
    if (!canvasObject || !canvasObject->GetComponent<UICanvas>()) return false;
    if (canvasObject->GetChildCount() < kBarCount) return false;

    for (int i = 0; i < kBarCount; ++i) {
        GameObject* child = canvasObject->GetChild(i);
        if (!child) return false;
        m_bars[static_cast<std::size_t>(i)] = EntityRef{ child->GetID() };
    }
    m_canvas = EntityRef{ canvasObject->GetID() };
    return true;
}

inline void AimMarkerComponent::Build()
{
    /// @note 対象はなぞるたびに乗り換わり、敵の剛体には物理補間も掛かる。親を張り替えると
    ///       枠の位置が親の変換と TransformSystem の巡回順に依存しずれの切り分けができない。
    GameObject& canvasObject = scene.Create(CanvasName());
    canvasObject.runtimeGenerated = true;
    UICanvas& canvas = canvasObject.AddComponent<UICanvas>();
    canvas.renderMode = UIRenderMode::WorldSpace;
    canvas.faceCamera = true;
    canvas.worldScale = 1.0f / kPixelsPerUnit;

    for (int i = 0; i < kBarCount; ++i) {
        GameObject& bar = scene.Create(kCanvasName + std::string("_Bar"));
        bar.runtimeGenerated = true;
        bar.SetParent(canvasObject);
        bar.AddComponent<UIImage>().sortOrder = 0;
        m_bars[static_cast<std::size_t>(i)] = EntityRef{ bar.GetID() };
    }

    m_canvas = EntityRef{ canvasObject.GetID() };
}

inline float AimMarkerComponent::FrameSize(GameObject& target) const
{
    const bodybounds::Extents extents = bodybounds::Of(target);
    if (!extents.measured) return std::max(fallbackBodySize, minSize);

    /// @note 体を包む一辺。高さと幅の大きい方を採らないと、縦長の相手で枠が胴体を切る。
    const float body = std::max(extents.top - extents.bottom, extents.radius * 2.0f);
    return std::max(body + padding * 2.0f, minSize);
}

inline Vector4 AimMarkerComponent::CurrentColor(GameObject& target) const
{
    (void)target;
    return markerColor;
}

inline void AimMarkerComponent::LayoutBars(float size)
{
    const float half  = size * 0.5f;
    const float width = Clamp(lineWidth * kPixelsPerUnit, 1.0f, half);
    /// @note 角の腕。half まで伸ばすと隣の角と繋がって閉じた四角形になる。
    const float arm   = Clamp(size * cornerRatio, width, half);

    /// @note UI 要素の position は矩形の左上、localScale.xy は倍率ではなく幅・高さ (どちらも
    ///       Canvas ピクセル)。原点は Canvas の左上で y は下向き。
    ///       角は (左/右) × (上/下) の 4 通りで、それぞれ横棒と縦棒の 2 本で L 字を作る。
    for (int corner = 0; corner < 4; ++corner) {
        const bool right  = (corner & 1) != 0;
        const bool bottom = (corner & 2) != 0;

        GameObject* horizontal = m_bars[static_cast<std::size_t>(corner * 2)].Resolve(scene);
        if (horizontal) {
            horizontal->transform.position = { right  ? size - arm   : 0.0f,
                                               bottom ? size - width : 0.0f, 0.0f };
            horizontal->transform.scale    = { arm, width, 1.0f };
        }

        GameObject* vertical = m_bars[static_cast<std::size_t>(corner * 2 + 1)].Resolve(scene);
        if (vertical) {
            vertical->transform.position = { right  ? size - width : 0.0f,
                                             bottom ? size - arm   : 0.0f, 0.0f };
            vertical->transform.scale    = { width, arm, 1.0f };
        }
    }
}

inline void AimMarkerComponent::OnLateUpdate()
{
    GameObject* canvasObject = m_canvas.Resolve(scene);
    if (!canvasObject) return;

    auto* aim = Aim();
    GameObject* target = aim ? aim->CurrentTarget() : nullptr;
    debugTargetName = target ? target->name : std::string{};

    auto* canvas = canvasObject->GetComponent<UICanvas>();
    if (!showAimMarker || !target) {
        if (canvas) canvas->enabled = false;
        m_lastTarget = {};
        return;
    }
    if (canvas) canvas->enabled = true;

    /// @note 乗り移った瞬間だけ大きく出す。同じ相手を指し続けている間は演出しない。
    if (target->GetID() != m_lastTarget) {
        m_lastTarget    = target->GetID();
        m_lockRemaining = std::max(lockSeconds, 0.0f);
    }

    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);
    m_lockRemaining = std::max(0.0f, m_lockRemaining - dt);

    /// @note 掴んだ直後は lockScale 倍から等倍へ縮む。ヒットストップ中でも縮み続けるよう、
    ///       時間は実時間で数える (止まった画面で枠だけ固まると、掴んだことが伝わらない)。
    const float lockProgress = lockSeconds > 0.0f
        ? Clamp01(1.0f - m_lockRemaining / lockSeconds) : 1.0f;
    const float lockFactor   = Lerp(std::max(lockScale, 1.0f), 1.0f, lockProgress);
    const float pulse = 1.0f + std::sin(Time::time * pulseHz * TWO_PI) * pulseDepth;

    const float size = FrameSize(*target) * lockFactor * pulse * kPixelsPerUnit;

    if (canvas) {
        canvas->canvasWidth  = size;
        canvas->canvasHeight = size;
    }

    /// @note Canvas はルートなので local = world。この関数は LateScript で走り UI 描画より前なので、
    ///       world まで書けば TransformSystem を待たずに同じフレームへ反映される。
    const Vector3 anchor = bodybounds::CenterWorld(*target, fallbackBodySize);
    canvasObject->transform.position      = anchor;
    canvasObject->transform.worldPosition = anchor;

    LayoutBars(size);

    const Vector4 color = CurrentColor(*target);
    for (const EntityRef& bar : m_bars)
        ui.SetImageColor(bar.Resolve(scene), color);
}

inline void AimMarkerComponent::OnDestroy()
{
    /// @note ルートに置いた以上、プレイヤーと一緒には消えない。持ち主が畳む。
    if (GameObject* canvasObject = m_canvas.Resolve(scene))
        scene.Destroy(*canvasObject);
    m_canvas = {};
    m_bars.fill(EntityRef{});
}

} // namespace sandbox
