# Sequence — 演出タイムライン (`.sequence`)

複数の GameObject を 1 本の時間軸に載せ、**どの時刻でも同じ絵になる**形で再生する仕組み。
エンジンが提供するのは「時刻 → 状態」の器 (Track / Clip / Key / Binding) だけで、
何を演出とみなすかはゲーム側が EventTrack で受け取る。

最初の使い手は GreenWare のボス「ポラリティ・コア」の登場・フェーズ移行・撃破 (§10)。

---

## 1. 現状と課題

### 1.1 今ある部品

| 部品 | できること | 足りないこと |
|---|---|---|
| `AnimationClip` ([AnimationClip.hpp](../../Projects/Engine/include/Engine/Asset/AnimationClip.hpp)) | `NodeAnimationTrack` / `PropertyAnimationTrack` / `AnimEvent` を持つ | **1 つの Animator に閉じる**。ターゲットは Animator 所有 GameObject からの相対パスのみ |
| `AnimationSlotPlayback` ([AnimatorComponent.hpp:235](../../Projects/Engine/include/Engine/Scene/Components/AnimatorComponent.hpp)) | レイヤーへワンショットを差し込み、フェードで戻す | 自分で `time` を進めるので**外から時刻を指定できない** |
| `Coroutine` ([Coroutine.hpp](../../Projects/Engine/include/Engine/Scene/Coroutine.hpp)) | `co_await WaitForSeconds` で時間軸処理を直線的に書ける | 巻き戻せない / エディタに何も出ない / DLL リロードで消える |
| `ScriptTweenProxy` | 単発の MoveTo / Value | 1 オブジェクト 1 値。合奏できない |
| `UIAnimator` | UI 1 要素の色・位置・スケール Tween | UI 専用。3 本固定 |
| `VFXEditor` ([VFXTimelineView.hpp](../../Projects/Editor/include/Editor/VFXEditor/Views/VFXTimelineView.hpp)) | 決定論スクラブ付きのタイムライン UI | VFX グラフ専用。他のオブジェクトを載せられない |

つまり **「時間軸で 1 つの対象を動かす」機構は 4 種類あって、「複数の対象を 1 本の時間軸へ載せる」ものだけが無い。**

### 1.2 コルーチンで押し切れない理由

演出をコルーチンで書くと、以下が全部起きる。ボス演出は 3 本とも 3 秒以上あるので、どれも致命的になる。

- **巻き戻せない。** 演出の良し悪しは数値ではなく尺で決まるのに、0.2 秒ずらすたびにゲームを頭から再生することになる。
- **途中から再生できない。** 現在の絵が「それまでの副作用の積み重ね」なので、4 秒目の絵を見る手段が「4 秒待つ」しかない。
- **途中終了で壊れる。** スクリプト DLL のホットリロードと Play/Stop 往復でコルーチンは破棄される
  ([Script.hpp:1393](../../Projects/Engine/include/Engine/Scene/Script.hpp) の注記)。
  入力を止めて AI を止めた状態でコルーチンだけが消えると、**二度と操作が戻らない**。
- **誰が何を触っているか読めない。** 1 本の演出が Camera / Animator / VFX / Audio / 4 つのマネージャーを
  順不同に叩くコードになり、後から「この揺れはどこから来たか」が追えない。

---

## 2. ゴール / 非ゴール

**ゴール**

- `.sequence` アセット 1 本 = 演出 1 本。シーンから独立して持ち回れる。
- **決定論**: 同じ時刻を指定すれば常に同じ絵になる。エディタでスクラブできる。
- ボス演出 3 本が、新しい C++ クラスを 1 つも書かずに作れる。
- ランタイムの入口は `sequence.Play(path)` の 1 行。演出中の合図は `OnSequenceEvent` で受ける。
- 途中終了しても、触ったものが必ず元へ戻る。

**非ゴール**

- ノードグラフによるオーサリング。VFXEditor が既にその領域を持っている (§11-4)。
- ゲームプレイのステートマシン置き換え。ボスの行動選択は `BossAiComponent` のまま。
- タイムラインからの物理シミュレーション制御。シーケンスが動かす対象は物理の外に置く (§5.5)。
- キーフレームの**カーブ編集**。初版は Step / Linear / Cubic の選択のみ。
- ネットワーク同期・リプレイ。

---

## 3. 全体像

