# Game Settings — ユーザー定義シリアライズと Option 画面

セーブデータ (進行) と環境設定 (Option) を、ゲーム側がスキーマを決められる形で
永続化する。あわせて Option 画面から触る必要のある表示・画質の口を ScriptProxy へ出す。

---

## 1. 現状と課題

### 1.1 シリアライズ

| 部品 | 現状 |
|---|---|
| `IScriptSerializable` ([Script.hpp:54](../../Projects/Engine/include/Engine/Scene/Script.hpp)) | `Reflect(IReflector&)` 1 本。既に DLL 境界を越えている |
| `IReflector` ([Script.hpp:126](../../Projects/Engine/include/Engine/Scene/Script.hpp)) | `ObjectField` (入れ子) / `ListField` (配列 7 型) / `BeginObject` を持つ |
| `TomlWriteReflector` / `TomlReadReflector` | **2 か所に重複実装** — [DataAssetRegistry.cpp:78](../../Projects/Engine/src/Asset/DataAssetRegistry.cpp) と [SceneSerializer.cpp:504](../../Projects/Engine/src/Scene/SceneSerializer.cpp) |
| `util::SaveData` ([SaveData.hpp](../../Projects/Engine/include/Engine/Util/SaveData.hpp)) | TOML の flat KVS。**全 static = テーブルが 1 本しかない** |
| `ScriptSaveProxy` | `SetInt` / `GetVector3` 等のスカラーのみ。ユーザー定義型を渡す口が無い |

つまり **入れ子・配列を TOML へ落とす機構はもう完成していて、`save` から呼べないだけ**。

`SaveData` が全 static なのはこの先で効いてくる。`SetSlotPath` + `Load` はテーブルを
まるごと置き換えるので、オプション設定を `save` へ書くと**セーブ枠を切り替えた瞬間に消える**。
進行データと環境設定は寿命もスコープも違うので、同じテーブルに同居できない。

### 1.2 グラフィック設定

`ScriptApplicationProxy` は 5 関数しかない (`Quit` / `IsRunning` / `GetWindowWidth` /
`GetWindowHeight` / `IsPlaying`)。Option 画面に要る項目を並べると:

| 項目 | エンジン側の現状 |
|---|---|
| フレームレート上限 | ✅ `time.SetTargetFps()` ([ScriptTimeProxy.hpp:25](../../Projects/Engine/include/Engine/Scene/ScriptProxy/ScriptTimeProxy.hpp)) |
| 画質設定の実体 | △ `RenderSettings` に全部あるが script から触る口が無い |
| 明るさ | △ `postProcess.exposure` で代用できるが後述の理由で不可 |
| フルスクリーン切替 | ❌ `Window::Config.fullscreen` は `Initialize` 時のみ |
| 解像度変更 | ❌ `Window` に寸法変更 API 自体が無い (WM_SIZE 受けのみ) |
| VSync | ❌ DX12 は `Present(0)` 決め打ち ([DX12Context.cpp:18](../../Projects/Engine/src/Renderer/Platform/DX12/DX12Context.cpp)) |
| 描画スケール | ❌ 無い (§9.3 で追加) |

**明るさを `postProcess` に置けない理由:** [PostProcessBlend.cpp:27](../../Projects/Engine/src/Renderer/PostProcessBlend.cpp) が
`out.postProcess = volume.post;` と**丸ごと差し替える**。PostProcessVolume に入った瞬間、
プレイヤーが設定した明るさが消える。`postprocess.LoadProfile()` も同様。
ユーザー設定はアーティストのオーサリングと別レイヤーに置く必要がある。

---

## 2. ゴール / 非ゴール

**ゴール**
- ゲーム側が `struct` を 1 つ書いて `Reflect` を生やすだけでセーブできる。
- セーブ枠 (進行) と環境設定を別ストアに分ける。
- Option 画面が必要とする表示・画質の項目を ScriptProxy から触れる。
- 明るさ・画質はアーティストのオーサリングを壊さない別レイヤーに乗る。

**非ゴール**
- 排他フルスクリーン (DXGI `SetFullscreenState`)。ボーダーレスのみに絞る。
- 設定ファイルの自動適用。config の読み込みと適用は**ゲームスクリプトの責務**とする
  (§7 に理由)。
- セーブデータの暗号化・改ざん検知。TOML で人間が読める形を維持する。

---

## 3. 全体像

