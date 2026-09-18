/// @file    GameSettingsComponent.hpp
/// @brief   Option 設定の保存・読み込みと、表示 / 画質 / 音量 / 操作への反映
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note config のキー名・構造はゲームが決める (Docs/design/game-settings.md)。エンジンが
///       読むと "fullscreen" のようなキー名をエンジンが固定してしまう。
/// @note Apply (即座に効かせる) と Save (ディスクへ残す) は別の操作。Option 画面はスライダー
///       を動かしながら耳と目で合わせるため、確定前の値も即座に効く必要がある。
/// @note 表示・音量はグローバルへ書きシーンをまたいで残るが、視野角・揺れ・ヒットストップ・
///       振動・感度は Instance() 越しにしか読めないため遊ぶシーンにも置く。
/// @note 起動時は Load → Apply。ウィンドウは ProjectSettings の寸法で生成済みのため、
///       フルスクリーン設定が違うときだけ一瞬見える (ボーダーレスで Resize 1 回のみ)。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/GameSettingsRegistry.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

using namespace fbzz::scene;

namespace sandbox {

/// 表示・画質。config の [video] テーブルへ往復する。
struct VideoConfig : IScriptSerializable {
    bool  fullscreen = true;
    bool  vsync      = false;
    int   width      = 1920;
    int   height     = 1080;
    float brightness  = 1.0f;
    float bloom       = 0.6f;   ///< 発光の強さ 0 - 1 (bloom.intensity への倍率は ×2)
    float renderScale = 1.0f;
    /// -1 = 未選択。プレイヤーが画質を選ぶまでは ProjectSettings の描画設定をそのまま使う。
    ///
    /// @note 既定は High にしない。プリセット適用は影・SSR・TAA など 15 項目を一括で
    ///       上書きするため、既定値を持たせると初回起動でプロジェクトの描画設定が
    ///       まるごと塗り替わってしまう。
    int   quality     = -1;
    int   targetFps   = 60;

    void Reflect(IReflector& r) override
    {
        r.Field("fullscreen",  fullscreen);
        r.Field("vsync",       vsync);
        r.Field("width",       width);
        r.Field("height",      height);
        r.Field("brightness",  brightness);
        r.Field("bloom",       bloom);
        r.Field("renderScale", renderScale);
        r.Field("quality",     quality);
        r.Field("targetFps",   targetFps);
    }
};

/// 入力。config の [input] テーブルへ往復する。
///
/// @note device を保存する。パッドを挿していても「キーボードで遊ぶ」選択はありうるため、
///       接続の有無で勝手に切り替えると Option で選んだ設定が起動ごとに変わってしまう。
struct InputConfig : IScriptSerializable {
    int   device    = 0;      ///< 0 = マウス&キーボード, 1 = ゲームパッド
    float mouseSens = 2.4f;   ///< 0.1 - 10.0
    float stickSens = 2.0f;   ///< 0.1 - 10.0
    int   curve     = 1;      ///< 0 = リニア, 1 = 標準, 2 = 強め
    float deadzone  = 0.15f;  ///< 0 - 0.5
    float vibration = 0.8f;   ///< 0 - 1

    void Reflect(IReflector& r) override
    {
        r.Field("device", device);        r.Field("mouseSens", mouseSens);
        r.Field("stickSens", stickSens);  r.Field("curve", curve);
        r.Field("deadzone", deadzone);    r.Field("vibration", vibration);
    }
};

/// mouseSens / stickSens は画面に出る「感度の数値」で、倍率ではない。
/// 既定値のときにちょうど 1.0 倍になるよう、この値で割って倍率へ直す。
/// @note 倍率としては保存しない。Option に出る 2.40 という数値を「既定の何倍か」に
///       変えると、設定ファイルを直接見たときに表示と意味が食い違う。
inline constexpr float kMouseSensReference = 2.4f;
inline constexpr float kStickSensReference = 2.0f;

/// 応答カーブ (0 = リニア / 1 = 標準 / 2 = 強め) を指数へ直す。
[[nodiscard]] inline float CurveExponent(int curve)
{
    static const float kExponents[] = { 1.0f, 2.0f, 3.0f };
    return kExponents[std::clamp(curve, 0, 2)];
}

/// フレームレート上限の選択肢 (添字 → fps)。0 は無制限。
///
/// @note Option 画面ではなくここに置く。宣言簿への登録もここが行うため、画面側に
///       置くと宣言する側が画面を include することになり依存の向きが逆になる。
[[nodiscard]] inline int FpsCapValue(int index)
{
    static const int values[] = { 0, 30, 60, 120, 144, 240 };
    return values[std::clamp(index, 0, 5)];
}

/// ゲームプレイの好み。config の [game] テーブルへ往復する。
///
/// 読む側: 視野角と点火・集束時の FOV 変化は TpsCameraComponent、揺れ・止め・振動は
///         各マネージャーの要求受け口。
///         いずれも Instance() が無い場面 (エディタでの単体再生など) では
///         既定値で動くよう、下の静的アクセサ越しに読む。
struct GameConfig : IScriptSerializable {
    float fov       = 74.0f;  ///< 60 - 110 [deg]
    float shake     = 0.7f;   ///< 0 - 1
    float hitstop   = 1.0f;   ///< 0 - 1
    bool  fovBurst  = true;