```text
  .sequence (アセット / シーンを知らない)        SequencePlayerComponent (シーン)
  ┌────────────────────────────┐              ┌──────────────────────────┐
  │ duration / wrapMode        │              │ sequencePath             │
  │ [[tracks]]                 │              │ bindings[]               │
  │   type    = "Animation"    │  binding 名  │   "Boss"   → EntityRef   │
  │   binding = "Boss"    ─────┼──────────────┼→  "Camera" → EntityRef   │
  │   clips / keys             │   で突き合わす │ time / speed / playing   │
  └────────────────────────────┘              └──────────────────────────┘
                                                          │
                            Phase::LateScript             ▼
              LateScriptSystem → [SequenceSystem] → VFXGraphSystem → AudioSystem
                                       │
                                Evaluate(tPrev, t)
                                       │
              ┌────────────────────────┼────────────────────────┐
              ▼                        ▼                        ▼
        連続トラック              離散トラック              Event
     (Transform/Property/     (Audio/VFX/Activation)    OnSequenceEvent
      Animation の姿勢)         区間 (tPrev, t] で発火     → ゲーム側が解釈
              │
              ▼  Phase::LateUpdate
      TransformLateUpdate → AnimatorSystem → IKSystem
```

**設計の骨は 3 つ。**

1. **アセットはシーンを知らない** — トラックは名前 (binding キー) しか持たず、実体の解決は Player 側の表 (§4.5)。
2. **進めるのではなく、時刻で評価する** — `Evaluate(t)` は前フレームに依存しない (§5.1)。
3. **触ったものは必ず戻す** — トラックごとに開始時スナップショット / 終了時リストア (§5.4)。

---

## 4. データモデル

### 4.1 用語

| 語 | 意味 |
|---|---|
| **Sequence** | 演出 1 本。`.sequence` ファイル 1 個。 |
| **Track** | 1 つの binding キーに対する 1 系統の変化。種別ごとに中身が違う。 |
| **Clip** | Track 内の時間区間 (AnimationTrack / AudioTrack / VFXTrack が持つ)。 |
| **Key** | Track 内の時刻付きの値 (TransformTrack / PropertyTrack / EventTrack が持つ)。 |
| **Binding** | binding キー → `EntityRef` の対応。Player が持つ。 |

### 4.2 キーと補間は `.anim` の型をそのまま使う

`FloatKey` / `VectorKey` / `QuaternionKey` / `Vector4Key` / `AnimInterp` / `AnimValueType` /
`AnimTargetType` は [AnimationClip.hpp](../../Projects/Engine/include/Engine/Asset/AnimationClip.hpp) に既にある。
**新しいキー型を作らない。**

> **WHY:** 補間規則を 2 か所に持つと、`.anim` の Cubic とシーケンスの Cubic が別物になった瞬間に
> 「Animator で見たときと演出で見たときで動きが違う」が出る。原因を特定できる形の壊れ方ではない。
> 型を共有していれば、評価関数も 1 つで済む。

### 4.3 トラック共通ヘッダー

```cpp
enum class SequenceTrackType : uint8_t {
    Animation = 0, Transform, Property, Activation, Audio, VFX, Event,
};

struct SequenceTrack {
    SequenceTrackType type = SequenceTrackType::Event;
    std::string name;        // 表示名 (エディタのみ)
    std::string binding;     // バインディングキー。空 = ターゲット無し (Event のみ許す)
    bool  muted        = false;
    bool  restoreOnStop = true;   // 触った値を停止時に戻すか (§5.4)
};
```

### 4.4 トラック 7 種

| Track | 何を書くか | 連続/離散 | 停止時の復帰 |
|---|---|---|---|
| **AnimationTrack** | `AnimatorComponent` の Slot (§4.4.1) | 連続 | Slot をフェードアウト |
| **TransformTrack** | `Transform` の position / rotation / scale | 連続 | スナップショット復帰 |
| **PropertyTrack** | Component / Material / Morph のプロパティ | 連続 | スナップショット復帰 |
| **ActivationTrack** | `GameObject::active` | 離散 (状態) | スナップショット復帰 |
| **AudioTrack** | `AudioSourceComponent` / one-shot | 離散 | ループのみ停止 |
| **VFXTrack** | `VFXGraphComponent` の Restart / Stop | 離散 (+scrub) | `Stop()` |
| **EventTrack** | Script への通知 | 離散 | 何もしない |

#### 4.4.1 AnimationTrack — 既存の Slot に乗せる

```cpp
struct SequenceAnimationClip {
    double start = 0.0;          // シーケンス時刻
    double duration = 0.0;       // 0 ならクリップ長
    std::string sourcePath;      // guid: 付き。.fbx / .anim
    std::string clipName;
    float  speed    = 1.0f;
    bool   loop     = false;
    double clipIn   = 0.0;       // クリップ内の開始位置 (秒)
    float  blendIn  = 0.0f;
    float  blendOut = 0.0f;
};

struct SequenceAnimationTrack : SequenceTrack {
    std::string layerName = "Base";      // Animator のレイヤー名
    std::vector<SequenceAnimationClip> clips;   // start 昇順・重なりなし
};
```

**新しい再生器を作らず、`AnimationSlotPlayback` へ時刻を書く。**
必要なエンジン側の変更は `AnimationSlotPlayback` にフィールドを 1 つ足すことだけ。

```cpp
// AnimatorComponent.hpp / AnimationSlotPlayback
bool driven = false;   // true の間、AnimatorSystem は time を進めない (外部が書く)
```

