# Input System — ゲームパッド対応と Input Action 抽象

キーボード / マウスのみだった `fbzz::input::Input` に、ゲームパッド (XInput) と
「アクション名 → 物理入力バインド」の抽象層を追加する。
既存の `Input::KeyDown()` 系 API と `ScriptInputProxy` の互換は完全に維持する。

## 現状の課題

| # | 課題 | 根拠 |
|---|------|------|
| 1 | ゲームパッド入力が存在しない | `Input.hpp` はキーボード 256 キー + マウス 3 ボタンのみ。XInput / DirectInput の参照ゼロ |
| 2 | 入力が物理デバイスに直結している | スクリプトが `KeyCode::SPACE` を直接見るため、キーコンフィグもパッド対応も不可能 |
| 3 | `SetVirtualAxis` / `GetVirtualAxis` は AI テスト注入専用 | EditorMCP が決定論的にゲームを操作するための注入口であり、ゲーム側の入力抽象ではない。名前空間が衝突している |
| 4 | エディタの `HotkeyManager` はエディタ専用 | ランタイムのキーコンフィグに再利用できない |

## 設計方針

- **既存 API を壊さない。** `Input::KeyDown(KeyCode)` はそのまま残す。アクション層はその上に積む。
- **デバイス層とアクション層を分離する。** `Input` は生のデバイス状態のみを持ち、
  `InputActionMap` がバインド解決とデッドゾーン処理を担う。
- **XInput のみを実装する。** DirectInput は非採用。
  **WHY**: XInput は Xbox 系コントローラーで API が単純かつ Windows 標準。
  DirectInput は汎用性と引き換えにデバイス列挙とフォースフィードバックの複雑さが跳ね上がり、
  ポートフォリオでの投資対効果が悪い。PS 系パッドは Steam Input / DS4Windows 経由で
  XInput に化けるため実用上の欠落は小さい。
- **ポーリング方式にする。** `XInputGetState` をフレーム先頭で 4 スロット分呼ぶ。
  **WHY**: Win32 メッセージポンプに乗らないデバイスであり、既存の `Input::Update()` と
  同じ「フレーム先頭で現在状態を確定 → 前フレームを保存」というモデルに素直に収まる。
- **既存の virtual axis と衝突させない。** AI 注入用の `SetVirtualAxis` は残しつつ、
  アクション層の解決結果に注入値を合成する (注入が優先)。
  **WHY**: EditorMCP の自動プレイテストがアクション層を通しても動くようにするため。

## レイヤー構成

```
Script / Game
   ↓  GetAction("Jump") / GetActionAxis2D("Move")
InputActionMap        ... アクション名 → バインド解決・デッドゾーン・リバインド
   ↓  KeyHeld / GetGamepadAxis / GetGamepadButton
Input (デバイス層)     ... キーボード / マウス / ゲームパッド の生状態
   ↓
Win32 メッセージ + XInputGetState
```

## 追加ファイル

| ファイル | 内容 |
|---------|------|
| `Engine/include/Engine/Input/GamepadButton.hpp` | `GamepadButton` / `GamepadAxis` 列挙 |
| `Engine/include/Engine/Input/Gamepad.hpp` | XInput ラッパー。4 スロットのポーリングと振動 |
| `Engine/src/Input/Gamepad.cpp` | 実装 |
| `Engine/include/Engine/Input/InputBinding.hpp` | `InputBinding` / `InputAction` / `AxisBinding` のデータ定義 |
| `Engine/include/Engine/Input/InputActionMap.hpp` | アクション解決・リバインド・保存/読込 |
| `Engine/src/Input/InputActionMap.cpp` | 実装 |
| `Engine/src/Input/InputActionMapSerializer.cpp` | `.inputactions` (TOML) の読み書き |

## デバイス層 — `Gamepad`

```cpp
enum class GamepadButton : uint16_t {
    A, B, X, Y,
    DPadUp, DPadDown, DPadLeft, DPadRight,
    LeftShoulder, RightShoulder,
    LeftStick, RightStick,          // スティック押し込み
    Start, Back,
    LeftTrigger, RightTrigger,      // アナログトリガーをボタンとしても扱う (閾値 0.5)
    COUNT
};

enum class GamepadAxis : uint8_t {
    LeftStickX, LeftStickY,
    RightStickX, RightStickY,
    LeftTrigger, RightTrigger,
    COUNT
};
```