```
[ゲームスクリプト]
    struct PlayerSave : IScriptSerializable { void Reflect(IReflector&) override; }
        │
   save.Write("player", data)          config.Write("video", videoCfg)
        │                                       │
  ┌─────┴───────────────┬───────────────────────┴─────┐
  │  SaveStore (slot)   │      SaveStore (config)     │   ← Layer 2
  │  Saves/save0.toml   │      Config/settings.toml   │
  └─────────┬───────────┴─────────────┬───────────────┘
            └────── TomlReflector ────┘                    ← Layer 1 (共通化)

[Option 画面スクリプト]  OnStart で config を読み、下の setter を呼ぶ
    audio.SetBusVolume("BGM", v)          ← 既存
    time.SetTargetFps(fps)                ← 既存
    display.SetFullscreen(true)           ← Layer 4/5 (新規)
    display.SetVSync(true)
    graphics.SetBrightness(1.2f)          ← Layer 6 (新規)
    graphics.SetQualityPreset(High)
```

---

## 4. Layer 1 — TOML リフレクタの共通化

`Engine/Util/TomlReflector.hpp` / `.cpp` を新設し、`DataAssetRegistry.cpp` と
`SceneSerializer.cpp` の実装をここへ寄せる。

```cpp
namespace fbzz::util {
class TomlWriteReflector final : public scene::IReflector { ... };
class TomlReadReflector  final : public scene::IReflector { ... };
}
```

**WHY 先にやるか:** save 用に 3 個目を書くと、`ListField` の型を 1 つ足すたびに
3 か所を直すことになる。2 実装の差分 (SceneSerializer 側は `EntityID` の再マップを持つ)
は仮想関数のオーバーライドで吸収し、共通部分だけを持ち上げる。

---

## 5. Layer 2 — ストアの分離

`util::SaveData` を static クラスからインスタンス化可能な `util::SaveStore` へ変える。

```cpp
namespace fbzz::util {
class SaveStore {
public:
    explicit SaveStore(std::string defaultPath);
    void SetPath(const std::string& path);
    // 既存の SetInt / GetVector3 ... はそのまま非 static メンバーへ
    bool Save();  bool Load();  bool IsDirty();
};
}
```

`Application` が 2 本持つ:

| ストア | 既定パス | 寿命 |
|---|---|---|
| セーブ (`save`) | `Saves/save0.toml` | `SetSlot` で切り替わる |
| 設定 (`config`) | `Config/settings.toml` | スロットに依らず 1 本 |

**WHY ストアを分けるか:** セーブ枠の切り替えは `Load` でテーブルを置き換える。
同居させると「別のセーブをロードしたら音量が戻った」という、原因の見えない不具合になる。

**WHY `SaveData` を残さないか:** 全 static のまま 2 本目を足すと、どちらのテーブルを
触っているかが呼び出し側から見えない。既存の `util::SaveData` は `SaveStore` への
薄い転送として残すか、呼び出し元が `ScriptSaveProxy` 経由だけなら削る。

---

## 6. Layer 3 — ユーザー定義シリアライズ

### 6.1 API

`ScriptSaveProxy` / `ScriptConfigProxy` の両方に同じ 2 本を生やす。

```cpp
/// object.Reflect() が並べたフィールドを key のテーブル下へ書き出す。
/// 呼んだ時点ではメモリ上のテーブルを更新するだけ。ディスクへは Save() で落ちる。
bool Write(std::string_view key, scene::IScriptSerializable& object) const;

/// key のテーブルから読み戻す。キーが無いフィールドは object の値を保つ。
/// @ret テーブル自体が存在しなければ false (object は無変更)。
bool Read(std::string_view key, scene::IScriptSerializable& object) const;
```

### 6.2 使い方

コンポーネントの `Reflect` と同じ書き方になる — 覚えることが増えない。

```cpp
struct PlayerSave : scene::IScriptSerializable {
    int           level = 1;
    float         hp    = 100.0f;
    math::Vector3 position;
    std::vector<std::string> items;

    void Reflect(scene::IReflector& r) override {
        r.Field("level", level);
        r.Field("hp", hp);
        r.Field("position", position);
        r.ListField("items", items);
    }
};

// 保存
PlayerSave data{ 7, 62.5f, transform.GetPosition(), { "Potion", "Key" } };
save.Write("player", data);
save.Save();

// 復元 (キーが無ければメンバー初期化子がそのまま残る)
PlayerSave data;
save.Load();
save.Read("player", data);
```

