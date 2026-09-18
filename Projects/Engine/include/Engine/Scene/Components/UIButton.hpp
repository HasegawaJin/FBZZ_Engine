/// @file    UIButton.hpp
/// @brief   ランタイム UI ボタンの状態コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// ヒット判定結果と遷移状態を保持し、UISystem が入力から更新する。
/// クリック時の処理は script / event 側へ接続する。
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

/// @brief 状態別スプライト欄が受け付ける拡張子 (ImageImporter が読める画像)。
/// @note 先頭の .sprite は Sprite のコマも受ける印 (Editor の kSpriteAssetFilter と同じ規約)。
///       ここへ入った値は UIImage.texturePath へ渡るので、UIImage が受けるものと同じ集合になる。
inline constexpr const char* kSpriteFieldExtensions =
    ".sprite,.fztex,.png,.jpg,.jpeg,.tga,.dds,.bmp";

/// @brief UI の効果音欄が受け付ける拡張子 (AudioSourceComponent と同じ集合)。
inline constexpr const char* kUISoundFieldExtensions =
    ".wav,.mp3,.ogg,.flac,.synth";

struct UIButton {
    math::Vector4 normalColor   = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector4 hoverColor    = { 0.85f, 0.85f, 0.85f, 1.0f };
    math::Vector4 pressedColor  = { 0.7f, 0.7f, 0.7f, 1.0f };
    math::Vector4 disabledColor = { 0.5f, 0.5f, 0.5f, 0.5f };

    /// @brief 状態ごとの絵。同じ GameObject の UIImage.texturePath を差し替える。
    ///        Sprite サブアセット参照 (`<画像>.png::sprite::<id>`) も入れられる。
    /// @note 色の明暗と別に要る理由: 枠が光る・アイコンが変わるなど絵そのものが違うボタンは
    ///       色の相対変換では作れない。空欄は「変えない」の意味で、押下だけ差し替えたい
    ///       ボタンでも同じ絵を 3 回指定しなくて済む。
    std::string normalSprite;
    std::string hoveredSprite;
    std::string pressedSprite;
    std::string disabledSprite;

    /// @brief 状態が変わった瞬間に鳴らす音。空欄なら鳴らさない。
    /// @note 「触れたら鳴る・押したら鳴る」は全ボタン共通の挙動のため、スクリプトへ分散させず
    ///       UIAudioSystem に集約する。.synth も受け付けるのは UI 効果音が短い手続き音で
    ///       足りることが多いため。
    /// @see Docs/design/audio-system.md
    std::string hoverSound;
    std::string clickSound;

    bool isInteractable = true;
    bool enabled = true;

    UIButtonState state = UIButtonState::NORMAL;
    /// @note 以下の 3 フラグは UISystem が毎フレーム更新する 1 フレーム限定イベント。Script の
    ///       OnUpdate() 内で読み取り、次フレームには false に戻る。
    bool onClick = false;
    bool onEnter = false;
    bool onExit  = false;

    /// @note ランタイム専用 (シリアライズしない)。押下がこのボタン上で開始されたかを追跡し、
    ///       ドラッグアウト後の誤 onClick 発火と、他要素から流入した押下の誤検出を防ぐ。
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
        r.Group("Sound");
        r.FileField("hoverSound", hoverSound, kUISoundFieldExtensions,
                    "カーソルが乗った瞬間に鳴らします。空欄で無音");
        r.FileField("clickSound", clickSound, kUISoundFieldExtensions,
                    "クリックが成立した瞬間に鳴らします。空欄で無音");
        static constexpr const char* kStateLabels[] = { "Normal", "Hovered", "Pressed" };
        const int stateIndex = static_cast<int>(state);
        r.Readonly("state", isInteractable && stateIndex >= 0 && stateIndex < 3
            ? std::string(kStateLabels[stateIndex])
            : std::string("Disabled"));
    }
};

} // namespace fbzz::scene
