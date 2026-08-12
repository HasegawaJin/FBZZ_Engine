# Audio System — ミキサーバス・ストリーミング・サウンドキュー

既存の `AudioManager` / `AudioSystem` / 空間 Audio コンポーネント群を土台に、
**実体のあるミキサーバス**、**ストリーミング再生**、**サウンドキュー**、
**ボイス管理**を追加する。

## 現状の棚卸し

実装済みで再利用するもの:

| 要素 | 状態 |
|------|------|
| `IAudioDevice` / `XAudio2Device` | 抽象境界は妥当。ボイス単位の volume / pitch / pan / lowpass を持つ |
| `AudioSourceComponent` | 距離減衰 (min/max/rolloff)・`spatialBlend`・pitch を保持 |
| `AudioListenerComponent` | priority による受聴点選択が `AudioSystem` に実装済み |
| `AudioSystem` | 距離減衰・パン・Reverb Zone ブレンド・Occlusion レイキャストを毎フレーム転送 |
| `AudioReverbZoneComponent` / `AudioOcclusionComponent` / `AudioMixerSendComponent` | コンポーネントとして存在 |

## 現状の課題

| # | 課題 | 根拠 |
|---|------|------|
| 1 | **ミキサーバスが名前だけで実体がない** | `AudioMixerSendComponent::busName` はどこからも読まれない。`AudioSystem.cpp:98` は `sendLevel` を単なるゲイン倍率として掛けるのみ |
| 2 | **カテゴリ音量が効かない** | `AudioManager::m_bgmVolume` / `m_seVolume` は `PlayBGM` / `PlaySE` 専用。Scene の全音源が通る `PlayVoice` 経路には一切適用されない |
| 3 | **リバーブが偽物** | `AudioReverbZoneComponent` の wet はローパス量に潰されているだけ。`decayTime` は完全に未使用 |
| 4 | ストリーミング再生がない | `AudioManager::WavBuffer` が PCM 全体をメモリ常駐させる。5 分の BGM (48kHz/16bit/stereo) で約 55MB |
| 5 | 同時発音数の上限がない | `PlayVoice` は無制限に voice を作る。パーティクル連動 SE で容易に破綻する |
| 6 | サウンドキューの概念がない | 足音のランダム差し替え、連続発音のクールダウン、ピッチゆらぎがすべてスクリプト側の手書きになる |
| 7 | Pause が再開位置を保持しない | `AudioSystem.cpp` のコメントで明示済み。Play で先頭に戻る |
| 8 | Occlusion が 0/1 の二値 | 遮蔽の出入りで音量が瞬間的に飛ぶ |

## 設計方針

- **`IAudioDevice` の抽象境界は維持する。** XAudio2 依存は `XAudio2Device` に閉じたままにする。
- **既存コンポーネントのフィールドは壊さない。** `busName` / `wetLevel` / `decayTime` は
  今まで「宣言されていたが効いていなかった」だけなので、**実装を後から与える**形にする。
- **バス構成はアセット化する。** ゲームごとに Master/BGM/SE/Voice/Ambient の構成は異なる。

---

## 1. ミキサーバス

### XAudio2 のサブミックスボイスに写像する

```
SourceVoice (音源ごと)
   └→ SubmixVoice "SE"
          └→ SubmixVoice "Master"
                 └→ MasteringVoice
```

**WHY サブミックスを使うか**: バス音量を「その下の全音源に個別に掛ける」実装にすると、
音源が 200 個あればフレームごとに 200 回の `SetVolume` 呼び出しになる。
サブミックスなら 1 回で済み、さらにバス単位のエフェクト (リバーブ・コンプ) を
1 インスタンスで共有できる。

### `IAudioDevice` の拡張

```cpp
class IAudioDevice {
public:
    // ... 既存メンバは変更なし ...

    // --- バス ---
    // バスを作成する。parentBusId=0 は Master 直下。戻り値 0 は失敗。
    [[nodiscard]] virtual uint32_t CreateBus(const char* name, uint32_t parentBusId) = 0;
    virtual void DestroyBuses() = 0;
    virtual void SetBusVolume(uint32_t busId, float volume) = 0;
    // バスへのリバーブ挿入。wet=0 で実質バイパス。
    virtual void SetBusReverb(uint32_t busId, float wet, float decayTime, float hfRatio) = 0;

    // 再生時に出力先バスを指定する版。busId=0 は Master 直結。
    [[nodiscard]] virtual uint32_t PlayBufferOnBus(
        const void* pcmData, size_t bytes, const WaveFormat& fmt,
        bool loop, uint32_t busId) = 0;

    // 再生中のボイスの出力先バスを差し替える。
    virtual void SetVoiceBus(uint32_t voiceId, uint32_t busId) = 0;
};
```