> **WHY レイヤー合成を自前でやらないか:** マスク・Additive 参照ポーズ・レイヤー重みの合成規則は
> `AnimatorSystem` にしかない。シーケンスが独自にポーズを混ぜると、ボスの `Aim` (Override) と
> `Add_Hit` (Additive) をどう扱うかが 2 か所に分かれる
> ([Boss/README.md](../../GreenWare/Assets/Models/Boss/README.md) の 2 レイヤー構成)。
> Slot に流し込む形なら、合成は今までどおり Animator が 1 か所で決める。

**制約: 1 AnimationTrack = 1 レイヤー。** Slot はレイヤーごとに 1 本しかないので、
同一レイヤーでクリップをクロスフェードすることはできない。重ねたい場合はレイヤーを 2 枚使い、
トラックも 2 本置く。初版はこの制約を受け入れる (§12-1)。

#### 4.4.2 TransformTrack

```cpp
enum class SequenceTransformSpace : uint8_t {
    Local = 0,        // 親ローカル
    World,            // ワールド絶対
    RelativeToStart,  // 再生開始時の姿勢を原点とした相対
};

struct SequenceTransformTrack : SequenceTrack {
    SequenceTransformSpace space = SequenceTransformSpace::Local;
    AnimInterp interp = AnimInterp::Cubic;
    std::vector<VectorKey>     positions;
    std::vector<QuaternionKey> rotations;
    std::vector<VectorKey>     scales;
};
```

`RelativeToStart` が要る理由は、ボスの登場を**アリーナのどこで再生しても同じ形にする**ため。
World で焼くと、ボスの出現位置を変えるたびに全キーを打ち直すことになる。

**契約: TransformTrack のターゲットに Animator のボーンを指定しない。**
`SequenceSystem` は `AnimatorSystem` より前 (§5.3) なので、ボーンへ書いても同フレームで上書きされる。
違反は「ターゲットが `BoneComponent` を持つ」で検出でき、警告を 1 回だけ出す。

#### 4.4.3 PropertyTrack

`PropertyAnimationTrack` ([AnimationClip.hpp:73](../../Projects/Engine/include/Engine/Asset/AnimationClip.hpp)) を
そのまま使い、`targetPath` の代わりに `binding` で解決する。
`AnimTargetType::MaterialProperty` があるので、**極性リングの発光を時間で上げ下げする**用途がそのまま乗る。

#### 4.4.4 ActivationTrack / AudioTrack / VFXTrack

```cpp
struct SequenceRange { double start = 0.0; double end = 0.0; };

struct SequenceActivationTrack : SequenceTrack {
    std::vector<SequenceRange> ranges;   // この区間だけ active
};

struct SequenceAudioClip {
    double start = 0.0;
    std::string clipPath;     // guid: 付き
    std::string bus = "SE";
    float volume = 1.0f;
    bool  loop   = false;     // loop は end で止める
    double end   = 0.0;
};

struct SequenceVfxClip {
    double start = 0.0;
    double end   = 0.0;       // end で Stop()。0 ならグラフ側の尺に任せる
    bool   restart = true;    // start で Restart() するか Resume() か
};
```

VFXTrack はターゲットの `VFXGraphComponent` を叩くだけで、グラフ自体は `.vfx` が持つ。
スクラブ中は `editorScrubTime` へ書く ([VFXGraphComponent.hpp:91](../../Projects/Engine/include/Engine/Scene/Components/VFXGraphComponent.hpp)) —
VFXEditor が既に使っている決定論スクラブの経路にそのまま乗る。

#### 4.4.5 EventTrack

```cpp
struct SequenceEventKey {
    double time = 0.0;
    std::string name;         // "boss.entry.impact" のような点付き名
    int32_t intParam   = 0;
    float   floatParam = 0.0f;
};

struct SequenceEventTrack : SequenceTrack {
    std::vector<SequenceEventKey> keys;
};
```

**演出専用トラック (カメラ寄せ / レターボックス / ヒットストップ / スロー) を作らない。**

> **WHY:** それらは `CameraFollowManagerComponent` / `HitstopManagerComponent` /
> `TimeManagerComponent` といった**ゲーム側マネージャーの語彙**であって、エンジンの語彙ではない。
> エンジンに `HitstopTrack` を生やすと、GreenWare 専用の器がエンジンに 1 つ増える。しかも
> 「強さ 0.8」の意味を決めているのはゲーム側なので、エンジンは値を右から左へ渡すだけになる。
> EventTrack 1 本あれば全部渡り、受け手はゲーム側に置いたままにできる。

### 4.5 バインディング — アセットは `EntityRef` を持たない

`.sequence` が持つのは binding **キー名**だけ。実体の対応は `SequencePlayerComponent` が持つ。

```cpp
struct SequenceBinding {
    std::string key;      // .sequence 側の binding 名と一致させる
    EntityRef   target;
};
```

