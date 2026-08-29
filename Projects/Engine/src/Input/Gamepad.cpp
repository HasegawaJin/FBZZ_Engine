/// @file    Gamepad.cpp
/// @brief   XInputGetState のポーリングと、ボタン差分・振動寿命の管理。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include "Engine/Input/Gamepad.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Xinput.h>

namespace fbzz::input {

namespace {

// XInput のボタンビットマスク → GamepadButton の対応表。
// トリガー 2 種はアナログ値から導出するためこの表には含めない。
struct ButtonMapping {
    WORD          mask;
    GamepadButton button;
};

constexpr std::array<ButtonMapping, 14> BUTTON_MAPPINGS = {{
    { XINPUT_GAMEPAD_A,              GamepadButton::A },
    { XINPUT_GAMEPAD_B,              GamepadButton::B },
    { XINPUT_GAMEPAD_X,              GamepadButton::X },
    { XINPUT_GAMEPAD_Y,              GamepadButton::Y },
    { XINPUT_GAMEPAD_DPAD_UP,        GamepadButton::DPAD_UP },
    { XINPUT_GAMEPAD_DPAD_DOWN,      GamepadButton::DPAD_DOWN },
    { XINPUT_GAMEPAD_DPAD_LEFT,      GamepadButton::DPAD_LEFT },
    { XINPUT_GAMEPAD_DPAD_RIGHT,     GamepadButton::DPAD_RIGHT },
    { XINPUT_GAMEPAD_LEFT_SHOULDER,  GamepadButton::LEFT_SHOULDER },
    { XINPUT_GAMEPAD_RIGHT_SHOULDER, GamepadButton::RIGHT_SHOULDER },
    { XINPUT_GAMEPAD_LEFT_THUMB,     GamepadButton::LEFT_STICK },
    { XINPUT_GAMEPAD_RIGHT_THUMB,    GamepadButton::RIGHT_STICK },
    { XINPUT_GAMEPAD_START,          GamepadButton::START },
    { XINPUT_GAMEPAD_BACK,           GamepadButton::BACK },
}};

constexpr size_t BUTTON_COUNT = static_cast<size_t>(GamepadButton::COUNT);
constexpr size_t AXIS_COUNT   = static_cast<size_t>(GamepadAxis::COUNT);

// 未接続スロットの再試行間隔 [s]。
// WHY: XInputGetState は未接続スロットに対して ERROR_DEVICE_NOT_CONNECTED を
//      返すまでに数百 µs かかる実装が知られている。4 スロットを毎フレーム叩くと
//      最悪 1ms 超のフレーム食いとなり、Profiler 上で原因不明のスパイクとして現れる。
//      接続検出が最大 0.5 秒遅れても体感上の問題はないため、間引く。
constexpr float DISCONNECTED_RETRY_INTERVAL = 0.5f;

struct PadState {
    bool connected = false;

    std::array<bool, BUTTON_COUNT>  current  = {};
    std::array<bool, BUTTON_COUNT>  previous = {};
    std::array<float, AXIS_COUNT>   axes     = {};

    // 未接続と判定してからの経過時間。DISCONNECTED_RETRY_INTERVAL を超えたら再試行する。
    float retryTimer = 0.0f;

    // 振動の残り時間 [s]。0 以下で停止する。
    float vibrationRemaining = 0.0f;
    bool  vibrationActive    = false;
};

struct GamepadContext {
    std::array<PadState, Gamepad::MAX_PADS> pads = {};
    bool  simulationEnabled = false;

    // 前回 Update からの実時間を測るための QPC 値。0 は未初期化。
    int64_t lastCounter = 0;
};

// WHY 関数内 static か: fbzz_engine は SHARED ライブラリで WINDOWS_EXPORT_ALL_SYMBOLS を
//     使っているが、この設定はデータシンボルを自動エクスポートしない。
//     状態をファイルスコープの static 変数に置くと DLL 境界を越えた参照が壊れるため、
//     必ずエクスポート済み関数を経由して触れる形にする (AssetManager の S_init() と同方針)。
GamepadContext& Ctx()
{
    static GamepadContext context;
    return context;
}

// 前回呼び出しからの実経過秒を返す。
// WHY Time::DeltaTime() を使わないか: 振動の寿命はゲーム内時間ではなく実時間で切るべきで、
//     ポーズ中やスローモーション中に振動が伸びるのは不自然。また Input は Time より
//     早い段階で更新されるため、依存の向きを増やさない方が安全。
float TickRealSeconds()
{
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);

