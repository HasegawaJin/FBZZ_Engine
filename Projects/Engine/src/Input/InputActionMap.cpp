/// @file    InputActionMap.cpp
/// @brief   アクション/軸の評価、デッドゾーン処理、平滑化、リバインド。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include "Engine/Input/InputActionMap.hpp"
#include "Engine/Input/Input.hpp"
#include "Engine/Input/Gamepad.hpp"
#include "Engine/Input/KeyCode.hpp"
#include <Engine/Core/Platform/Cursor.hpp>
#include "GameInputConsumption.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <unordered_map>

namespace fbzz::input {

namespace {

/// @note バインドは仮想キーコードの生値で持つ。KeyCode の値は VK_* と一致する。
constexpr uint32_t Vk(KeyCode key) noexcept
{
    return static_cast<uint32_t>(key);
}

/// @brief std::string_view で unordered_map を引くための透過ハッシュ。
/// @note GetAction は 1 フレームに数十回呼ばれ、毎回 std::string を構築するとヒープ確保が
/// @note ホットパスに乗るため、C++20 の異種検索でこれを回避する。
struct TransparentStringHash {
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }
    [[nodiscard]] size_t operator()(const std::string& value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }
};

using NameIndexMap =
    std::unordered_map<std::string, uint16_t, TransparentStringHash, std::equal_to<>>;

struct ActionRuntime {
    bool current = false; ///< 今フレームにいずれかのバインドが押されているか。
    bool previous = false; ///< 前フレームの押下状態。開始・解除のエッジを求める基準。
};

struct AxisRuntime {
    float rawAnalog = 0.0f; ///< デッドゾーン適用前のアナログ生値
    float keyValue  = 0.0f; ///< デジタル入力を平滑化した値
    float value     = 0.0f; ///< 最終値
};

/// @brief ユーザーが入力を割り当てている間に保持する対象と待機状態。
struct RebindState {
    bool active = false; ///< 次に押された入力を待っているか。
    bool isAxis = false; ///< 変更対象が軸か。false はアクション。
    std::string targetName; ///< マップ内で変更対象を引くアクション名または軸名。
    int slot = 0; ///< 軸内のバインド種別。0 は正方向、1 は負方向、2 はアナログ。
    int bindingIndex = 0; ///< 変更対象のバインド配列内で置き換える添字。
    int skipFrames = 1; ///< 開始操作のクリックを捕捉対象から外すため入力を待つ残りフレーム数。
};

struct MapContext {
    bool initialized = false; ///< 入力定義を読み込み、名前の索引を構築済みか。
    std::vector<InputAction> actions; ///< 現在のアクション定義と入力バインド。
    std::vector<InputAxis> axes; ///< 現在の軸定義と方向・アナログの入力バインド。

    std::vector<ActionRuntime> actionRuntime; ///< actions と同じ添字で引く押下・解除の状態。
    std::vector<AxisRuntime> axisRuntime; ///< axes と同じ添字で引く評価値と平滑化の履歴。

    NameIndexMap actionIndex; ///< アクション名から actions の添字を引く索引。
    NameIndexMap axisIndex; ///< 軸名から axes の添字を引く索引。

    RebindState rebind; ///< 現在の入力割り当て要求と残り待機時間。
    bool rebindCompleted = false; ///< 今フレームに新しいバインドを取り込んだか。

