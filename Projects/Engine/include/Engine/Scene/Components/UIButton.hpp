// FBZZ Engine
// UIButton.hpp | fbzz::scene
// ランタイム UI ボタンの状態コンポーネント
// ヒット判定結果と遷移状態を保持し、UISystem が入力から更新する。
// クリック時の処理は script / event 側へ接続する。
#pragma once
#include <Math/Vector4.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>

namespace fbzz::scene {

enum class UIButtonState {
    NORMAL,
    HOVERED,
    PRESSED
};

// 状態別スプライト欄が受け付ける拡張子 (ImageImporter が読める画像)。
inline constexpr const char* kSpriteFieldExtensions =
    ".fztex,.png,.jpg,.jpeg,.tga,.dds,.bmp";

struct UIButton {
    math::Vector4 normalColor   = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector4 hoverColor    = { 0.85f, 0.85f, 0.85f, 1.0f };
    math::Vector4 pressedColor  = { 0.7f, 0.7f, 0.7f, 1.0f };
    math::Vector4 disabledColor = { 0.5f, 0.5f, 0.5f, 0.5f };

    // 状態ごとの絵。同じ GameObject の UIImage.texturePath を差し替える。
    // Sprite サブアセット参照 ("<画像>.png::sprite::<id>") も入れられる。
    //
    // WHY 色の明暗と別に要るか:
    //   押した / 触れたを明るさだけで伝えられるのは、元の絵が中間の明るさで、
    //   かつ形が変わらない場合に限られる。枠が光る・アイコンが変わるといった
    //   絵そのものが違うボタンは、色の相対変換ではどうやっても作れない。
    //
    // WHY 空欄を「変えない」にするか:
    //   4 つ全部を必ず用意させると、押下だけ差し替えたいボタンでも同じ絵を
    //   3 回指定することになる。空欄は UIImage に元から入っている絵のまま。
    std::string normalSprite;
    std::string hoveredSprite;
    std::string pressedSprite;
    std::string disabledSprite;

    bool isInteractable = true;
    bool enabled = true;

    UIButtonState state = UIButtonState::NORMAL;
    // 以下の 3 フラグは UISystem が毎フレーム更新する 1 フレーム限定イベント。
    // Script の OnUpdate() 内で読み取り、次フレームには false に戻る。
    bool onClick = false;
    bool onEnter = false;
    bool onExit  = false;

    // ランタイム専用: シリアライズしない
    // WHY: 押下がこのボタン上で開始されたかを追跡し、ドラッグアウト後の
    //      誤 onClick 発火と、他要素から流入した押下の誤検出を防ぐ。
    bool wasPressedOnThis = false;
    bool lastMouseState   = false;

    const char* GetTypeName() const { return "UIButton"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",        enabled);
        r.Field("isInteractable", isInteractable);
        r.ColorField("normalColor",    normalColor);
        r.ColorField("hoverColor",     hoverColor);
        r.ColorField("pressedColor",   pressedColor);
        r.ColorField("disabledColor",  disabledColor);
        r.FileField("normalSprite",   normalSprite,   kSpriteFieldExtensions,
                    "状態ごとに UIImage の絵を差し替えます。空欄なら差し替えません");
        r.FileField("hoveredSprite",  hoveredSprite,  kSpriteFieldExtensions);
        r.FileField("pressedSprite",  pressedSprite,  kSpriteFieldExtensions);
        r.FileField("disabledSprite", disabledSprite, kSpriteFieldExtensions);
        static constexpr const char* kStateLabels[] = { "Normal", "Hovered", "Pressed" };
        const int stateIndex = static_cast<int>(state);
        r.Readonly("state", isInteractable && stateIndex >= 0 && stateIndex < 3
            ? std::string(kStateLabels[stateIndex])
            : std::string("Disabled"));
    }
};

} // namespace fbzz::scene