```cpp
class Gamepad {
public:
    static constexpr int MAX_PADS = 4;

    static void Update();                       // Input::Update() の先頭から呼ぶ

    static bool  IsConnected(int pad = 0);
    static bool  ButtonHeld(GamepadButton b, int pad = 0);
    static bool  ButtonDown(GamepadButton b, int pad = 0);   // 押した瞬間のみ
    static bool  ButtonUp  (GamepadButton b, int pad = 0);
    static float Axis(GamepadAxis a, int pad = 0);           // -1..1 (トリガーは 0..1)

    // 振動。durationSeconds 経過で自動停止する (呼び出し側の停止忘れを防ぐ)
    static void SetVibration(float lowFreq, float highFreq,
                             float durationSeconds, int pad = 0);
    static void StopVibration(int pad = 0);
};
```

**未接続スロットのポーリングコスト対策**: `XInputGetState` は未接続スロットに対して
`ERROR_DEVICE_NOT_CONNECTED` を返すまでに数百 µs かかることが知られている。
未接続と判定したスロットは **0.5 秒に 1 回だけ**再試行する。
**WHY**: 毎フレーム 4 スロットを叩くと最悪 1ms 超のフレーム食いが発生し、
Profiler 上で原因不明のスパイクとして現れるため。

**デッドゾーン**: デバイス層では**適用しない**。生の -1..1 を返す。
**WHY**: デッドゾーンはアクションごとに適正値が異なる (移動は大きめ、カメラは小さめ)。
デバイス層で潰すと後段で復元できない。

## アクション層 — `InputActionMap`

### データモデル

```cpp
// 1 つの物理入力ソース。key / gamepadButton / gamepadAxis のいずれか 1 つが有効。
struct InputBinding {
    enum class Source : uint8_t { Key, MouseButton, GamepadButton, GamepadAxis };

    Source        source        = Source::Key;
    uint32_t      code          = 0;      // KeyCode / MouseBtn / GamepadButton / GamepadAxis の値
    float         scale         = 1.0f;   // 軸の向き反転に使う (-1 で反転)
    bool          invertAsButton = false; // 軸をボタン扱いする際に負方向で発火させる
};

// ボタン的アクション ("Jump", "Attack")。いずれかのバインドが立てば true。
struct InputAction {
    std::string               name;
    std::vector<InputBinding> bindings;
};

// 1 次元軸アクション ("MoveX")。
// 軸バインドはそのまま、キーバインドは positive / negative の対で -1/+1 を作る。
struct InputAxis {
    std::string               name;
    std::vector<InputBinding> positive;   // 押されたら +1
    std::vector<InputBinding> negative;   // 押されたら -1
    std::vector<InputBinding> analog;     // GamepadAxis 直結
    float deadZone   = 0.25f;             // アナログのデッドゾーン
    float gravity    = 8.0f;              // キー入力時に 0 へ戻る速度 (単位/秒)
    float sensitivity = 8.0f;             // キー入力時に目標値へ向かう速度 (単位/秒)
    bool  snap        = true;             // 逆方向入力で即座に 0 を経由するか
};
```

### 解決規則

- **ボタン**: 全バインドの OR。`Down` / `Up` は前フレーム結果との差分で算出する。
  **WHY**: バインドごとに Down を取ると、キーとパッドを同時に押した際に 2 回発火する。
  アクション単位の状態を 1 フレーム分保持して差分を取るのが正しい。
- **軸**: `analog` バインドがデッドゾーンを超えていればそれを採用。
  超えていなければ `positive`/`negative` のキー入力を `sensitivity`/`gravity` で平滑化した値を使う。
  **WHY**: パッドとキーボードを同時に接続していても、実際に動かしている方が勝つ。
- **デッドゾーン**: 半径方向で適用する (`Vector2` として長さを見る)。
  **WHY**: 軸ごとに独立して切ると、斜め入力時に正方形のデッドゾーンとなり
  スティックを斜めに倒したときの感度が方向によって変わる。