> **WHY アセットへ焼かないか:** `.scene` の `EntityRef` は GameObject の `instanceId` (GUID) で解決される
> ([SceneSerializer.cpp:593](../../Projects/Engine/src/Scene/SceneSerializer.cpp))。これをアセットへ焼くと
> **その .scene 専用の演出**になり、同じ登場演出を別のシーンやテスト用シーンで再生できない。
> `AnimationClip::targetPath` が Animator 相対のパスしか持たないのと同じ判断。

- 予約キー **`$self`** — Player を持つ GameObject 自身。単体オブジェクトの演出をバインド無しで書ける。
- **タグ解決はエンジンでやらない。** `"Player"` タグを引くのはゲームの知識なので、
  スクリプトから `sequence.Bind("Player", go)` を呼ぶ。
- 解決できないキーがあったら、**そのトラックだけ黙らせて警告を 1 回**出す。
  シーケンス全体を止めない (演出の一部が欠けても進行は止まらないほうが被害が小さい)。

---

## 5. 評価モデル

### 5.1 「進める」のではなく「時刻で評価する」

```cpp
enum class SequenceEvalFlags : uint32_t {
    None    = 0,
    Scrub   = 1 << 0,   // エディタのスクラブ。離散トラックを発火しない
    Restore = 1 << 1,   // 停止処理。連続トラックを復帰値で 1 回だけ書く
};

void Evaluate(double tPrev, double t, SequenceEvalFlags flags);
```

| トラック | 評価規則 |
|---|---|
| Transform / Property / Animation の姿勢 | **`t` だけの関数**。`tPrev` を見ない |
| Activation | `t` が範囲内かどうか。状態なので `tPrev` を見ない |
| Event / Audio / VFX | 半開区間 **`(tPrev, t]`** に入るキー・クリップ頭を、時刻昇順に発火 |

規則を 3 つ固定する。

1. **`t < tPrev` (逆再生・巻き戻し) では離散トラックを発火しない。** 連続トラックと Activation は再解決する。
2. **`Scrub` では離散トラックを発火しない。** VFX だけは `editorScrubTime` を書いて絵を出す。
3. **1 フレームで複数キーをまたいだら全部発火する。** 落ちたフレームでイベントが消えると、
   入力を戻す `end` イベントだけが飛ばされる、という壊れ方をする。

### 5.2 ラップ

```cpp
enum class SequenceWrapMode : uint8_t { Once = 0, Loop, HoldEnd };
```

- `Once` — 終端で停止し、`restoreOnStop` に従って復帰する。
- `Loop` — `t = fmod(t, duration)`。ラップした frame は `(tPrev, duration]` と `(0, t]` の 2 区間を発火する。
- `HoldEnd` — 終端の姿勢を保ったまま再生状態を続ける。**ボスの撃破**がこれ (Death は崩れたまま終わる)。

### 5.3 実行フェーズ

`SequenceSystem` を **`Phase::LateScript`** に置き、`LateScriptSystem` の後・`VFXGraphSystem` の前に並べる。

```cpp
Phase GetPhase() const override { return Phase::LateScript; }
OrderingHints GetOrder() const override {
    return OrderingHints{}.After<LateScriptSystem>()
                          .Before<VFXGraphSystem>()
                          .Before<AudioSystem>();
}
RunMode GetRunMode() const override { return RunMode::Always; }   // 停止中のスクラブ用
```

| 書いた先 | 誰が同じフレームで消費するか |
|---|---|
| Transform (local) | `TransformLateUpdate` (Phase::LateUpdate) がワールド行列を作り直す |
| Animator の Slot | `AnimatorSystem` (Phase::LateUpdate) がサンプリングする |
| `VFXGraphComponent` | `VFXGraphSystem` (同 Phase、後ろ) |
| `AudioSourceComponent` | `AudioSystem` (同 Phase、後ろ) |
| Material プロパティ | 描画がそのまま読む |

> **WHY `Phase::LateUpdate` に入れないか:** `TransformLateUpdate` は `AnimatorSystem` より前で、
> シーケンスがそこより後ろに入るとワールド行列が 1 フレーム古いまま描画される。
> `LateScript` に置けば、Transform も Animator も VFX も Audio も**全部同じフレームで拾われる**。

`RunMode::Always` にするのは、**エディタで停止したままスクラブする**ため。
シミュレーション中でないときは `editorPreview = true` の Player だけを評価する。

### 5.4 スナップショットと復帰

**演出は必ず途中で死ぬ** — スクリプト DLL のホットリロード、Play/Stop 往復、シーン遷移。
死んだときに「入力を止めたまま」「ボスが空中に浮いたまま」を残さない仕組みを、
呼び出し側の後始末ではなく**トラックの性質として**持たせる。

- 再生開始時、各トラックは**自分が書く値だけ**をスナップショットする。
- 停止時 (正常終了・`Stop()`・`OnDisable`・`OnDestroy`) に `Evaluate(..., Restore)` を 1 回だけ流す。
- 戻すのは Transform / Property / Activation / Animation (Slot 停止) の 4 つ。
  Audio と VFX と Event は**戻せない出来事**なので、ループしている音を止める以外は何もしない。

