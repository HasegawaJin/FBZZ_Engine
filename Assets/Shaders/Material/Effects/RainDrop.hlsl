/// @file    RainDrop.hlsl
/// @brief   雨粒 1 つを手続きで描く。伸びたビルボードを細い筋に整形する
/// @author  Hasegawa Jin
/// @date    2026-08-25
///
/// ParticleEmitter.renderMode = StretchedBillboard と組で使う。ビルボードは速度方向へ
/// 伸ばされた矩形として渡ってくるので、この PS はその矩形を «筋» に削るだけでよい。
///
/// WHY テクスチャを使わないか:
///   雨は画面いっぱいに何千枚も出る。1 枚あたりが数ピクセルしかない絵にテクスチャを
///   引くと、ミップの選択が粒ごとにばらついて «ちらつく点» になる。手続きなら
///   矩形内の座標から直接不透明度を出せるので、どれだけ小さくなっても筋のまま残る。
///
/// WHY 先端と後端で太さを変えるか:
///   等幅の筋はガラス片に見える。落下する水は前が丸く後ろへ細く引くので、
///   その非対称だけで «水» と読めるようになる。
#include "Material/Effects/ParticleMaterial.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 筋の色。雨は空の色を拾うので、わずかに青へ寄せた白が既定。
    float4 albedo;

    /// 横方向の絞り。大きいほど中心だけが残り細い筋になる。
    float  coreSharpness;

    /// 先端 (進行方向側) の丸みの大きさ [0, 0.5]。
    float  headSize;

    /// 後端へ向かう減衰の強さ。大きいほど早く消えて短く見える。
    float  tailFalloff;

    /// 全体の不透明度倍率。雨量ではなく «水の濃さ» を決める。
    float  opacity;
};

float4 PSMain(ParticlePSIn p) : SV_Target0
{
    // localUv は矩形内 [0,1]。StretchedBillboard では y が速度方向に沿う。
    const float2 uv = p.localUv;

    // 中心線からの距離。1 = 中心、0 = 縁。
    const float across = 1.0f - saturate(abs(uv.x * 2.0f - 1.0f));
    const float core   = pow(across, max(coreSharpness, 1.0f));

    // 先端は丸く立ち上げ、後端は指数的に細らせる。
    const float head = smoothstep(0.0f, max(headSize, 1.0e-3f), uv.y);
    const float tail = exp(-uv.y * max(tailFalloff, 0.0f));

    float alpha = core * head * tail * saturate(opacity);

    // 芯だけわずかに白く抜く。水の «光っている縁» はこれで足りる。
    const float3 color = albedo.rgb + core * core * 0.25f;

    alpha *= albedo.a * p.color.a;
    return float4(color * p.color.rgb, alpha);
}
