/// @file    StageCatalog.hpp
/// @brief   ステージ 1 つぶんの «どのシーンか / 何という名前か / 何が居るか»
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// WHY 1 つの表に畳むか:
///   ステージの情報は 3 か所が別々に要る ─ 選択画面の行 (名前)、選択画面の右パネル
///   (ボスの構成)、そして «PLAY / RETRY で何を読み込むか» (シーン名)。
///   別々に持つと、ステージを 1 本足すたびに 3 か所を直すことになり、片方だけ直した
///   状態が «名前は出ているのに押すと 1 面が始まる» という形で出る。
///
/// WHY シーン名を «無い» にできるようにするか:
///   行は 7 つあるが、作ってあるのは 2 つ。空の枠を «解放されているが押せない» と
///   表せないと、クリアした次の行が «押すと読み込みに失敗して固まる» になる。
///   空文字は «まだ作っていない» の意味で、選択画面はそこへ進ませない。
///
/// WHY 構成 (ボスの手) をここに書くか:
///   右パネルはクリア後にだけ出す «記録» で、遊ぶ前の予習ではない
///   (StageSelectComponent の RefreshPane)。文言はステージの設計そのものなので、
///   画面ではなく台帳が持つ。
#pragma once

namespace sandbox {

/// ステージ 1 つぶんの静的な情報。
struct StageEntry {
    /// 読み込むシーン名。空なら «まだ作っていない枠»。
    const char* scene = "";
    /// 解放されてから出す名前。伏せる側 ("????") は画面が決める。
    const char* name = "????";

    // 右パネルの «ボスの構成»。クリアするまで伏せる。
    const char* boss     = "";
    /// もぎ取る部位の数。ボスに HP は無く、進行はこれだけで刻まれる
    /// (Docs/break-parry.md)。
    const char* parts    = "";
    const char* phase1   = "";
    const char* phase1Sub = "";
    const char* phase2   = "";
    const char* phase2Sub = "";
    const char* solution = "";
};

/// 行数はここと `StageProgressState::kCount`、シーンの Row の数の 3 つを揃える。
inline constexpr int kStageCount = 7;

inline constexpr StageEntry kStages[kStageCount] = {
    { "Stage_01", "ARENA",
      "ポラリティ・コア", "背のコア",
      "踏みつけ / 突進 → 激突",       "どちらも弾ける ・ 輪は跳んで越える",
      "＋ コアビーム / 磁力パルス",   "脚 2 本で据え付けの砲台へ",
      "脚を落として倒し、登って背のコアを叩く" },

    { "Stage_02", "SERPENT PIT",
      "ポラリティ・サーペント", "28 → 6 節",
      "突き上げ / 薙ぎ / せり上がり", "28〜23節 ・ 弾けるのは噛みつきと薙ぎ",
      "＋ 頭の突進 / 囲い込み",       "22〜15節 ・ 潜行が速くなる",
      "弾いて崩し、とどめで尾側 4 節を落とす" },

    {}, {}, {}, {}, {},
};

/// index のステージ。範囲外は «空の枠» を返すので、呼ぶ側に境界を書かせない。
[[nodiscard]] inline const StageEntry& StageAt(int index)
{
    static constexpr StageEntry kNone{};
    return (index >= 0 && index < kStageCount) ? kStages[index] : kNone;
}

/// 遊べる実体があるか (シーンが作ってある)。解放されているかは別の話。
[[nodiscard]] inline bool StageExists(int index)
{
    const char* scene = StageAt(index).scene;
    return scene && scene[0] != '\0';
}

} // namespace sandbox