    bool enabled = true; ///< 通常のアクションと軸をフレームごとに評価するか。
};

/// @brief fbzz_engine の状態を保持する関数内 static。
/// @note SHARED ライブラリの WINDOWS_EXPORT_ALL_SYMBOLS はデータシンボルを自動エクスポートしない
/// @note ため、状態への参照は必ずエクスポート済み関数を経由させる (AssetManager と同方針)。
MapContext& Ctx()
{
    static MapContext context;
    return context;
}

/// @note バインドが指すゲームパッドのスロットを解決する。
/// @note -1 (既定) は「接続中の最初のパッド」。1 台も無い場合は 0 を返す
/// @note (未接続スロットへの問い合わせは false / 0 を返すため無害)。
int ResolvePad(int padIndex)
{
    if (padIndex >= 0) {
        return padIndex;
    }
    const int first = Gamepad::GetFirstConnectedPad();
    return first >= 0 ? first : 0;
}

float MouseAxisValue(uint32_t code)
{
    switch (static_cast<MouseAxis>(code)) {
    case MouseAxis::DELTA_X: return input_detail::GameInputAccess::MouseDelta().x;
    case MouseAxis::DELTA_Y: return input_detail::GameInputAccess::MouseDelta().y;
    case MouseAxis::SCROLL:  return input_detail::GameInputAccess::MouseScrollDelta();
    default:                 return 0.0f;
    }
}

/// @note バインドをアナログ値として評価する。デジタルソースは押下時に scale を返す。
float EvaluateAnalog(const InputBinding& binding)
{
    const auto& consumed = input_detail::GameInputConsumptionState();
    switch (binding.source) {
    case BindingSource::KEY:
        return input_detail::GameInputAccess::KeyHeld(static_cast<KeyCode>(binding.code)) ? binding.scale : 0.0f;
    case BindingSource::MOUSE_BUTTON:
        return input_detail::GameInputAccess::MouseButton(static_cast<int>(binding.code)) ? binding.scale : 0.0f;
    case BindingSource::GAMEPAD_BUTTON:
        return core::Cursor::CanReadGameInput() && !consumed.PadButtonBlocked(ResolvePad(binding.padIndex), binding.code)
            && Gamepad::ButtonHeld(static_cast<GamepadButton>(binding.code),
                                   ResolvePad(binding.padIndex))
                   ? binding.scale : 0.0f;
    case BindingSource::GAMEPAD_AXIS:
        return !core::Cursor::CanReadGameInput() || consumed.PadAxisBlocked(ResolvePad(binding.padIndex), binding.code) ? 0.0f
            : Gamepad::Axis(static_cast<GamepadAxis>(binding.code), ResolvePad(binding.padIndex)) * binding.scale;
    case BindingSource::MOUSE_AXIS:
        return MouseAxisValue(binding.code) * binding.scale;
    default:
        return 0.0f;
    }
}

/// @note バインドをボタンとして評価する (押されているか)。
bool EvaluateHeld(const InputBinding& binding)
{
    const auto& consumed = input_detail::GameInputConsumptionState();
    switch (binding.source) {
    case BindingSource::KEY:
        return input_detail::GameInputAccess::KeyHeld(static_cast<KeyCode>(binding.code));
    case BindingSource::MOUSE_BUTTON:
        return input_detail::GameInputAccess::MouseButton(static_cast<int>(binding.code));
    case BindingSource::GAMEPAD_BUTTON:
        return core::Cursor::CanReadGameInput() && !consumed.PadButtonBlocked(ResolvePad(binding.padIndex), binding.code)
            && Gamepad::ButtonHeld(static_cast<GamepadButton>(binding.code), ResolvePad(binding.padIndex));
    case BindingSource::GAMEPAD_AXIS:
    case BindingSource::MOUSE_AXIS:
        // 軸をボタンとして使う場合、scale を掛けた値が閾値を超えたら押下とみなす。
        // scale が -1 なら負方向に倒したときに発火する。
        return EvaluateAnalog(binding) >= binding.buttonThreshold;
    default:
        return false;
    }
}

/// @note current を target へ maxDelta だけ近づける。
float MoveTowards(float current, float target, float maxDelta)
{
    const float diff = target - current;
    if (std::fabs(diff) <= maxDelta) {
        return target;
    }
    return current + (diff > 0.0f ? maxDelta : -maxDelta);
}

/// @brief デッドゾーンを適用し、残りの範囲を 0..1 へ線形に引き伸ばす。
/// @note 単に切り捨てるだけだと、デッドゾーンを抜けた瞬間に値が 0 から deadZone へ不連続に飛び、
/// @note 動き出しがカクつくため引き伸ばす。
float ApplyDeadZone(float value, float deadZone)
{
    const float magnitude = std::fabs(value);
    if (magnitude <= deadZone) {
        return 0.0f;
    }

    const float range = std::max(1.0f - deadZone, 0.0001f);
    const float remapped = std::min((magnitude - deadZone) / range, 1.0f);
    return value < 0.0f ? -remapped : remapped;
}

/// @brief 複数のアナログバインドのうち、絶対値が最大のものを採用する。
/// @note 左スティックとマウスを同時に定義した軸で両方が半分ずつ入力されると、加算では合計が
/// @note 1 を超えて暴れる。「実際に動かしている側が勝つ」が自然なため最大値を採る。
float SelectStrongestAnalog(const std::vector<InputBinding>& bindings)
{
    float best = 0.0f;
    for (const InputBinding& binding : bindings) {
        const float value = EvaluateAnalog(binding);
        if (std::fabs(value) > std::fabs(best)) {
            best = value;
        }
    }
    return best;
}

bool AnyHeld(const std::vector<InputBinding>& bindings)
{
    for (const InputBinding& binding : bindings) {
        if (EvaluateHeld(binding)) {
            return true;
        }
    }
    return false;
}

void RebuildIndex()
{
    MapContext& context = Ctx();

    context.actionIndex.clear();
    for (size_t i = 0; i < context.actions.size(); ++i) {
        context.actionIndex[context.actions[i].name] = static_cast<uint16_t>(i);
    }
    context.actionRuntime.resize(context.actions.size());

    context.axisIndex.clear();
    for (size_t i = 0; i < context.axes.size(); ++i) {
        context.axisIndex[context.axes[i].name] = static_cast<uint16_t>(i);
    }
    context.axisRuntime.resize(context.axes.size());
}

const ActionRuntime* FindActionRuntime(std::string_view name)
{
    const MapContext& context = Ctx();
    const auto it = context.actionIndex.find(name);
    if (it == context.actionIndex.end()) {
        return nullptr;
    }
    if (it->second >= context.actionRuntime.size()) {
        return nullptr;
    }
    return &context.actionRuntime[it->second];
}

const AxisRuntime* FindAxisRuntime(std::string_view name)
{
    const MapContext& context = Ctx();
    const auto it = context.axisIndex.find(name);
    if (it == context.axisIndex.end()) {
        return nullptr;
    }
    if (it->second >= context.axisRuntime.size()) {
        return nullptr;
    }
    return &context.axisRuntime[it->second];
}

/// @name リバインド用: 今フレーム新たに押された入力を 1 つ探す
bool CaptureFirstPressedInput(InputBinding& out)
{
    // キーボード / マウスボタン。
    // ESC はリバインドの取消に予約するため捕捉しない。
    for (uint32_t code = 1; code < 256; ++code) {
        if (code == Vk(KeyCode::ESCAPE)) {
            continue;
        }
        if (!Input::KeyDown(static_cast<KeyCode>(code))) {
            continue;
        }

        // マウスボタンはキー配列にも入るため、マウスボタンとして記録する。
        if (code == Vk(KeyCode::MouseLeft) || code == Vk(KeyCode::MouseRight)
            || code == Vk(KeyCode::MouseMiddle)) {
            out = InputBinding{};
            out.source = BindingSource::MOUSE_BUTTON;
            out.code   = code == Vk(KeyCode::MouseLeft)  ? 0u
                       : (code == Vk(KeyCode::MouseRight) ? 1u : 2u);
            return true;
        }
        out = InputBinding{};
        out.source = BindingSource::KEY;
        out.code   = code;
        return true;
    }

    // ゲームパッドのボタン。
    for (int pad = 0; pad < Gamepad::MAX_PADS; ++pad) {
        if (!Gamepad::IsConnected(pad)) {
            continue;
        }
        for (uint16_t index = 0; index < static_cast<uint16_t>(GamepadButton::COUNT); ++index) {
            if (!Gamepad::ButtonDown(static_cast<GamepadButton>(index), pad)) {
                continue;
            }
            out = InputBinding{};
            out.source   = BindingSource::GAMEPAD_BUTTON;
            out.code     = index;
            // 特定スロットに固定しない
            out.padIndex = -1;
            return true;
        }
    }

    // ゲームパッドのスティック。大きく倒した場合のみ軸として捕捉する。通常操作で通過しうる
    // 値 (0.3〜0.5) を拾うと、リバインド待機中にスティックへ軽く触れただけで確定してしまう。
    constexpr float AXIS_CAPTURE_THRESHOLD = 0.7f;
    for (int pad = 0; pad < Gamepad::MAX_PADS; ++pad) {
        if (!Gamepad::IsConnected(pad)) {
            continue;
        }
        for (uint8_t index = 0; index < static_cast<uint8_t>(GamepadAxis::COUNT); ++index) {
            const float value = Gamepad::Axis(static_cast<GamepadAxis>(index), pad);
            if (std::fabs(value) < AXIS_CAPTURE_THRESHOLD) {
                continue;
            }
            out = InputBinding{};
            out.source   = BindingSource::GAMEPAD_AXIS;
            out.code     = index;
            out.scale    = value < 0.0f ? -1.0f : 1.0f;
            out.padIndex = -1;
            return true;
        }
    }

    return false;
}

std::vector<InputBinding>* ResolveAxisSlot(InputAxis& axis, int slot)
{
    switch (slot) {
    case 0:  return &axis.positive;
    case 1:  return &axis.negative;
    case 2:  return &axis.analog;
    default: return nullptr;
    }
}

void ApplyCapturedBinding(const InputBinding& binding)
{
    MapContext& context = Ctx();
    RebindState& rebind = context.rebind;

    std::vector<InputBinding>* target = nullptr;
    if (rebind.isAxis) {
        InputAxis* axis = InputActionMap::FindAxis(rebind.targetName);
        if (axis) {
            target = ResolveAxisSlot(*axis, rebind.slot);
        }
    } else {
        InputAction* action = InputActionMap::FindAction(rebind.targetName);
        if (action) {
            target = &action->bindings;
        }
    }

    if (target) {
        // 軸の positive / negative へ割り当てる場合、捕捉時の scale (倒した向き) は
        // デジタル扱いでは意味を持たないため 1.0 に戻す。
        InputBinding stored = binding;
        if (rebind.isAxis && rebind.slot != 2) {
            stored.scale = 1.0f;
        }

        if (rebind.bindingIndex >= 0
            && rebind.bindingIndex < static_cast<int>(target->size())) {
            (*target)[static_cast<size_t>(rebind.bindingIndex)] = stored;
        } else {
            target->push_back(stored);
        }
    }

    rebind = RebindState{};
    rebind.active = false;
    context.rebindCompleted = true;
}

} // namespace