    void Reflect(IReflector& r) override
    {
        r.Field("fov", fov);            r.Field("shake", shake);
        r.Field("hitstop", hitstop);    r.Field("fovBurst", fovBurst);
    }
};

/// 音量。値は 0-1 の線形倍率ではなく、スライダーの位置 (0-1) をそのまま持つ。
/// 実際のバス音量へ変換するのは Apply の仕事 ([[GameSettingsComponent]]::ToGain)。
struct AudioConfig : IScriptSerializable {
    float master = 1.0f;
    float bgm    = 1.0f;
    float se     = 1.0f;
    float ui     = 1.0f;

    void Reflect(IReflector& r) override
    {
        r.Field("master", master);
        r.Field("bgm",    bgm);
        r.Field("se",     se);
        r.Field("ui",     ui);
    }
};

/// キーコンフィグ。config の [bind] テーブルへ往復する。
/// @note 「変更したものだけ」を持つ。全件を写すと既定を作り直したときに古い config が
///       それを打ち消し、新しい既定が誰にも届かなくなる。
/// @note 並列配列にする。TOML は型が揃っていれば素直に読めるが、エンジンの InputBinding
///       をそのまま写すと scale / padIndex / 閾値まで固定して既定の調整が殺される。
struct InputBindConfig : IScriptSerializable {
    std::vector<std::string> action;   ///< アクション名 ("Dodge" 等)
    std::vector<int> device;           ///< 0 = マウス&キーボード, 1 = パッド
    std::vector<int> source;           ///< scene::ScriptInputBinding::source と同じ
    std::vector<int> code;

    void Reflect(IReflector& r) override
    {
        r.ListField("action", action);
        r.ListField("device", device);
        r.ListField("source", source);
        r.ListField("code",   code);
    }

    /// 4 本の長さが揃っている件数。壊れた config でも読める分までは使う。
    [[nodiscard]] std::size_t Count() const
    {
        return (std::min)((std::min)(action.size(), device.size()),
                          (std::min)(source.size(), code.size()));
    }

    /// 同じ (アクション, デバイス) が既にあれば置き換える。
    void Set(std::string_view name, int deviceIndex, int sourceIndex, int inputCode)
    {
        for (std::size_t i = 0; i < Count(); ++i) {
            if (action[i] != name || device[i] != deviceIndex) continue;
            source[i] = sourceIndex;
            code[i]   = inputCode;
            return;
        }
        action.emplace_back(name);
        device.push_back(deviceIndex);
        source.push_back(sourceIndex);
        code.push_back(inputCode);
    }

    void Clear()
    {
        action.clear();
        device.clear();
        source.clear();
        code.clear();
    }
};

class GameSettingsComponent : public Script {
    FBZZ_SCRIPT(GameSettingsComponent)

public:
    /// 保存先。既定 (エンジン側) は «実行ファイルの隣の Config/settings.toml»。
    ///
    /// @note 実行ファイルの隣では、エディタと配布ビルドが別の exe のため既定のまま
    ///       だと別々のファイルになり、エディタで調整した設定がゲームに出てこない。
    ///       ユーザーごとに 1 つの場所へ寄せれば、どちらから起動しても同じ設定になる。
    FBZZ_GROUP("Storage")
    FBZZ_FIELD(bool, perUserConfig, true, "Per User")
    FBZZ_TOOLTIP("設定を %LOCALAPPDATA%/<Folder>/settings.toml へ保存する。"
                 "切ると実行ファイルの隣 (Config/settings.toml) に戻る ─ "
                 "USB へ入れて持ち運ぶような «可搬» の配布にするときだけ切ること")
    FBZZ_FIELD(std::string, configFolder, "GreenWare", "Folder")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugResolution, "-", "Resolution")
    FBZZ_FIELD_READ_ONLY(std::string, debugQuality, "-", "Quality")
    FBZZ_FIELD_READ_ONLY(std::string, debugConfigPath, "-", "Config Path")
    FBZZ_TOOLTIP("実際に読み書きしているファイル。設定が «保存されない» ときは"
                 "まずここを見ること (見ているファイルが期待と違うことが多い)")

    /// シーンに 1 つだけ置く前提。Option 画面の UI はここから値を読み書きする。
    [[nodiscard]] static GameSettingsComponent* Instance() { return s_instance; }

    void OnStart() override;
    /// 宣言簿が増えていないかだけを見る。増えていれば保存値で埋め直す。
    void OnUpdate() override { SyncRegistry(); }
    void OnDestroy() override;

