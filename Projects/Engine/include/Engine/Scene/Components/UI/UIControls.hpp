/// @file    UIControls.hpp
/// @brief   Slider、Toggle、Scroll、Mask、Text入力、汎用Pointer EventのUI Component。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <Engine/Scene/References/EntityRef.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector2.hpp>
#include <cstddef>
#include <string>

namespace fbzz::scene {

struct UISlider {
    bool enabled = true; ///< スライダーを入力処理の対象にするかどうか。
    bool interactable = true; ///< スライダーをユーザーが操作できるかどうか。
    float minimum = 0.0f; ///< スライダーが選択できる値の下限。
    float maximum = 1.0f; ///< スライダーが選択できる値の上限。
    float value = 0.0f; ///< スライダーで現在選択されている値。
    bool wholeNumbers = false; ///< スライダーが返す値を整数に揃えるかどうか。
    bool vertical = false; ///< スライダーの操作軸を Y にするかどうか。false は X。
    bool onValueChanged = false; ///< 今回の入力処理でスライダー値が変わったかどうか。
    bool runtimeDragging = false; ///< ポインター操作でスライダーをドラッグ中かどうか。
    bool runtimeLastMouse = false; ///< スライダーの押下と解放の判定に使う前回のマウスボタン状態。

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
    bool enabled = true; ///< トグルを入力処理の対象にするかどうか。
    bool interactable = true; ///< トグルをユーザーが操作できるかどうか。
    bool isOn = false; ///< トグルで現在オンが選択されているかどうか。
    bool onValueChanged = false; ///< 今回の入力処理でトグルの選択が変わったかどうか。
    bool runtimePressedHere = false; ///< 押してから離した操作を判定するため、押下がこのトグルで始まったかを保持する。
    bool runtimeLastMouse = false; ///< トグルの押下と解放の判定に使う前回のマウスボタン状態。

    const char* GetTypeName() const { return "UI Toggle"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("interactable", interactable);
        r.Field("isOn", isOn);
    }
};

struct UIScrollView {
    bool enabled = true; ///< スクロール領域を入力とスクロール更新の対象にするかどうか。
    bool horizontal = false; ///< スクロール領域を X 方向へ移動できるかどうか。
    bool vertical = true; ///< スクロール領域を Y 方向へ移動できるかどうか。
    math::Vector2 contentSize = { 100.0f, 100.0f }; ///< スクロールする内容全体の幅と高さ。表示領域と同じ座標単位。
    math::Vector2 scrollPosition = math::Vector2::ZERO; ///< 内容部分の現在のスクロールオフセット。
    float sensitivity = 24.0f; ///< 入力からスクロール移動量への変換倍率。
    bool inertia = true; ///< 入力が終わった後もスクロール速度を減衰させながら移動するかどうか。
    float decelerationRate = 8.0f; ///< 慣性スクロールの速度を時間に応じて減衰させる係数。
    math::Vector2 velocity = math::Vector2::ZERO; ///< 慣性スクロールで次の入力更新へ持ち越す X、Y の速度。
    bool onValueChanged = false; ///< 今回の入力処理でスクロール位置が変わったかどうか。

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
    bool enabled = true; ///< この UI 要素をマスク領域として処理するかどうか。
    bool showMaskGraphic = true; ///< 子をクリップするマスク自身のグラフィックも描画するかどうか。
    bool affectChildren = true; ///< 子孫へマスクのクリップ領域を適用するかどうか。

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
    bool enabled = true; ///< テキスト入力を処理するかどうか。
    bool interactable = true; ///< テキスト入力欄にフォーカスして編集できるかどうか。
    std::string text; ///< 現在入力されている UTF-8 文字列。
    std::string placeholder; ///< 入力が空のときに表示する案内文字列。
    int characterLimit = 0; ///< 入力を受け付ける文字数の上限。0 は無制限。
    UIInputContentType contentType = UIInputContentType::Standard; ///< 入力の種類に応じて文字の受け付け方や表示を選ぶ。
    bool multiline = false; ///< 改行を入力し、複数行にできるかどうか。
    bool focused = false; ///< 現在キーボード入力を受け取るフォーカスがあるかどうか。
    bool onValueChanged = false; ///< 今回の入力処理で文字列が変わったかどうか。
    bool onSubmit = false; ///< 今回の入力処理で確定操作が発生したかどうか。
    std::size_t caretPosition = 0; ///< UTF-8 の text で次に編集する位置を示すバイトオフセット。

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

/// @note キーボード / ゲームパッドでフォーカスを渡り歩くための印。
/// @note 専用の選択状態を作らずポインターを動かす形にするのは、UIButton / UISlider / UIToggle が
/// @note すべて「Canvas 空間の座標 1 つと押下状態」しか見ていないため (UIPointer.hpp)。方向キーで
/// @note ポインターを要素の中心へ瞬間移動させれば、既存ウィジェットは 1 行も変えずに操作できる。
/// @note Canvas 側 (navigationEnabled) で有効化するのは、方向キーがフォーカス移動へ吸われると
/// @note ポインター操作前提で作った既存画面の挙動が変わるため。
enum class UINavigationMode {
    Automatic, ///< @note 方向ごとに、その向きにある最も近い要素へ移る
    Explicit,  ///< @note up / down / left / right の指定先だけへ移る
    None,      ///< @note 移動先にならない (この要素で行き止まり)
};

struct UINavigation {
    bool enabled = true; ///< この要素を方向入力によるフォーカス移動の対象にするかどうか。
    UINavigationMode mode = UINavigationMode::Automatic; ///< フォーカス移動先を自動で探すか、明示参照を使うか。
    EntityRef up{}; ///< 上方向入力でフォーカスを移す先のシーン参照。
    EntityRef down{}; ///< 下方向入力でフォーカスを移す先のシーン参照。
    EntityRef left{}; ///< 左方向入力でフォーカスを移す先のシーン参照。
    EntityRef right{}; ///< 右方向入力でフォーカスを移す先のシーン参照。
    bool selectOnStart = false; ///< この Canvas でまだフォーカスが決まっていないとき、最初に置く先。

