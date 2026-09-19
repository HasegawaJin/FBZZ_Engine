/// @file    BossTelegraph.hpp
/// @brief   ボスの攻撃が «どこへ来るか» の形。出す側と描く側の取り決め
/// @author  Hasegawa Jin
/// @date    2026-08-30
///
/// @note 形は円/線の 2 つに畳む。プレイヤーが読みたいのは «今の位置が危ないか» の 1 点だけ。
/// @note progress を渡す。予兆は «あと何秒で来るか» を言うためにあり、避ける開始時刻をプレイヤーが決められる。
/// @note 形は出す側 (AI) が決める。着弾点・向き・射程は攻撃の進行が持つ値で、描く側が二重計算すると AI 変更で予兆だけ古くなる。
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
/// @note 踏みつけと磁力パルスは同じ円形だが避け方が正反対 (真下から出る/外へ逃げる)。
///       色軸は極性・危険で既に埋まっているため、向きを別軸として持つ。
enum class BossThreatOrigin : int {
    /// 地を這って外へ広がる。パルス・突き上げ。リングは外向きに動く。
    Ground = 0,
    /// 上から降ってくる。踏みつけ・着地・叩きつけ。リングは内向きに縮む。
    Above,
};

/// 予兆が指している攻撃。部位発光がどこを光らせるかを決める。
///
/// @note 床のデカールは «どこ» しか言えない。ボス1は10mで画面高さの71%を占め、
///       接近するほど足元が本体に隠れるため、«何が来るか» はボスの体で言う。
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
/// @note 出す側 (AI) には書かせない。踏みつけが «上から» なのは攻撃の性質であり、
///       AI 判断にするとボス毎に同じ攻撃へ違う向きが付く事故が起きる。
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

/// 予兆の «時刻» の作り方。床のデカール・部位発光・レーザーの溜めが同じ 1 つを通る。
///
/// @note 絶対時刻 (`sin(Time::time * hz)`) は出た瞬間の位相が毎回違い «そろそろ» としか
///       言えなかった。進み [0,1] を pips 等分した拍にすると、尺が手ごとに違っても
///       «あと何回» が一定になり、離散的な合図として «今» まで示せる。
/// @note 回避窓は拍と別軸に持つ。拍は回数を、窓は最後だけ色/太さを変えて質的に «今» を示す。
struct BossTelegraphCue {
    /// @name 設定
    /// @{
    /// 拍の数。最後の 1 拍が着弾に重なる。0 で拍なし。
    int   pips = 3;
    /// 拍が来た瞬間の明るさの跳ね。1.0 で跳ねなし。
    float pipGain = 2.1f;
    /// 跳ねが収まるまで [秒]。長いと «光りっぱなし» になって拍が数えられない。
    float pipDecay = 0.10f;
    /// 回避窓の入口 [0,1]。ここから «今» の状態へ入る。
    float strikeFrom = 0.82f;
    /// 斜線の流れ [周/秒]。回避窓では止める ─ 動きが止まると «決まった» が出る。
    float scrollHz = 0.45f;
    /// 着弾の弾けの尺 [秒]。
    float burstSeconds = 0.12f;
    /// @}

    /// @name 出力 (Tick が書く)
    /// @{
    /// 全体の明るさ倍率。拍の跳ねが乗る。
    float pulse = 1.0f;
    /// 回避窓の中での位置 [0,1]。0 = まだ / 1 = 着弾。
    float strike = 0.0f;
    /// 斜線の位相。回避窓に入ると進まなくなる。
    float scroll = 0.0f;
    /// 弾けの残り [0,1]。1 = 通常 / 0 = 弾け切り。
    float burstFade = 1.0f;
    /// 予兆が今フレーム «見えているか»。着弾後の弾けの間も true。
    bool  visible = false;

    /// @param progress 予兆の進み [0,1]
    /// @param dt       秒
    /// @param alive    今フレーム予兆が出ているか。false なら着弾後の弾けへ入る
    void Tick(float progress, float dt, bool alive)
    {
        const float step = dt > 0.0f ? dt : 0.0f;

        if (!alive) {
            /// @note 出ていた予兆が消えた ＝ 着弾した。少しだけ残して «今のが着弾» を見せる。
            if (m_wasAlive) {
                m_wasAlive = false;
                m_burst    = burstSeconds > 0.0f ? burstSeconds : 0.0f;
            }
            m_burst   = m_burst > step ? m_burst - step : 0.0f;
            burstFade = burstSeconds > 0.0f ? m_burst / burstSeconds : 0.0f;
            /// @note 弾けている間は素の明るさで。ここで拍を跳ねさせると着弾が 2 回に見える。
            pulse   = 1.0f;
            strike  = 1.0f;
            visible = burstFade > 0.0f;
            return;
        }

        if (!m_wasAlive) Reset();
        m_wasAlive = true;
        burstFade  = 1.0f;
        visible    = true;

        const float p = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);

        /// @note 拍は進みを pips 等分し、境目を跨いだフレームで 1 度だけ跳ねる。秒基準だと
        ///       手や詰まった段 (Telegraph Cut) ごとに拍数が変わるため、進み基準で数える。
        if (pips > 0) {
            const int beat = static_cast<int>(p * static_cast<float>(pips));
            if (beat > m_beat) {
                m_beat  = beat;
                m_spike = pipDecay > 0.0f ? pipDecay : 0.0f;
            }
        }
        m_spike = m_spike > step ? m_spike - step : 0.0f;
        const float spike01 = pipDecay > 0.0f ? m_spike / pipDecay : 0.0f;
        /// @note 跳ねは «鋭く落ちる»。線形だと明滅ではなく «脈» に見える。
        pulse = 1.0f + (pipGain - 1.0f) * spike01 * spike01;

        const float from = strikeFrom < 0.0f ? 0.0f : (strikeFrom > 1.0f ? 1.0f : strikeFrom);
        strike = p <= from ? 0.0f : (p - from) / (1.0f - from > 1.0e-3f ? 1.0f - from : 1.0e-3f);

        /// @note 回避窓では模様を止める。動いているものが止まるのは «構え終わった» の合図で、
        ///       明るさの変化より視界の端でも拾いやすい。
        if (strike <= 0.0f) m_scroll += step * scrollHz;
        scroll = m_scroll;
    }

    void Reset()
    {
        m_beat     = -1;
        m_spike    = 0.0f;
        m_scroll   = 0.0f;
        m_burst    = 0.0f;
        m_wasAlive = false;
        pulse      = 1.0f;
        strike     = 0.0f;
        scroll     = 0.0f;
        burstFade  = 1.0f;
        visible    = false;
    }
    /// @}

private:
    int   m_beat     = -1;
    float m_spike    = 0.0f;
    float m_scroll   = 0.0f;
    float m_burst    = 0.0f;
    bool  m_wasAlive = false;
};

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
    /// この手の拍数。0 なら受け側 (BossTelegraphComponent の «ピップの数») に任せる。
    ///
    /// @note 受け側の固定値だと溜めの長さに関わらず拍数が同じになり、盤面のテンポが揃わない。
    ///       出す側は溜めの長さと盤面の拍の両方を知るので、そこで割ると拍数がそのまま
    ///       «重い手ほど拍が多い» になる。
    int pips = 0;
    /// 帯が «始点から終点へ走る» 手か。Circle では使わない。
    ///
    /// @note 帯の満ちは既定で中心線から横へ広がるため、薙ぎ等 «沿って» 来る手も
    ///       «太っていく» 絵にしかならない。どちらが正しいかは手の側しか知らないため出す側が言う。
    bool travels = false;
};

} // namespace sandbox
