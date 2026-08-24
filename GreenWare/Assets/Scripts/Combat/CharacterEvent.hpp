/// @file CharacterEvent.hpp
/// @brief 戦闘のあいだに 1 体のキャラクターの身へ起きたこと
/// @author Hasegawa Jin
/// @date 2026-08-24
///
/// WHY 出来事の側に名前を付けるか:
///   「見つけた」「攻撃を出した」「殴られた」「倒れた」は、伝える先 (表情・アニメーション・
///   HUD・ボイス) が増えても中身が変わらない。受け取る側の都合で名前を付けると、反応を
///   1 つ足すたびに語彙が増え、同じ出来事が別名で 2 回流れることになる。
///
/// WHY 独立したヘッダーにするか:
///   配るのは CombatManager、受けるのは EyeSprite で、依存は「配る → 受ける」の
///   片方向でなければならない。どちらかの中へ enum を置くと include が輪になる。
#pragma once

namespace sandbox {

enum class CharacterEvent {
    /// 相手を見つけた。気づいた側の出来事で、見つけられた側には届かない。
    Spotted,
    /// 攻撃を出した。当たったかどうかは問わない。
    Attack,
    /// ダメージを受けた。この一撃で倒れた場合は代わりに Defeated が届く。
    Hurt,
    /// 倒れた。
    Defeated,
    /// 戦闘前の状態へ戻った (見失った / 落ち着いた)。倒れた後には効かない。
    Recovered,
};

} // namespace sandbox