    /// @name 値の変更 (UI から呼ぶ)
    /// どれも即座に Apply される。ディスクへ残すのは Save() を呼んだときだけ。
    ///@{
    void SetFullscreen(bool enabled)      { MutableVideo().fullscreen = enabled; Apply(); }
    void SetVSync(bool enabled)           { MutableVideo().vsync = enabled; Apply(); }
    void SetBrightness(float value)       { MutableVideo().brightness = value; Apply(); }
    /// 内部の描画解像度だけを変える倍率 (0.5 - 2.0)。UI と出力寸法は変わらない。
    void SetRenderScale(float scale)      { MutableVideo().renderScale = scale; Apply(); }
    /// プレイヤーが画質を選んだ。以後は ProjectSettings の描画設定より優先される。
    void SetQualityPreset(int preset) { MutableVideo().quality = std::clamp(preset, 0, 3); Apply(); }
    void SetTargetFps(int fps)            { MutableVideo().targetFps = fps; Apply(); }
    /// EnumResolutions() の添字で指定する。範囲外は無視。
    void SetResolutionIndex(int index);

    void SetMasterVolume(float value) { MutableAudio().master = value; ApplyAudio(); }
    void SetBgmVolume(float value)    { MutableAudio().bgm    = value; ApplyAudio(); }
    void SetSeVolume(float value)     { MutableAudio().se     = value; ApplyAudio(); }
    void SetUiVolume(float value)     { MutableAudio().ui     = value; ApplyAudio(); }
    ///@}

    /// Option 画面が行を編集するための書き込み参照。呼ばれた時点で「編集が起きた」と
    /// みなし、終了時の保存対象になる。
    /// @note setter は並べない。行が 20 個以上あり、編集面は Option 画面 1 つしか無いため、
    ///       構造体を直接触らせて Apply() で反映させる方が破綻しない。
    ///@{
    [[nodiscard]] VideoConfig& MutableVideo() { m_dirty = true; return m_video; }
    [[nodiscard]] AudioConfig& MutableAudio() { m_dirty = true; return m_audio; }
    [[nodiscard]] InputConfig& MutableInput() { m_dirty = true; return m_input; }
    [[nodiscard]] GameConfig&  MutableGame()  { m_dirty = true; return m_game; }
    ///@}

    /// @name 現在値 (UI の初期表示に使う)
    ///@{
    [[nodiscard]] const VideoConfig& Video() const { return m_video; }
    [[nodiscard]] const AudioConfig& Audio() const { return m_audio; }
    [[nodiscard]] const InputConfig& Input() const { return m_input; }
    [[nodiscard]] const GameConfig&  Game()  const { return m_game; }
    /// 選択できる解像度。降順。
    [[nodiscard]] const std::vector<DisplayResolution>& Resolutions() const { return m_resolutions; }
    /// 現在の width/height に対応する Resolutions() の添字。無ければ 0。
    [[nodiscard]] int ResolutionIndex() const;
    ///@}

    /// @name 遊ぶ側が読む窓口
    /// Instance() が無い場面 (マネージャーだけを単体で再生する等) でも既定値で通る。
    /// @note 生の Game() は配らない。呼ぶ側が毎回 null 判定と既定値を書くことになり、
    ///       1 箇所書き忘れると「そこだけ設定が効かない」という探しにくい壊れ方をする。
    ///@{
    [[nodiscard]] static const GameConfig&  GameOrDefault();
    [[nodiscard]] static const InputConfig& InputOrDefault();
    /// カメラ揺れ / ヒットストップ / 振動の全体倍率 (0 - 1)。
    [[nodiscard]] static float ShakeScale()     { return Clamp01_(GameOrDefault().shake); }
    [[nodiscard]] static float HitstopScale()   { return Clamp01_(GameOrDefault().hitstop); }
    [[nodiscard]] static float VibrationScale() { return Clamp01_(InputOrDefault().vibration); }
    /// 既定を 1.0 とした感度倍率。カメラの Inspector 値へ掛ける。
    [[nodiscard]] static float MouseSensScale();
    [[nodiscard]] static float StickSensScale();
    ///@}

    /// @name キーコンフィグ
    ///@{
    /// device (0 = マウス&キーボード / 1 = パッド) に対応する最初のバインドの添字。
    /// 無ければ -1。
    [[nodiscard]] int BindingIndexFor(std::string_view action, int deviceIndex) const;
    /// リバインドの結果 (bindingIndex 番目) をエンジンから読み取って差分として覚える。
    /// @note 添字を受け取る。「その機器の最初のバインド」で引き直すと、差し替えの結果
    ///       その行が別の機器のものになった瞬間、隣の行を指してしまう。
    void CaptureBinding(std::string_view action, int deviceIndex, int bindingIndex);
    /// 1 つの割り当てを起動時の既定 (Input.inputactions) へ戻す。
    void ResetBinding(std::string_view action, int deviceIndex);
    /// 覚えている差分をすべて破棄し、起動時の既定へ戻す。
    void ResetBindings();
    /// そのアクションの割り当てが既定から変えられているか。表示の切り替えに使う。
    [[nodiscard]] bool IsBindingOverridden(std::string_view action, int deviceIndex) const;
    ///@}

    /// 現在の値を表示・画質・音量・操作へ反映する。
    void Apply();
    /// config ファイルへ書き出す。
    bool Save();
    /// 最後の保存以降に編集があったときだけ書き出す。
    bool SaveIfDirty() { return m_dirty ? Save() : true; }
    /// 既定値へ戻して反映する (保存はしない)。
    void ResetToDefaults();