`SequencePlayerComponent` の再生位置 (`time` / `playing`) は Reflect しない。
Play/Stop 往復で既定値へ戻るのが正しい ([SceneSerializer 往復の性質](../../Projects/Engine/src/Scene/SceneSerializer.cpp))。

### 5.5 物理を持つ対象

TransformTrack が `RigidBodyComponent` を持つ対象へ書くと、`Phase::PrePhysics` の同期と押し合いになる。
シーケンス側で勝手に kinematic にはせず、**契約として明示**する。

> 物理を持つ対象を動かすシーケンスは、`PropertyTrack` で `RigidBodyComponent.isKinematic` を
> 区間の頭で `true`、終わりで元へ戻すこと。`restoreOnStop` が効くので、途中終了しても戻る。

### 5.6 時間スケール

```cpp
enum class SequenceTimeMode : uint8_t { Scaled = 0, Unscaled };
```

既定は `Scaled`。ヒットストップやスローがかかると演出も一緒に伸びる (通常はこれが自然)。
`Unscaled` は「止まっている画面の上で進めたい」演出専用 — ボスの撃破がこれ (§10.3)。

**シーケンスから `timeScale` を書かない。** スローをかけるのは `TimeManagerComponent` の仕事で、
シーケンスは EventTrack で合図するだけ。書き手を 2 つにすると、どちらが最後に書いたかで結果が決まる。

---

## 6. ランタイム API

### 6.1 `SequencePlayerComponent`

```cpp
struct SequencePlayerComponent {
    std::string sequencePath;                 // guid: 付き
    std::vector<SequenceBinding> bindings;
    bool  playOnAwake = false;
    float speed       = 1.0f;
    SequenceWrapMode wrapMode = SequenceWrapMode::Once;   // アセット既定を上書き
    SequenceTimeMode timeMode = SequenceTimeMode::Scaled;

    // ランタイム状態 (Reflect しない)
    bool   playing = false;
    double time    = 0.0;
    double timePrev = 0.0;
    float  editorScrubTime = -1.0f;

    void Play();  void Stop();  void Pause();  void Resume();
    void SetTime(double t);
};
```

### 6.2 `sequence` ScriptProxy

```cpp
struct ScriptSequenceProxy {
    // Self の SequencePlayerComponent を対象にする
    void Play(std::string_view path) const;
    void Play() const;
    void Stop() const;
    void Pause() const;
    void Resume() const;
    void SetTime(float seconds) const;
    void SetSpeed(float speed) const;
    void Bind(std::string_view key, GameObject* target) const;

    [[nodiscard]] bool  IsPlaying() const;
    [[nodiscard]] float Time() const;
    [[nodiscard]] float Duration() const;

    // 別オブジェクトの Player を回す版
    void PlayOn(GameObject& owner, std::string_view path) const;
    void StopOn(GameObject& owner) const;
};
```

`ScriptProxyMembers.inl` の**末尾**へ追加する
([ScriptProxyMembers.inl](../../Projects/Engine/include/Engine/Scene/ScriptProxy/ScriptProxyMembers.inl) の
「新規 Proxy は末尾へ」の注記どおり。既存プロキシのオフセットが動くと DLL の ABI が壊れる)。

### 6.3 受け取り側

`AnimationEventInfo` と**同じ形**にする。覚えることを増やさない。

```cpp
// Script.hpp
struct SequenceEventInfo {
    const char* name       = "";
    int32_t     intParam   = 0;
    float       floatParam = 0.0f;
    float       sequenceTime = 0.0f;
    const char* sequenceName = "";
};

virtual void OnSequenceEvent(const SequenceEventInfo&) {}
virtual void OnSequenceFinished(const char* /*sequenceName*/) {}
```

配送先は 2 系統。

| binding | 配送先 |
|---|---|
| 指定あり | そのターゲット GameObject に載っている全 Script の `OnSequenceEvent` |
| 空 | `ScriptEventBus` へ `SequenceEvent{ name, intParam, floatParam }` を Publish |

> **WHY 2 系統持つか:** 「ボス自身に効く合図」(発光を消す) はターゲットへ直接届けたいが、
> 「盤面全体に効く合図」(入力を止める・BGM を切り替える) の受け手は演出の当事者ではない。
> 後者を binding で縛ると、演出のたびに GameFlow を全シーケンスへバインドして回ることになる。

**Script へ仮想関数を足すときは既存の後ろへ追加する** (vtable のオフセットが動くと DLL 側が壊れる)。

### 6.4 使用例

