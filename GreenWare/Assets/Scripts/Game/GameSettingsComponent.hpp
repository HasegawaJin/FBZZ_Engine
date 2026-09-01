/// @file    GameSettingsComponent.hpp
/// @brief   Option 設定の保存・読み込みと、表示 / 画質 / 音量 / 操作への反映
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// WHY エンジンではなくゲームが持つか:
///   config ストアはキー名も構造もゲームが決める前提で作られている
///   (Docs/design/game-settings.md)。エンジンが読む形にすると "fullscreen" という
///   キー名をエンジンが固定することになる。機構はエンジン、方針はゲーム。
///
/// WHY Apply と Save を分けるか:
///   Option 画面はスライダーを動かしながら耳と目で合わせる操作で、確定前の値も
///   即座に効かないと調整にならない。一方でディスクへ落とすのは「適用」を
///   押したときだけにしたい。効かせる (Apply) と残す (Save) は別の操作。
///
/// WHY 遊ぶシーンにも置くか:
///   表示・音量はアプリ寿命のグローバル (Time::targetFps / AudioManager /
///   ProjectSettings.render) へ書くのでシーンをまたいでも残る。一方で視野角・カメラ揺れ・
///   ヒットストップ・振動・感度は、遊んでいる最中に毎フレーム読まれる値で、
///   読む側は Instance() 越しにしか辿れない。タイトルにしか置かないと
///   「保存はされるが遊びには一切効かない」という、最も気付きにくい壊れ方をする。
///
/// 起動時の流れ:
///   Load → Apply。ウィンドウは ProjectSettings の寸法で既に生成済みなので、
///   フルスクリーンの設定が違うときだけ一瞬ウィンドウが見える。ボーダーレスなので
///   スタイル変更と Resize 1 回で済み、スワップチェーンは作り直されない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
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
    /// WHY 既定を High にしないか: プリセットの適用は影・SSR・TAA など 15 項目を
    ///     一括で上書きする。既定値を持たせると、初回起動でプロジェクトの描画設定が
    ///     まるごと High へ塗り替わり、作り込んだ設定が誰にも見られないまま消える。
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
/// WHY device を保存するか: パッドを挿していても「キーボードで遊ぶ」選択はありうる。
///     接続の有無で勝手に切り替えると、Option で選んだ設定が起動ごとに変わる。
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
/// WHY 倍率として保存しないか: Option に出るのは 2.40 という数値で、これが
///     「既定の何倍か」だと読めない。表示と保存で意味を変えると、設定ファイルを
///     直接見たときに解釈が食い違う。
inline constexpr float kMouseSensReference = 2.4f;
inline constexpr float kStickSensReference = 2.0f;

/// 応答カーブ (0 = リニア / 1 = 標準 / 2 = 強め) を指数へ直す。
[[nodiscard]] inline float CurveExponent(int curve)
{
    static const float kExponents[] = { 1.0f, 2.0f, 3.0f };
    return kExponents[std::clamp(curve, 0, 2)];
}

/// ゲームプレイの好み。config の [game] テーブルへ往復する。
///
/// 読む側: 視野角と点火・集束時の FOV 変化は TpsCameraComponent、揺れ・止め・振動は
///         各マネージャーの要求受け口、チェイン表示は ChainDisplayComponent。
///         いずれも Instance() が無い場面 (エディタでの単体再生など) では
///         既定値で動くよう、下の静的アクセサ越しに読む。
struct GameConfig : IScriptSerializable {
    float fov       = 74.0f;  ///< 60 - 110 [deg]
    float shake     = 0.7f;   ///< 0 - 1
    float hitstop   = 1.0f;   ///< 0 - 1
    bool  fovBurst  = true;
    bool  chain     = true;

