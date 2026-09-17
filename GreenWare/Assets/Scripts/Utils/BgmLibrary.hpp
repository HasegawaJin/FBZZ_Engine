/// @file    BgmLibrary.hpp
/// @brief   BGM の所在表と、場面が変わったときの掛け替え
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// @note «どの画面でどの曲か» は素材のファイル名がすでに宣言している
///       (_Title/_StageSelect/_Option/_Boss01) ため、表はコードに持つ (SeLibrary.hpp と
///       同じ理由)。ステージだけは盤面ごとに曲が変わるので GameFlowComponent の
///       フィールドが持つ。BGM は AudioManager の «常に 1 本» のスロットで PlayBGM を
///       呼び直すと頭から鳴るため、掛け替えはここへ閉じる ─ 今の曲を 1 か所で覚え、
///       同じ曲なら何もしない。覚える側を静的にする ─ 曲はシーンをまたいで鳴り続ける
///       (LoadScene は voice を止めない) ため、シーンに属すと宣言者が消えた後に
///       «同じ曲か» を誰も答えられない (SceneTransition.hpp の State と同じ理由)。
#pragma once

#include <Engine/Scene/ScriptProxy/ScriptAudioProxy.hpp>
#include <algorithm>
#include <string>
#include <string_view>

namespace sandbox::bgm {

inline constexpr std::string_view kTitle =
    "Assets/Sound/BGM/maou_bgm_cyber06_Title.mp3";
inline constexpr std::string_view kOptions =
    "Assets/Sound/BGM/maou_bgm_cyber39_Option.mp3";
inline constexpr std::string_view kStageSelect =
    "Assets/Sound/BGM/maou_bgm_cyber21_StageSelect.mp3";
inline constexpr std::string_view kLoad =
    "Assets/Sound/BGM/maou_bgm_cyber45_Load.mp3";
/// 教えている間の曲。ステージのフィールドから指す (ここは «どのファイルか» だけを言う)。
inline constexpr std::string_view kTutorial =
    "Assets/Sound/BGM/maou_bgm_cyber44_チュートリアル.mp3";
inline constexpr std::string_view kBoss01 =
    "Assets/Sound/BGM/maou_game_boss05_Boss01.mp3";
inline constexpr std::string_view kBoss02 =
    "Assets/Sound/BGM/maou_game_battle30_Boss02.mp3";
inline constexpr std::string_view kBoss03 =
    "Assets/Sound/BGM/maou_game_lastboss01_Boss03.mp3";
/// 決着の画面の曲。**どちらも数秒の «締め» で、繰り返さない** (PlayOnce)。
///
/// @note ステージの曲はフィールドだが、この 2 本は表が持つ。勝ち負けはステージごとに
///       変わらず、どの盤面から来ても «勝った/負けた» は同じ 1 つの出来事なので、盤面
///       ごとのスロットにすると増やすたびに «1 つだけ空» という壊れ方をする。
inline constexpr std::string_view kResultClear =
    "Assets/Sound/BGM/maou_game_jingle01_Result_Clear.mp3";
inline constexpr std::string_view kResultFailed =
    "Assets/Sound/BGM/Result_Failed.mp3";

/// 画面が変わるときのすれ違い [s]。扉 (ワイプ) の 0.55 秒より長く取る ─
/// 画が切り替わった後も少し前の曲が残っている方が、境目が «途切れ» に聞こえない。
inline constexpr float kSceneFade = 1.4f;
/// 決着で落とすときの長さ [s]。
inline constexpr float kStopFade = 1.2f;

namespace detail {

/// 最後に «鳴らす» と宣言された曲。空なら «鳴らしていない»。
inline std::string& Declared()
{
    static std::string declared;
    return declared;
}

} // namespace detail

/// 曲を落とす。鳴っていなければ何もしない。
inline void Stop(const fbzz::scene::ScriptAudioProxy& audio, float fadeSeconds = kStopFade)
{
    detail::Declared().clear();
    audio.StopBGM(std::max(fadeSeconds, 0.0f));
}

/// この場面の曲を宣言する。同じ曲が既に鳴っていれば何もしない。
/// @param path 空なら «この場面に曲は無い» として落とす。
inline void Play(const fbzz::scene::ScriptAudioProxy& audio, std::string_view path,
                 float fadeSeconds = kSceneFade)
{
    if (path.empty()) {
        Stop(audio, fadeSeconds);
        return;
    }

    /// @note 覚えている曲名«だけ»では判断しない。Play→Stop も一括停止 (StopAllVoices) も
    ///       voice を黙って畳むため、宣言だけが残って «鳴っているつもり» になる。
    std::string& declared = detail::Declared();
    if (declared == path && audio.IsBGMPlaying()) return;

    declared.assign(path);
    audio.PlayBGM(path, /*loop=*/true, std::max(fadeSeconds, 0.0f));
}

/// 1 度だけ鳴らす «締め» を流す (リザルトのジングル)。
///
/// @note Play とは分ける。Play は «同じ曲が鳴っていれば何もしない» だが、鳴り終わった
///       ジングルは `IsBGMPlaying()` が false になるため、Play のままだと同じ画面で
///       もう一度宣言されて頭から鳴り直す。BGM スロットは使い続ける ─ SE として重ねると
///       «戦闘の曲の上にジングル» になりうるため、1 本きりの枠で «前の曲は止まり、
///       これが鳴る» を保証する。
inline void PlayOnce(const fbzz::scene::ScriptAudioProxy& audio, std::string_view path,
                     float fadeSeconds = 0.0f)
{
    if (path.empty()) {
        Stop(audio, fadeSeconds);
        return;
    }

    std::string& declared = detail::Declared();
    if (declared == path) return;

    declared.assign(path);
    audio.PlayBGM(path, /*loop=*/false, std::max(fadeSeconds, 0.0f));
}

} // namespace sandbox::bgm