```cpp
void BossDirectorComponent::OnStart()
{
    sequence.Bind("Boss",   scene.Find("Boss"));
    sequence.Bind("Camera", scene.FindWithTag("MainCamera"));
}

void BossDirectorComponent::OnSequenceEvent(const SequenceEventInfo& e)
{
    const std::string_view name = e.name;
    if (name == "boss.entry.begin") {
        SetGameplayGate(true);
    } else if (name == "boss.entry.impact") {
        if (auto* fx = ImpactFeedbackManagerComponent::Instance())
            fx->Play(FeedbackEvent::AnchorImpact, e.floatParam);
    } else if (name == "boss.entry.end") {
        SetGameplayGate(false);
    }
}
```

**ゲートは必ず「入れる合図」と「戻す合図」を対にする。** どちらかが欠けると操作が戻らない。
`SetGameplayGate` は `OnDisable` / `OnDestroy` でも false へ戻すこと (§5.4 と同じ理由)。

---

## 7. アセット形式 (`.sequence` / TOML)

`.animcontroller` と同じく TOML。GUID 参照は `guid:<32hex>|<path>`
([GuidRefCodec.hpp](../../Projects/Engine/include/Engine/Asset/GuidRefCodec.hpp))。

```toml
[sequence]
name     = "SEQ_Boss_Entry"
duration = 6.50
wrapMode = "Once"
timeMode = "Scaled"

# --- 落下 → 着地 ---
[[tracks]]
type    = "Animation"
name    = "Boss Body"
binding = "Boss"
layer   = "Base"

  [[tracks.clips]]
  start  = 0.00
  source = "guid:1f0c…|Assets/Models/Boss/FallIdle.fbx"
  clip   = "FallIdle"
  loop   = true
  duration = 1.10

  [[tracks.clips]]
  start  = 1.10
  source = "guid:44a9…|Assets/Models/Boss/Land.fbx"
  clip   = "Land"
  blendIn = 0.08

[[tracks]]
type    = "Transform"
name    = "Boss Fall"
binding = "Boss"
space   = "RelativeToStart"
interp  = "Cubic"
positions = [
  { time = 0.00, value = [ 0.0, 38.0, 0.0 ] },
  { time = 1.80, value = [ 0.0,  0.0, 0.0 ] },
]

[[tracks]]
type    = "Event"
name    = "Cues"
binding = ""
keys = [
  { time = 0.00, name = "boss.entry.begin" },
  { time = 1.80, name = "boss.entry.impact", float = 1.0 },
  { time = 2.40, name = "boss.entry.name" },
  { time = 6.50, name = "boss.entry.end" },
]
```

ローダーは `IAssetImporter<SequenceAsset>` の実装 1 本。

```cpp
class SequenceImporter final : public IAssetImporter<SequenceAsset> {
    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".sequence" };
        return kExts;
    }
};
```

`.meta` は他のアセットと同じ規約に従う (GUID を持つ)。

---

## 8. エディタ

初版は **Panel 1 枚**。VFXEditor のような MVP 分割はしない (トラック 7 種・キー編集のみで、
グラフ・ノード・配線が無いため分ける対象が無い)。

```text
┌ Sequence ────────────────────────────────────────────────────┐
│ [SEQ_Boss_Entry.sequence ▾]  ◀◀ ▶ ⏸ ▶▶   t = 1.80 / 6.50    │
├───────────────┬──────────────────────────────────────────────┤
│ ▸ Boss Body   │ ▓▓▓▓▓▓▓░░░░│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓             │
│   Animation   │  FallIdle   │  Land                          │
│ ▸ Boss Fall   │ ●───────────●                                │
│   Transform   │                                              │
│ ▸ Cues        │ ◆         ◆    ◆                       ◆     │
│   Event       │                                              │
└───────────────┴──────────────────────────────────────────────┘
```

- 時間軸のスクラブは `SequencePlayerComponent::editorScrubTime` へ書き、`RunMode::Always` の
  `SequenceSystem` が評価する。**離散トラックは発火しない** (§5.1)。
- バインディングの割り当ては Inspector の `SequencePlayerComponent` で行う
  (`widgets::` の既存フィールドを使う)。
- **Record モードは作らない。** キーは数値入力とドラッグのみ。

`kScrubStep = 1.0f / 60.0f` などの定数は
[VFXEditorUiCommon.hpp](../../Projects/Editor/include/Editor/VFXEditor/Views/VFXEditorUiCommon.hpp) と揃える。

---

## 9. 追加・変更するファイル