```cpp
// 2 軸をまとめて取る。デッドゾーンを半径で適用してから正規化する。
math::Vector2 GetActionAxis2D(std::string_view xName, std::string_view yName);
```

### 公開 API

```cpp
class InputActionMap {
public:
    // ProjectSettings の起動時に既定マップを読み込む
    static bool LoadFromFile(const std::string& path);
    static bool SaveToFile(const std::string& path);
    static void LoadDefaults();          // WASD + XInput の標準バインド

    static void Update(float dt);        // Input::Update() の直後に呼ぶ

    static bool  GetAction    (std::string_view name);
    static bool  GetActionDown(std::string_view name);
    static bool  GetActionUp  (std::string_view name);
    static float GetAxis      (std::string_view name);
    static math::Vector2 GetAxis2D(std::string_view xName, std::string_view yName);

    // --- リバインド ---
    // 次に押された任意の入力を name のバインドとして記録する非同期リクエスト。
    // 1 フレームで完結しないため、ポーリング形式にする (UI から進行状況を見せられる)。
    static void  BeginRebind(std::string_view actionName, int bindingIndex);
    static bool  IsRebinding();
    static void  CancelRebind();

    static const std::vector<InputAction>& GetActions();
    static const std::vector<InputAxis>&   GetAxes();
};
```

### 既定バインド (`LoadDefaults`)

| アクション / 軸 | キーボード | ゲームパッド |
|---------------|-----------|-------------|
| `MoveX` | A / D | LeftStickX |
| `MoveY` | S / W | LeftStickY |
| `LookX` | — (マウス Delta X) | RightStickX |
| `LookY` | — (マウス Delta Y) | RightStickY |
| `Jump` | Space | A |
| `Attack` | MouseLeft | X |
| `Dodge` | LeftShift | B |
| `Interact` | E | Y |
| `Pause` | Escape | Start |

## アセット形式 — `.inputactions`

`Assets/EditorConfig/` ではなく **プロジェクト直下 `ProjectSettings/Input.inputactions`** に置く。
**WHY**: 入力バインドはエディタ設定ではなくゲーム設定であり、
`BuildPipeline` の `CopyProjectFiles` ステップで配布物に含める必要がある。

```toml
[[axis]]
name = "MoveX"
deadZone = 0.25
gravity = 8.0
sensitivity = 8.0
positive = [ { source = "Key", code = 68 } ]      # D
negative = [ { source = "Key", code = 65 } ]      # A
analog   = [ { source = "GamepadAxis", code = 0 } ]

[[action]]
name = "Jump"
bindings = [
  { source = "Key",           code = 32 },  # Space
  { source = "GamepadButton", code = 0  },  # A
]
```

**WHY TOML**: シーン (`.scene`) / プロジェクト設定と同じ toml++ を使い、依存を増やさない。

## Script への公開

`ScriptInputProxy` に追記する (既存メンバは変更しない)。

```cpp
// --- アクション層 ---
bool  GetAction    (std::string_view name) const;
bool  GetActionDown(std::string_view name) const;
bool  GetActionUp  (std::string_view name) const;
float GetActionAxis(std::string_view name) const;
math::Vector2 GetMoveAxis() const;   // ("MoveX","MoveY") のショートハンド
math::Vector2 GetLookAxis() const;   // ("LookX","LookY") のショートハンド

// --- 生のゲームパッド (アクション層を通さない直接アクセス) ---
bool  GetPadButton    (input::GamepadButton b, int pad = 0) const;
bool  GetPadButtonDown(input::GamepadButton b, int pad = 0) const;
bool  GetPadButtonUp  (input::GamepadButton b, int pad = 0) const;
float GetPadAxis      (input::GamepadAxis a,  int pad = 0) const;
bool  IsPadConnected  (int pad = 0) const;
void  SetVibration(float low, float high, float seconds, int pad = 0) const;
```

