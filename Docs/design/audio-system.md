# Audio System — 手続き音生成と Mixer Bus

作成日: 2026-08-23 / 対象: `fbzz::audio`, `fbzz::scene` (Audio 系), `fbzz::editor`

---

## 1. 現状と課題

空間側 (`AudioSystem`) は距離減衰・等電力パン・ReverbZone・Raycast 遮蔽まで実装済みで、
作り直す理由はない。穴は**入口 (音の作り方)** と**出口 (音量の統合)** の 2 箇所に集中している。

| # | 課題 | 現状 |
|---|------|------|
| 1 | 音素材が無いと何も鳴らない | `GreenWare/Assets` に Sounds フォルダ自体が存在しない |
| 2 | BGM/SE の 2 バス構造が退化 | `PlayBGM` / `PlaySE` は**全コードベースで呼び出し元ゼロ**。実際に動くのは `PlayVoice` のみ |
| 3 | ProjectSettings の音量が死に設定 | `bgmVolume` / `seVolume` はスライダーも永続化もあるが、`AudioManager::SetBGMVolume/SetSEVolume` の呼び出し元が無い |
| 4 | `AudioMixerSendComponent.busName` が未使用 | バスが存在しないため、文字列を持つだけで `sendLevel` が素のゲイン倍率になっている |
| 5 | `.flac` の参照が壊れる | AssetBrowser / `ScriptAssetType::AudioClip` では選べるのに `ShouldHaveMeta` に無く、guid が振られない |

`.wav` / `.mp3` / `.ogg` の `.meta` 生成と `guid:` 参照化は正常に機能している (確認済み)。
`GuidRefCodec` は拡張子駆動で TOML 全文字列を走査するため、`clipPath` / `soundPath` は
どのアセット種別からでも自動で guid 化される。

---

## 2. ゴール

1. **音素材ゼロで音が鳴る。** パラメーターから PCM を合成し、実行時に再生する。
2. **手続き音が第一級アセットになる。** `.synth` として保存・guid 参照・Editor で編集できる。
3. **スクリプトから音を「作れる」。** 固定クリップの再生ではなく、パラメーターを組み立て・
   変化させて鳴らす経路を `ScriptProxy` に公開する。
4. **音量が 1 本の経路に統合される。** 名前付き Mixer Bus を導入し、ProjectSettings と結線する。

---

## 3. 全体像

```
Assets/Sounds/laser.synth ──▶ SynthAsset::Load ──▶ SynthSpec ─┐
Script が組み立てた SynthSpec ────────────────────────────────┤
                                                              ▼
                                                     Synth::Render
                                                              │ PCM
Assets/Sounds/bgm.mp3 ──▶ Media Foundation ──────────────────┤
Assets/Sounds/hit.wav ──▶ RIFF parse ────────────────────────┤
                                                              ▼
                                              AudioManager (クリップ登録簿)
                                                              │ PlayVoice(clip, bus)
                                                              ▼
                                              AudioMixer (バスグラフ)
                                                              ▼
                                              IAudioDevice ──▶ XAudio2Device
                                                                 (submix voice)
```

**設計の要:** 合成結果を「クリップ」という 1 つの概念へ落とし、`.wav` と同じ経路に合流させる。
これにより `AudioSourceComponent.clipPath` / VFX Graph の Audio ノード / BehaviorTree の
`PlayAudio` / `ScriptAudioProxy::Play` の**すべてが無改修で手続き音を扱える**。
`.synth` をアセットにする選択の最大の利点はここにある。

---

## 4. Layer 1 — Synth コア (`Engine/Audio/Synth.hpp` / `Synth.cpp`)

XAudio2 にも Scene にも依存しない純粋な PCM 生成器。決定論的で、同じ `SynthSpec` からは
常に同じバイト列が出る (`seed` を含むため Noise も再現する)。

### 4.1 `SynthSpec`

sfxr 由来のパラメーター集合を整理したもの。**依存ゼロの POD** として
`Engine/Audio/SynthSpec.hpp` に単独で置く (ScriptProxy ヘッダーから include するため)。