| ファイル | 内容 |
|---|---|
| `Engine/Asset/SequenceAsset.hpp` | 新規。§4 の型定義 |
| `Engine/Asset/SequenceImporter.hpp` / `.cpp` | 新規。`.sequence` TOML ローダー |
| `Engine/Scene/Components/SequencePlayerComponent.hpp` | 新規。§6.1 |
| `Engine/Scene/Systems/SequenceSystem.hpp` / `.cpp` | 新規。§5 |
| `Engine/Scene/ScriptProxy/SequenceProxy.hpp` | 新規。§6.2 |
| `Engine/Scene/ScriptProxy/ScriptProxyMembers.inl` | **末尾へ** 1 行追加 |
| `Engine/Scene/ScriptProxy/AllScriptProxies.hpp` | include 追加 |
| `Engine/Scene/Script.hpp` | `SequenceEventInfo` / `OnSequenceEvent` / `OnSequenceFinished` を**末尾へ** |
| `Engine/Scene/Components/AnimatorComponent.hpp` | `AnimationSlotPlayback::driven` を 1 つ追加 |
| `Engine/src/Scene/Systems/AnimatorSystem.cpp` | `driven` の間は `slot.time` を進めない |
| `Engine/src/Scene/SceneManager.cpp` | `Phase::LateScript` へ `SequenceSystem` を登録 |
| `Engine/src/Scene/SceneSerializer.cpp` | `SequencePlayerComponent` の入出力 |
| `Editor/include/Editor/Panels/SequencePanel.hpp` / `.cpp` | 新規 |
| `Editor/src/EditorApp.cpp` | パネル登録 |

---

## 10. ボス演出への当てはめ

企画書 8 章 / 10.6 のボス「ポラリティ・コア」。クリップの時刻は
[Boss/README.md](../../GreenWare/Assets/Models/Boss/README.md) の実測値。

**共通のバインディング**: `Boss` / `BossCore` (発光メッシュ) / `Camera` / `Director` (未使用、Event は broadcast)。

### 10.1 `SEQ_Boss_Entry` — 登場 (6.50 秒 / Scaled)

登場専用クリップは無いので、**`FallIdle` → `Land` を空からの落下として使う。**
`Land` は f22 = 0.70 秒が接地・衝撃フレームなので、接地を 1.80 秒に置くならクリップ開始は 1.10 秒。

| 時刻 | Track | 内容 |
|---|---|---|
| 0.00 | Event | `boss.entry.begin` — 入力停止 / `BossAiComponent` 停止 / 雑魚の供給停止 |
| 0.00 | Activation (Boss) | 表示を開始 |
| 0.00 | Animation (Boss/Base) | `FallIdle` loop |
| 0.00 | Transform (Boss, RelativeToStart) | +38 m → 0 m。Cubic (加速して落ちる) |
| 0.00 | Transform (Camera, RelativeToStart) | 見上げアングルへ |
| 1.10 | Animation (Boss/Base) | `Land` (blendIn 0.08) |
| **1.80** | Event | `boss.entry.impact` (float 1.0) — 揺れ / ヒットストップ / 振動 / FOV パンチ |
| 1.80 | VFX | `FX_IMP_Explosion` (着地の土煙) |
| 1.80 | Audio | 着地 SE |
| 1.93 | Property (BossCore, emissive) | 0 → 1。**f26 = 0.83 の潰れ最下点の直後に極性リングが灯る** |
| 2.20–4.00 | Transform (Camera) | 頭部へ寄る |
| 2.40 | Event | `boss.entry.name` — ボス名 (`IBoss::BossName`) と HP バーを出す |
| 3.70 | Animation (Boss/Base) | `Land` 終端 (f78 = 2.60 秒) → `Idle` へ戻る |
| 4.40 | Animation (Boss/Base) | `Magnetic_Pulse` (咆哮の代わり。専用クリップが無いため) |
| 5.20 | Event | `boss.entry.pulse` — 衝撃波 VFX + 揺れ。**f24–29 = 0.80–0.97 秒に合わせる** |
| 6.50 | Event | `boss.entry.end` — 入力と AI を戻す |

### 10.2 `SEQ_Boss_PhaseShift` — フェーズ移行 (2.40 秒 / Scaled)

**カットシーンにしない。** 10.6 は「切り替えが来る前にコンボを終わらせる」リズムで成立しているので、
ここで操作を奪うとリズムが切れる。**入力は止めず、カメラを緩めるだけ**にする。
「シーケンスは必ずカットシーンである」わけではない、という例。

| 時刻 | Track | 内容 |
|---|---|---|
| 0.00 | Event | `boss.phase.warn` — カメラを緩める / 警告音。**入力は止めない** |
| 0.00 | Property (BossCore, emissive) | 明滅 (7 Hz、深さ 0.85)。`BossPolarityCoreComponent` の予兆値と同値 |
| 0.10 | Animation (Boss/Base) | `Magnetic_Pulse` (60F / 2.00 秒) |
| **0.90** | Event | `boss.phase.burst` — 磁力パルス発生。**f24 = 0.80 秒 (クリップ開始 0.10 + 0.80)** |
| 0.90 | VFX | 衝撃波リング (半径 22 m) |
| 0.90 | Audio | パルス SE |
| 1.10 | Property (BossCore, emissive) | 新しい極の色へ切り替え |
| 2.40 | Event | `boss.phase.end` |

> 極の切り替えそのもの (`BossPolarityCoreComponent::Switch`) はシーケンスがやらない。
> 極を決めるのはあちらの仕事で、シーケンスは**その瞬間の見た目**だけを持つ。

