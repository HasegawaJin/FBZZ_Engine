# Audio System 設計書

`fbzz::audio` — XAudio2 ベースの BGM / SE 再生。他モジュールへの依存なし。

---

## 依存関係

```
fbzz::audio
└── XAudio2  (Windows SDK)
```

---

## AudioSystem

BGM (ループ再生) と SE (単発再生) のみ。シーン側からは `audio::AudioSystem` を直接呼ぶ。

```cpp
class AudioSystem {
public:
    bool Init();
    void Shutdown();

    void PlayBGM(const std::string& path, bool loop = true);
    void StopBGM();
    void PlaySE(const std::string& path);

    void SetBGMVolume(float volume);  // 0.0f 〜 1.0f
    void SetSEVolume(float volume);
};
```

### 使い方

```cpp
audio::AudioSystem audio;
audio.Init();

audio.PlayBGM("assets/bgm/title.wav");
audio.SetBGMVolume(0.8f);

// SE はパスをキーにキャッシュし、呼ぶたびに新しい SourceVoice で再生する
audio.PlaySE("assets/se/jump.wav");

audio.StopBGM();
audio.Shutdown();
```

---

## 内部構造

```
XAudio2 (IXAudio2)
└── MasteringVoice
    ├── BGM SourceVoice  (1 本。PlayBGM で差し替え)
    └── SE  SourceVoice  (再生ごとに生成し、終了で自動解放)
```

WAV データは `std::vector<BYTE>` にロードしてキャッシュする。  
同じパスを繰り返し鳴らす SE はキャッシュから再利用する。