```cpp
enum class SynthWave : uint8_t { Sine, Square, Saw, Triangle, Noise };

struct SynthSpec {
    SynthWave wave = SynthWave::Square;

    // エンベロープ (秒)。総再生長 = attack + sustain + decay
    float attack  = 0.0f;
    float sustain = 0.10f;
    float decay   = 0.20f;
    float punch   = 0.0f;    // sustain 開始直後の音量ブースト率 (0-1)

    // 音程
    float startFrequency = 440.0f;  // Hz
    float minFrequency   = 0.0f;    // スライドの下限。0 で下限なし
    float slide          = 0.0f;    // 1 秒あたりの周波数倍率変化 (対数)
    float vibratoDepth   = 0.0f;    // 0-1
    float vibratoRate    = 0.0f;    // Hz

    // 音色
    float dutyCycle      = 0.5f;    // Square のみ。0-1
    float dutySweep      = 0.0f;    // 1 秒あたりの duty 変化
    float lowPassCutoff  = 1.0f;    // 0-1 正規化。1 で無加工
    float lowPassSweep   = 0.0f;
    float highPassCutoff = 0.0f;    // 0-1 正規化。0 で無加工
    float bitCrush       = 0.0f;    // 0 で無加工、1 に近いほど粗い量子化

    // 反復・アルペジオ (コイン取得・パワーアップの「ピロン」を作る)
    float repeatRate  = 0.0f;       // Hz。0 で反復なし
    float arpeggioMod = 0.0f;       // 反復ごとの周波数倍率。0 で無変化

    // 出力
    float    amplitude  = 0.5f;     // 0-1
    uint32_t seed       = 0;        // Noise の再現性
    uint32_t sampleRate = 0;        // 0 = 44100
};
```

sfxr から**落としたもの**と理由:

- `deltaSlide` (スライドの二階微分) — 聴感上の寄与に対してパラメーター数が増えすぎる
- `phaser` — 実装コストの割に用途が「宇宙船」に偏る
- `lowPassResonance` — LPF は遮蔽 (`AudioOcclusionComponent`) でも使うため、
  合成側とエフェクト側で挙動が二重定義になるのを避ける

### 4.2 API

```cpp
/// spec から 16bit モノラル PCM を生成する。
/// @param outFmt 生成した PCM のフォーマット。sampleRate=0 の spec には 44100 を入れて返す。
/// @ret duration が 0 以下、または amplitude が 0 なら空の vector。
[[nodiscard]] std::vector<uint8_t> Render(const SynthSpec& spec, WaveFormat& outFmt);

enum class SynthPreset : uint8_t {
    Pickup, Laser, Explosion, PowerUp, Hit, Jump, Blip, Count
};

/// プリセットの基準 spec。seed を変えると同系統の別バリエーションになる。
[[nodiscard]] SynthSpec MakePreset(SynthPreset preset, uint32_t seed = 0);

/// spec の各パラメーターを seed 由来の乱数で ±amount だけ揺らす。
/// 足音・被弾音の「毎回わずかに違う」を作る用途。
[[nodiscard]] SynthSpec Mutate(const SynthSpec& base, float amount, uint32_t seed);

/// PCM を .wav (RIFF) としてバイト列化する。Editor の Export と、
/// 生成音を実素材へ焼き出す経路で使う。
[[nodiscard]] std::vector<uint8_t> EncodeWav(const void* pcm, size_t bytes, const WaveFormat& fmt);
```

`Mutate` は「手続き的に音を生成する」という要求の中核。1 つのプリセットから無限に
バリエーションが出せるので、素材を用意せずにゲーム全体の SFX を賄える。

---

## 5. Layer 2 — `.synth` アセット (`Engine/Asset/SynthAsset.hpp` / `.cpp`)

`.physmat` と同じ native アセットの型。TOML で保存し、`.meta` / guid を持つ。

```toml
[synth]
preset         = "Laser"      # 由来の記録。ロード時の値には影響しない
wave           = "Square"
attack         = 0.0
sustain        = 0.06
decay          = 0.18
startFrequency = 880.0
slide          = -6.0
dutyCycle      = 0.35
amplitude      = 0.5
seed           = 7
```

```cpp
[[nodiscard]] bool LoadSynthAsset(const std::string& path, audio::SynthSpec& out);
[[nodiscard]] bool SaveSynthAsset(const std::string& path, const audio::SynthSpec& spec);
```

**既定値と同じ項目は書かない** (`FbxMetaSerializer` と同じ方針)。
差分が読める `.synth` にして、git のレビューで「どこを触ったか」が分かるようにする。

### 5.1 統合が必要な箇所 (`.physmat` の前例に揃える)