    bool focused = false; ///< ランタイム専用: いまフォーカスが乗っているか。見た目を変えたい側が読む。

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

/// @note 掴んで運べる要素。
/// @note UIEventTrigger はドラッグの「量」までしか出さない。運んだ結果として要るのは
/// @note 「どこへ落ちたか」で、落とした瞬間にポインター下の受け皿を全要素の矩形から
/// @note 探して初めて分かる (それを知っているのは UISystem だけ)。
struct UIDragSource {
    bool enabled = true; ///< この要素からドラッグを開始できるかどうか。
    /// @note ドラッグ中に要素自身をポインターへ追従させる。
    /// @note false なら位置は動かさず状態フラグだけが立つ (ゴーストを別に出す構成用)。
    bool moveWithPointer = true; ///< ドラッグ中に対象の要素をポインターへ追従させるかどうか。
    bool returnOnDrop = true; ///< 受け皿が見つからなかったとき、掴む前の位置へ戻す。
    std::string payload; ///< 受け皿が見る識別子。空なら種類なし。

    bool dragging = false; ///< ランタイム専用
    bool dropped = false; ///< 1 フレーム限定。落ちた先は droppedOn (無効なら受け皿なし)。
    EntityRef droppedOn{}; ///< 直近にドロップを受け付けた UI 要素の参照。
    math::Vector2 originPosition = math::Vector2::ZERO; ///< ドラッグ開始前の要素の位置。戻す場合に参照する。
    math::Vector2 grabOffset = math::Vector2::ZERO; ///< ポインターに対して要素のどこをつかんだかを表すオフセット。
    bool runtimeLastMouse = false; ///< ドラッグの開始と終了を判定するための前回のマウスボタン状態。

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

/// @note 落とし先。
struct UIDropTarget {
    bool enabled = true; ///< この要素がドラッグ元からのドロップを受け付けるかどうか。
    std::string accepts; ///< 受け付ける payload。空なら何でも受ける。

    /// @note ランタイム専用
    /// @note ドラッグ中の要素がこの上にあるか (見た目を変える側が読む)。
    bool hovered = false; ///< 対応するドラッグが現在このドロップ先の内側にあるかどうか。
    bool received = false; ///< 1 フレーム限定。受け取った直後に立つ。
    EntityRef receivedFrom{}; ///< 直近に受け入れたドロップのドラッグ元の参照。
    std::string receivedPayload; ///< 直近に受け入れたドロップから受け取った文字列データ。

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
    bool enabled = true; ///< この要素でポインターイベントの状態を更新するかどうか。
    bool pointerEnter = false; ///< 今回の入力処理でポインターが要素内に入ったかどうか。
    bool pointerExit = false; ///< 今回の入力処理でポインターが要素外に出たかどうか。
    bool pointerDown = false; ///< 今回の入力処理で要素上で押下を始めたかどうか。
    bool pointerUp = false; ///< 今回の入力処理で要素に対応する押下を解放したかどうか。
    bool pointerClick = false; ///< 今回の入力処理で要素上のクリックが成立したかどうか。
    bool beginDrag = false; ///< 今回の入力処理でドラッグを開始したかどうか。
    bool drag = false; ///< 今回の入力処理でドラッグ中かどうか。
    bool endDrag = false; ///< 今回の入力処理でドラッグを終了したかどうか。
    math::Vector2 pointerPosition = math::Vector2::ZERO; ///< 今回のポインター位置。キャンバス空間。
    math::Vector2 dragDelta = math::Vector2::ZERO; ///< 前回から今回までのポインターの変位。キャンバス空間。
    bool runtimeHovered = false; ///< 要素内外への出入りを検出するための直近のホバー状態。
    bool runtimePressedHere = false; ///< 押下がこの要素で始まり、解放やクリックを通知する対象かどうか。
    bool runtimeLastMouse = false; ///< 押下と解放の検出に使う前回のマウスボタン状態。
    math::Vector2 runtimeLastPointer = math::Vector2::ZERO; ///< 移動差分を求めるために保存した前回のキャンバス空間のポインター位置。

    const char* GetTypeName() const { return "UI Event Trigger"; }
    void Reflect(IReflector& r) { r.Field("enabled", enabled); }
};

}