**WHY 生アクセスも公開するか**: 「B ボタンを押しっぱなしでダッシュ」のような
UI 表示 (ボタンアイコン) を伴う処理では、どの物理ボタンが割り当たっているかを
スクリプト側が知る必要がある。アクション層だけでは表現できない。

## エディタ統合

- **`ProjectSettingsPanel` に Input タブを追加**する。既存の `HotkeyEditorPanel` は
  エディタ操作用として据え置き、ゲーム入力とは別管理にする。
  **WHY**: 混ぜるとエディタのショートカットとゲームのキーコンフィグが同じ UI に並び、
  ユーザーがどちらを編集しているか分からなくなる。
- アクション / 軸の追加・削除、バインドのリバインド (押して割り当て)、
  デッドゾーン等のパラメーター編集、接続中パッドの実測値表示を持つ。
  **WHY 実測値表示**: スティックのドリフト量を目で見てデッドゾーンを決められる。

## Application への結線

```cpp
// Application::Run() のメインループ
input::Input::Update();                    // 既存 — 内部で Gamepad::Update() を呼ぶ
m_window->PollEvents();                    // 既存 — ここでキーボード/マウスの現在状態が入る
input::InputActionMap::Update(             // 追加
    Time::unscaledDeltaTime);
```

**WHY `PollEvents()` の「後」か**: `Input::Update()` は前フレーム状態を退避するだけで、
キーボード / マウスの現在状態は `PollEvents()` 内の Win32 メッセージで更新される。
アクション層をその前で評価すると、常に 1 フレーム古い入力を見ることになる。

**WHY `unscaledDeltaTime` か**: 入力の平滑化はプレイヤーの操作感であり、
スローモーション演出 (`TimeScale`) に引きずられて鈍くなるべきではない。

**WHY `Input::Update()` の内部で `Gamepad::Update()` を呼ぶか**: 呼び出し漏れを防ぐため。
Editor / Standalone / Sandbox の 3 箇所に散らばる更新経路すべてに追記する必要がなくなる。

## Play モードでの扱い

エディタの Play モード中のみアクションを評価する。エディット中はビューポート操作と
アクションが二重発火するため、`InputActionMap::SetEnabled(false)` で止める。
**WHY**: エディタでシーン編集中に W を押すとプレイヤーが歩き出す、という事故を防ぐ。

## テスト

`Projects/Tests/Input/main.cpp` を新設する。XInput 実機に依存しないよう、
`Gamepad` の状態を注入するテスト用フックを設ける。

- [ ] ボタンの Down / Held / Up が 1 フレームだけ立つ
- [ ] キーとパッドの同時押しで Down が 2 回発火しない
- [ ] 半径デッドゾーンが斜め入力で正しく効く
- [ ] `gravity` / `sensitivity` による軸の平滑化が単調に収束する
- [ ] `.inputactions` の保存 → 読込で全フィールドが往復する
- [ ] リバインドが次の入力を捕捉し、既存バインドを置換する

## 実装チェックリスト

- [x] ✅ `GamepadButton.hpp` / `Gamepad.hpp` / `Gamepad.cpp`
- [x] ✅ 未接続スロットの再試行スロットリング
- [x] ✅ 振動の自動停止タイマー
- [x] ✅ `InputBinding.hpp` / `InputActionMap.hpp` / `InputActionMap.cpp`
- [x] ✅ `.inputactions` TOML シリアライザ
- [x] ✅ `LoadDefaults()` の既定バインド
- [x] ✅ `Input::Update()` から `Gamepad::Update()` を呼ぶ
- [x] ✅ `Application` / Editor Play モードへの結線
- [x] ✅ `ProjectSettings::Load` からの `.inputactions` 自動読み込み
- [x] ✅ `ScriptInputProxy` 拡張 + `ScriptProxies.cpp` 実装 + `Script.hpp` 登録
- [x] ✅ `ProjectSettingsPanel` の Input タブ (リバインド / パッド実測値 / ライブプレビュー)
- [x] ✅ `BuildPipeline` の `CopyProjectFiles` に `.inputactions` を追加
- [x] ✅ `Projects/Tests/Input/main.cpp` + `Tests/CMakeLists.txt` 登録
- [ ] Visual Studio 2022 全体ビルド