    /// このコンポーネントが実際に読み書きするファイル。
    /// @note 静的に切り出す (OnStart で 1 度だけ決めない)。シーンによっては居ないため
    ///       (StageSelect / Result)、その間に config.Save() を呼ぶと既定のパスへ落ちる。
    ///       読むときも書くときも同じ式で決め直せば、いつ呼ばれても行き先が 1 つになる。
    [[nodiscard]] static std::string ResolveConfigPath(bool perUser, std::string_view folder);

private:
    static inline GameSettingsComponent* s_instance = nullptr;

    /// @note 最後の値を静的に控える。このコンポーネントは 7 シーン中 4 つにしか置かれず、
    ///       Instance() が null のシーンでは既定値が返って «Option で 0 にした揺れが
    ///       リザルトでだけ既定に戻る» という壊れ方をする。Apply のたびに写しておけば、
    ///       実体が居なくなった後も最後に効いていた値を返せる。
    static inline GameConfig  s_lastGame{};
    static inline InputConfig s_lastInput{};
    static inline VideoConfig s_lastVideo{};
    static inline AudioConfig s_lastAudio{};

    [[nodiscard]] static float Clamp01_(float v) { return std::clamp(v, 0.0f, 1.0f); }

    /// 組み込みの 24 項目を宣言簿へ登録する。値の置き場は今までどおり下の構造体。
    void DeclareBuiltinSettings();
    /// 宣言簿の値を config の [settings] から読み直す。宣言が増えたときだけ走る。
    void SyncRegistry();

    void ApplyVideo();
    void ApplyAudio();
    void ApplyBindings();
    /// 上書きを当てる前の割り当てを控える。「既定へ戻す」の戻り先になる。
    /// @note エンジンからは読み直さない。.inputactions を読んだのは Application の
    ///       初期化で、その置き場所はゲームから辿れないため、上書き直前の値を控える。
    void CaptureDefaultBindings();
    void RefreshDebugText();

    /// スライダー位置 (0-1) を音量倍率へ変換する。
    /// @note 線形のまま渡さない。人の音量感は対数に近く、線形だとスライダーの真ん中が
    ///       「ほぼ最大」に聞こえる。三乗は dB 変換より安く 0 で完全な無音になる。
    [[nodiscard]] static float ToGain(float slider)
    {
        const float t = std::clamp(slider, 0.0f, 1.0f);
        return t * t * t;
    }

    VideoConfig m_video;
    AudioConfig m_audio;
    InputConfig m_input;
    GameConfig  m_game;
    InputBindConfig m_bind;
    /// 起動時の割り当て。保存はしない (次回起動で .inputactions から取り直す)。
    InputBindConfig m_defaultBind;
    std::vector<DisplayResolution> m_resolutions;

    /// 最後の保存以降に MutableXxx() が呼ばれたか。終了時に書き戻すかを決める。
    bool m_dirty = false;
    /// 直前に当てたプリセット。同じ値を撃ち直して手動調整を潰さないための番人。
    int  m_appliedQuality = -1;
    /// 最後に config から読み直したときの宣言簿の版。増えていたら読み直す。
    ///
    /// @note 版で見る。スクリプトが設定を宣言する OnStart はこちらより後になりうる
    ///       (開始順は保証されない) ため、«宣言が増えた» を毎フレーム安く判定する。
    int  m_registryRevision = -1;
    /// 最後に見た «値の版»。進んでいたら宣言簿の側で編集があったということ。
    int  m_registryValues   = -1;
};

FBZZ_REFLECT(GameSettingsComponent)

/// @note 実体ではなく «最後に効いていた値» を返す。実体が居るシーンでは m_game と
///       s_lastGame は Apply のたびに一致し、居ないシーンでは s_lastGame だけが残る。
///       常にこちらを返せば «どのシーンでも同じ答え» を式の形で保証できる。
inline const GameConfig& GameSettingsComponent::GameOrDefault() { return s_lastGame; }

inline const InputConfig& GameSettingsComponent::InputOrDefault() { return s_lastInput; }

inline std::string GameSettingsComponent::ResolveConfigPath(bool perUser,
                                                            std::string_view folder)
{
    /// @note 既定は実行ファイルの隣。SaveStore が相対パスを exe 基準で解決する。
    if (!perUser) return "Config/settings.toml";

    const char* root = std::getenv("LOCALAPPDATA");
    if (!root || !*root) return "Config/settings.toml";

    const std::string name = folder.empty() ? std::string("GreenWare") : std::string(folder);
    return std::string(root) + "/" + name + "/settings.toml";
}

inline float GameSettingsComponent::MouseSensScale()
{
    return std::max(InputOrDefault().mouseSens, 0.0f) / kMouseSensReference;
}

inline float GameSettingsComponent::StickSensScale()
{
    return std::max(InputOrDefault().stickSens, 0.0f) / kStickSensReference;
}

