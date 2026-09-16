// FBZZ Engine
// Material/UIRule.hlsl | UI
// 区切り線。素材の 1 本線に «端の抜け» と «芯の締まり» を足す。
//
// WHY 線を素材のまま出さないか:
//   Options には Divider が 24 本、CtrlRow_div が 7 本ある。全部が同じ濃さで
//   端から端まで同じ太さだと、画面が «罫線に区切られた表» に見える。情報の区切りは
//   要るが、罫線そのものは読ませたいものではない。端を抜いて中央だけ残せば、
//   «区切りがある» ことは伝わったまま、線そのものは背景へ下がる。
//
// WHY 端を «UV» で抜くか (素材を作り直さないか):
//   同じ Divider.png が 24 か所で幅を変えて使われている。抜きを焼き込むと、
//   幅の違う場所で抜けの長さが変わってしまう。矩形内 0..1 で抜けば、
//   どの幅でも «両端の 1 割» が同じ割合で抜ける。
//
// WHY 芯を明るくするか:
//   一様な線は «塗り» に見える。中央 1px だけをわずかに持ち上げると、線に
//   «光を反射する面» があるように読めて、彫られた溝や差し込まれた板に近づく。
#include "UI/UICommon.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 lineColor;   // 線の色 (a = 全体の不透明度)        offset  0
    float4 coreColor;   // 芯へ足す色 (a = 足す量)            offset 16
    float  endFade;     // 両端を抜く割合 [0,0.5]             offset 32
    float  coreWidth;   // 芯の太さ [0,1]。矩形の高さに対する比 offset 36
    float  bias;        // 芯の位置 [0,1]。0.5 で中央          offset 40
    float  _pad0;       //                                    offset 44
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    const float4 texel = g_Texture.Sample(g_Sampler, input.uv);

    const float2 uv = input.localUv;

    // 両端の抜け。左右それぞれ endFade ぶんを 0 まで落とす。
    // WHY smoothstep か: 線形に落とすと «先が尖った線» になり、切れているのではなく
    //     «細くなっている» ように見える。滑らかに消せば «溶けて終わる»。
    const float fade = max(endFade, 0.0f);
    const float ends = fade <= 0.0f
        ? 1.0f
        : smoothstep(0.0f, fade, uv.x) * smoothstep(0.0f, fade, 1.0f - uv.x);

    // 芯。矩形の高さ方向で bias の位置に置く。
    const float d    = abs(uv.y - saturate(bias));
    const float core = 1.0f - smoothstep(0.0f, max(coreWidth, 1.0e-3f), d);

    float3 rgb   = lineColor.rgb + coreColor.rgb * core * coreColor.a;
    float  alpha = texel.a * lineColor.a * ends;

    float4 result = float4(rgb, alpha) * UITint(input);
    clip(result.a - 0.002f);
    return result;
}