    GamepadContext& context = Ctx();
    if (context.lastCounter == 0) {
        context.lastCounter = counter.QuadPart;
        return 0.0f;
    }

    const int64_t delta = counter.QuadPart - context.lastCounter;
    context.lastCounter = counter.QuadPart;
    if (frequency.QuadPart <= 0) return 0.0f;

    const float seconds = static_cast<float>(static_cast<double>(delta)
                                             / static_cast<double>(frequency.QuadPart));
    // フレーム落ちやブレークポイント停止で巨大な dt が出ると振動が即座に切れる。
    // 実用上 0.1 秒で頭打ちにしておけば十分。
    return std::clamp(seconds, 0.0f, 0.1f);
}

// SHORT (-32768..32767) を -1..1 に正規化する。
// WHY 負側を 32768 で割るか: -32768 を 32767 で割ると -1.0000305 となり、
//     範囲外の値が後段のデッドゾーン計算や平方根に混入する。符号ごとに除数を変えて
//     厳密に -1..1 へ収める。
float NormalizeStick(SHORT value)
{
    return value < 0 ? static_cast<float>(value) / 32768.0f
                     : static_cast<float>(value) / 32767.0f;
}

float NormalizeTrigger(BYTE value)
{
    return static_cast<float>(value) / 255.0f;
}

bool IsValidPad(int pad)
{
    return pad >= 0 && pad < Gamepad::MAX_PADS;
}

} // namespace

void Gamepad::Update()
{
    GamepadContext& context = Ctx();
    const float dt = TickRealSeconds();

    for (int index = 0; index < MAX_PADS; ++index) {
        PadState& pad = context.pads[index];

        // 現在状態を前フレームへ退避する。
        // シミュレーション時も同じ経路を通ることで Down/Up の判定規則を共通化する。
        pad.previous = pad.current;

        // --- 振動の寿命管理 ---
        if (pad.vibrationActive) {
            pad.vibrationRemaining -= dt;
            if (pad.vibrationRemaining <= 0.0f) {
                StopVibration(index);
            }
        }

        // シミュレーション中は XInput をポーリングせず、注入された値をそのまま使う。
        if (context.simulationEnabled) continue;

        // --- 未接続スロットの再試行スロットリング ---
        if (!pad.connected) {
            pad.retryTimer -= dt;
            if (pad.retryTimer > 0.0f) {
                continue;
            }
            pad.retryTimer = DISCONNECTED_RETRY_INTERVAL;
        }

        XINPUT_STATE state{};
        const DWORD result = XInputGetState(static_cast<DWORD>(index), &state);
        if (result != ERROR_SUCCESS) {
            // 接続していたパッドが外れた場合、押しっぱなし状態が残らないよう全解除する。
            // WHY: 残すとキャラクターが走り続けるなど、抜線後に操作不能へ見える不具合になる。
            if (pad.connected) {
                pad.current.fill(false);
                pad.axes.fill(0.0f);
                StopVibration(index);
            }
            pad.connected  = false;
            pad.retryTimer = DISCONNECTED_RETRY_INTERVAL;
            continue;
        }

        pad.connected  = true;
        pad.retryTimer = 0.0f;

        const XINPUT_GAMEPAD& gamepad = state.Gamepad;

        for (const ButtonMapping& mapping : BUTTON_MAPPINGS) {
            pad.current[static_cast<size_t>(mapping.button)] =
                (gamepad.wButtons & mapping.mask) != 0;
        }

        pad.axes[static_cast<size_t>(GamepadAxis::LEFT_STICK_X)]  = NormalizeStick(gamepad.sThumbLX);
        pad.axes[static_cast<size_t>(GamepadAxis::LEFT_STICK_Y)]  = NormalizeStick(gamepad.sThumbLY);
        pad.axes[static_cast<size_t>(GamepadAxis::RIGHT_STICK_X)] = NormalizeStick(gamepad.sThumbRX);
        pad.axes[static_cast<size_t>(GamepadAxis::RIGHT_STICK_Y)] = NormalizeStick(gamepad.sThumbRY);

        const float leftTrigger  = NormalizeTrigger(gamepad.bLeftTrigger);
        const float rightTrigger = NormalizeTrigger(gamepad.bRightTrigger);
        pad.axes[static_cast<size_t>(GamepadAxis::LEFT_TRIGGER)]  = leftTrigger;
        pad.axes[static_cast<size_t>(GamepadAxis::RIGHT_TRIGGER)] = rightTrigger;

        // アナログトリガーをボタンとしても公開する。
        pad.current[static_cast<size_t>(GamepadButton::LEFT_TRIGGER)] =
            leftTrigger >= GAMEPAD_TRIGGER_BUTTON_THRESHOLD;
        pad.current[static_cast<size_t>(GamepadButton::RIGHT_TRIGGER)] =
            rightTrigger >= GAMEPAD_TRIGGER_BUTTON_THRESHOLD;
    }
}