**WHY `PlayBuffer` を残して `PlayBufferOnBus` を足すか**: XAudio2 の
`CreateSourceVoice` は送信先リスト (`XAUDIO2_VOICE_SENDS`) を**生成時に**要求する。
生成後の付け替えは `SetOutputVoices` で可能だが再構成コストがある。
再生時にバスが決まっているのが普通なので、それを主経路にする。
`PlayBuffer` は Master 直結の薄いラッパーとして残し、既存呼び出しを壊さない。

### リバーブの実装

XAudio2 標準の `XAudio2CreateReverb` (XAPO) をサブミックスに挿す。
**WHY 自前 DSP を書かないか**: 「数学・物理は自作」という本プロジェクトの方針は
ゲームプレイの根幹に対するもので、AGENTS.md の許可ライブラリにも `XAudio2` が
明記されている。リバーブアルゴリズム自作は投資対効果が見合わない。

これにより `AudioReverbZoneComponent::decayTime` が初めて意味を持つ。

### バス構成アセット — `.audiomixer`

`ProjectSettings/Audio.audiomixer` に置く (Input と同じ理由でプロジェクト設定扱い)。

```toml
[[bus]]
name   = "Master"
parent = ""
volume = 1.0

[[bus]]
name   = "BGM"
parent = "Master"
volume = 0.8

[[bus]]
name   = "SE"
parent = "Master"
volume = 1.0

[[bus]]
name   = "Voice"
parent = "Master"
volume = 1.0

# --- スナップショット: 一括で複数バスの音量を切り替える ---
[[snapshot]]
name = "Paused"
volumes = { BGM = 0.3, SE = 0.0, Voice = 0.0 }

[[snapshot]]
name = "Cutscene"
volumes = { BGM = 0.5, SE = 0.7 }
```

### スナップショットと遷移

```cpp
// fadeSeconds かけて現在のバス音量からスナップショットの値へ補間する。
void AudioMixer::ApplySnapshot(std::string_view name, float fadeSeconds);
```

**WHY スナップショットを持つか**: ポーズメニュー・カットシーン・被ダメージ時の
「BGM だけ下げる」演出は、個別 `SetBusVolume` の手書きだと呼び忘れで
音量が戻らないバグを量産する。名前付きの状態にすれば復帰が 1 行で書ける。

### ダッキング

```toml
[[duck]]
trigger = "Voice"     # このバスが鳴っている間
target  = "BGM"       # こちらを下げる
amount  = 0.4         # 音量倍率
attack  = 0.1         # 秒
release = 0.5         # 秒
```

**WHY**: セリフ再生中に BGM を自動で下げる処理は極めて頻出で、
毎回スクリプトに書くと再生経路ごとに漏れる。宣言的に持つべき設定。

### `AudioMixerSendComponent` の接続

`busName` を `AudioMixer` が解決した `busId` に変換し、
`PlayBufferOnBus` の出力先として使う。`sendLevel` は
「バスへ送る前の音源個別ゲイン」という本来の意味に戻す。

---

## 2. ストリーミング再生

### 対象と判定

**ファイルサイズ閾値ではなく、明示フラグで決める。**

```cpp
struct AudioSourceComponent {
    // ... 既存 ...
    bool streaming = false;   // true なら全展開せず逐次デコードする
};
```

**WHY 自動判定にしないか**: サイズで自動切替すると、
「短いはずの SE が閾値をまたいだ瞬間に挙動が変わり、レイテンシが増えた」
という再現困難な問題を生む。BGM は作者が BGM だと知っているので明示させる。

### 実装 — バッファキュー方式

XAudio2 の `SubmitSourceBuffer` を 3 枚のリングで回す。

```
[デコード済みチャンク 0] → Submit
[デコード済みチャンク 1] → Submit
[デコード中チャンク 2]   ← ワーカースレッド
```