inline void GameSettingsComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("GameSettingsComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;

    m_resolutions = display.EnumResolutions();

    /// @note 解像度の既定はモニターの実寸。1920x1080 決め打ちだと、それより小さい
    ///       ノート PC で初回起動時に画面からはみ出す。
    const DisplayResolution monitor = display.GetMonitorSize();
    if (monitor.width > 0 && monitor.height > 0) {
        m_video.width  = static_cast<int>(monitor.width);
        m_video.height = static_cast<int>(monitor.height);
    }

    /// @note 保存先を決めてから読む。SetPath はテーブルを差し替えるので、必ず Load より先。
    config.SetPath(ResolveConfigPath(perUserConfig, configFolder));
    debugConfigPath = config.GetPath();

    /// @note 読めなくても (初回起動 / 破損) 既定値で進む。config.Read はテーブルが
    ///       無ければ false を返すだけで、構造体には触らない。
    config.Load();
    config.Read("video", m_video);
    config.Read("audio", m_audio);
    config.Read("input", m_input);
    config.Read("game",  m_game);
    config.Read("bind",  m_bind);

    CaptureDefaultBindings();
    /// @note 宣言は Apply より先。組み込みの行が宣言簿に無いまま Option が開くと、
    ///       1 フレームだけ «項目が 1 つも無い» 画面になる。
    DeclareBuiltinSettings();
    Apply();
    /// @note 宣言簿の側の値を保存ファイルから埋める。組み込み項目は上の Read で
    ///       既に埋まっているので、ここで効くのはスクリプトが宣言した項目だけ。
    m_registryRevision = -1;
    SyncRegistry();
    /// @note 読み込んだ直後は「未編集」。終了時に無意味な書き戻しをしない
    m_dirty = false;
}