void Gamepad::Reset()
{
    GamepadContext& context = Ctx();
    StopAllVibration();
    for (PadState& pad : context.pads) {
        pad.current.fill(false);
        pad.previous.fill(false);
        pad.axes.fill(0.0f);
        pad.retryTimer = 0.0f;
        // connected は実デバイスの状態なので保持する。次の Update で再確認される。
    }
}

bool Gamepad::IsConnected(int pad)
{
    if (!IsValidPad(pad)) return false;
    return Ctx().pads[static_cast<size_t>(pad)].connected;
}

int Gamepad::GetFirstConnectedPad()
{
    const GamepadContext& context = Ctx();
    for (int index = 0; index < MAX_PADS; ++index) {
        if (context.pads[static_cast<size_t>(index)].connected) return index;
    }
    return -1;
}

bool Gamepad::ButtonHeld(GamepadButton button, int pad)
{
    if (!IsValidPad(pad) || button >= GamepadButton::COUNT) return false;
    return Ctx().pads[static_cast<size_t>(pad)].current[static_cast<size_t>(button)];
}

bool Gamepad::ButtonDown(GamepadButton button, int pad)
{
    if (!IsValidPad(pad) || button >= GamepadButton::COUNT) return false;
    const PadState& state = Ctx().pads[static_cast<size_t>(pad)];
    const size_t    index = static_cast<size_t>(button);
    return state.current[index] && !state.previous[index];
}

bool Gamepad::ButtonUp(GamepadButton button, int pad)
{
    if (!IsValidPad(pad) || button >= GamepadButton::COUNT) return false;
    const PadState& state = Ctx().pads[static_cast<size_t>(pad)];
    const size_t    index = static_cast<size_t>(button);
    return !state.current[index] && state.previous[index];
}

float Gamepad::Axis(GamepadAxis axis, int pad)
{
    if (!IsValidPad(pad) || axis >= GamepadAxis::COUNT) return 0.0f;
    return Ctx().pads[static_cast<size_t>(pad)].axes[static_cast<size_t>(axis)];
}

void Gamepad::SetVibration(float lowFrequency, float highFrequency,
                           float durationSeconds, int pad)
{
    if (!IsValidPad(pad)) return;

    GamepadContext& context = Ctx();
    PadState&       state   = context.pads[static_cast<size_t>(pad)];

    // 継続時間が 0 以下の要求は「振動させない」意図とみなし、実行中の振動も止める。
    if (durationSeconds <= 0.0f) {
        StopVibration(pad);
        return;
    }

    state.vibrationRemaining = durationSeconds;
    state.vibrationActive    = true;

    if (context.simulationEnabled) return;
    if (!state.connected)          return;

    XINPUT_VIBRATION vibration{};
    vibration.wLeftMotorSpeed = static_cast<WORD>(
        std::clamp(lowFrequency, 0.0f, 1.0f) * 65535.0f);
    vibration.wRightMotorSpeed = static_cast<WORD>(
        std::clamp(highFrequency, 0.0f, 1.0f) * 65535.0f);
    XInputSetState(static_cast<DWORD>(pad), &vibration);
}

void Gamepad::StopVibration(int pad)
{
    if (!IsValidPad(pad)) return;

    GamepadContext& context = Ctx();
    PadState&       state   = context.pads[static_cast<size_t>(pad)];
    state.vibrationRemaining = 0.0f;
    state.vibrationActive    = false;

    if (context.simulationEnabled) return;

    // 未接続でも停止要求は投げる。抜き挿しの隙間で振動が残るのを防ぐ。
    XINPUT_VIBRATION vibration{};
    XInputSetState(static_cast<DWORD>(pad), &vibration);
}

void Gamepad::StopAllVibration()
{
    for (int index = 0; index < MAX_PADS; ++index) {
        StopVibration(index);
    }
}

// ── テスト用フック ───────────────────────────────────────────────────────────

void Gamepad::SetSimulationEnabled(bool enabled)
{
    GamepadContext& context = Ctx();
    if (context.simulationEnabled == enabled) return;

    context.simulationEnabled = enabled;
    // モード切替時に状態を持ち越すと、実機の押下が注入値として残るなど混乱の元になる。
    for (PadState& pad : context.pads) {
        pad.current.fill(false);
        pad.previous.fill(false);
        pad.axes.fill(0.0f);
        pad.connected  = false;
        pad.retryTimer = 0.0f;
    }
}

