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

/// 予兆の «時刻» の作り方。床のデカール・部位発光・レーザーの溜めが同じ 1 つを通る。
///
/// WHY 明滅を各自に持たせないか (2026-09-07):
///   3 か所とも `sin(Time::time * hz)` で明滅していた。**絶対時刻で回すので、予兆が
///   出た瞬間の位相が毎回違う** ─ 同じ手でも「何回光ったら来る」が成立せず、
///   明滅は «そろそろ» としか言えていなかった。避ける判断に要るのは «そろそろ» では
///   なく «今» で、それは進みに位相を揃えた離散的な合図でしか出せない。
///
/// WHY 拍 (pips) にするか:
///   進みは満ちる面と枠の明るさで既に連続量として出ている。人は連続量から着弾時刻を
///   当てるのが苦手なので、等間隔の «拍» を数えさせる。3 拍なら «3・2・1» で、
///   足元が本体に隠れていても耳と周辺視でタイミングが取れる。
///
/// WHY 回避窓を別に持つか:
///   拍は «あと何回» を言うが «今» は言わない。最後の窓だけ色と太さを切り替えれば、
///   量ではなく質の変化になって «この瞬間» が読める。
struct BossTelegraphCue {
    // ── 設定 ──
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

    // ── 出力 (Tick が書く) ──
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
            // 出ていた予兆が消えた ＝ 着弾した。少しだけ残して «今のが着弾» を見せる。
            if (m_wasAlive) {
                m_wasAlive = false;
                m_burst    = burstSeconds > 0.0f ? burstSeconds : 0.0f;
            }
            m_burst   = m_burst > step ? m_burst - step : 0.0f;
            burstFade = burstSeconds > 0.0f ? m_burst / burstSeconds : 0.0f;
            // 弾けている間は素の明るさで。ここで拍を跳ねさせると着弾が 2 回に見える。
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

        // 拍。進みを pips 等分し、境目を跨いだフレームで 1 度だけ跳ねる。
        //
        // WHY 進みで数えるか: 予兆の尺は手ごとに違い、段でも詰まる (Telegraph Cut)。
        //     秒で刻むと手によって拍の数が変わり、«3・2・1» が «4・3・2・1» になる。
        //     進みで割れば、尺がいくつでも拍の数は必ず同じになる。
        if (pips > 0) {
            const int beat = static_cast<int>(p * static_cast<float>(pips));
            if (beat > m_beat) {
                m_beat  = beat;
                m_spike = pipDecay > 0.0f ? pipDecay : 0.0f;
            }
        }
        m_spike = m_spike > step ? m_spike - step : 0.0f;
        const float spike01 = pipDecay > 0.0f ? m_spike / pipDecay : 0.0f;
        // 跳ねは «鋭く落ちる»。線形だと明滅ではなく «脈» に見える。
        pulse = 1.0f + (pipGain - 1.0f) * spike01 * spike01;

        const float from = strikeFrom < 0.0f ? 0.0f : (strikeFrom > 1.0f ? 1.0f : strikeFrom);
        strike = p <= from ? 0.0f : (p - from) / (1.0f - from > 1.0e-3f ? 1.0f - from : 1.0e-3f);

        // 回避窓では模様を止める。動いているものが止まるのは «構え終わった» の合図で、
        // 明るさの変化より視界の端でも拾いやすい。
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
    /// 帯が «始点から終点へ走る» 手か。Circle では使わない。
    ///
    /// WHY 要るか: 帯の満ちは中心線から横へ広がる作りだったので、薙ぎ・噛みつき・
    ///     走りのように «帯に沿って» 来る手でも «太っていく» 絵しか出せなかった。
    ///     叩きつけと檻は一斉に来るので、そちらは太るのが正しい ─ 手の側しか
    ///     どちらか知らないので、出す側が言う。
    bool travels = false;
};

} // namespace sandbox
