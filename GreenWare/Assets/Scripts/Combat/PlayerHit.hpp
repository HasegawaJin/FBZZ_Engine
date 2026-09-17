/// @file    PlayerHit.hpp
/// @brief   プレイヤーへの一撃の «種類» と «結末»。攻撃する側と受ける側の取り決め
/// @author  Hasegawa Jin
/// @date    2026-09-04
///
/// @note 弾ける/弾けないの 2 値のみ渡す。攻撃ごとの名前を渡すと受け手が攻撃表を持つことになる。
/// @note 結末は bool でなく enum で返す。«無敵で通らなかった» と «弾き返された» を区別するため。
#pragma once

namespace sandbox {

/// この一撃は刀で弾けるか。
enum class PlayerHitKind : int {
    /// 弾けない。跳ぶ・走る・回避で応える (衝撃波の輪・ビーム・パルス・柱)。
    Unblockable = 0,
    /// 弾ける。予兆の直後に受ければ無効化して崩しが溜まる (踏みつけ・突進・噛みつき・槍)。
    Parryable,
};

/// 一撃がどう終わったか。
enum class PlayerHitResult : int {
    /// 何も起きなかった (無敵時間・死亡済み・量が 0)。
    Ignored = 0,
    /// 通った。HP が減った。
    Damaged,
    /// 回避中で当たらなかった (ジャスト回避)。
    Dodged,
    /// 刀で弾いた。
    Parried,
    /// 構えたまま受け止めた (通常ガード)。
    ///
    /// @note 弾きと見返りが正反対。弾きは崩しゲージを溜めるが、ガードは 1 も溜めない。
    Guarded,
};

} // namespace sandbox