`IReflector::ObjectField` があるので入れ子もそのまま通る:

```cpp
struct GameSave : scene::IScriptSerializable {
    PlayerSave player;
    int        chapter = 1;
    void Reflect(scene::IReflector& r) override {
        r.ObjectField("player", player);
        r.Field("chapter", chapter);
    }
};
```

### 6.3 バージョニング

`Read` は欠損キーを既定値で埋めるので、**フィールドの追加は自動で前方互換**になる。
壊れるのは「意味を変えた」ときだけなので、そこだけ明示的に扱う。

```cpp
void Reflect(scene::IReflector& r) override {
    r.Field("version", version);   // 先頭で読む
    ...
}
```

エンジン側にマイグレーション機構は置かない。**WHY:** 何をどう移行するかはゲームの
ドメイン知識で、汎用の枠を用意しても結局ゲーム側に分岐を書くことになる。

### 6.4 DLL 境界

`IReflector` の vtable は既に Scripts.dll を越えている (Inspector が Scripts.dll の
コンポーネントを反射している)。新規の ABI リスクは無い。
ただし `IReflector` に仮想関数を足すときは**末尾追加のみ** — 既存の並びを崩すと
古い Scripts.dll が別の関数を呼ぶ。

---

## 7. Layer 4 — Window / IRenderer のランタイム変更 API

ここが**唯一の重い工程**。プロキシに生やす前に下の 2 つを作る。

### 7.1 `Window`

```cpp
enum class WindowMode : uint8_t { Windowed, BorderlessFullscreen };

void       SetWindowMode(WindowMode mode);
WindowMode GetWindowMode() const;
/// Windowed のときだけ効く。BorderlessFullscreen 中はモニター解像度に従う。
void SetClientSize(uint32_t width, uint32_t height);
/// ウィンドウが載っているモニターの現在解像度。
void GetMonitorSize(uint32_t& outWidth, uint32_t& outHeight) const;
```

`BorderlessFullscreen` の実装は
`SetWindowLongPtr(GWL_STYLE, WS_POPUP)` → `MonitorFromWindow` → `SetWindowPos` の 3 手。
`WM_SIZE` が飛ぶので既存の `SetResizeCallback` → `IRenderer::Resize`
([IRenderer.hpp:71](../../Projects/Engine/include/Engine/Renderer/IRenderer.hpp)) の経路がそのまま繋がる。

**WHY 排他フルスクリーンを採らないか:** DXGI `SetFullscreenState` は
スワップチェーンの作り直しとデバイスロスト処理を DX11 / DX12 の両バックエンドに
要求する。得られるのは「モニター解像度そのものを変えられる」ことだけで、
それは描画スケール (§9.2) で代替できる。Alt+Tab も壊れない。

**復帰用に元の矩形を覚えておく。** ウィンドウへ戻すとき、`WS_OVERLAPPEDWINDOW` を
戻すだけでは位置とサイズが失われる。`SetWindowMode` に入る前の `WINDOWPLACEMENT` を
保持して復元する。

### 7.2 `IRenderer`

```cpp
/// Present の同期間隔。false で 0 (対応環境では tearing 許可)。
virtual void SetVSync(bool enabled) = 0;
[[nodiscard]] virtual bool GetVSync() const = 0;
```

DX12 は現在 `Present(0)` 固定なので、`m_syncInterval` を持たせて分岐させる。
DX11 側は既に VSync 無効時の分岐がある ([DX11Renderer.hpp:194](../../Projects/Engine/src/Renderer/Platform/DX11/DX11Renderer.hpp)) ので、
そこへ繋ぐ。

---

## 8. Layer 5 — `display` プロキシ

```cpp
struct ScriptDisplayProxy {
    Script* script = nullptr;

    /// @name ウィンドウモード
    ///@{
    void SetFullscreen(bool enabled) const;   ///< ボーダーレス最大化 ⇔ ウィンドウ
    [[nodiscard]] bool IsFullscreen() const;
    /// ウィンドウモード時のクライアント寸法。フルスクリーン中は次にウィンドウへ
    /// 戻したときの寸法として覚えるだけで、即座には効かない。
    void SetResolution(uint32_t width, uint32_t height) const;
    [[nodiscard]] uint32_t GetWidth()  const;
    [[nodiscard]] uint32_t GetHeight() const;
    /// Option のドロップダウン用。モニターが対応する寸法を降順で返す。
    [[nodiscard]] std::vector<math::Vector2> EnumResolutions() const;
    ///@}

    /// @name 同期
    ///@{
    void SetVSync(bool enabled) const;
    [[nodiscard]] bool GetVSync() const;
    ///@}
};
```

