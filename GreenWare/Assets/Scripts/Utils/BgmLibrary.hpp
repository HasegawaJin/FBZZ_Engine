/// @file    BgmLibrary.hpp
/// @brief   BGM の所在表と、場面が変わったときの掛け替え
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// WHY 表をコードに持つか:
///   SeLibrary.hpp と同じ理由 ─ «どの画面でどの曲か» は素材のファイル名がすでに
///   宣言している (_Title / _StageSelect / _Option / _Boss01)。画面ごとに Inspector の
///   スロットを置くと、シーンを作り直すたびに割り当て直しになる。
///   ただしステージだけは «どの曲か» が盤面ごとに変わるので、そこは
///   GameFlowComponent のフィールドが持つ (曲の差し替えに再ビルドを要らなくする)。
///
/// WHY 掛け替えをここへ閉じるか:
///   BGM は AudioManager の «常に 1 本» のスロットで、PlayBGM を呼び直すと頭から鳴る。
///   Title → Options → Title のように «画面は変わるが曲は続く» 経路があるので、
///   各画面が素直に呼ぶと画面を行き来するたびに曲が頭へ戻る。
///   いま何を鳴らしていると宣言したかを 1 か所で覚え、同じ曲なら何もしない。
///
/// WHY 静的に覚えるか:
///   曲はシーンをまたいで鳴り続ける (LoadScene は voice を止めない)。覚える側が
///   シーンに属していると、宣言した当人が消えた後は誰も «同じ曲か» を答えられない
///   (SceneTransition.hpp の State と同じ理由)。
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
/// WHY 画面の表がこの 2 本を持つか (ステージの曲はフィールドなのに):
///   勝ち負けはステージごとに変わらない。どの盤面から来ても «勝った / 負けた» は
///   同じ 1 つの出来事なので、盤面ごとのスロットにすると 3 か所へ同じ曲を割り当てる
///   ことになり、増やすたびに «1 つだけ空» という壊れ方をする。
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

    // WHY 覚えている曲名«だけ»で判断しないか: Play→Stop も一括停止 (StopAllVoices) も
    //     voice を黙って畳むので、宣言だけが残って «鳴っているつもり» になる。
    //     症状は «2 回目の再生から BGM が無い» で、画面には何も出ない。
    std::string& declared = detail::Declared();
    if (declared == path && audio.IsBGMPlaying()) return;

    declared.assign(path);
    audio.PlayBGM(path, /*loop=*/true, std::max(fadeSeconds, 0.0f));
}

/// 1 度だけ鳴らす «締め» を流す (リザルトのジングル)。
///
/// WHY Play と分けるか: Play は «同じ曲が鳴っていれば何もしない» で場面の継続を守るが、
///     鳴り終わったジングルは `IsBGMPlaying()` が false なので、同じ画面でもう一度
///     宣言されると頭から鳴り直してしまう。**一度きり**はそれ自体が意味なので、
///     宣言を «鳴らし終えた» ままにできる別の口にする。
///
/// WHY それでも BGM スロットを使うか: リザルトは前の画面の曲を必ず断つ場面で、
///     SE として重ねると «戦闘の曲の上にジングル» になりうる。1 本きりの枠へ置けば
///     «前の曲は止まり、これが鳴る» が枠の性質として保証される。
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
