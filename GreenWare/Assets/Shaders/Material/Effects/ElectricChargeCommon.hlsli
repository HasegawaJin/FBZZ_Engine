/// @file    ElectricChargeCommon.hlsli
/// @brief   電荷パーティクルの絵。1 粒を ＋ / − の記号そのものとして手続きで描く。
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// 差すのはこのファイルではなく、シミュレーション経路に合わせた薄いラッパー:
///   ElectricCharge.hlsl     … CPU シミュレーション用 (頂点バッファから展開)
///   ElectricChargeGPU.hlsl  … GPU シミュレーション用 (StructuredBuffer から展開)
///
/// WHY 絵をヘッダーへ出すか:
///   経路で違うのは VS だけで、PS は 1 文字も変わらない。それでもファイルを 2 本に
///   分けて書き写すと、記号の太さや白飛びの手当てを片方だけ直したときに、
///   同じ ＋ が simulationMode を変えただけで違う見た目になる。
///   «違うのは VS だけ» を構造で保証する。
///
/// WHY 1 粒を記号にするか:
///   電荷の «雲» は遠目には綺麗な靄でしかなく、何が飛んでいるのか情報が無い。
///   1 粒 1 粒が読める ＋ / − なら、雲そのものが «電荷の集まり» という説明になる。
///   ＋極の雲と−極の雲が混ざっても、どちらの粒がどちらから来たのかが形で分かる。
///
/// WHY 符号を .mat で決めるか (材質を極ごとに分ける理由):
///   ParticlePass は材質を materialPath をキーにグローバルに 1 つだけ持つ
///   (ParticlePass.cpp の g_particleMaterials)。GameObject ごとの paramOverrides は
///   パーティクルには届かないので、同じ .mat を差した 2 つの電極は必ず同じ絵になる。
///   ＋と−は «同じ素材の違う設定» ではなく «違う素材» として持つのが唯一の道。
///
/// WHY テクスチャで用意しないか:
///   記号は近づくほど鋭くあってほしい。SDF なら粒がどれだけ大きくなっても
///   縁が滲まず、画面上の大きさに合わせて fwidth でアンチエイリアスも決まる。
///
/// WHY 形の和をピーク 1 に正規化するか:
///   加算合成は色の比率を保つが、トーンマップは全チャンネルが 1 を超えた時点で
///   比率を潰して白にする。1 粒の «倍率» が 1 を大きく超えると、重なった所から
///   赤も青も同じ白へ抜ける。形はピーク 1 で止め、明るさの単位は
///   Glow (gEmissiveScale) 1 本に持たせる。
#ifndef FBZZ_ELECTRIC_CHARGE_COMMON_HLSLI
#define FBZZ_ELECTRIC_CHARGE_COMMON_HLSLI

// VS・定数バッファ・ビルボード展開はここが供給する。GPU 経路のラッパーは
// これより前に FBZZ_PARTICLE_GPU を定義済みで、VS だけが差し替わる。
#include "Material/Effects/ParticleMaterial.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 粒の色に掛ける色味。粒ごとの色は寿命グラデーションが持つので、通常は白のまま。
    float4 albedo;

    /// 描く符号。正で ＋ (棒 2 本)、負で − (棒 1 本)、0 で記号なし (丸い芯だけ)。
    /// 0 は PolarityTypes.hpp の Polarity::None (無極) に対応する。
    float  chargeSign;
    /// 棒の長さの半分。クワッドの半径を 1 とした比。
    float  signScale;
    /// 棒の太さの半分。同じくクワッドの半径を 1 とした比。
    /// WHY 細くしすぎないか: 粒は画面上で数十 px しかない。1〜2 px の棒は
    ///     アンチエイリアスで灰色に溶け、記号ではなく «ぼやけた点» になる。
    float  signWeight;
    /// 記号の外へにじむ光の締まり。大きいほど記号の縁で急に落ちる。
    float  signGlow;
};

/// 角の丸い矩形の符号付き距離。両端を丸めた «棒» を 1 本描くのに使う。
/// halfSize の短辺と radius を揃えるとカプセル (完全な丸端) になる。
///
/// NOTE: 引数を point へ戻さないこと。point は HLSL のモディファイア語
///       (point / line / triangle / lineadj / triangleadj / sample / linear / centroid …)
///       で、識別子には使えない。DXC は «modifiers must appear before type» で弾く。
///       型より前に来るはずの語が型の後に現れたと解釈されるため、エラーは
///       その行だけでなく次の使用箇所へも連鎖して出る。
float ChargeBar(float2 local, float2 halfSize, float radius)
{
    const float2 edge = abs(local) - halfSize + radius;
    return length(max(edge, 0.0f)) + min(max(edge.x, edge.y), 0.0f) - radius;
}

float4 PSMain(ParticlePSIn p) : SV_Target0
{
    // クワッド中心を原点、縁を半径 1 とした座標で形を作る。
    const float2 d = p.localUv * 2.0f - 1.0f;
    const float  r = length(d);

    // WHY ここで r >= 1 を discard しないか:
    //   下で縁の柔らかさを fwidth (画面微分) から取る。discard したレーンの
    //   その後の微分は仕様上未定義なので、クワッドの角を先に落とすと縁 1 列の
    //   アンチエイリアス幅が壊れうる。角は edgeFade と末尾の clip で畳む。
    const float weight = max(signWeight, 0.02f);
    const float arm    = max(signScale, weight);

    // 横棒。短辺と丸め半径を揃えてカプセルにする。
    float sd = ChargeBar(d, float2(arm, weight), weight);
    if (chargeSign > 0.5f) {
        // ＋ は縦棒を重ねる。2 本の和ではなく «近いほう» を採るので、
        // 交差部が二重に明るくならず 1 枚の板として繋がる。
        sd = min(sd, ChargeBar(d.yx, float2(arm, weight), weight));
    } else if (chargeSign > -0.5f) {
        // 無極。符号を持たないので芯だけの丸に落とす。
        sd = r - weight * 1.6f;
    }

    // 画面上の大きさから縁の柔らかさを決める。粒が小さいときだけ自動的に甘くなり、
    // 大きく寄っても縁が滲まない。
    const float aa    = fwidth(sd) * 1.2f + 1.0e-4f;
    const float glyph = 1.0f - smoothstep(-aa, aa, sd);
    // 記号の «外側» だけへ広がる光。形を保ったまま発光させる。
    const float halo  = exp(-max(sd, 0.0f) * max(signGlow, 0.01f));

    // クワッドの縁で halo を畳む。畳まないと丸い切り口が四角い板の角で切れる。
    const float edgeFade = 1.0f - smoothstep(0.72f, 1.0f, r);

    // 係数の和は内側 (glyph = halo = 1) で 1 をわずかに超えるだけ。saturate で頭を打つ。
    const float shape = saturate(glyph * 0.82f + halo * 0.30f) * edgeFade;

    // 色相は粒の色のまま。白を混ぜないので、重なっても比率が崩れにくい。
    // 出力は非事前乗算。ADDITIVE の src.a はブレンド側が掛ける
    // (方程式は RenderState.hpp の BlendMode を参照)。
    const float3 rgb = p.color.rgb * albedo.rgb * max(gEmissiveScale, 0.0f);
    // アルファは «形 × 寿命フェード»。ブレンドを通って RGB の重みになる。
    const float alpha = shape * p.color.a * albedo.a;

    clip(alpha - 0.003f);
    return float4(rgb, alpha);
}

#endif // FBZZ_ELECTRIC_CHARGE_COMMON_HLSLI