/// @name ライフサイクル

void InputActionMap::Clear()
{
    MapContext& context = Ctx();
    context.initialized = true;
    context.actions.clear();
    context.axes.clear();
    context.actionRuntime.clear();
    context.axisRuntime.clear();
    context.actionIndex.clear();
    context.axisIndex.clear();
    context.rebind = RebindState{};
    context.rebind.active = false;
    context.rebindCompleted = false;
}

void InputActionMap::Initialize()
{
    if (!Ctx().initialized) {
        LoadDefaults();
    }
}

void InputActionMap::LoadDefaults()
{
    Clear();
    MapContext& context = Ctx();

    const auto key = [](uint32_t code, float scale = 1.0f) {
        InputBinding binding{};
        binding.source = BindingSource::KEY;
        binding.code   = code;
        binding.scale  = scale;
        return binding;
    };
    const auto mouseButton = [](uint32_t index) {
        InputBinding binding{};
        binding.source = BindingSource::MOUSE_BUTTON;
        binding.code   = index;
        return binding;
    };
    const auto padButton = [](GamepadButton button) {
        InputBinding binding{};
        binding.source = BindingSource::GAMEPAD_BUTTON;
        binding.code   = static_cast<uint32_t>(button);
        return binding;
    };
    const auto padAxis = [](GamepadAxis axis, float scale = 1.0f) {
        InputBinding binding{};
        binding.source = BindingSource::GAMEPAD_AXIS;
        binding.code   = static_cast<uint32_t>(axis);
        binding.scale  = scale;
        return binding;
    };
    const auto mouseAxis = [](MouseAxis axis, float scale = 1.0f) {
        InputBinding binding{};
        binding.source = BindingSource::MOUSE_AXIS;
        binding.code   = static_cast<uint32_t>(axis);
        binding.scale  = scale;
        return binding;
    };

    // 移動
    {
        InputAxis axis{};
        axis.name     = "MoveX";
        axis.positive = { key('D') };
        axis.negative = { key('A') };
        axis.analog   = { padAxis(GamepadAxis::LEFT_STICK_X) };
        context.axes.push_back(axis);
    }
    {
        InputAxis axis{};
        axis.name     = "MoveY";
        axis.positive = { key('W') };
        axis.negative = { key('S') };
        axis.analog   = { padAxis(GamepadAxis::LEFT_STICK_Y) };
        context.axes.push_back(axis);
    }

    // 視点
    // マウス Delta は既にフレーム間の移動量であり、-1..1 への正規化やデッドゾーン、
    // 平滑化を掛けると視点操作が破綻するため raw = true にする。
    {
        InputAxis axis{};
        axis.name     = "LookX";
        axis.raw      = true;
        axis.deadZone = 0.15f;
        axis.analog   = { mouseAxis(MouseAxis::DELTA_X),
                          padAxis(GamepadAxis::RIGHT_STICK_X) };
        context.axes.push_back(axis);
    }
    {
        // 画面座標系の Y は下向きが正。上に動かしたら上を向くよう scale = -1 で反転する。
        InputAxis axis{};
        axis.name     = "LookY";
        axis.raw      = true;
        axis.deadZone = 0.15f;
        axis.analog   = { mouseAxis(MouseAxis::DELTA_Y, -1.0f),
                          padAxis(GamepadAxis::RIGHT_STICK_Y) };
        context.axes.push_back(axis);
    }

    // ボタンアクション
    const struct { const char* name; InputBinding keyboard; InputBinding pad; } DEFAULT_ACTIONS[] = {
        { "Jump",     key(Vk(KeyCode::SPACE)),  padButton(GamepadButton::A) },
        { "Attack",   mouseButton(0),           padButton(GamepadButton::X) },
        { "Dodge",    key(Vk(KeyCode::SHIFT)),  padButton(GamepadButton::B) },
        { "Interact", key(Vk(KeyCode::E)),      padButton(GamepadButton::Y) },
        { "Pause",    key(Vk(KeyCode::ESCAPE)), padButton(GamepadButton::START) },
    };
    for (const auto& entry : DEFAULT_ACTIONS) {
        InputAction action{};
        action.name     = entry.name;
        action.bindings = { entry.keyboard, entry.pad };
        context.actions.push_back(action);
    }

    RebuildIndex();
}