/// 組み込み 24 行の宣言。
/// @note 値の置き場は宣言簿へ移さない。表示・音量・入力の値は display/audio/input
///       プロキシへ «効かせる» 手順と対で意味を持つため、構造体に残し宣言簿へは
///       «出し入れの口» だけを差す。
/// @note get は静的な控え (s_lastVideo 等) を読む。宣言簿はシーンをまたいで生き残る
///       ため、Instance() を握ると実体が消えた瞬間に 0 を返してキャッシュを潰す。
inline void GameSettingsComponent::DeclareBuiltinSettings()
{
    using settings::Kind;
    using settings::Setting;

    /// @note 「実体が居るときだけ書く」を 1 か所へ。居ないシーンで Option は開けないので、
    ///       書けない場面は起こらないが、握った実体を素で参照しない形にしておく。
    const auto edit = [](auto&& fn) {
        if (auto* self = Instance()) { fn(*self); self->Apply(); }
    };

    const auto declare = [](const char* id, const char* page, Kind kind,
                            std::function<float()> get, std::function<void(float)> set,
                            float minimum = 0.0f, float maximum = 1.0f,
                            int decimals = 0, const char* suffix = "",
                            std::vector<std::string> choices = {}) {
        Setting setting;
        setting.id       = id;
        setting.page     = page;
        setting.kind     = kind;
        setting.min      = minimum;
        setting.max      = maximum;
        setting.decimals = decimals;
        setting.suffix   = suffix;
        setting.choices  = std::move(choices);
        setting.get      = std::move(get);
        setting.set      = std::move(set);
        settings::Declare(std::move(setting));
    };

    /// @name INPUT
    declare("device", "Tab_INPUT", Kind::Choice,
            [] { return static_cast<float>(s_lastInput.device); },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableInput().device = static_cast<int>(std::lround(v)); }); },
            0.0f, 1.0f, 0, "", { "マウス＆キーボード", "ゲームパッド" });
    declare("mouseSens", "Tab_INPUT", Kind::Number,
            [] { return s_lastInput.mouseSens; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableInput().mouseSens = v; }); },
            0.1f, 10.0f, 2);
    declare("stickSens", "Tab_INPUT_PAD", Kind::Number,
            [] { return s_lastInput.stickSens; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableInput().stickSens = v; }); },
            0.1f, 10.0f, 2);
    declare("curve", "Tab_INPUT_PAD", Kind::Choice,
            [] { return static_cast<float>(s_lastInput.curve); },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableInput().curve = static_cast<int>(std::lround(v)); }); },
            0.0f, 2.0f, 0, "", { "リニア", "標準", "強め" });
    /// @note Percent にはしない。デッドゾーンは 0-0.5 の量で、つまみ全域を 0-1 に写すと
    ///       後半が «スティックを半分倒すまで無入力» という操作にならない領域になる。
    declare("deadzone", "Tab_INPUT_PAD", Kind::Number,
            [] { return s_lastInput.deadzone; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableInput().deadzone = v; }); },
            0.0f, 0.5f, 2);
    declare("vibration", "Tab_INPUT_PAD", Kind::Percent,
            [] { return s_lastInput.vibration; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableInput().vibration = v; }); });

    /// @name GAME
    declare("fov", "Tab_GAME", Kind::Number,
            [] { return s_lastGame.fov; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableGame().fov = v; }); },
            60.0f, 110.0f, 0, "\xc2\xb0");
    declare("shake", "Tab_GAME", Kind::Percent,
            [] { return s_lastGame.shake; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableGame().shake = v; }); });
    declare("hitstop", "Tab_GAME", Kind::Percent,
            [] { return s_lastGame.hitstop; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableGame().hitstop = v; }); });
    declare("fovBurst", "Tab_GAME", Kind::Toggle,
            [] { return s_lastGame.fovBurst ? 1.0f : 0.0f; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableGame().fovBurst = v >= 0.5f; }); });

    /// @name VIDEO
    declare("displayMode", "Tab_VIDEO", Kind::Choice,
            [] { return s_lastVideo.fullscreen ? 1.0f : 0.0f; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableVideo().fullscreen = v >= 0.5f; }); },
            0.0f, 1.0f, 0, "", { "ウィンドウ", "フルスクリーン" });
    /// @note 選択肢はモニターが決めるので、宣言のたびに今の一覧から作り直す。
    {
        std::vector<std::string> modes;
        modes.reserve(m_resolutions.size());
        for (const DisplayResolution& mode : m_resolutions) {
            char buffer[64] = {};
            std::snprintf(buffer, sizeof(buffer), "%u \xc3\x97 %u", mode.width, mode.height);
            modes.emplace_back(buffer);
        }
        declare("resolution", "Tab_VIDEO", Kind::Choice,
                [] { auto* s = Instance(); return s ? static_cast<float>(s->ResolutionIndex()) : 0.0f; },
                [edit](float v) { edit([v](GameSettingsComponent& s) {
                    s.SetResolutionIndex(static_cast<int>(std::lround(v))); }); },
                0.0f, 1.0f, 0, "", std::move(modes));
    }
    declare("vsync", "Tab_VIDEO", Kind::Toggle,
            [] { return s_lastVideo.vsync ? 1.0f : 0.0f; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableVideo().vsync = v >= 0.5f; }); });
    declare("fpsCap", "Tab_VIDEO", Kind::Choice,
            [] {
                for (int i = 0; i < 6; ++i)
                    if (FpsCapValue(i) == s_lastVideo.targetFps) return static_cast<float>(i);
                return 2.0f;
            },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableVideo().targetFps = FpsCapValue(static_cast<int>(std::lround(v))); }); },
            0.0f, 5.0f, 0, "", { "無制限", "30", "60", "120", "144", "240" });
    declare("brightness", "Tab_VIDEO", Kind::Number,
            [] { return s_lastVideo.brightness; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableVideo().brightness = v; }); },
            0.5f, 2.0f, 2);
    declare("bloom", "Tab_VIDEO", Kind::Percent,
            [] { return s_lastVideo.bloom; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableVideo().bloom = v; }); });
    /// @note 未選択 (-1) のうちは ProjectSettings の設定がそのまま効いている。保存値ではなく
    ///       実際に効いている段を出す ─ -1 をそのまま出すと «低» に見えてしまう。
    declare("quality", "Tab_VIDEO", Kind::Choice,
            [] {
                if (s_lastVideo.quality >= 0) return static_cast<float>(s_lastVideo.quality);
                auto* s = Instance();
                return s ? static_cast<float>(static_cast<int>(s->graphics.GetQualityPreset()))
                         : 2.0f;
            },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.SetQualityPreset(static_cast<int>(std::lround(v))); }); },
            0.0f, 3.0f, 0, "", { "低", "中", "高", "最高" });
    declare("renderScale", "Tab_VIDEO", Kind::Number,
            [] { return s_lastVideo.renderScale; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableVideo().renderScale = v; }); },
            0.5f, 2.0f, 2);

    /// @name AUDIO
    declare("master", "Tab_AUDIO", Kind::Percent,
            [] { return s_lastAudio.master; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableAudio().master = v; }); });
    declare("sfx", "Tab_AUDIO", Kind::Percent,
            [] { return s_lastAudio.se; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableAudio().se = v; }); });
    declare("bgm", "Tab_AUDIO", Kind::Percent,
            [] { return s_lastAudio.bgm; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableAudio().bgm = v; }); });
    declare("ui", "Tab_AUDIO", Kind::Percent,
            [] { return s_lastAudio.ui; },
            [edit](float v) { edit([v](GameSettingsComponent& s) {
                s.MutableAudio().ui = v; }); });
}

inline void GameSettingsComponent::SyncRegistry()
{
    /// @note 宣言簿の値が書き換わっていたら «編集された» を立てる。組み込み項目は
    ///       MutableXxx() が申告するが、宣言簿が値を持つ項目にはその口が無い。
    if (m_registryValues != settings::ValueRevision()) {
        const bool first = m_registryValues < 0;
        m_registryValues = settings::ValueRevision();
        /// @note 読み込みで動いた分は編集ではない。初回だけ札を立てない。
        if (!first) m_dirty = true;
    }

    if (m_registryRevision == settings::Revision()) return;
    m_registryRevision = settings::Revision();

    /// @note 欠けているキーは既定値のまま残る (TomlReadReflector の約束)。
    ///       宣言が増えるたびに全件を読み直しても、既に入っている値は変わらない。
    if (config.Read("settings", settings::PersistentStore()))
        settings::PersistentStore().ApplyAll();
    /// @note 読み戻しで onApply が走ると値の版が進む。それは編集ではないので飲み込む。
    m_registryValues = settings::ValueRevision();
}

