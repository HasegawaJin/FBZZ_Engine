/// @file    UIControls.hpp
/// @brief   Slider、Toggle、Scroll、Mask、Text入力、汎用Pointer EventのUI Component。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <cstddef>
#include <string>

namespace fbzz::scene {

struct UISlider {
    bool enabled = true;
    bool interactable = true;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float value = 0.0f;
    bool wholeNumbers = false;
    bool vertical = false;
    bool onValueChanged = false;
    bool runtimeDragging = false;
    bool runtimeLastMouse = false;

    const char* GetTypeName() const { return "UI Slider"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("interactable", interactable);
        r.Field("minimum", minimum);
        r.Field("maximum", maximum);
        r.Field("value", value);
        r.Field("wholeNumbers", wholeNumbers);
        r.Field("vertical", vertical);
    }
};

struct UIToggle {
    bool enabled = true;
    bool interactable = true;
    bool isOn = false;
    bool onValueChanged = false;
    bool runtimePressedHere = false;
    bool runtimeLastMouse = false;

    const char* GetTypeName() const { return "UI Toggle"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("interactable", interactable);
        r.Field("isOn", isOn);
    }
};

struct UIScrollView {
    bool enabled = true;
    bool horizontal = false;
    bool vertical = true;
    math::Vector2 contentSize = { 100.0f, 100.0f };
    math::Vector2 scrollPosition = math::Vector2::ZERO;
    float sensitivity = 24.0f;
    bool inertia = true;
    float decelerationRate = 8.0f;
    math::Vector2 velocity = math::Vector2::ZERO;
    bool onValueChanged = false;

    const char* GetTypeName() const { return "UI Scroll View"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("horizontal", horizontal);
        r.Field("vertical", vertical);
        r.Field("contentSize", contentSize);
        r.Field("scrollPosition", scrollPosition);
        r.Field("sensitivity", sensitivity);
        r.Field("inertia", inertia);
        r.Field("decelerationRate", decelerationRate);
    }
};

struct UIMask {
    bool enabled = true;
    bool showMaskGraphic = true;
    bool affectChildren = true;

    const char* GetTypeName() const { return "UI Mask"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("showMaskGraphic", showMaskGraphic);
        r.Field("affectChildren", affectChildren);
    }
};

enum class UIInputContentType : int { Standard = 0, Integer = 1, Decimal = 2, Password = 3 };

struct UIInputField {
    bool enabled = true;
    bool interactable = true;
    std::string text;
    std::string placeholder;
    int characterLimit = 0;
    UIInputContentType contentType = UIInputContentType::Standard;
    bool multiline = false;
    bool focused = false;
    bool onValueChanged = false;
    bool onSubmit = false;
    std::size_t caretPosition = 0;

    const char* GetTypeName() const { return "UI Input Field"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("interactable", interactable);
        r.Field("text", text);
        r.Field("placeholder", placeholder);
        r.Field("characterLimit", characterLimit);
        int value = static_cast<int>(contentType);
        static constexpr const char* TYPES[] = { "Standard", "Integer", "Decimal", "Password" };
        r.Enum("contentType", value, TYPES);
        contentType = static_cast<UIInputContentType>(value < 0 || value > 3 ? 0 : value);
        r.Field("multiline", multiline);
        r.Readonly("focused", focused);
    }
};

/// キーボード / ゲームパッドでフォーカスを渡り歩くための印。
/// @note 専用の選択状態を作らずポインターを動かす形にするのは、UIButton / UISlider / UIToggle が
///       すべて「Canvas 空間の座標 1 つと押下状態」しか見ていないため (UIPointer.hpp)。方向キーで
///       ポインターを要素の中心へ瞬間移動させれば、既存ウィジェットは 1 行も変えずに操作できる。
/// @note Canvas 側 (navigationEnabled) で有効化するのは、方向キーがフォーカス移動へ吸われると
///       ポインター操作前提で作った既存画面の挙動が変わるため。
enum class UINavigationMode {
    Automatic, ///< 方向ごとに、その向きにある最も近い要素へ移る
    Explicit,  ///< up / down / left / right の指定先だけへ移る
    None,      ///< 移動先にならない (この要素で行き止まり)
};

struct UINavigation {
    bool enabled = true;
    UINavigationMode mode = UINavigationMode::Automatic;
    EntityRef up{};
    EntityRef down{};
    EntityRef left{};
    EntityRef right{};
    /// この Canvas でまだフォーカスが決まっていないとき、最初に置く先。
    bool selectOnStart = false;