bool Gamepad::IsSimulationEnabled()
{
    return Ctx().simulationEnabled;
}

void Gamepad::SimulateConnected(int pad, bool connected)
{
    if (!IsValidPad(pad)) return;
    Ctx().pads[static_cast<size_t>(pad)].connected = connected;
}

void Gamepad::SimulateButton(GamepadButton button, bool pressed, int pad)
{
    if (!IsValidPad(pad) || button >= GamepadButton::COUNT) return;
    Ctx().pads[static_cast<size_t>(pad)].current[static_cast<size_t>(button)] = pressed;
}

void Gamepad::SimulateAxis(GamepadAxis axis, float value, int pad)
{
    if (!IsValidPad(pad) || axis >= GamepadAxis::COUNT) return;

    PadState& state = Ctx().pads[static_cast<size_t>(pad)];
    const bool isTrigger = axis == GamepadAxis::LEFT_TRIGGER || axis == GamepadAxis::RIGHT_TRIGGER;
    const float clamped  = isTrigger ? std::clamp(value, 0.0f, 1.0f)
                                     : std::clamp(value, -1.0f, 1.0f);
    state.axes[static_cast<size_t>(axis)] = clamped;

    // 実機経路と同様、トリガー軸の注入はボタン状態にも反映する。
    if (axis == GamepadAxis::LEFT_TRIGGER) {
        state.current[static_cast<size_t>(GamepadButton::LEFT_TRIGGER)] =
            clamped >= GAMEPAD_TRIGGER_BUTTON_THRESHOLD;
    } else if (axis == GamepadAxis::RIGHT_TRIGGER) {
        state.current[static_cast<size_t>(GamepadButton::RIGHT_TRIGGER)] =
            clamped >= GAMEPAD_TRIGGER_BUTTON_THRESHOLD;
    }
}

// ── 文字列化 ─────────────────────────────────────────────────────────────────

const char* ToString(GamepadButton button) noexcept
{
    switch (button) {
    case GamepadButton::A:              return "A";
    case GamepadButton::B:              return "B";
    case GamepadButton::X:              return "X";
    case GamepadButton::Y:              return "Y";
    case GamepadButton::DPAD_UP:        return "DPadUp";
    case GamepadButton::DPAD_DOWN:      return "DPadDown";
    case GamepadButton::DPAD_LEFT:      return "DPadLeft";
    case GamepadButton::DPAD_RIGHT:     return "DPadRight";
    case GamepadButton::LEFT_SHOULDER:  return "LeftShoulder";
    case GamepadButton::RIGHT_SHOULDER: return "RightShoulder";
    case GamepadButton::LEFT_STICK:     return "LeftStick";
    case GamepadButton::RIGHT_STICK:    return "RightStick";
    case GamepadButton::START:          return "Start";
    case GamepadButton::BACK:           return "Back";
    case GamepadButton::LEFT_TRIGGER:   return "LeftTrigger";
    case GamepadButton::RIGHT_TRIGGER:  return "RightTrigger";
    default:                            return "None";
    }
}

const char* ToString(GamepadAxis axis) noexcept
{
    switch (axis) {
    case GamepadAxis::LEFT_STICK_X:  return "LeftStickX";
    case GamepadAxis::LEFT_STICK_Y:  return "LeftStickY";
    case GamepadAxis::RIGHT_STICK_X: return "RightStickX";
    case GamepadAxis::RIGHT_STICK_Y: return "RightStickY";
    case GamepadAxis::LEFT_TRIGGER:  return "LeftTrigger";
    case GamepadAxis::RIGHT_TRIGGER: return "RightTrigger";
    default:                         return "None";
    }
}

bool ParseGamepadButton(const char* name, GamepadButton& out) noexcept
{
    if (!name) return false;
    for (uint16_t index = 0; index < static_cast<uint16_t>(GamepadButton::COUNT); ++index) {
        const auto button = static_cast<GamepadButton>(index);
        if (std::strcmp(name, ToString(button)) == 0) {
            out = button;
            return true;
        }
    }
    return false;
}

bool ParseGamepadAxis(const char* name, GamepadAxis& out) noexcept
{
    if (!name) return false;
    for (uint8_t index = 0; index < static_cast<uint8_t>(GamepadAxis::COUNT); ++index) {
        const auto axis = static_cast<GamepadAxis>(index);
        if (std::strcmp(name, ToString(axis)) == 0) {
            out = axis;
            return true;
        }
    }
    return false;
}

} // namespace fbzz::input
