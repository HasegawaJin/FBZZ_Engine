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
    /// 破壊対象と決着までの目安。
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
    // WHY 解法を書き換えたか (2026-09-14): «登って背のコアを叩く» という決着は
    //     2026-09-10 に取り下げられ、脚 4 本へ戻っている (Docs/climb-core.md)。
    //     台帳だけがそれより前の版で残っていて、選択画面が**存在しない遊びを説明して
    //     いた**。ここはクリア後に出る «記録» なので、嘘があっても遊びは壊れない ──
    //     だからこそ誰も気づかない場所になる。
    //
    { "Stage_01", "チュートリアル",
      "IRON WARDEN", "鉄骸の番人 / 脚 4 本",
      "斬撃・回避・弾きを学ぶ",       "相棒の案内に合わせて操作を覚える",
      "崩して、脚にとどめ",           "脚を落とすと新たな攻撃が解禁",
      "倒れた脚を 1 本ずつ破壊。4 本で撃破" },

    { "Stage_02", "SERPENT PIT",
      "NIDHOGG", "双獄の大蛇 / 2 体 / 各 28 節 → 6 節で撃破",
      "床の開口と地上の突進に注意",   "噛みつきや薙ぎを弾いて体勢を崩す",
      "短くなるほど攻撃が加速",       "残り 22 節・14 節で段階が変わる",
      "倒れた胴にとどめ。1 回で最大 4 節を破壊" },

    // ⚠ この行だけ **一度も実行していない**。盤面 (孤島のテレイン・プレイヤー・
    //    ボス一式) は揃っているが、手触りの確認はまだ。最初の起動で崩れるなら、
    //    戻すのはこの `scene` の 1 語 ─ 空にすれば «解放されているが押せない枠» に戻る。
    //    ステージ名は仮。ボスの表示名は SERAPH。
    { "Stage_03", "ISLE",
      "SERAPH", "焔翼の熾天使 / 本体HP / 翼6枚・投げた翼も破壊可",
      "連続叩きつけ・翼の投擲",       "飛んでくる翼を弾き返して反撃",
      "HP半分か翼3枚で繭を経て加速",       "レーザー着弾後の残り火を避けて反撃",
      "翼を弾き返して落下。本体へ連撃" },

    {}, {}, {}, {},
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
