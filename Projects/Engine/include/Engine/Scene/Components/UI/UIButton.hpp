/// @file    UIButton.hpp
/// @brief   ランタイム UI ボタンの状態コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// @note ヒット判定結果と遷移状態を保持し、UISystem が入力から更新する。
/// @note クリック時の処理は script / event 側へ接続する。
#pragma once
#include <Math/LinearAlgebra/Vector4.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <string>

namespace fbzz::scene {

enum class UIButtonState {
    NORMAL,
    HOVERED,
    PRESSED
};

/// @brief 状態別スプライト欄が受け付ける拡張子 (ImageImporter が読める画像)。
/// @note 先頭の .sprite は Sprite のコマも受ける印 (Editor の kSpriteAssetFilter と同じ規約)。
/// @note ここへ入った値は UIImage.texturePath へ渡るので、UIImage が受けるものと同じ集合になる。
inline constexpr const char* kSpriteFieldExtensions =
    ".sprite,.fztex,.png,.jpg,.jpeg,.tga,.dds,.bmp";

/// @brief UI の効果音欄が受け付ける拡張子 (AudioSourceComponent と同じ集合)。
inline constexpr const char* kUISoundFieldExtensions =
    ".wav,.mp3,.ogg,.flac,.synth";

struct UIButton {
    math::Vector4 normalColor   = { 1.0f, 1.0f, 1.0f, 1.0f }; ///< 通常状態でボタンのグラフィックに適用する RGBA 色。
    math::Vector4 hoverColor    = { 0.85f, 0.85f, 0.85f, 1.0f }; ///< ポインターが上にあるときにボタンのグラフィックへ適用する RGBA 色。
    math::Vector4 pressedColor  = { 0.7f, 0.7f, 0.7f, 1.0f }; ///< 押下中のボタンのグラフィックに適用する RGBA 色。
    math::Vector4 disabledColor = { 0.5f, 0.5f, 0.5f, 0.5f }; ///< 操作不能なボタンのグラフィックに適用する RGBA 色。

    /// @brief 状態ごとの絵。同じ GameObject の UIImage.texturePath を差し替える。
    /// @note Sprite サブアセット参照 (`<画像>.png::sprite::<id>`) も入れられる。
    /// @note 色の明暗と別に要る理由: 枠が光る・アイコンが変わるなど絵そのものが違うボタンは
    /// @note 色の相対変換では作れない。空欄は「変えない」の意味で、押下だけ差し替えたい
    /// @note ボタンでも同じ絵を 3 回指定しなくて済む。
    std::string normalSprite; ///< 通常のボタン状態で選択するスプライト参照。
    std::string hoveredSprite; ///< ポインターが上にあるときに選択するスプライト参照。
    std::string pressedSprite; ///< ボタンを押している間に選択するスプライト参照。
    std::string disabledSprite; ///< 操作不能なボタンに選択するスプライト参照。

    /// @brief 状態が変わった瞬間に鳴らす音。空欄なら鳴らさない。
    /// @note 「触れたら鳴る・押したら鳴る」は全ボタン共通の挙動のため、スクリプトへ分散させず
    /// @note UIAudioSystem に集約する。.synth も受け付けるのは UI 効果音が短い手続き音で
    /// @note 足りることが多いため。
    /// @see Docs/design/audio-system.md
    std::string hoverSound; ///< ポインターがボタンへ入ったときのサウンド参照。
    std::string clickSound; ///< クリックが成立したときに再生するサウンドの参照。

    bool isInteractable = true; ///< 入力を受け取り、操作可能なボタンとして表示するかどうか。
    bool enabled = true; ///< このボタンを入力と表示状態の更新対象にするかどうか。

    UIButtonState state = UIButtonState::NORMAL; ///< 直近の入力判定で確定したボタンの表示状態。
    /// @note 以下の 3 フラグは UISystem が毎フレーム更新する 1 フレーム限定イベント。Script の
    /// @note OnUpdate() 内で読み取り、次フレームには false に戻る。
    bool onClick = false; ///< 今回の入力更新でクリックが成立したかどうか。
    bool onEnter = false; ///< 今回の入力更新でポインターがボタン内に入ったかどうか。
    bool onExit  = false; ///< 今回の入力更新でポインターがボタンから出たかどうか。

    /// @note ランタイム専用 (シリアライズしない)。押下がこのボタン上で開始されたかを追跡し、
    /// @note ドラッグアウト後の誤 onClick 発火と、他要素から流入した押下の誤検出を防ぐ。
    bool wasPressedOnThis = false; ///< 現在のマウス押下がこのボタンで始まったかどうか。
    bool lastMouseState   = false; ///< 押下と解放を検出するために保持する前回のマウスボタン状態。

    bool overrideThemeColors = false; ///< テーマのボタン色の代わりに、このボタンの色設定を使うかどうか。

    const char* GetTypeName() const { return "UIButton"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",        enabled);
        r.Field("isInteractable", isInteractable);
        r.Field("overrideThemeColors", overrideThemeColors);
        r.Tooltip("Use this button's local state colors instead of the Canvas theme.");
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

}