`app` に足さず新設する。**WHY:** `app` は「アプリの生死と実行モード」を答える場所で、
表示設定が混ざると `IsPlaying()` を探すときに 20 個の setter を読むことになる。

`SetTargetFps` は `time` に既にあるので**ここへ複製しない**。

---

## 9. Layer 6 — `graphics` プロキシ

### 9.1 明るさ

`RenderSettings` の**トップレベル**に置く (`postProcess` の中ではない)。

```cpp
struct RenderSettings {
    ...
    PostProcessSettings postProcess;

    // ユーザー設定レイヤー — PostProcessVolume のブレンド対象に含めない。
    // WHY: PostProcessBlend が postProcess を丸ごと差し替えるため、ここに置かないと
    //      ボリュームへ入った瞬間にプレイヤーの明るさ設定が消える。
    float userBrightness = 1.0f;   // 0.5 〜 2.0
};
```

`CompositePass` が `PostProcCB` へ流し、**画面フェードの直前**に 1 回掛ける
([CompositePass.cpp:162](../../Projects/Engine/src/Scene/Systems/RenderPasses/PostProcess/CompositePass.cpp) の隣)。

```hlsl
color.rgb *= userBrightness;
color.rgb  = lerp(color.rgb, screenFadeColor, screenFadeAlpha);
```

**WHY フェードの前か:** 逆にするとフェードアウトの黒が明るさで持ち上がり、
暗転しきらない。

### 9.2 画質

```cpp
enum class QualityPreset : uint8_t { Low, Medium, High, Ultra };

struct ScriptGraphicsProxy {
    Script* script = nullptr;

    void  SetBrightness(float value) const;   ///< 0.5 - 2.0
    [[nodiscard]] float GetBrightness() const;

    /// 影解像度 / SSR / GTAO / TAA / VolumetricLight をまとめて切り替える。
    void SetQualityPreset(renderer::QualityPreset preset) const;
    [[nodiscard]] renderer::QualityPreset GetQualityPreset() const;

    /// 個別調整 (プリセットの後に呼べば上書きできる)
    void SetShadowResolution(uint32_t resolution) const;  ///< 512-4096
    void SetShadowCascades(int count) const;              ///< 1-4
    void SetSSR(bool enabled) const;
    void SetGTAO(bool enabled) const;
    void SetTAA(bool enabled) const;
    void SetVolumetricLight(bool enabled) const;
    void SetMotionBlur(bool enabled) const;
};
```

プリセットの中身は `renderer::ApplyQualityPreset(RenderSettings&, QualityPreset)` として
Engine 側へ 1 か所に置く。**WHY:** プリセットの定義がゲーム側に散ると、機能を足したときに
既存タイトルのプリセットが古いままになる。

`SetTAA` / `SetGTAO` は `RenderSettings::NormalizeExclusivePipelineSlots()` を通すこと
(FXAA/TAA と SSAO/GTAO は排他)。

### 9.3 描画スケール

内部の描画解像度だけを倍率で変え、出力先の寸法は保つ。GPU コストは面積比で効くので
`0.7` でおよそ半分になる。**UI はポストプロセスの後に出力先へ直接描くため影響を受けない。**

```cpp
void  SetRenderScale(float scale) const;   ///< 0.5 - 2.0
[[nodiscard]] float GetRenderScale() const;
/// 倍率と下限を適用した後の実サイズ。「1920x1080 -> 1344x756」を Option へ出す用。
[[nodiscard]] uint32_t GetRenderWidth() const;
[[nodiscard]] uint32_t GetRenderHeight() const;
```

**倍率を掛けるのは 1 か所だけ。**
[RenderSystem.cpp](../../Projects/Engine/src/Scene/Systems/RenderSystem.cpp) の中間 RT 生成で
出力寸法 → 内部寸法へ変換すれば、`passCtx.width` を経由して各パスのビューポート・
`texelSize`・TAA のジッターまで追従する。