void InputActionMap::SetEnabled(bool enabled)
{
    MapContext& context = Ctx();
    if (context.enabled == enabled) {
        return;
    }
    context.enabled = enabled;

    // 無効化時に押下状態を残すと、再有効化した最初のフレームで
    // 押しっぱなしの Down/Up が誤って発火する。
    for (ActionRuntime& runtime : context.actionRuntime) {
        runtime = ActionRuntime{};
    }
    for (AxisRuntime& runtime : context.axisRuntime) {
        runtime = AxisRuntime{};
    }
}

void InputActionMap::ResetRuntime()
{
    MapContext& context = Ctx();
    for (ActionRuntime& runtime : context.actionRuntime) {
        runtime = {};
    }
    for (AxisRuntime& runtime : context.axisRuntime) {
        runtime = {};
    }
}

bool InputActionMap::IsEnabled()
{
    return Ctx().enabled;
}

/// @name 毎フレーム更新

void InputActionMap::Update(float dt)
{
    MapContext& context = Ctx();
    context.rebindCompleted = false;

    // 割り当てを完了したフレームも、使ったキーがゲーム操作として発火しないようにする。
    const bool wasRebinding = context.rebind.active;
    if (wasRebinding) {
        if (context.rebind.skipFrames > 0) {
            --context.rebind.skipFrames;
        } else if (Input::KeyDown(KeyCode::ESCAPE)) {
            CancelRebind();
        } else {
            InputBinding captured{};
            if (CaptureFirstPressedInput(captured)) {
                ApplyCapturedBinding(captured);
            }
        }
    }

    // 通常入力を評価できない間も、アクションの Release はこのフレームの差分に残す。
    if (wasRebinding || !context.enabled) {
        for (ActionRuntime& runtime : context.actionRuntime) {
            runtime.previous = runtime.current;
            runtime.current  = false;
        }
        for (AxisRuntime& runtime : context.axisRuntime) {
            runtime = AxisRuntime{};
        }
        return;
    }

    // バインドの集合をアクションごとの押下状態へ変換する。
    for (size_t actionIndex = 0; actionIndex < context.actions.size() && actionIndex < context.actionRuntime.size(); ++actionIndex) {
        const InputAction& action = context.actions[actionIndex];
        ActionRuntime& runtime = context.actionRuntime[actionIndex];

        runtime.previous = runtime.current;

        // 全バインドの OR を取ってからアクション単位で差分を取る。バインドごとに Down を
        // 判定すると、キーとパッドを同時に押した際に Down が 2 回発火し二段ジャンプになる。
        bool held = AnyHeld(action.bindings);

        // AI 自動プレイテストの注入をアクション層にも合流させる。EditorMCP がアクション名で
        // 操作できないと、入力抽象の導入で自動テストがゲームを操作できなくなる。
        if (Input::GetVirtualButton(action.name)) {
            held = true;
        }

        runtime.current = held;
    }

    // 軸ごとにアナログとデジタルの入力から今フレームの目標値を作る。
    for (size_t axisIndex = 0; axisIndex < context.axes.size() && axisIndex < context.axisRuntime.size(); ++axisIndex) {
        const InputAxis& axis = context.axes[axisIndex];
        AxisRuntime& runtime = context.axisRuntime[axisIndex];

        runtime.rawAnalog = SelectStrongestAnalog(axis.analog);

        // デジタル入力の目標値を作る。両方押されている場合は打ち消し合って 0。
        float digitalTarget = 0.0f;
        if (AnyHeld(axis.positive)) {
            digitalTarget += 1.0f;
        }
        if (AnyHeld(axis.negative)) {
            digitalTarget -= 1.0f;
        }

        if (digitalTarget != 0.0f) {
            // 逆方向へ切り返した瞬間に 0 を経由させ、反応の鈍さを消す。
            if (axis.snap && runtime.keyValue != 0.0f
                && (runtime.keyValue > 0.0f) != (digitalTarget > 0.0f)) {
                runtime.keyValue = 0.0f;
            }
            runtime.keyValue = MoveTowards(runtime.keyValue, digitalTarget,
                                           std::max(axis.sensitivity, 0.0f) * dt);
        } else {
            runtime.keyValue = MoveTowards(runtime.keyValue, 0.0f,
                                           std::max(axis.gravity, 0.0f) * dt);
        }

        if (axis.raw) {
            // 生値モードはデッドゾーンも平滑化も掛けない。
            // アナログが無入力ならデジタル値をそのまま使う。
            runtime.value = runtime.rawAnalog != 0.0f ? runtime.rawAnalog : runtime.keyValue;
        } else {
            const float analog = ApplyDeadZone(runtime.rawAnalog, axis.deadZone);
            runtime.value = analog != 0.0f ? analog : runtime.keyValue;
        }

        // AI 注入の軸値があれば優先する。
        // 制約: GetVirtualAxis は「未設定」と「0 に設定」を区別できないため、
        // 0 以外が設定されている場合のみ上書きする。
        const float injected = Input::GetVirtualAxis(axis.name);
        if (injected != 0.0f) {
            runtime.value = injected;
        }
    }
}

