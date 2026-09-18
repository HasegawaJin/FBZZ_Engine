/// @file    CharacterEvent.hpp
/// @brief   戦闘のあいだに 1 体のキャラクターの身へ起きたこと
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// @note 名前は起きたこと基準 (見た/攻撃/被弾/倒れた)。受け手ごとに付け直すと語彙が二重化する。
/// @note 配るのは CombatManager、受けるのは EyeSprite の片方向依存。逆参照を作ると include が輪になる。
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