### 10.3 `SEQ_Boss_Death` — 撃破 (6.00 秒 / **Unscaled** / HoldEnd)

`Death` は 120F / 4.00 秒で、f44 = 1.47 秒に胴体が崩れ落ちる。終端は崩れたままなので `HoldEnd`。
`Unscaled` にするのは、`boss.death.begin` でスロー (0.25 倍) をかけたまま演出だけ実時間で進めるため。

| 時刻 | Track | 内容 |
|---|---|---|
| 0.00 | Event | `boss.death.begin` — 入力停止 / 全敵停止 / BGM フェード / スロー 0.25 |
| 0.00 | Animation (Boss/Base) | `Death` (4.00 秒) |
| 0.00 | Property (Boss) | `EnemyHealthComponent` / `PolarityTargetComponent` を無効化 |
| 0.00–2.00 | Transform (Camera, RelativeToStart) | ボスの正面へ回り込む |
| 0.35 / 0.90 / 1.35 | VFX | 連鎖爆発 3 発 |
| **1.47** | Event | `boss.death.collapse` (float 1.0) — 大きな揺れ + ヒットストップ。**f44 の崩落** |
| 1.47 | Property (BossCore, emissive) | 1 → 0 (消灯) |
| 2.00 | Event | `boss.death.motes` — `EnemyDeathVfxComponent::Begin()` をボス個体で起動 |
| 4.00 | Animation | 終端保持 (`HoldEnd`) |
| 4.20 | Event | `boss.death.clear` — `GameFlowComponent::BeginEnd(true)` |
| 6.00 | — | 終了 (`OnSequenceFinished`) |

### 10.4 攻撃の予兆はシーケンスにしない

突進の溜め・踏みつけの静止・ビームの構えは `BossAiComponent` が持つ**行動**であって演出ではない。
プレイヤーの位置で長さも向きも変わるので、固定の時間軸に載らない。
シーケンスにするのは「**プレイヤーの入力と無関係に、決まった尺で進むもの**」だけに絞る。

---

## 11. 実装順

| Step | 内容 | ここまでで何ができるか |
|---|---|---|
| 1 | `SequenceAsset` + `SequenceImporter` (Event / Activation / Transform のみ) | `.sequence` が読める |
| 2 | `SequencePlayerComponent` + `SequenceSystem` (Evaluate / Snapshot / Restore) | 再生・復帰が動く |
| 3 | `sequence` プロキシ + `OnSequenceEvent` / `OnSequenceFinished` | **合図が全部出せる。演出本体はスクリプトで書ける** |
| 4 | AnimationTrack (`AnimationSlotPlayback::driven`) | ボスのクリップが時間軸に載る |
| 5 | Property / Audio / VFX Track | **C++ を書かずに演出が組める** |
| 6 | `SequencePanel` (スクラブ + キー編集) | 尺を目で詰められる |
| 7 | ボス 3 本 (§10) | — |

Step 3 と Step 5 が実用上のマイルストーン。Step 3 の時点で、
演出をコルーチンではなく**アセットの時間軸**で持てるようになり、§1.2 の 4 つの問題が消える。

---

## 12. 検討して採らなかった案

1. **コルーチンだけで押し切る。** §1.2 の 4 点。特に DLL ホットリロードでゲートが戻らないのが致命的。
2. **`AnimationClip` を複数ターゲット対応に拡張する。** `.anim` は DCC からの焼き込み物で、
   `Library/Baked/` 配下に GUID 導出で置かれる。人が編集する層と混ぜると**再インポートで消える**。
3. **カメラ / レターボックス / ヒットストップの専用トラックを作る。** ゲームの語彙がエンジンへ入る (§4.4.5)。
4. **ノードグラフでオーサリングする。** VFXEditor が既にその形を持っているが、演出は
   「何時何秒に何が起きるか」が主で、依存関係のグラフではない。時間軸を主にしたほうが読める。
5. **Playable 相当の汎用実行グラフを移植する。** 型と抽象が増えるだけで、今要るのはトラック 7 種。
   入れ子が本当に要るようになってから `SubSequenceTrack` を足す。

---

## 13. 未決事項

1. **AnimationTrack のクロスフェードをレイヤー 2 枚で表現する制約** (§4.4.1) は許容できるか。
   ボス 3 本では `Base` 1 枚で足りるが、演出が増えたときに破綻しないか。
2. **キーのカーブ編集**をどこまでパネルでやるか。初版は `AnimInterp` の 3 択のみ。
3. **BGM とシーケンスの尺の同期。** 撃破演出は音楽のキメに合わせたくなるが、
   現状 `AudioSystem` に再生位置を問い合わせる口が無い。
4. **`SubSequenceTrack` (入れ子)** が実際に要るか。ボス 3 本では不要。
5. **複数の Player が同じターゲットへ書いたとき**の優先順位。初版は「後勝ち + 警告」。