- チャンク長は **0.5 秒**。
  **WHY**: 短すぎるとコールバック頻度が上がりスレッド起床コストが増え、
  長すぎると Stop / Seek の応答が鈍る。0.5 秒 × 3 枚 = 1.5 秒のバッファなら、
  重いシーンロード中でもアンダーランしない。
- デコードは `TaskSystem` のワーカーで行う。
  `Docs/design/async-asset-loading.md` のワーカー制約 (Logger 直呼び禁止等) に従う。
- `XAUDIO2_VOICE_STATE::BuffersQueued` が 2 を下回ったら次チャンクを投入する。

### Seek と Pause の修正

ストリーミング化に伴い、再生位置を扱えるようになる。
これを使って **課題 7 (Pause が先頭に戻る)** を解消する。

```cpp
void  AudioManager::SetVoicePosition(uint32_t voiceId, float seconds);
float AudioManager::GetVoicePosition(uint32_t voiceId);
```

非ストリーミング (全展開) ボイスも、`XAUDIO2_BUFFER::PlayBegin` を使えば
同じ API で再開位置を指定できる。**Pause は位置を保存 → Stop、
Resume は保存位置から Play** という統一実装にする。

### 対応フォーマット

現状 `AudioManager::LoadWav` + `LoadWithMediaFoundation` の 2 経路がある。
ストリーミングは **Media Foundation の `IMFSourceReader` 経由**で実装する。

**WHY**: MF は MP3 / AAC / WMA を扱え、既に依存に入っている。
Ogg Vorbis 対応のために stb_vorbis を追加する案もあるが、
Windows 専用エンジンで MF が使える以上、依存を増やす理由がない。

---

## 3. サウンドキュー — `.soundcue`

「1 つの論理的な音」に複数のバリエーションと再生規則を束ねるアセット。

```toml
name = "Footstep_Stone"
bus  = "SE"

# ランダムに 1 つ選ぶ。直前に選んだものは除外する (連続同一を防ぐ)
selection = "RandomNoRepeat"   # Random | RandomNoRepeat | Sequential | Weighted

volumeRange = [0.85, 1.0]      # 再生ごとにこの範囲で乱択
pitchRange  = [0.92, 1.08]

cooldownSeconds = 0.05         # この間隔以内の再要求を無視する
maxConcurrent   = 4            # このキューの同時発音上限
priority        = 128          # ボイス枯渇時の生存優先度 (大きいほど残る)

[[clip]] path = "Audio/SE/footstep_stone_01.wav"  weight = 1.0
[[clip]] path = "Audio/SE/footstep_stone_02.wav"  weight = 1.0
[[clip]] path = "Audio/SE/footstep_stone_03.wav"  weight = 1.0
```

**WHY `cooldownSeconds` を持つか**: パーティクルやアニメーションイベントから
SE を鳴らすと、1 フレームに同じ音が 10 回要求されることが実際に起きる。
これは位相干渉で音量が跳ね上がり、耳に痛い出力になる。
呼び出し側の責任にすると必ず漏れるため、アセット側の性質として持たせる。

**WHY `RandomNoRepeat` を既定に含めるか**: 純粋乱択だと同じ足音が 2 回続く確率が
バリエーション数 N に対して 1/N あり、人間は明確に「同じ音が鳴った」と知覚する。

### Script API

```cpp
// ScriptAudioProxy
uint32_t PlayCue(std::string_view cuePath) const;                       // 2D
uint32_t PlayCueAt(std::string_view cuePath, math::Vector3 pos) const;  // 3D one-shot
void     StopVoice(uint32_t voiceId) const;
```

---

## 4. ボイス管理 (Voice Pool)

同時発音数の上限を設け、超過時は優先度で刈る。

```cpp
class VoicePool {
public:
    static constexpr int MAX_VOICES = 64;

    // 空きがなければ「優先度が低く、かつ再生残量が最も少ない」ボイスを停止して奪う。
    // 奪えない (要求より優先度が高い voice しかない) 場合は 0 を返し、再生しない。
    [[nodiscard]] uint32_t Acquire(int priority, float estimatedDurationSeconds);
};
```