/// @name 照会

bool InputActionMap::GetAction(std::string_view name)
{
    const ActionRuntime* runtime = FindActionRuntime(name);
    return runtime && runtime->current;
}

bool InputActionMap::GetActionDown(std::string_view name)
{
    const ActionRuntime* runtime = FindActionRuntime(name);
    return runtime && runtime->current && !runtime->previous;
}

bool InputActionMap::GetActionUp(std::string_view name)
{
    const ActionRuntime* runtime = FindActionRuntime(name);
    return runtime && !runtime->current && runtime->previous;
}

float InputActionMap::GetAxis(std::string_view name)
{
    const AxisRuntime* runtime = FindAxisRuntime(name);
    return runtime ? runtime->value : 0.0f;
}

math::Vector2 InputActionMap::GetAxis2D(std::string_view xName, std::string_view yName)
{
    const MapContext& context = Ctx();

    const auto xIt = context.axisIndex.find(xName);
    const auto yIt = context.axisIndex.find(yName);
    if (xIt == context.axisIndex.end() || yIt == context.axisIndex.end()) {
        return {};
    }
    if (xIt->second >= context.axes.size() || yIt->second >= context.axes.size()) {
        return {};
    }

    const InputAxis&   axisX = context.axes[xIt->second];
    const InputAxis&   axisY = context.axes[yIt->second];
    const AxisRuntime& runX  = context.axisRuntime[xIt->second];
    const AxisRuntime& runY  = context.axisRuntime[yIt->second];

    // 生値モードの軸は再マップせずそのまま返す (マウス Delta など)。
    if (axisX.raw || axisY.raw) {
        return { runX.value, runY.value };
    }

    // 半径方向のデッドゾーン。軸ごとに独立して切ると正方形のデッドゾーンになり、
    // スティックを斜めに倒したときの実効感度が方向によって変わる。
    const float deadZone = std::max(axisX.deadZone, axisY.deadZone);
    const float length   = std::sqrt(runX.rawAnalog * runX.rawAnalog
                                     + runY.rawAnalog * runY.rawAnalog);

    if (length > deadZone) {
        const float range     = std::max(1.0f - deadZone, 0.0001f);
        const float magnitude = std::min((length - deadZone) / range, 1.0f);
        const float inverse   = magnitude / length;
        return { runX.rawAnalog * inverse, runY.rawAnalog * inverse };
    }

    // アナログがデッドゾーン内ならデジタル入力を使う。
    return { runX.keyValue, runY.keyValue };
}