**アップスケール専用のパスは要らない。** ポストプロセスはどれもフルスクリーン三角形の
再サンプルで、`SetRenderTarget` がビューポートを RT 全体へ戻す規約
([IRenderer.hpp](../../Projects/Engine/include/Engine/Renderer/IRenderer.hpp))。
小さい `hdrRT` を実寸の `outputRT` へ描いた時点で linear サンプラーが引き伸ばす。

丸め規則は `renderer::ResolveRenderResolution()` に閉じる。
**WHY:** RT の生成側とビューポート計算側で 1 ピクセルずれると、ポストプロセスの UV が
半テクセルずれる。

**下限は 640x360、ただし元の寸法がそれより小さければ元の寸法。**
小さなビューポートで床が勝って「縮小のつもりが拡大」になるのを防ぐ。

⚠ **値が変わると中間 RT を作り直す。** 連続スライダーから毎フレーム書くと再確保が
走り続けるので、UI 側は離散値 (50/60/…/100%) か「離した時だけ反映」にすること。

---

## 10. 使用例 — Option 画面スクリプト

```cpp
struct VideoConfig : scene::IScriptSerializable {
    bool  fullscreen = true;
    bool  vsync      = true;
    int   width      = 1920;
    int   height     = 1080;
    float brightness = 1.0f;
    int   quality    = 2;   // QualityPreset
    void Reflect(scene::IReflector& r) override {
        r.Field("fullscreen", fullscreen);  r.Field("vsync", vsync);
        r.Field("width", width);            r.Field("height", height);
        r.Field("brightness", brightness);  r.Field("quality", quality);
    }
};

struct AudioConfig : scene::IScriptSerializable {
    float master = 1.0f, bgm = 1.0f, se = 1.0f, ui = 1.0f;
    void Reflect(scene::IReflector& r) override {
        r.Field("master", master);  r.Field("bgm", bgm);
        r.Field("se", se);          r.Field("ui", ui);
    }
};

class OptionsComponent : public scene::Script {
    VideoConfig m_video;
    AudioConfig m_audio;

    void OnStart() override {
        config.Load();
        config.Read("video", m_video);
        config.Read("audio", m_audio);
        Apply();
    }

    void Apply() {
        display.SetResolution(m_video.width, m_video.height);
        display.SetFullscreen(m_video.fullscreen);
        display.SetVSync(m_video.vsync);
        graphics.SetBrightness(m_video.brightness);
        graphics.SetQualityPreset(static_cast<renderer::QualityPreset>(m_video.quality));

        audio.SetBusVolume("Master", m_audio.master);
        audio.SetBusVolume("BGM",    m_audio.bgm);
        audio.SetBusVolume("SE",     m_audio.se);
        audio.SetBusVolume("UI",     m_audio.ui);
    }

    void OnApplyButton() {
        Apply();
        config.Write("video", m_video);
        config.Write("audio", m_audio);
        config.Save();
    }
};
```

### 10.1 起動時の一瞬のウィンドウ表示

config の適用はゲームスクリプトの責務にしたので、ウィンドウは
`ProjectSettings.window` の寸法で一度生成されてから `OnStart` で切り替わる。

**緩和策:** `ProjectSettings.window` の既定をフルスクリーン相当にしておき、
config が違うときだけ `OnStart` で切り替える。ボーダーレスなのでスタイル変更と
`Resize` 1 回で済み、スワップチェーンの作り直しは起きない。

**WHY エンジンに自動適用させないか:** config のスキーマをゲームが決められることが
この設計の眼目で、エンジンが読むなら `fullscreen` というキー名をエンジンが
固定することになる。「機構はエンジン、方針はゲーム」で揃える。

---

## 11. 実装順

| 段 | 内容 | 依存 | 状態 |
|---|---|---|---|
| 1 | `Util/TomlReflector.hpp/.cpp` を新設し、既存 2 実装を寄せる | — | 済 |
| 2 | `SaveData` → `SaveStore` へインスタンス化。`Application` が save/config の 2 本を持つ | 1 | 済 |
| 3 | `ScriptSaveProxy::Write/Read` + `ScriptConfigProxy` 新設 | 2 | 済 |
| 4 | `Window::SetWindowMode` / `SetClientSize` / `GetMonitorSize` | — | 済 |
| 5 | `IRenderer::SetVSync` (DX11 / DX12 両方) | — | 済 |
| 6 | `ScriptDisplayProxy` 新設 | 4, 5 | 済 |
| 7 | `RenderSettings::userBrightness` + `CompositePass` / HLSL へ配線 | — | 済 |
| 8 | `renderer::QualityPreset` + `ApplyQualityPreset` | — | 済 |
| 9 | `ScriptGraphicsProxy` 新設 | 7, 8 | 済 |
| 10 | GreenWare に Option 画面を実装して通しで確認 | 3, 6, 9 | スクリプトのみ済 |