inline void GameSettingsComponent::OnDestroy()
{
    /// @note ここで保存する。Option 画面の Esc 経路だけが保存を持つと、Alt+F4 や別経路の
    ///       タイトル遷移だけで調整した値が丸ごと消える。OnDestroy は終了・遷移を必ず通る。
    SaveIfDirty();
    if (s_instance == this) s_instance = nullptr;
}

inline void GameSettingsComponent::SetResolutionIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(m_resolutions.size())) return;
    const DisplayResolution& mode = m_resolutions[static_cast<size_t>(index)];
    MutableVideo().width  = static_cast<int>(mode.width);
    MutableVideo().height = static_cast<int>(mode.height);
    Apply();
}

inline int GameSettingsComponent::ResolutionIndex() const
{
    for (size_t i = 0; i < m_resolutions.size(); ++i) {
        if (static_cast<int>(m_resolutions[i].width) == m_video.width &&
            static_cast<int>(m_resolutions[i].height) == m_video.height)
            return static_cast<int>(i);
    }
    return 0;
}

inline void GameSettingsComponent::Apply()
{
    ApplyVideo();
    ApplyAudio();
    ApplyBindings();
    /// @note 遊びの側が Instance() 無しでも読めるように写す (理由は s_lastGame を参照)。
    ///       宣言簿の組み込み項目もここから読む。
    s_lastGame  = m_game;
    s_lastInput = m_input;
    s_lastVideo = m_video;
    s_lastAudio = m_audio;
    RefreshDebugText();
}

inline void GameSettingsComponent::ApplyVideo()
{
    /// @note 解像度を先に決めてからモードを切り替える。逆にすると、フルスクリーンを
    ///       解除した瞬間に古い寸法のウィンドウが 1 フレーム出る。
    display.SetResolution(static_cast<uint32_t>(std::max(m_video.width, 640)),
                          static_cast<uint32_t>(std::max(m_video.height, 480)));
    display.SetFullscreen(m_video.fullscreen);
    display.SetVSync(m_video.vsync);

    graphics.SetBrightness(m_video.brightness);
    /// @note スライダーは 0-1。既定の 0.6 が素の bloom.intensity と一致するよう 2 倍で送る。
    graphics.SetBloomScale(m_video.bloom * 2.0f);
    graphics.SetRenderScale(m_video.renderScale);

    /// @note 未選択 (-1) の間は触らない。同じ値を撃ち直さないのは、プリセット適用が
    ///       影や TAA を一括で書き戻すため、当てるたびに手で詰めた設定が消えるから。
    if (m_video.quality >= 0) {
        const int quality = std::clamp(m_video.quality, 0, 3);
        if (quality != m_appliedQuality) {
            graphics.SetQualityPreset(static_cast<fbzz::renderer::QualityPreset>(quality));
            m_appliedQuality = quality;
        }
    }

    /// @note VSync 中に上限を重ねると、表示周期と上限の最小公倍数でカクつく。
    time.SetTargetFps(m_video.vsync ? 0 : std::max(m_video.targetFps, 0));
}

inline void GameSettingsComponent::ApplyAudio()
{
    /// @note Master は他の 3 本の親なので、ここで掛けた分は子へも乗る。
    audio.SetBusVolume("Master", ToGain(m_audio.master));
    audio.SetBusVolume("BGM",    ToGain(m_audio.bgm));
    audio.SetBusVolume("SE",     ToGain(m_audio.se));
    audio.SetBusVolume("UI",     ToGain(m_audio.ui));
}

inline int GameSettingsComponent::BindingIndexFor(std::string_view action, int deviceIndex) const
{
    /// @note ScriptInputBinding::source: 0=Key 1=MouseButton 2=GamepadButton 3=GamepadAxis 4=MouseAxis。
    ///       キーボードとマウスは 1 つの機器として扱う (Option の「入力デバイス」がその単位)。
    const int count = input.GetActionBindingCount(action);
    for (int i = 0; i < count; ++i) {
        const ScriptInputBinding binding = input.GetActionBinding(action, i);
        if (!binding.IsValid()) continue;
        const bool isPad = binding.source == 2 || binding.source == 3;
        if (isPad == (deviceIndex == 1)) return i;
    }
    return -1;
}

inline void GameSettingsComponent::CaptureBinding(std::string_view action, int deviceIndex,
                                                 int bindingIndex)
{
    const ScriptInputBinding binding = input.GetActionBinding(action, bindingIndex);
    if (!binding.IsValid()) return;

    m_bind.Set(action, deviceIndex, binding.source, static_cast<int>(binding.code));
    m_dirty = true;
}

inline bool GameSettingsComponent::IsBindingOverridden(std::string_view action,
                                                      int deviceIndex) const
{
    for (std::size_t i = 0; i < m_bind.Count(); ++i)
        if (m_bind.action[i] == action && m_bind.device[i] == deviceIndex) return true;
    return false;
}