/// @name 編集

const std::vector<InputAction>& InputActionMap::GetActions() { return Ctx().actions; }
const std::vector<InputAxis>&   InputActionMap::GetAxes()    { return Ctx().axes; }

InputAction* InputActionMap::FindAction(std::string_view name)
{
    MapContext& context = Ctx();
    const auto it = context.actionIndex.find(name);
    if (it == context.actionIndex.end() || it->second >= context.actions.size()) {
        return nullptr;
    }
    return &context.actions[it->second];
}

InputAxis* InputActionMap::FindAxis(std::string_view name)
{
    MapContext& context = Ctx();
    const auto it = context.axisIndex.find(name);
    if (it == context.axisIndex.end() || it->second >= context.axes.size()) {
        return nullptr;
    }
    return &context.axes[it->second];
}

bool InputActionMap::AddAction(const InputAction& action)
{
    if (action.name.empty()) {
        return false;
    }

    MapContext& context = Ctx();
    context.initialized = true;
    if (InputAction* existing = FindAction(action.name)) {
        *existing = action;
        return true;
    }
    context.actions.push_back(action);
    RebuildIndex();
    return true;
}

bool InputActionMap::AddAxis(const InputAxis& axis)
{
    if (axis.name.empty()) {
        return false;
    }

    MapContext& context = Ctx();
    context.initialized = true;
    if (InputAxis* existing = FindAxis(axis.name)) {
        *existing = axis;
        return true;
    }
    context.axes.push_back(axis);
    RebuildIndex();
    return true;
}