新規プロキシは `ScriptProxyMembers.inl` の**末尾へ追加**する
([同ファイル L59 の注意書き](../../Projects/Engine/include/Engine/Scene/ScriptProxy/ScriptProxyMembers.inl))。
段 3 / 6 / 9 の後は **FBZZSDK を先にビルドしてから** Scripts.dll をビルドすること。

---

## 12. 実装ノート (設計から変えた点)

**`SaveData` は残さず `SaveStore` へ置き換えた。**
static のまま 2 本目を足すと、どちらのテーブルを触っているかが呼び出し側から見えない。
`SaveData.hpp/.cpp` は削除した (呼び出し元は `ScriptProxies.cpp` の 1 か所だけだった)。
`toml++` を `Application.hpp` から波及させないため `SaveStore` は pImpl。

**save / config は共通の実体を継承した 2 つの薄い型にした。**
`ScriptStoreProxyBase` が全 API を持ち、`ScriptSaveProxy` / `ScriptConfigProxy` は
`SaveStoreKind` を立てるだけ。仮想関数を持たないので DLL 境界は従来どおり安全。
API を 2 回書くと、片方にだけメソッドを足す形の取りこぼしが起きる。

**`userBrightness` は `PostProcCB` の既存の空きスロット (`_qualityPad1`) を流用した。**
`float3 colorFilter` の 4 成分目にあたる 1 float がもともと padding だったので、
cbuffer のレイアウトは変わらない。
⚠ `PostProcCB` は zero-init して使うパスがあり、そこでは 0 (真っ黒) になる。
現状 Composite しか読まないので実害は無いが、別のパスで読むなら必ず明示的に埋めること。

**graphics の書き換え先は `Application::SetActiveRenderSettings()` で登録する。**
`RenderSettings` の実体は `ProjectSettings::render` で、それを持つのが Editor か
Standalone かで違う。Application は composition root として参照だけ預かる。
- Standalone (`StandaloneProjectModule` / `EditorLauncher::StandaloneApp`) は起動時に登録。
- **Editor は Play 中だけ登録し、`StopPlayMode` でスナップショットへ戻す。**
  EditorApp は終了時に `ProjectSettings.toml` を保存するため、Play 中の画質変更が
  そのままプロジェクトの既定値になってしまう。選択状態だけは編集の続きなので復元から外す。

**`Window::Config.fullscreen` はこれまで読まれていなかった。**
ProjectSettings から渡ってはいたが `Initialize` が無視しており、Standalone の
フルスクリーン起動は動いていなかった。`SetWindowMode` の実装と同時に配線した。

**`Window::EnumerateResolutions()` を追加した (`EnumDisplaySettingsW`)。**
固定表を持つと、ウルトラワイドや縦置きで選べない解像度が並ぶ。
列挙に失敗する環境 (リモートデスクトップ等) では現在の寸法 1 つを返す。

**`TomlWriteReflector` に挿入ポリシー (先勝ち / 後勝ち) の切り替えを持たせた。**
Scene の直列化は `insert` (先勝ち)、DataAsset と SaveStore は `insert_or_assign` (後勝ち)。
統一すると既存のシーン保存結果が変わりうるため、呼び出し側に選ばせている。

**副作用: DataAsset が `ListField` (配列フィールド) を扱えるようになった。**
従来の DataAsset 用リフレクタは `ListField` を実装しておらず、`std::vector<float>` 等の
フィールドは黙って保存されていなかった。共通版に寄せたことで往復するようになる。
古いファイルはキーが無いだけなので既定値のままで、後方互換は保たれる。

**`float` の読み出しは整数ノードも受けるようにした。**
TOML は `1.0` を整数 `1` として書き戻すことがあり、`double` だけを見ると
`SetFloat(1.0f)` → `GetFloat()` が既定値へ落ちる。`SaveData` にあった対策を共通版へ移した。

**描画スケールは当初「後回し」としたが、調べ直したら 1 か所で済んだので実装した。**
懸念として挙げていた 2 つは実際には問題ではなかった:
- **ピッキングは影響を受けない。** Scene View の選択は `selectionMaskRT` の読み戻しでは
  なく CPU 側のレイキャストで、`ctx.viewportWidth`(出力寸法)基準
  ([ViewportPicking.cpp](../../Projects/Editor/src/Panels/Viewport/ViewportPicking.cpp))。