| ファイル | 変更 |
|---|---|
| [AssetDatabase.cpp:188](../../Projects/Engine/src/Asset/AssetDatabase.cpp#L188) | `ShouldHaveMeta` に `.synth` と `.flac` を追加 |
| [AssetBrowserCreate.cpp](../../Projects/Editor/src/Panels/AssetBrowser/AssetBrowserCreate.cpp) | Create メニューにプリセット選択付き `.synth` 生成 |
| [AssetBrowserImport.cpp:49](../../Projects/Editor/src/Panels/AssetBrowser/AssetBrowserImport.cpp#L49) | native 扱い (インポート対象外) に追加 |
| [AssetBrowserItems.cpp:260](../../Projects/Editor/src/Panels/AssetBrowser/AssetBrowserItems.cpp#L260) | バッジ `SYNTH` と色 |
| [AssetBrowserPanel.cpp:49](../../Projects/Editor/src/Panels/AssetBrowserPanel.cpp#L49) | `TypeFilter::Audio` に `.synth` を追加 |
| [ImGuiReflector.hpp:87](../../Projects/Editor/include/Editor/ImGuiReflector.hpp#L87) / [:542](../../Projects/Editor/include/Editor/ImGuiReflector.hpp#L542) | `clipPath` フィルタを `.wav,.mp3,.ogg,.flac,.synth` に**統一** (現在 2 種類ある) |
| [ImportSettingsSchema.cpp:37](../../Projects/Editor/src/Import/ImportSettingsSchema.cpp#L37) | `ImportCategory::Native` へ。`.flac` は `Audio` へ |
| [EditorApp_CommandPalette.cpp:43](../../Projects/Editor/src/EditorApp_CommandPalette.cpp#L43) | クイックオープンの拡張子一覧 |
| [EditorContext.hpp:440](../../Projects/Editor/include/Editor/EditorContext.hpp#L440) | ファイル監視によるリロード対象 |
| [JsonReflector.hpp:405](../../Projects/Editor/include/Editor/Ai/JsonReflector.hpp#L405) | AI 経由の型名解決 |

---

## 6. Layer 3 — AudioManager のクリップ層

### 6.1 `.synth` はロード経路の分岐で吸収する

`GetOrLoad(path)` は現在 `.wav` か否かで 2 分岐している。ここに 3 本目を足すだけで、
パス参照するすべての面が手続き音に対応する。

```cpp
const WavBuffer* AudioManager::GetOrLoad(const std::string& path)
{
    // .wav      → LoadWav
    // .synth    → LoadSynthAsset + Synth::Render   ← 追加
    // それ以外  → LoadWithMediaFoundation (.mp3 / .ogg / .flac)
}
```

### 6.2 実行時生成クリップの登録簿

アセットを経由しない、スクリプトが組み立てた `SynthSpec` 用の経路。

```cpp
using ClipId = uint32_t;                     // 0 は無効ハンドル

/// spec を PCM 化して保持し、再生に使えるハンドルを返す。
/// 同じ spec (ハッシュ一致) は同じ ClipId を返し、二重生成しない。
[[nodiscard]] ClipId CreateClip(const SynthSpec& spec);

/// ClipId を voice として再生する。戻り値は既存の voiceId と同じ扱い。
[[nodiscard]] uint32_t PlayClipVoice(ClipId clip, bool loop, BusIndex bus);

/// 参照を手放す。再生中の voice が残っていれば、終了するまで実体は解放しない。
void ReleaseClip(ClipId clip);
```

### 6.3 ⚠ XAudio2 のバッファ寿命 — 最重要の落とし穴

`XAUDIO2_BUFFER::pAudioData` は**ポインタであり、`SubmitSourceBuffer` は PCM をコピーしない**
([XAudio2Device.cpp](../../Projects/Engine/src/Audio/XAudio2Device.cpp))。
生成した `std::vector` をローカル変数のまま渡すと、再生中に解放されてノイズかクラッシュになる。

現状の `m_cache` (`unordered_map<string, WavBuffer>`) がたまたま安全なのは、
node-based コンテナで要素のアドレスが安定し、かつ `Shutdown` まで一切消さないため。
生成クリップは寿命が動的なので、同じ前提を明示的に作る:

- 実体は `std::unordered_map<ClipId, ClipEntry>` で保持 (アドレス安定)
- `ClipEntry` は `refCount` (スクリプトの参照数) と `voiceCount` (再生中の voice 数) を持つ
- **両方 0 になって初めて erase** する
- `StopAllVoices()` (Play→Stop 境界) で全 voice を止めた後、参照ゼロのクリップを一掃する
- 合計バイト数に予算 (既定 32MB) を設け、超過時は `FBZZ_LOG_WARN` で名指しする

### 6.4 Play/Stop 往復での状態

`ClipId` は**シリアライズしない**。Play→Stop で `SceneSerializer` 往復が走るため、
スクリプトが保持した `SynthClip` メンバーは Play のたびに `OnStart` で作り直す前提とする
(`.synth` アセット参照はパスなので往復しても生き残る)。ヘッダーにこの制約を明記する。

---

## 7. Layer 4 — Mixer Bus

### 7.1 バスの定義

```cpp
// Engine/Audio/AudioBus.hpp
using BusIndex = uint16_t;
inline constexpr BusIndex kInvalidBus = 0xFFFF;
inline constexpr BusIndex kMasterBus  = 0;

struct BusDesc {
    std::string name;            // "Master" / "BGM" / "SE" / "UI" ...
    std::string parent;          // 空なら Master 直下。Master 自身は空
    float       volume = 1.0f;
    float       lowPassCutoff = 1.0f;   // 0-1。水中・気絶などの一括加工用
};
```

組み込みは `Master` / `BGM` / `SE` / `UI` / `Voice` の 5 本。
ProjectSettings で追加・階層変更できる。

### 7.2 `IAudioDevice` の拡張

XAudio2 の submix voice へ 1:1 で写す。抽象境界は保つ (XAudio2 型は出さない)。

```cpp
/// バスグラフを構築し直す。descs は親が子より前に並んでいること (トポロジカル順)。
/// 既存 voice はすべて停止してから呼ぶこと。
virtual bool RebuildBuses(const BusDesc* descs, size_t count) = 0;
virtual void SetBusVolume(BusIndex bus, float volume) = 0;
virtual void SetBusLowPass(BusIndex bus, float normalizedCutoff) = 0;

// 既存シグネチャに bus を追加
[[nodiscard]] virtual uint32_t PlayBuffer(
    const void* pcmData, size_t bytes, const WaveFormat& fmt, bool loop, BusIndex bus) = 0;
```

**XAudio2 実装の注意点:**

- submix の `ProcessingStage` は子 < 親 でなければならない。`RebuildBuses` は
  深さを計算して stage に入れる (葉が 0)
- `SetPan` が現在 `SetOutputMatrix(m_masterVoice, ...)` を呼んでいるが、
  バス導入後は **voice の実際の出力先 (submix) を渡さないとパンが効かなくなる**。
  `VoiceEntry` に出力先 submix を持たせて修正する
- 遮蔽・ReverbZone の LPF は従来どおり voice 単位のフィルター。バス LPF とは別段

### 7.3 ProjectSettings

```toml
[audio]
masterVolume = 1.0

[[audio.bus]]
name = "BGM"
volume = 0.8

[[audio.bus]]
name = "SE"
volume = 1.0

[[audio.bus]]
name = "UI"
parent = "SE"
```

**後方互換:** `[[audio.bus]]` が無く旧 `bgmVolume` / `seVolume` があれば、
BGM / SE バスの volume として読み込む。保存時は新形式へ寄せる。

`Application` が ProjectSettings 読み込み後に `AudioManager::ApplyBusSettings()` を呼ぶ。
ProjectSettingsPanel の保存でも同じ経路を通す (課題 3 の解消)。

### 7.4 音源のバス指定

`AudioSourceComponent` に `busName`(既定 `"SE"`) を追加し、これを**主出力**とする。

`AudioMixerSendComponent` は名前どおり**補助センド**の意味に戻す。
ただし XAudio2 の複数出力 (`SetOutputVoices`) を伴うため、今回は主出力の結線までとし、
補助センドは後続とする。それまで `sendLevel` は現行どおりゲイン倍率として動く
(挙動は変わらないが、意味が暫定であることをヘッダーに明記する)。

---

## 8. Layer 5 — ScriptProxy API

`SynthSpec` は依存ゼロの POD なので、`ScriptAudioProxy.hpp` から
`Engine/Audio/SynthSpec.hpp` を直接 include する。既存プロキシも
`Engine/Core/Cursor.hpp` や `Engine/Renderer/RenderSettings.hpp` を include しており前例に沿う。

```cpp
// ScriptAudioProxy への追加

/// 実行時生成クリップのハンドル。Play/Stop 往復で無効になるため
/// メンバーに持つ場合は OnStart で作り直すこと。
struct SynthClip { uint32_t id = 0; [[nodiscard]] bool IsValid() const { return id != 0; } };

// --- 生成 ---
[[nodiscard]] SynthClip Synthesize(const audio::SynthSpec& spec) const;
[[nodiscard]] SynthClip Synthesize(audio::SynthPreset preset, uint32_t seed = 0) const;
/// .synth アセットの spec を可変コピーとして取り出す。
/// 「アセットで音を決めて、スクリプトで毎回揺らす」ための入口。
[[nodiscard]] bool LoadSpec(const AudioClipRef& synthAsset, audio::SynthSpec& out) const;
void ReleaseClip(SynthClip clip) const;

// --- 再生 (自 GO の AudioSource 経由 = 3D 減衰・遮蔽が効く) ---
void PlayClip(SynthClip clip) const;
void PlaySynth(const audio::SynthSpec& spec) const;   // 生成 + 即再生 (使い捨て)

// --- 再生 (Listener 非依存の 2D。UI 音・システム音) ---
void PlaySynth2D(const audio::SynthSpec& spec, std::string_view bus = "UI") const;

// --- Mixer ---
void  SetBus(std::string_view bus) const;             // 自 GO の AudioSource の出力先
void  SetBusVolume(std::string_view bus, float volume) const;
[[nodiscard]] float GetBusVolume(std::string_view bus) const;
```

`SetBusVolume` / `GetBusVolume` はオプション画面の音量スライダーをスクリプトから
組めるようにするためのもの。これが無いとゲーム側で音量設定 UI が作れない。

### 使用例

```cpp
// Assets/Scripts/PlayerGun.hpp
void OnStart() override
{
    // .synth アセットを基準にして、毎回揺らすための spec を取り出す
    if (!audio.LoadSpec(m_shotSynth, m_shotSpec))
        m_shotSpec = audio::MakePreset(audio::SynthPreset::Laser);
}

void Fire()
{
    // 毎発わずかに違う音。素材 1 つでも「使い回し感」が出ない
    m_shotSpec.seed = random.Range(0u, 10000u);
    audio.PlaySynth(audio::Mutate(m_shotSpec, 0.12f, m_shotSpec.seed));
}

FBZZ_SERIALIZE_FIELD(AudioClipRef, m_shotSynth);
audio::SynthSpec m_shotSpec{};
```

---

## 9. Layer 6 — Editor: SFX Editor パネル

`Editor/include/Editor/Panels/SfxEditorPanel.hpp` + `Editor/src/Panels/SfxEditorPanel.cpp`。
`EditorApp.cpp` に登録する。

- **プリセット行** — Pickup / Laser / Explosion / PowerUp / Hit / Jump / Blip の 7 ボタン
- **Randomize / Mutate** — 全パラメーターのランダム生成と、現在値からの微揺らし
- **パラメーター** — `SynthSpec` を分類ごとに (エンベロープ / 音程 / 音色 / 反復 / 出力)
- **波形プレビュー** — レンダリング済み PCM を `ImGui::PlotLines` で表示。
  値を変えた瞬間に波形が変わるのが見えることが、このパネルの価値の半分を占める
- **▶ Preview** — `AudioManager` へ直接投げる。Play モードでなくても鳴らせること
- **Save `.synth`** / **Export `.wav`** — 後者は `Synth::EncodeWav` で実素材へ焼き出す

`.synth` の Inspector は `InspectorPanel_Asset.cpp` に `.physmat` と同じ形
(Undo トラッカー + 遅延保存) で追加し、パネルを開かなくても値が見えるようにする。

### 9.1 AI から触れる形にする

パネルのボタンを直接書かず、**すべて `sfx.*` Operator を呼ぶ**。
`editor.op.list` / `invoke` / `query` は登録簿を総当たりで公開するので、
Operator にした時点で AI からも同じ実装が呼べる (`Docs/design/editor-operator-model.md`)。
専用の MCP ツールを起こすと contracts / tools.ts / C++ ハンドラの 3 箇所へ
同じ意味を書くことになり、人の面には出ないまま層が増える。

| Operator | kind | パレットに出るか |
|---|---|---|
| `sfx.load_preset` | Action | 出る (プリセットごとに 1 行へ展開) |
| `sfx.set_wave` | Action | 出る (波形ごとに 1 行) |
| `sfx.set_param` | Action | 出ない (必須引数 2 つ) |
| `sfx.randomize` / `sfx.mutate` | Action | 出る |
| `sfx.preview` | Action | 出る |
| `sfx.save` / `sfx.export_wav` | Action | 出る |
| `sfx.inspect` | Query | 出ない (Query) |

必須引数を持つ操作はコマンドパレットが自動で除外する
(`EditorApp_CommandPalette.cpp` の `singleEnumParam` / `hasRequiredParam` 判定) ので、
AI 向けの細かい粒度を登録しても人の面は汚れない。

**編集中の実体は `EditorContext::sfxEditorSpec` ただ 1 つ。**
パネルが状態を抱えて Operator が要求を積む形にすると、AI が `sfx.set_param` の
直後に `sfx.inspect` を呼んだとき、パネルがまだ要求を消費しておらず 1 フレーム
古い値が返る。読み書きが同じ場所を指すよう、状態を context へ置いてパネルは
それを描く面にする。

### 9.2 AI は音を聴けない — feedback をどう与えるか

反復オーサリングの前提として、`sfx.inspect` は合成結果の客観的な特徴量を返す
(`audio::Analyze`)。これが無いと「もっと重く」を spec の変更へ翻訳できない。

| 指標 | 意味 | 効かせ方 |
|---|---|---|
| `brightnessHz` | パワー加重の実効周波数 = 明るさ | 下げる → `startFrequency` ↓ / `lowPassCutoff` ↓ |
| `rmsAmplitude` | 体感音量 | `amplitude` と `sustain` |
| `timeToPeakSeconds` | 打撃感 | `attack` ↓ / `punch` ↑ |
| `zeroCrossingHz` | ノイズ性 | `wave = noise` かどうかで大きく変わる |
| `clippedRatio` | 0 より大きければ `amplitude` 過大 | 歪みの検出 |

`brightnessHz` は窓付き FFT のスペクトル重心ではなく、一次差分から求める
RMS 周波数 `fs/(2π)·√(Σ(Δx)²/Σx²)`。必要なのは「明るいか暗いか」の 1 スカラーだけで、
窓関数もビン幅の選択も要らず O(N) で済む。名前も実体に合わせて
「スペクトル重心」ではなく brightness としてある。

`audio_inspect` はミキサーバスの一覧も返す。`busName` に指定できる名前が
分からないと AI は綴りを推測するしかなく、未知の名前は Master へ落ちるだけで
エラーにならない (「なぜか音量設定が効かない」形でしか現れない)。

---

## 10. 実装順

各段は単体で動作確認できる粒度に切ってある。

| 段 | 内容 | 主なファイル |
|---|---|---|
| 1 | Synth コア | `Engine/Audio/SynthSpec.hpp`, `Synth.hpp/.cpp` (新規) |
| 2 | `.flac` / 拡張子まわりの不整合是正 | `AssetDatabase.cpp`, `ImGuiReflector.hpp`, `ImportSettingsSchema.cpp` |
| 3 | `.synth` アセット + ロード経路 | `Asset/SynthAsset.hpp/.cpp` (新規), `AudioManager.cpp` |
| 4 | クリップ登録簿 (寿命・予算) | `AudioManager.hpp/.cpp` |
| 5 | Mixer Bus | `Audio/AudioBus.hpp` (新規), `IAudioDevice.hpp`, `XAudio2Device.hpp/.cpp`, `AudioManager.hpp/.cpp` |
| 6 | ProjectSettings 結線 (課題 3 の解消) | `ProjectSettings.hpp/.cpp`, `Application.cpp`, `ProjectSettingsPanel.cpp` |
| 7 | AudioSourceComponent の bus 対応 | `AudioSourceComponent.hpp`, `AudioSystem.cpp` |
| 8 | ScriptProxy API | `ScriptAudioProxy.hpp`, `ScriptProxies.cpp` |
| 9 | Editor 統合 (AssetBrowser / Inspector) | §5.1 の一覧 |
| 10 | SFX Editor パネル | `Panels/SfxEditorPanel.hpp/.cpp` (新規), `EditorApp.cpp` |
| 11 | AI 連携 (§9.1 / §9.2) | `Op/SfxOperators.cpp` (新規), `Audio/Synth.cpp` の `Analyze`, `EditorBusDispatcher.cpp`, `EditorMcp/src/tools.ts` |

段 5 は既存 voice の停止を伴うため、`SetPan` の出力先修正 (§7.2) を同じ段で必ず行う。
分けると「バスは効くがパンが死ぬ」状態が中間で発生する。

---

## 10.1 使い勝手の是正 (段 12)

実装後にゲーム側コードから逆算して監査したところ、API の形ではなく**周辺の導線**に
問題が集中していた。GreenWare には `audio.PlayOneShot(...)` が 9 箇所すでに書かれて
いるのに、シーンに AudioSource も AudioListener も無く、**全部が無言の no-op** だった。

| 症状 | 原因 | 対処 |
|---|---|---|
| 音が鳴らないのに何も出ない | プロキシは対象コンポーネントが無ければ黙って return。`FBZZ_OPTIONAL_COMPONENT` は赤帯も出さない | 書き込み系 API で `(オブジェクト名, API 名)` ごとに一度だけ Console へ警告 |
| `.synth` / `.mp3` がどのスロットにも入らない | 音声フィールドが `FBZZ_FIELD_FILE(..., ".wav,.ogg")` の手書きで、12 箇所へ複製されていた | `FBZZ_FIELD_AUDIO` を追加し、拡張子の正本を `scene::kAudioClipExtensions` へ一元化。Editor 側の `kAudioClipAssetFilter` もこれを参照する |
| Inspector で設定した clipPath をスクリプトから鳴らせない | 引数なしの `Play()` が無かった | `Play()` を追加 |
| Pause しても続きから再生できない | `StopVoice` で voice ごと破棄していた | `PauseBuffer`/`ResumeBuffer` (XAudio2 は `FlushSourceBuffers` を呼ばなければ位置を保つ) と `Resume()` |
| 同フレームの 2 発目の one-shot が消える | `m_oneShotPath` が単一スロットで上書きされていた | `m_pendingOneShots` の列に変更 (上限 16)。BehaviorTree の PlayAudio も同じ経路へ |
| 発生源が消えた音を鳴らせない | proxy はすべて self の AudioSource 前提 | `PlayAtPoint` を追加。減衰とパンは AudioSystem が Listener を選んだ後に一度だけ焼き込む |
| BGM をスクリプトから切り替えられない | `AudioManager::PlayBGM` が proxy に出ていなかった | `PlayBGM` / `StopBGM` / `IsBGMPlaying` |
| busName の綴りミスが無言で Master へ落ちる | 素のテキスト入力 | `FieldHint::AudioBus` を追加し、Inspector が ProjectSettings のバス一覧から選ばせる |
| 新規シーンに Listener が無い | Camera プリセットに含まれていなかった | `MakeCamera` が `AudioListenerComponent` も付ける |

`PlayOneShot` は音量倍率も受け取るようになった (被弾の強弱を `source.volume` の
書き換えで表現して戻し忘れる、という形の不具合を避ける)。BehaviorTree の
PlayAudio ノードが持っていた `volume` は今まで参照されていなかったが、
one-shot が倍率を持てるようになったので繋いだ。

### AssetBrowser の音声プレビュー

`.wav` / `.mp3` / `.ogg` / `.flac` を選ぶと Inspector 下部に波形・形式・長さと
Play / Loop が出る。テクスチャにプレビューがあって音だけ無いと、目的の素材を
探す手間が桁違いに変わるため。

デコード結果は AudioManager のキャッシュに載り Shutdown まで解放されないので、
**2 MB を超えるファイルは自動で開かず** 「Load Preview」を挟む。数分の BGM が
並んだフォルダをクリックして回るだけでメモリが積み上がるのを防ぐ。

## 11. 実装ノート (設計から変えた点)

実装中に判断を変えた箇所。設計書の上の記述より、こちらが現状の正。

**Inspector の `.synth` は読み取り専用にした** (§9 では `.physmat` と同じ編集面と書いていた)
SFX Editor はファイルの内容をメモリに持って編集する。Inspector からも書けるようにすると、
片方の未保存の変更をもう片方が黙って上書きする。Inspector は波形の要約と Play、
「Open in SFX Editor」に絞り、編集面を 1 つに保つ。

**`PlayBGM` / `PlaySE` / `SetBGMVolume` / `SetSEVolume` は残した** (§1 課題 2 では退化と書いた)
削除ではなく、BGM / SE バスへのショートハンドとして実装し直した。
`SetBGMVolume` は `SetBusVolume("BGM", …)` に等しく、ProjectSettings のスライダーが
そのまま効く。BGM の「同時に 1 本だけ」という意味も使いどころがある。

**`AudioManager::Update()` は Application が回す** (§6.2 では AudioSystem を想定していた)
AudioSystem は SimOnly なので Edit モードでは走らない。そこへ回収を任せると、
SFX Editor のプレビュー再生ぶんが Play を開始するまで解放されない。
`Application::Run(IModule&)` の OnLateUpdate 直後で回す。

**ついでに直した既存の不具合**
`AudioManager::LoadWav` が `std::ifstream(path)` へ論理パスを直接渡していたため、
カレントディレクトリがプロジェクト直下でない場合と、日本語を含むパスで読み込みに
失敗していた (Media Foundation 経由の .mp3 だけが `ToWide` で対応済みだった)。
`AssetManager::ResolveAssetPath` → `FileSystem::PathFromUtf8` を通す形へ統一した。

**バス構成の編集はグラフを組み直さない**
音量とフィルターだけの変更なら submix を作り直さず値だけ送る (§7.2 の
RebuildBuses は再生中の音を全部止めるため)。名前や親を変えたときだけ組み直す。
ただし `reverb` の切り替えは組み直しが要る (残響 XAPO は submix の生成時にしか
差し込めないため)。`ProjectSettingsPanel` の同一グラフ判定にも `reverb` を含めてある。

## 11.1 段 13 — 鳴り方の是正

実装後に「音の破綻」を軸に監査した結果を反映した段。API の追加より、
既存の経路が**耳に出る形で壊れていた**箇所の修正が主。

| 症状 | 原因 | 対処 |
| BGM の切り替わりで音が途切れる | フェードの仕組みが無く `PlayBGM` が前の曲を即 `StopVoice` していた | voice ごとにフェードゲインを持たせ、`PlayBGM(path, loop, fade)` でクロスフェード |
| 音の停止で「プツッ」と鳴る | 波形の途中で `StopBuffer` していた | `StopVoice` が既定 20ms の立ち下がりを挟む。`StopVoiceImmediate` は即断のまま残す |
| one-shot が湧くと source voice が無制限に増える | 同時発音の上限が無かった | `SetVoiceLimit` (既定 48) と `AudioSource.priority` によるスティール。ループ音は奪わない |
| 物陰を横切ると音量と音色が跳ぶ | 遮蔽レイキャストの結果 (0 か 1) を直接当てていた | `AudioOcclusion.transitionTime` (既定 0.15s) で目標値へ寄せる |
| 遮蔽が同一フレームに何度も評価される | 評価が `applyVoiceParameters` の中にあり、one-shot の本数だけ走っていた | 音源ごとに 1 回だけ評価する `evaluateEffects` へ切り出し |
| `obstacleLayerMask` が効かない | `Raycast` に `ColliderFilter` を渡していなかった | レイヤーマスクとトリガー除外を渡す |

### フェードゲインは音量と別枠で持つ

`AudioSystem` は毎フレーム距離減衰から音量を計算し直して `SetVoiceVolume` で書き込む。
フェードを同じ変数へ書くと次のフレームで消えるため、`VoiceState` は
`volume` (上位の意図) と `fadeGain` (フェードの現在値) を分けて持ち、
デバイスへ届くのは常にその積になる。この分離が無いと、
「フェード中の音が距離を変えた瞬間に元の音量へ跳ね返る」形の不具合になる。

## 11.2 段 14 — 空間表現

**ReverbZone は本当に残響を鳴らすようになった**
従来 `wetLevel` はボイスのローパス係数へ潰され、`decayTime` は完全に未参照だった。
XAudio2 の残響 XAPO を submix のエフェクトチェーンに載せ、`BusDesc.reverb` が
true のバスだけが残響を受ける。既定では `SE` と `Voice` のみで、`BGM` / `UI` は素通し
(残響は空間の性質であって音楽に掛けるものではない)。`wet = 0` の間は
`DisableEffect` で DSP ごと止める。

**バスはマスターのチャンネル数でなくステレオで作る**
定位は `SetPan` が左右 2 チャンネルへ書く形しか持っておらず、5.1 の submix を作っても
後ろ 4 本は常に無音だった。加えて XAudio2 の残響は入力 1-2ch しか受け付けないため、
マスターに合わせると 5.1 環境でだけ ReverbZone が無効になる。
マスターへの展開は XAudio2 の既定行列に任せる。

**距離ローパスと前後の手掛かり**
`AudioSource.airAbsorption` (既定 0) で、遠ざかるほど高域を吸う。
背後の音は `kBehindDamping` ぶんだけ曇らせる — 音量は変えない。
前後で音量まで変えると、振り向いただけで大きさが変わり距離を見誤るため。

**Doppler は座標の差分から出す** (`AudioSource.dopplerLevel`、既定 0)
物理の速度を見ないのは、音源が RigidBody を持たない飾りであることが多く、
アニメーションやスクリプトで動かされた分も拾う必要があるため。
差分ゆえテレポートで音速を超える値が出るので、相対速度は ±0.5c で頭を打つ。

**`AudioMixerSendComponent.preFader` は削除した**
XAudio2 の voice 音量はすべての送り先に等しく掛かるので、
単一出力のままでは pre-fader を正しく実装できない。
実装できない意味を持つフィールドは、無いよりも紛らわしい。
補助センド本体 (§7.4) は引き続き後続。

## 12. 検討して採らなかった案

**`synth:` 仮想パス** (`clipPath = "synth:laser?freq=880"`)
既存の参照面をすべて無改修で通せる点は `.synth` アセットと同じだが、パラメーターが
文字列に埋まるため Editor で編集できず、タイプミスが実行時まで分からない。
`.synth` は同じ利点を型と guid 参照を保ったまま得られるので、こちらを採った。

**ハンドル API のみ (アセット無し)**
最小コストだが、Inspector・VFX Graph・BehaviorTree から手続き音を指定できない。
音が「スクリプトを読まないと分からないもの」になり、オーサリングの外側へ出てしまう。

**補助センド (aux send) の即時実装**
`AudioMixerSendComponent` を本来の意味に戻すには XAudio2 の複数出力が要る。
主出力の結線と同時にやると段 5 が膨らむため、後続に切り出した (§7.4)。
