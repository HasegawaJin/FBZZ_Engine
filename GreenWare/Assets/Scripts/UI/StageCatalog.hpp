/// @file    StageCatalog.hpp
/// @brief   ステージ 1 つぶんの «どのシーンか / 何という名前か / 何が居るか»
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// @note 選択画面の行・右パネル (ボスの構成)・PLAY/RETRY が読むシーン名の 3 者が
///       同じ情報を要るため、ここ 1 つの表に畳む。`scene` が空の行は «実体の無い枠»
///       (StageExists が false を返し、選択画面はそこへ進ませない)。ボスの構成は
///       StageSelectComponent::RefreshPane がクリア後にだけ出す «記録» の文言。
#pragma once

namespace sandbox {

/// ステージ 1 つぶんの静的な情報。
struct StageEntry {
    /// 読み込むシーン名。空なら «まだ作っていない枠»。
    const char* scene = "";
    /// 解放されてから出す名前。伏せる側 ("????") は画面が決める。
    const char* name = "????";

    /// 右パネルの «ボスの構成»。クリアするまで伏せる。
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
    /// @note 解法の文言は旧版 (登攀) のまま。現行の決着は脚 4 本破壊 (Docs/climb-core.md)。
    ///       クリア後にだけ出る «記録» なので、誤りがあっても遊びには影響しない。
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

    /// @note ⚠ この行だけ **一度も実行していない**。盤面 (孤島のテレイン・プレイヤー・
    ///       ボス一式) は揃っているが、手触りの確認はまだ。最初の起動で崩れるなら、
    ///       戻すのはこの `scene` の 1 語 ─ 空にすれば «解放されているが押せない枠» に戻る。
    ///       ステージ名は仮。ボスの表示名は SERAPH。
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