bool InputActionMap::RemoveAction(std::string_view name)
{
    MapContext& context = Ctx();
    const auto it = std::find_if(context.actions.begin(), context.actions.end(),
                                 [&](const InputAction& a) { return a.name == name; });
    if (it == context.actions.end()) {
        return false;
    }

    context.actions.erase(it);
    RebuildIndex();
    return true;
}

bool InputActionMap::RemoveAxis(std::string_view name)
{
    MapContext& context = Ctx();
    const auto it = std::find_if(context.axes.begin(), context.axes.end(),
                                 [&](const InputAxis& a) { return a.name == name; });
    if (it == context.axes.end()) {
        return false;
    }

    context.axes.erase(it);
    RebuildIndex();
    return true;
}

/// @name リバインド

void InputActionMap::BeginRebindAction(std::string_view actionName, int bindingIndex)
{
    if (!FindAction(actionName)) {
        return;
    }

    MapContext& context = Ctx();
    context.rebind = RebindState{};
    context.rebind.active       = true;
    context.rebind.isAxis       = false;
    context.rebind.targetName   = std::string(actionName);
    context.rebind.bindingIndex = bindingIndex;
}

void InputActionMap::BeginRebindAxis(std::string_view axisName, int slot, int bindingIndex)
{
    if (!FindAxis(axisName)) {
        return;
    }
    if (slot < 0 || slot > 2) {
        return;
    }

    MapContext& context = Ctx();
    context.rebind = RebindState{};
    context.rebind.active       = true;
    context.rebind.isAxis       = true;
    context.rebind.targetName   = std::string(axisName);
    context.rebind.slot         = slot;
    context.rebind.bindingIndex = bindingIndex;
}

bool InputActionMap::IsRebinding()
{
    return Ctx().rebind.active;
}