inline void GameSettingsComponent::ResetBinding(std::string_view action, int deviceIndex)
{
    for (std::size_t i = 0; i < m_defaultBind.Count(); ++i) {
        if (m_defaultBind.action[i] != action || m_defaultBind.device[i] != deviceIndex) continue;

        const int index = BindingIndexFor(action, deviceIndex);
        if (index < 0) return;
        input.SetActionBinding(action, index,
                               ScriptInputBinding{ m_defaultBind.source[i],
                                                   static_cast<uint32_t>(m_defaultBind.code[i]) });

        /// @note 差分の側からも消す。既定と同じ値を差分として持ち続けると、
        ///       既定を作り直したときにこの 1 件だけが古い値へ固定される。
        for (std::size_t j = 0; j < m_bind.Count(); ++j) {
            if (m_bind.action[j] != action || m_bind.device[j] != deviceIndex) continue;
            const auto at = static_cast<std::ptrdiff_t>(j);
            m_bind.action.erase(m_bind.action.begin() + at);
            m_bind.device.erase(m_bind.device.begin() + at);
            m_bind.source.erase(m_bind.source.begin() + at);
            m_bind.code.erase(m_bind.code.begin() + at);
            break;
        }
        m_dirty = true;
        return;
    }
}

inline void GameSettingsComponent::ResetBindings()
{
    /// @note 控えた既定を全件当て直す。走査中に m_bind が縮むので、控えの側を回す。
    for (std::size_t i = 0; i < m_defaultBind.Count(); ++i)
        ResetBinding(m_defaultBind.action[i], m_defaultBind.device[i]);
    m_bind.Clear();
    m_dirty = true;
}

inline void GameSettingsComponent::CaptureDefaultBindings()
{
    m_defaultBind.Clear();
    for (const actions::ControlRow& row : actions::kControlRows) {
        if (!row.action || !*row.action) continue;
        for (int deviceIndex = 0; deviceIndex < 2; ++deviceIndex) {
            const int index = BindingIndexFor(row.action, deviceIndex);
            if (index < 0) continue;
            const ScriptInputBinding binding = input.GetActionBinding(row.action, index);
            if (!binding.IsValid()) continue;
            m_defaultBind.Set(row.action, deviceIndex, binding.source,
                              static_cast<int>(binding.code));
        }
    }
}

inline void GameSettingsComponent::ApplyBindings()
{
    for (std::size_t i = 0; i < m_bind.Count(); ++i) {
        const int index = BindingIndexFor(m_bind.action[i], m_bind.device[i]);
        /// @note アクションが消えた古い config は黙って捨てる
        if (index < 0) continue;
        input.SetActionBinding(m_bind.action[i], index,
                               ScriptInputBinding{ m_bind.source[i],
                                                   static_cast<uint32_t>(m_bind.code[i]) });
    }
}

inline bool GameSettingsComponent::Save()
{
    /// @note 書く直前に行き先を立て直す。ストアはアプリ全体で 1 本なので、
    ///       このコンポーネントの居ないシーンを通ると既定のパスへ戻りうる
    ///       (理由は ResolveConfigPath を参照)。
    config.SetPath(ResolveConfigPath(perUserConfig, configFolder));

    config.Write("video", m_video);
    config.Write("audio", m_audio);
    config.Write("input", m_input);
    config.Write("game",  m_game);
    config.Write("bind",  m_bind);
    /// @note スクリプトが宣言した項目。組み込みは get/set を差してあるので書かれない。
    config.Write("settings", settings::PersistentStore());
    const bool saved = config.Save();
    if (saved) {
        m_dirty = false;
        debugConfigPath = config.GetPath();
    }
    return saved;
}

inline void GameSettingsComponent::ResetToDefaults()
{
    m_video = VideoConfig{};
    m_audio = AudioConfig{};
    m_input = InputConfig{};
    m_game  = GameConfig{};
    ResetBindings();
    /// @note スクリプトが宣言した項目も戻す。組み込みは上の 4 つで戻っているので、
    ///       ここで動くのは宣言簿が値を持っている項目だけ。
    settings::ResetToDefaults();
    m_dirty = true;
    Apply();
}

inline void GameSettingsComponent::RefreshDebugText()
{
    /// @note 「出力 → 内部」を並べて出す。描画スケールが効いているかはこれでしか判らない
    ///       (画面の寸法は変わらないため)。
    debugResolution = std::to_string(display.GetWidth()) + "x" +
                      std::to_string(display.GetHeight()) + " -> " +
                      std::to_string(graphics.GetRenderWidth()) + "x" +
                      std::to_string(graphics.GetRenderHeight()) +
                      (display.IsFullscreen() ? " (fullscreen)" : " (windowed)");

    static const char* kQualityNames[] = { "Low", "Medium", "High", "Ultra" };
    const int quality = std::clamp(static_cast<int>(graphics.GetQualityPreset()), 0, 3);
    debugQuality = kQualityNames[quality];
}

} // namespace sandbox