    void Reflect(IReflector& r) override
    {
        r.Field("fov", fov);            r.Field("shake", shake);
        r.Field("hitstop", hitstop);    r.Field("fovBurst", fovBurst);
        r.Field("chain", chain);
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
///
/// WHY 全アクションではなく「変更したものだけ」を持つか:
///   既定の割り当ては ProjectSettings/Input.inputactions にある。全件を config へ
///   写すと、既定を作り直したときに古い config がそれを打ち消し、新しい既定が
///   誰にも届かなくなる。差分だけを持てば、触っていないアクションは常に最新の
///   既定に従う。アクションを増やしても古い config はそのまま使える。
///
/// WHY 並列配列か: TOML の配列は型が揃っていれば素直に読める形で、しかも
///   人が開いて意味が判る。エンジンの InputBinding をそのまま写すと
///   scale / padIndex / 閾値まで固定してしまい、既定の調整が config に殺される。
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
    // 保存先。既定 (エンジン側) は «実行ファイルの隣の Config/settings.toml»。
    //
    // WHY 実行ファイルの隣ではいけないか: エディタ (SDK/tools/…/Editor) と配布ビルド
    //     (Binaries/…/GreenWareStandalone) は別の exe なので、既定のままだと
    //     **別々のファイル**になる。エディタで調整した設定はゲームを起動しても
    //     どこにも無く、ゲームで変えた設定はエディタに出てこない ─
    //     «ランタイムでは設定が保存できていない» はこの形で現れる。
    //     ユーザーごとに 1 つの場所へ寄せれば、どちらから起動しても同じ設定になる。
    FBZZ_GROUP("Storage")
    FBZZ_FIELD(bool, perUserConfig, true, "Per User")
    FBZZ_TOOLTIP("設定を %LOCALAPPDATA%/<Folder>/settings.toml へ保存する。"
                 "切ると実行ファイルの隣 (Config/settings.toml) に戻る ─ "
                 "USB へ入れて持ち運ぶような «可搬» の配布にするときだけ切ること")
    FBZZ_FIELD(std::string, configFolder, "GreenWare", "Folder")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugResolution, "-", "Resolution")
    FBZZ_FIELD_READ_ONLY(std::string, debugQuality, "-", "Quality")
    FBZZ_FIELD_READ_ONLY(std::string, debugConfigPath, "-", "Config Path")
    FBZZ_TOOLTIP("実際に読み書きしているファイル。設定が «保存されない» ときは"
                 "まずここを見ること (見ているファイルが期待と違うことが多い)")

    /// シーンに 1 つだけ置く前提。Option 画面の UI はここから値を読み書きする。
    [[nodiscard]] static GameSettingsComponent* Instance() { return s_instance; }

    void OnStart() override;
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

    /// Option 画面が行を編集するための書き込み参照。
    /// WHY setter を並べないか: 行が 20 個以上あり、1 つずつ setter を生やすと
    ///     行を足すたびにここも直すことになる。編集面は Option 画面 1 つしか無いので、
    ///     構造体を直接触らせて Apply() で反映させる方が破綻しない。
    /// 呼ばれた時点で「編集が起きた」とみなし、終了時の保存対象になる。
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
    /// WHY 生の Game() を配らないか: 呼ぶ側が毎回 null 判定と既定値を書くことになり、
    ///     1 箇所書き忘れると「そこだけ設定が効かない」という探しにくい形で壊れる。
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
    /// WHY 添字を受け取るか: 「その機器の最初のバインド」で引き直すと、差し替えの
    ///     結果その行が別の機器のものになった瞬間、隣の行を指してしまう。
    ///     差し替えを始めたときの添字が唯一の正しい持ち主。
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

private:
    static inline GameSettingsComponent* s_instance = nullptr;

    [[nodiscard]] static float Clamp01_(float v) { return std::clamp(v, 0.0f, 1.0f); }

    void ApplyVideo();
    void ApplyAudio();
    void ApplyBindings();
    /// 上書きを当てる前の割り当てを控える。「既定へ戻す」の戻り先になる。
    /// WHY エンジンから読み直さないか: .inputactions を読んだのは Application の初期化で、
    ///     その置き場所はゲームからは辿れない。読み直す口を足すより、上書きする直前の
    ///     値を控えておく方が確実で、しかも 1 回で済む。
    void CaptureDefaultBindings();
    void RefreshDebugText();

    /// スライダー位置 (0-1) を音量倍率へ変換する。
    /// WHY 線形のまま渡さないか: 人の音量感は対数に近く、線形だとスライダーの
    ///     真ん中が「ほぼ最大」に聞こえる。三乗は dB 変換より安く、
    ///     0 で完全な無音になる (dB は -inf を別扱いする必要がある)。
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
};

FBZZ_REFLECT(GameSettingsComponent)

inline const GameConfig& GameSettingsComponent::GameOrDefault()
{
    static const GameConfig kDefaults{};
    return s_instance ? s_instance->m_game : kDefaults;
}

inline const InputConfig& GameSettingsComponent::InputOrDefault()
{
    static const InputConfig kDefaults{};
    return s_instance ? s_instance->m_input : kDefaults;
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

    // 解像度の既定はモニターの実寸。1920x1080 決め打ちだと、それより小さい
    // ノート PC で初回起動時に画面からはみ出す。
    const DisplayResolution monitor = display.GetMonitorSize();
    if (monitor.width > 0 && monitor.height > 0) {
        m_video.width  = static_cast<int>(monitor.width);
        m_video.height = static_cast<int>(monitor.height);
    }

    // 保存先を決めてから読む。SetPath はテーブルを差し替えるので、必ず Load より先。
    if (perUserConfig) {
        if (const char* root = std::getenv("LOCALAPPDATA")) {
            const std::string folder = configFolder.empty() ? std::string("GreenWare")
                                                            : configFolder;
            config.SetPath(std::string(root) + "/" + folder + "/settings.toml");
        }
    }
    debugConfigPath = config.GetPath();

    // 読めなくても (初回起動 / 破損) 既定値で進む。config.Read はテーブルが
    // 無ければ false を返すだけで、構造体には触らない。
    config.Load();
    config.Read("video", m_video);
    config.Read("audio", m_audio);
    config.Read("input", m_input);
    config.Read("game",  m_game);
    config.Read("bind",  m_bind);

    CaptureDefaultBindings();
    Apply();
    m_dirty = false;   // 読み込んだ直後は「未編集」。終了時に無意味な書き戻しをしない
}