bool InputActionMap::IsRebindTarget(std::string_view name, int slot, int bindingIndex)
{
    const RebindState& rebind = Ctx().rebind;
    if (!rebind.active) {
        return false;
    }
    if (rebind.targetName != name) {
        return false;
    }
    if (rebind.bindingIndex != bindingIndex) {
        return false;
    }

    // 軸のバインドは slot まで一致していること。アクションは slot < 0 で問い合わせる。
    return rebind.isAxis ? (rebind.slot == slot) : (slot < 0);
}

void InputActionMap::CancelRebind()
{
    MapContext& context = Ctx();
    context.rebind = RebindState{};
    context.rebind.active = false;
}

bool InputActionMap::ConsumeRebindCompleted()
{
    MapContext& context = Ctx();
    const bool completed = context.rebindCompleted;
    context.rebindCompleted = false;
    return completed;
}

/// @name 表示用文字列

namespace {

/// @note 主要な仮想キーの表示名。網羅は目指さず、既定バインドと
/// @note 一般的なキーコンフィグ対象を優先する。未知のキーは 16 進表記へフォールバックする。
const char* DescribeKeyCode(uint32_t code)
{
    switch (code) {
    case Vk(KeyCode::SPACE):     return "Space";
    case Vk(KeyCode::ENTER):     return "Enter";
    case Vk(KeyCode::ESCAPE):    return "Escape";
    case Vk(KeyCode::BACKSPACE): return "Backspace";
    case Vk(KeyCode::TAB):       return "Tab";
    case Vk(KeyCode::SHIFT):     return "Shift";
    case Vk(KeyCode::CTRL):      return "Ctrl";
    case Vk(KeyCode::ALT):       return "Alt";
    case Vk(KeyCode::LEFT):      return "Left";
    case Vk(KeyCode::RIGHT):     return "Right";
    case Vk(KeyCode::UP):        return "Up";
    case Vk(KeyCode::DOWN):      return "Down";
    default:         return nullptr;
    }
}

} // namespace

std::string InputActionMap::DescribeBinding(const InputBinding& binding)
{
    char buffer[64] = {};

    switch (binding.source) {
    case BindingSource::KEY: {
        if (const char* named = DescribeKeyCode(binding.code)) {
            return named;
        }
        if (binding.code >= 0x20 && binding.code < 0x7F) {
            std::snprintf(buffer, sizeof(buffer), "%c", static_cast<char>(binding.code));
            return buffer;
        }
        std::snprintf(buffer, sizeof(buffer), "Key(0x%02X)", binding.code);
        return buffer;
    }
    case BindingSource::MOUSE_BUTTON: {
        static const char* NAMES[] = { "Mouse Left", "Mouse Right", "Mouse Middle" };
        if (binding.code < 3) {
            return NAMES[binding.code];
        }
        std::snprintf(buffer, sizeof(buffer), "Mouse(%u)", binding.code);
        return buffer;
    }
    case BindingSource::GAMEPAD_BUTTON: {
        const char* name = ToString(static_cast<GamepadButton>(binding.code));
        if (binding.padIndex < 0) {
            std::snprintf(buffer, sizeof(buffer), "Pad:%s", name);
        } else {
            std::snprintf(buffer, sizeof(buffer), "Pad%d:%s", binding.padIndex, name);
        }
        return buffer;
    }
    case BindingSource::GAMEPAD_AXIS: {
        const char* name = ToString(static_cast<GamepadAxis>(binding.code));
        const char* sign = binding.scale < 0.0f ? "-" : "+";
        if (binding.padIndex < 0) {
            std::snprintf(buffer, sizeof(buffer), "Pad:%s%s", name, sign);
        } else {
            std::snprintf(buffer, sizeof(buffer), "Pad%d:%s%s", binding.padIndex, name, sign);
        }
        return buffer;
    }
    case BindingSource::MOUSE_AXIS: {
        static const char* NAMES[] = { "Mouse X", "Mouse Y", "Mouse Scroll" };
        const char* name = binding.code < 3 ? NAMES[binding.code] : "Mouse Axis";
        std::snprintf(buffer, sizeof(buffer), "%s%s", name, binding.scale < 0.0f ? "-" : "+");
        return buffer;
    }
    default:
        return "None";
    }
}

} // namespace fbzz::input