- **半解像度 AO は不均衡に悪化しない。** SSAO / GTAO / 接触影は内部解像度の半分なので、
  シーンに対する比率は倍率に関わらず一定。全体が均一に粗くなるだけ。
  代わりに内部解像度そのものへ下限 (640x360) を置いた。

実際に直したのは **UI のレイアウト寸法のフォールバック** 1 か所だけ。
`uiOptions->viewportWidth` が 0 のとき `sHdrW`(内部解像度)へ落ちていたので、
出力実寸を使うようにした。放置すると renderScale < 1 で UI が画面左上へ寄る。
なお実運用の呼び出し元はすべて明示的に寸法を渡しているので、これは潜在バグだった。

Project Settings > Graphics に「User Settings (preview)」を追加した。
明るさと描画スケールは保存されない値だが、確認手段が Play + スクリプトだけだと
調整のたびに再生し直すことになるため、保存されない旨を明記して置いた。
描画スケールのスライダーはドラッグ中に反映しない (中間 RT の再確保が走り続けるため)。

**GreenWare の Option 画面と、遊びへの配線 (2026-08-24)。**

`OptionsScreenComponent` が 24 行を扱う。設計時に想定していなかった穴が 3 つあった。

- **保存はされていたが誰も読んでいなかった。** INPUT / GAME の 11 項目は config へ
  往復していたのに、読む側が 1 つも無かった。しかも `GameSettingsComponent` を
  置いていたのは Title と Options だけで、遊ぶシーンでは `Instance()` が null だった。
  表示と音量が効いていたのは、反映先 (`Time::targetFps` / `AudioManager` /
  `ProjectSettings.render`) がアプリ寿命のグローバルだったからで、設計が正しかった
  わけではない。読む側は `GameOrDefault()` / `ShakeScale()` のような静的アクセサ越しに
  引く。null 判定と既定値を呼ぶ側に書かせると、1 か所抜けた場所だけが黙って効かなくなる。
- **画質プリセットの既定値が ProjectSettings を潰していた。** `ApplyQualityPreset` は
  影・SSR・TAA など 15 項目を一括で上書きするため、config が無い初回起動でも
  既定の High を撃つと、プロジェクトの描画設定が毎回塗り替わる。
  `VideoConfig::quality` の既定を `-1` (未選択) にして、プレイヤーが選ぶまで触らない。
- **保存の呼び出しが Esc の 1 経路しか無かった。** `OnDestroy` で
  「編集があったときだけ」書き戻す。`MutableXxx()` が呼ばれた = 編集、と見なす
  (編集面は Option 画面 1 つしか無い)。

**キーコンフィグは config の `[bind]` テーブルへ「差分だけ」を持つ。**
既定は `ProjectSettings/Input.inputactions` が正本。全件を config へ写すと、
既定を作り直したときに古い config がそれを打ち消し、新しい既定が誰にも届かなくなる。
エンジン側は `input.BeginRebindAction()` / `GetActionBinding()` / `SetActionBinding()` /
`DescribeActionBinding()` を `ScriptInputProxy` に足しただけで、保存先は知らない
(`Input.inputactions` は製品版では読み取り専用に置かれうる)。
差し替えは「今の入力機器」の枠に限る。キーボードの行にパッドのボタンが入ると、
機器の種類で枠を引き当てる経路が全て別の枠を指し始めるため。

## 13. 検討して採らなかった案

**`settings` プロキシ 1 本に音量まで含める**
Option 画面からは触りやすいが、`audio.SetBusVolume` と機能が二重になる。
バス構成は ProjectSettings が正本なので、音量の窓口は `audio` に一本化する。

**排他フルスクリーン (DXGI `SetFullscreenState`)**
§7.1 の理由。得るものに対してスワップチェーン再生成とデバイスロスト処理のコストが
DX11 / DX12 の両方に乗る。

**エンジンが config を自動で読んで適用する**
起動時のちらつきは無くなるが、キー名と型をエンジンが固定することになる。
§10.1 の理由でゲーム側の責務とした。

**セーブデータのバイナリ化**
`SaveData.hpp` が既に述べている通り、破損調査とテストのために人間が読める形を優先する。