    /// ランタイム専用: いまフォーカスが乗っているか。見た目を変えたい側が読む。
    bool focused = false;

    const char* GetTypeName() const { return "UI Navigation"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        static constexpr const char* kModeLabels[] = { "Automatic", "Explicit", "None" };
        int modeInt = static_cast<int>(mode);
        r.Enum("mode", modeInt, kModeLabels);
        modeInt = (modeInt < 0 || modeInt > 2) ? 0 : modeInt;
        mode = static_cast<UINavigationMode>(modeInt);
        r.FieldIf("up",    up,    mode == UINavigationMode::Explicit);
        r.FieldIf("down",  down,  mode == UINavigationMode::Explicit);
        r.FieldIf("left",  left,  mode == UINavigationMode::Explicit);
        r.FieldIf("right", right, mode == UINavigationMode::Explicit);
        r.Field("selectOnStart", selectOnStart);
        r.Readonly("focused", std::string(focused ? "yes" : "no"));
    }
};

/// 掴んで運べる要素。
/// @note UIEventTrigger はドラッグの「量」までしか出さない。運んだ結果として要るのは
///       「どこへ落ちたか」で、落とした瞬間にポインター下の受け皿を全要素の矩形から
///       探して初めて分かる (それを知っているのは UISystem だけ)。
struct UIDragSource {
    bool enabled = true;
    /// ドラッグ中に要素自身をポインターへ追従させる。
    /// false なら位置は動かさず状態フラグだけが立つ (ゴーストを別に出す構成用)。
    bool moveWithPointer = true;
    /// 受け皿が見つからなかったとき、掴む前の位置へ戻す。
    bool returnOnDrop = true;
    /// 受け皿が見る識別子。空なら種類なし。
    std::string payload;

    /// ランタイム専用
    bool dragging = false;
    /// 1 フレーム限定。落ちた先は droppedOn (無効なら受け皿なし)。
    bool dropped = false;
    EntityRef droppedOn{};
    math::Vector2 originPosition = math::Vector2::ZERO;
    math::Vector2 grabOffset = math::Vector2::ZERO;
    bool runtimeLastMouse = false;

    const char* GetTypeName() const { return "UI Drag Source"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("moveWithPointer", moveWithPointer);
        r.Tooltip("ドラッグ中に要素自身をポインターへ追従させます");
        r.Field("returnOnDrop", returnOnDrop);
        r.Tooltip("受け皿が無かったとき、掴む前の位置へ戻します");
        r.Field("payload", payload);
        r.Tooltip("受け皿の accepts と突き合わせる識別子。空なら種類なし");
    }
};

/// 落とし先。
struct UIDropTarget {
    bool enabled = true;
    /// 受け付ける payload。空なら何でも受ける。
    std::string accepts;

    /// ランタイム専用
    /// ドラッグ中の要素がこの上にあるか (見た目を変える側が読む)。
    bool hovered = false;
    /// 1 フレーム限定。受け取った直後に立つ。
    bool received = false;
    EntityRef receivedFrom{};
    std::string receivedPayload;

    const char* GetTypeName() const { return "UI Drop Target"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("accepts", accepts);
        r.Tooltip("受け付ける payload。空なら何でも受けます");
        r.Readonly("hovered", std::string(hovered ? "yes" : "no"));
    }
};

struct UIEventTrigger {
    bool enabled = true;
    bool pointerEnter = false;
    bool pointerExit = false;
    bool pointerDown = false;
    bool pointerUp = false;
    bool pointerClick = false;
    bool beginDrag = false;
    bool drag = false;
    bool endDrag = false;
    math::Vector2 pointerPosition = math::Vector2::ZERO;
    math::Vector2 dragDelta = math::Vector2::ZERO;
    bool runtimeHovered = false;
    bool runtimePressedHere = false;
    bool runtimeLastMouse = false;
    math::Vector2 runtimeLastPointer = math::Vector2::ZERO;

    const char* GetTypeName() const { return "UI Event Trigger"; }
    void Reflect(IReflector& r) { r.Field("enabled", enabled); }
};

} // namespace fbzz::scene
