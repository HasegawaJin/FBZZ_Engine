# Audio System 設計書

`fbzz::audio` — XAudio2 ベースの BGM / SE 再生。他モジュールへの依存なし。

---

## 依存関係

```
fbzz::audio
└── XAudio2  (Windows SDK)
```

---

## レイヤー構成

```
AudioSystem          高レベル管理 (BGM/SE, WAV キャッシュ)
    │
    │  IAudioDevice& (参照)
    ▼
IAudioDevice         バックエンド抽象インターフェース
    ▲
XAudio2Device        XAudio2 具体実装
```

IRenderer / DX11Renderer と同じパターン。将来 FMOD 等に差し替える場合は
`IAudioDevice` を実装した新クラスを追加するだけでよい。

---

## IAudioDevice

オーディオバックエンドの抽象。PCM データとフォーマットを渡すと `voiceId` を返す低レベル API。

```cpp
struct WaveFormat {
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t bitsPerSample;
};

class IAudioDevice {
public:
    virtual bool Init()     = 0;
    virtual void Shutdown() = 0;

    // 戻り値 0 は失敗。loop=true でループ再生
    [[nodiscard]] virtual uint32_t PlayBuffer(
        const void* pcmData, size_t bytes,
        const WaveFormat& fmt, bool loop) = 0;

    virtual void StopBuffer(uint32_t voiceId)             = 0;
    virtual void SetVolume(uint32_t voiceId, float volume) = 0;
};
```

---

## XAudio2Device

`IAudioDevice` の XAudio2 実装。

### 内部構造

```
IXAudio2 (ComPtr)
└── IXAudio2MasteringVoice
    └── IXAudio2SourceVoice × N  (voiceId → VoiceEntry マップで管理)
```

- ループ中のボイスは明示的に `StopSound` するまで保持する
- 非ループボイスは `PlaySound` 呼び出し時に `BuffersQueued == 0` のものを自動解放 (`PurgeFinishedVoices`)

---

## AudioSystem

WAV ファイルの読み込み・キャッシュと BGM/SE の高レベル管理。

```cpp
class AudioSystem {
public:
    explicit AudioSystem(IAudioDevice& device);

    bool Init();
    void Shutdown();

    void PlayBGM(const std::string& path, bool loop = true);
    void StopBGM();
    void PlaySE(const std::string& path);

    void SetBGMVolume(float volume);  // 0.0f 〜 1.0f
    void SetSEVolume(float volume);
};
```

### WAV キャッシュ

同じパスのファイルは初回ロード後 PCM データを `std::vector<uint8_t>` でキャッシュする。
2 回目以降はディスクアクセスなし。

### BGM 管理

BGM ボイスは常に 1 本。`PlayBGM` 呼び出し時に前の BGM を自動停止してから新しいボイスを生成する。

### SE 管理

`PlaySE` のたびに新しいボイスを `IAudioDevice::PlaySound` で生成する。
終了検知・解放は `XAudio2Device` 側の `PurgeFinishedVoices` が担う。

### 使い方

```cpp
fbzz::audio::XAudio2Device device;
fbzz::audio::AudioSystem   audio(device);
audio.Init();

audio.PlayBGM("assets/bgm/title.wav");
audio.SetBGMVolume(0.8f);

audio.PlaySE("assets/se/jump.wav");

audio.StopBGM();
audio.Shutdown();
```

---

## ファイルローダー

拡張子で自動ディスパッチする。

| 拡張子 | ローダー |
|--------|---------|
| `.wav` / `.WAV` | `LoadWav` (手動 RIFF パース) |
| それ以外 (`.mp3` 等) | `LoadWithMediaFoundation` |

### LoadWav

`AudioSystem::LoadWav` が RIFF/WAV を手動パース。PCM (audioFormat == 1) のみ対応。

```
RIFF chunk
└── WAVE
    ├── fmt  chunk → WaveFormat に変換
    └── data chunk → pcm バッファに格納
```

未知チャンク (`LIST`, `INFO` 等) はサイズ分スキップして継続する。

### LoadWithMediaFoundation

`IMFSourceReader` で MP3 / AAC / WMA 等を PCM にデコードする。

- `MFStartup` / `MFShutdown` は `AudioSystem::Init` / `Shutdown` で呼ぶ
- 出力フォーマットを `MFAudioFormat_PCM` に固定してから全サンプルを読み込む
- パスは ASCII 前提 (日本語パス非対応)

---

## ファイル構成

```
engine/include/engine/Audio/
├── IAudioDevice.hpp
├── XAudio2Device.hpp
└── AudioSystem.hpp

engine/src/Audio/
├── XAudio2Device.cpp
└── AudioSystem.cpp
```