**WHY「優先度が低く、かつ残量が少ない」で選ぶか**: 優先度だけで選ぶと、
鳴り始めたばかりの音を切って耳につくブツ切れを作る。
残量だけで選ぶと BGM が真っ先に犠牲になる。両者の組み合わせが必要。

**WHY 64 か**: XAudio2 自体の上限はもっと高いが、人間が同時に聞き分けられる音は
十数個程度で、それ以上はマスキングで潰れるだけの CPU 浪費になる。
64 は「切られたことに気づかれない」十分な余裕を持つ実用値。

---

## 5. Occlusion の平滑化 (課題 8)

現在の二値判定を、目標値への時定数補間に変える。

```cpp
// currentOcclusion を目標値へ指数補間する。
// 遮蔽に入るとき (attack) と出るとき (release) で時定数を変える。
const float target = blocked ? 1.0f : 0.0f;
const float tau    = blocked ? occlusion->attackTime : occlusion->releaseTime;
occlusion->currentOcclusion = math::Mathf::Damp(
    occlusion->currentOcclusion, target, tau, ctx.dt);
```

**WHY attack と release を分けるか**: 遮蔽に入る瞬間は速く反応した方が自然だが、
出る瞬間も同じ速さだと柱の陰を走り抜けるときに音量がバタつく。
release を長めに取ると滑らかになる。

**レイキャストは 1 本のまま**か、3 本 (中心 + 左右オフセット) にして
遮蔽率を 0/0.33/0.67/1.0 の段階値にするか。→ **決定事項として下記に記載**。

---

## エディタ統合

- **`AudioMixerPanel` を新設**する。バス階層のツリー表示、バスごとのフェーダーと
  ピークメーター、スナップショットの切替、ダッキング設定。
  **WHY メーターを持つか**: 「音が鳴らない」ときに、そもそも信号が来ていないのか
  バス音量が 0 なのかを切り分ける手段が現状まったくない。
- `.soundcue` の Inspector。クリップ一覧のドラッグ＆ドロップ追加と試聴ボタン。
- `ProjectSettingsPanel` に Audio タブ (既定ミキサーの指定、`MAX_VOICES`)。

## テスト

`Projects/Tests/Audio/main.cpp` を新設する。
実デバイスに依存しないよう、`IAudioDevice` のモック実装を使う。

- [ ] バス階層の音量が親子で乗算される
- [ ] スナップショット遷移が fadeSeconds で線形に収束する
- [ ] ダッキングの attack / release が指定時定数どおりに動く
- [ ] `.audiomixer` / `.soundcue` の保存 → 読込が往復する
- [ ] `RandomNoRepeat` が直前クリップを選ばない
- [ ] `cooldownSeconds` 以内の再要求が無視される
- [ ] `VoicePool` が上限超過時に最も低優先度かつ残量最小のボイスを奪う
- [ ] 高優先度ボイスしかない場合に低優先度の要求が拒否される (0 を返す)

## 実装チェックリスト

- [ ] `IAudioDevice` のバス API 拡張
- [ ] `XAudio2Device` のサブミックスボイス実装
- [ ] `XAudio2CreateReverb` のバス挿入
- [ ] `AudioMixer` (バス階層 / スナップショット / ダッキング) + `.audiomixer` シリアライザ
- [ ] `AudioMixerSendComponent::busName` の実接続
- [ ] `AudioReverbZoneComponent::decayTime` の実接続
- [ ] `AudioManager` のカテゴリ音量を `PlayVoice` 経路にも適用 (課題 2)
- [ ] ストリーミング再生 (`IMFSourceReader` + バッファキュー)
- [ ] `AudioSourceComponent::streaming` フィールド + Reflect + シリアライズ
- [ ] `SetVoicePosition` / `GetVoicePosition` と Pause / Resume の再実装 (課題 7)
- [ ] `SoundCueAsset` + `.soundcue` シリアライザ + Importer 登録
- [ ] `VoicePool`
- [ ] Occlusion の時定数補間 (課題 8)
- [ ] `ScriptAudioProxy` 拡張 (`PlayCue` / バス音量 / スナップショット)
- [ ] `AudioMixerPanel` + `.soundcue` Inspector + ProjectSettings の Audio タブ
- [ ] `Projects/Tests/Audio/main.cpp`
- [ ] Visual Studio 2022 全体ビルド