inline void GameSettingsComponent::OnDestroy()
{
    // WHY ここで保存するか: Option 画面から Esc で戻る経路だけが保存を持っていると、
    //     そのまま Alt+F4 で閉じた・タイトルへ別経路で戻った、というだけで
    //     調整した値が丸ごと消える。終了もシーン遷移も OnDestroy を必ず通る。
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
    RefreshDebugText();
}

inline void GameSettingsComponent::ApplyVideo()
{
    // 解像度を先に決めてからモードを切り替える。逆にすると、フルスクリーンを
    // 解除した瞬間に古い寸法のウィンドウが 1 フレーム出る。
    display.SetResolution(static_cast<uint32_t>(std::max(m_video.width, 640)),
                          static_cast<uint32_t>(std::max(m_video.height, 480)));
    display.SetFullscreen(m_video.fullscreen);
    display.SetVSync(m_video.vsync);

    graphics.SetBrightness(m_video.brightness);
    // スライダーは 0-1。既定の 0.6 が素の bloom.intensity と一致するよう 2 倍で送る。
    graphics.SetBloomScale(m_video.bloom * 2.0f);
    graphics.SetRenderScale(m_video.renderScale);

    // 未選択 (-1) の間は触らない。同じ値を撃ち直さないのは、プリセット適用が
    // 影や TAA を一括で書き戻すため、当てるたびに手で詰めた設定が消えるから。
    if (m_video.quality >= 0) {
        const int quality = std::clamp(m_video.quality, 0, 3);
        if (quality != m_appliedQuality) {
            graphics.SetQualityPreset(static_cast<fbzz::renderer::QualityPreset>(quality));
            m_appliedQuality = quality;
        }
    }

    // VSync 中に上限を重ねると、表示周期と上限の最小公倍数でカクつく。
    time.SetTargetFps(m_video.vsync ? 0 : std::max(m_video.targetFps, 0));
}

inline void GameSettingsComponent::ApplyAudio()
{
    // Master は他の 3 本の親なので、ここで掛けた分は子へも乗る。
    audio.SetBusVolume("Master", ToGain(m_audio.master));
    audio.SetBusVolume("BGM",    ToGain(m_audio.bgm));
    audio.SetBusVolume("SE",     ToGain(m_audio.se));
    audio.SetBusVolume("UI",     ToGain(m_audio.ui));
}

inline int GameSettingsComponent::BindingIndexFor(std::string_view action, int deviceIndex) const
{
    // ScriptInputBinding::source: 0=Key 1=MouseButton 2=GamepadButton 3=GamepadAxis 4=MouseAxis。
    // キーボードとマウスは 1 つの機器として扱う (Option の「入力デバイス」がその単位)。
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

        // 差分の側からも消す。既定と同じ値を差分として持ち続けると、
        // 既定を作り直したときにこの 1 件だけが古い値へ固定される。
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
    // 控えた既定を全件当て直す。走査中に m_bind が縮むので、控えの側を回す。
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
        if (index < 0) continue;   // アクションが消えた古い config は黙って捨てる
        input.SetActionBinding(m_bind.action[i], index,
                               ScriptInputBinding{ m_bind.source[i],
                                                   static_cast<uint32_t>(m_bind.code[i]) });
    }
}

inline bool GameSettingsComponent::Save()
{
    config.Write("video", m_video);
    config.Write("audio", m_audio);
    config.Write("input", m_input);
    config.Write("game",  m_game);
    config.Write("bind",  m_bind);
    const bool saved = config.Save();
    if (saved) m_dirty = false;
    return saved;
}

inline void GameSettingsComponent::ResetToDefaults()
{
    m_video = VideoConfig{};
    m_audio = AudioConfig{};
    m_input = InputConfig{};
    m_game  = GameConfig{};
    ResetBindings();
    m_dirty = true;
    Apply();
}

inline void GameSettingsComponent::RefreshDebugText()
{
    // 「出力 → 内部」を並べて出す。描画スケールが効いているかはこれでしか判らない
    // (画面の寸法は変わらないため)。
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
