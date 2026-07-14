// FBZZ Engine
// UIButton.hpp | fbzz::scene
// ランタイム UI ボタンの状態コンポーネント
// ヒット判定結果と遷移状態を保持し、UISystem が入力から更新する。
// クリック時の処理は script / event 側へ接続する。
#pragma once
#include <Math/Vector4.hpp>
#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

enum class UIButtonState {
    NORMAL,
    HOVERED,
    PRESSED
};

struct UIButton {
    math::Vector4 normalColor   = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector4 hoverColor    = { 0.85f, 0.85f, 0.85f, 1.0f };
    math::Vector4 pressedColor  = { 0.7f, 0.7f, 0.7f, 1.0f };
    math::Vector4 disabledColor = { 0.5f, 0.5f, 0.5f, 0.5f };
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
        r.Field("normalColor",    normalColor);
        r.Field("hoverColor",     hoverColor);
        r.Field("pressedColor",   pressedColor);
        r.Field("disabledColor",  disabledColor);
        static constexpr const char* kStateLabels[] = { "Normal", "Hovered", "Pressed" };
        const int stateIndex = static_cast<int>(state);
        r.Readonly("state", isInteractable && stateIndex >= 0 && stateIndex < 3
            ? std::string(kStateLabels[stateIndex])
            : std::string("Disabled"));
    }
};

} // namespace fbzz::scene
