/// @file    PolarityCore.hlsl
/// @brief   撃破コアの本体。1 粒を «そこに残っている帯電した塊» として手続きで描く
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// PSO: SOLID_NOCULL + ADDITIVE + DEPTH_READ
///
/// WHY 専用の絵が要るか:
///   撃破コアは «倒した敵がその場に残す的» で、数秒のあいだ盤面に居続ける
///   (Docs/polarity-system.md「撃破コア」)。極が乗った «瞬間» のフラッシュ
///   (FX_POL_Charge.vfx) を間借りしていた間は、打ち直すたびに «また誰かが帯電した»
///   と読めてしまい、そこに的が «在る» という状態が絵から出なかった。
///
/// WHY 芯・輪・記号の 3 層で描くか:
///   的として使うには 2 つが読めればよい ─ どこに在るか (芯) と、どちらの極か (記号)。
///   輪はその 2 つを繋ぐためだけに在る。芯だけだと «光の点» で、記号だけだと
///   «宙に浮いた文字» になる。輪が «塊» の輪郭を与えて、初めて物として見える。
///
/// WHY 記号を寿命で薄めるか (signFade):
///   コアは 1 粒ではなく «同じ場所に重なった数粒» で出来ている (KillCoreComponent が
///   点発生を流し続ける)。全部の粒が同じ濃さで記号を描くと、記号だけが飽和して
///   白い板になる。若い粒だけが記号を持ち、古い粒は広がる暈になる、と分ければ、
///   重なりがそのまま «脈打つ塊» になる。
///
/// WHY ＋と−で .mat を分けるか:
///   ParticlePass は材質を materialPath をキーにグローバルへ 1 つだけ持つ。
///   GameObject ごとの paramOverrides はパーティクルには届かないので、同じ .mat を
///   差した 2 つのコアは必ず同じ絵になる (ElectricChargeCommon.hlsli と同じ制約)。
///
/// WHY 形の和をピーク 1 で止めるか:
///   加算合成は色の比率を保つが、トーンマップは全チャンネルが 1 を超えた時点で
///   比率を潰して白にする。赤と青が同じ白へ抜けると、極が読めなくなる。
///   形は saturate で頭を打ち、明るさの単位は Glow (gEmissiveScale) 1 本に持たせる。
#include "Material/Effects/ParticleMaterial.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 粒の色に掛ける色味。極の色はここが正本 (.mat を極ごとに分けている)。
    float4 coreColor;

    /// 描く符号。正で ＋、負で −、0 で記号なし。
    float  chargeSign;
    /// 芯の半径。クワッドの半径を 1 とした比。
    float  coreSize;
    /// 芯の外へ広がる暈の締まり。大きいほど芯の際で急に落ちる。
    float  haloFalloff;
    /// 塊の輪郭になる輪の半径。
    float  ringRadius;

    /// 輪の太さ。
    float  ringWidth;
    /// 記号の腕の長さの半分。
    float  signScale;
    /// 記号の腕の太さの半分。
    float  signWeight;
    /// 記号が寿命で薄まる速さ。大きいほど «若い粒だけが記号を持つ»。
    float  signFade;
};

/// 角の丸い矩形の符号付き距離。両端を丸めた «棒» を 1 本描く。
///
/// NOTE: 引数を point という名前にしないこと。HLSL のモディファイア語なので
///       DXC が «modifiers must appear before type» で弾く
///       (ElectricChargeCommon.hlsli に同じ注意がある)。
float CoreBar(float2 local, float2 halfSize, float radius)
{
    const float2 edge = abs(local) - halfSize + radius;
    return length(max(edge, 0.0f)) + min(max(edge.x, edge.y), 0.0f) - radius;
}

float4 PSMain(ParticlePSIn p) : SV_Target0
{
    // クワッド中心を原点、縁を半径 1 とした座標。
    const float2 d = p.localUv * 2.0f - 1.0f;
    const float  r = length(d);

    // 寿命の残り。KillCoreComponent は色のグラデーションで «若い / 古い» を作る。
    // WHY alpha を使うか: 粒が持っている値のうち、経過そのものを表しているのはこれだけ。
    const float life = saturate(p.color.a);

    // ── 芯 ────────────────────────────────────────────────────────────────
    const float core = 1.0f - smoothstep(max(coreSize, 0.01f) * 0.6f,
                                         max(coreSize, 0.01f), r);
    // 芯の «外» だけへ広がる暈。芯を潰さずに発光させる。
    const float halo = exp(-max(r - coreSize, 0.0f) * max(haloFalloff, 0.01f));

    // ── 輪 ────────────────────────────────────────────────────────────────
    // WHY 画面微分で縁を決めるか: コアは近づくほど画面上で大きくなる。固定の
    //     ぼけ幅だと、寄ったときに輪だけが甘くなって «塊» の輪郭が消える。
    const float aa    = fwidth(r) * 1.2f + 1.0e-4f;
    const float width = max(ringWidth * 0.5f, aa);
    const float ring  = 1.0f - smoothstep(width, width + aa * 2.0f,
                                          abs(r - saturate(ringRadius)));

    // ── 記号 ──────────────────────────────────────────────────────────────
    float glyph = 0.0f;
    if (abs(chargeSign) > 0.5f)
    {
        const float weight = max(signWeight, 0.02f);
        const float arm    = max(signScale, weight);

        // 横棒。短辺と丸め半径を揃えてカプセルにする。
        float sd = CoreBar(d, float2(arm, weight), weight);
        if (chargeSign > 0.5f)
        {
            // ＋ は縦棒を重ねる。和ではなく «近いほう» を採るので、交差部が
            // 二重に明るくならず 1 枚の板として繋がる。
            sd = min(sd, CoreBar(d.yx, float2(arm, weight), weight));
        }
        const float glyphAa = fwidth(sd) * 1.2f + 1.0e-4f;
        glyph = 1.0f - smoothstep(-glyphAa, glyphAa, sd);
        // 若い粒だけが記号を持つ。重なりで記号が飽和して白い板になるのを防ぐ。
        glyph *= pow(life, max(signFade, 0.0f));
    }

    // クワッドの縁で畳む。畳まないと丸い切り口が四角い板の角で切れる。
    const float edgeFade = 1.0f - smoothstep(0.74f, 1.0f, r);

    // 芯 > 記号 > 輪 > 暈 の順に強い。和ではなく saturate で頭を打つ。
    const float shape = saturate(core * 0.95f + glyph * 0.55f
                               + ring * 0.45f + halo * 0.28f) * edgeFade;

    // 色相は粒の色のまま。白を混ぜないので、重なっても赤青の比率が崩れにくい。
    // 出力は非事前乗算。ADDITIVE の src.a はブレンド側が掛ける
    // (方程式は RenderState.hpp の BlendMode が正本)。
    const float3 rgb   = p.color.rgb * coreColor.rgb * max(gEmissiveScale, 0.0f);
    const float  alpha = shape * p.color.a * coreColor.a;

    clip(alpha - 0.003f);
    return float4(rgb, alpha);
}
