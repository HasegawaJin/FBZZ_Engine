/// @file    BossMoveGate.hpp
/// @brief   ボスに «出してよい手» を外から絞る門。教える側と戦う側の境目
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// @note 段の絞り方はボスの AI ではなく外側 (TutorialDirectorComponent) が持つ契約。
///       絞る側と絞られる側が互いを include せずに済むよう、この葉ヘッダーへ独立させてある。
#pragma once

namespace sandbox {

/// ボスの «手» 1 つ。BossAiComponent の Act とは 1 対 1 ではない
/// (突進の始動と走りは 1 つの手、大ジャンプの踏み切り/滞空/着地も 1 つ)。
enum class BossMove : unsigned {
    Stomp   = 1u << 0,   ///< 踏みつけ。弾ける
    Jump    = 1u << 1,   ///< 大ジャンプ → 着地の直撃。輪は跳んで越える
    Charge  = 1u << 2,   ///< 突進。弾くと激突と同じ転倒に落ちる (重い手)
    Beam    = 1u << 3,   ///< コアビーム。避ける手
    Pulse   = 1u << 4,   ///< 磁力パルス。弾けない ─ 落ちた脚を投げる手でもある
    FanBeam = 1u << 5,   ///< 扇。間合いを問わない全域の手
};

/// 全部許す (門を置いていない状態)。
inline constexpr unsigned kBossMoveAll = ~0u;
/// 1 手も許さない。«立っているが何もしない» ＝ 見せるだけの段。
inline constexpr unsigned kBossMoveNone = 0u;

[[nodiscard]] inline constexpr unsigned operator|(BossMove a, BossMove b)
{
    return static_cast<unsigned>(a) | static_cast<unsigned>(b);
}
[[nodiscard]] inline constexpr unsigned operator|(unsigned a, BossMove b)
{
    return a | static_cast<unsigned>(b);
}

/// 門 1 枚。既定は «何も絞らない» で、置かなければ従来どおりの戦いになる。
/// @note 手を絞るだけでなくテンポ・畳み掛けも合わせて渡す。個別に絞ると
///       «手は絞ったがテンポを戻し忘れた» の食い違いが起きるため。
struct BossMoveGate {
    /// 出してよい手。`kBossMoveAll` なら距離の表をそのまま通す。
    unsigned allow = kBossMoveAll;
    /// プレイヤーの手を読んで割り込むか。教える段では切る ─ 咎めは «読めた» の
    /// 先にある遊びで、まだ手を覚えていない相手には理不尽にしかならない。
    bool reactions = true;
    /// 畳み掛けの上限を上書きする。負なら Inspector の値。
    int chainMax = -1;
    /// テンポの上書き [倍]。0 以下なら Inspector の値 (増悪もそのまま効く)。
    float tempo = 0.0f;
    /// 攻撃終了後に確保する最短の隙 [ゲーム内秒]。
    float recoverySeconds = 0.0f;
    bool holdPosition = false;

    /// 何も絞っていないか。
    [[nodiscard]] bool IsOpen() const
    {
        return allow == kBossMoveAll && reactions && chainMax < 0 && tempo <= 0.0f && recoverySeconds <= 0.0f && !holdPosition;
    }
    [[nodiscard]] bool Allows(BossMove move) const
    {
        return (allow & static_cast<unsigned>(move)) != 0u;
    }
};

} // namespace sandbox
