/// @file    BossTelegraph.hpp
/// @brief   ボスの攻撃が «どこへ来るか» の形。出す側と描く側の取り決め
/// @author  Hasegawa Jin
/// @date    2026-08-30
///
/// WHY 攻撃ごとに別の予兆を作らないか:
///   踏みつけも突進もビームも、プレイヤーが読みたいのは «自分が今立っている場所が
///   危ないか» の 1 点しかない。攻撃の数だけ違う絵を出すと、覚えることが増えるわりに
///   判断は同じで、しかも 4 種類の «濃さ» を揃える作業が毎回発生する。
///   形を «円» と «線» の 2 つへ畳めば、読み方は 1 つで済む。
///
/// WHY 進み (progress) を渡すか:
///   予兆は «来る» ではなく «あと何秒で来る» を言うためにある。出しっぱなしの図形は
///   位置しか伝えず、避ける動作の開始時刻をプレイヤーが決められない。
///
/// WHY 出す側 (AI) が形を決めるか:
///   着弾点も向きも射程も、攻撃の進行そのものが持っている値。描く側が同じ計算を
///   持つと、AI の数値を触るたびに «予兆だけ古い場所に出る» が起きる。
#pragma once

#include <Math/Vector3.hpp>

namespace sandbox {

/// 地面に落とす予兆の形。
enum class BossTelegraphShape : int {
    None = 0,
    /// 着弾点を中心にした円。踏みつけ・着地・パルス。
    Circle,
    /// 始点から向きへ伸びる帯。突進・ビーム。
    Line,
};

/// 危険が «どこから» 来るか。形と直交する軸。
///
/// WHY 形だけでは足りないか:
///   踏みつけと磁力パルスはどちらも円で、床に出る絵が同じになる。しかし避け方は
///   «真下から出る» と «外へ逃げる» で正反対。円のまま区別を付けるには、
///   形ではなく «向き» を別の軸として持つしかない。
///   色を増やす手もあるが、赤青は極性・琥珀は危険で既に埋まっている (12.2)。
enum class BossThreatOrigin : int {
    /// 地を這って外へ広がる。パルス・突き上げ。リングは外向きに動く。
    Ground = 0,
    /// 上から降ってくる。踏みつけ・着地・叩きつけ。リングは内向きに縮む。
    Above,
};

/// 予兆が指している攻撃。部位発光がどこを光らせるかを決める。
///
/// WHY 形と別に持つか:
///   床のデカールは «どこ» しか言えず、しかもボス 1 は 10m で画面高さの 71% を
///   占めるので、接近するほど足元の絵が本体で隠れる。«何が来るか» は
///   «見えているもの» = ボスの体で言う必要がある。その宛先を決めるのがこれ。
enum class BossAttackKind : int {
    None = 0,
    /// 踏みつけ。該当する脚が光る。
    Stomp,
    /// 跳躍からの着地。四脚すべてが光る。
    Slam,
    /// 突進。前脚と頭部が光る。
    Charge,
    /// コアビーム。コアが光る。
    Beam,
    /// 磁力パルス。極性リング全周が光る。
    Pulse,
    /// 床から噴き上がる (蛇の突き上げ・電磁の柱)。
    Erupt,
    /// 水平に薙ぐ (蛇の薙ぎ)。
    Sweep,
};

/// 攻撃の種類から «どこから来るか» を引く。
///
/// WHY 出す側に書かせないか: 踏みつけが «上から» なのは攻撃の性質であって
///     AI の判断ではない。出す側に書かせると、ボス 1 と蛇で同じ攻撃に
///     違う向きが付く事故が起きる。
[[nodiscard]] inline BossThreatOrigin BossThreatOriginOf(BossAttackKind kind)
{
    switch (kind) {
    case BossAttackKind::Stomp:
    case BossAttackKind::Slam:
    case BossAttackKind::Sweep:
        return BossThreatOrigin::Above;
    default:
        return BossThreatOrigin::Ground;
    }
}

/// 今フレーム出すべき予兆。shape が None なら何も出さない。
struct BossTelegraph {
    BossTelegraphShape shape = BossTelegraphShape::None;
    /// どの攻撃か。部位発光の宛先と、リングの動く向きがここから決まる。
    BossAttackKind kind = BossAttackKind::None;
    /// 円の中心 / 帯の始点。いずれもワールド座標。
    fbzz::math::Vector3 origin{};
    /// 帯の向き (水平・正規化済み)。Circle では使わない。
    fbzz::math::Vector3 direction{ 0.0f, 0.0f, 1.0f };
    /// 帯の長さ [m]。Circle では使わない。
    float length = 0.0f;
    /// 円の半径 / 帯の半幅 [m]。
    float radius = 0.0f;
    /// 予兆の進み [0,1]。1 で着弾。
    float progress = 0.0f;
};

} // namespace sandbox
