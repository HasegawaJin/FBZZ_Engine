// FBZZ Engine
// Material/UITrackFill.hlsl | UI
// スライダーの «溜まっている» 側。平らな帯に断面の照りと極の色を与える。
//
// WHY 溝と溜まりを色だけで分けないか:
//   Track / Track_fill は同じ形の帯で、明るさだけが違う。それだけだと «少し明るい
//   ところまで» にしか見えず、量が «溜まっている» 感じにならない。断面に照りを
//   入れると、溝に何かが充填されているように読める ─ この作品は電荷の話なので、
//   «満ちている» の絵が要る。
//
// WHY 上下で非対称にするか:
//   光は上から来る。帯の上寄りに照りを置き、下を沈めると、断面が円柱に見える。
//   中央対称にすると «光る棒» になって、溝に入っている感じが消える。
//
// WHY 進行方向にも勾配を持たせるか:
//   UIImage の塗り (SetImageFillAmount) は矩形を切るので、切り口が «今どこまで
//   溜まっているか» になる。切り口へ向かって明るくしておくと、量の先端が
//   自分で目立つ ─ Knob を見なくても «どこまで» が読める。
#include "UI/UICommon.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 fillColor;    // 帯の色 (a = 全体の不透明度)         offset  0
    float4 sheenColor;   // 断面の照り (a = 量)                 offset 16
    float  sheenBias;    // 照りの位置 [0,1]。0 が上端           offset 32
    float  sheenWidth;   // 照りの太さ [0,1]                    offset 36
    float  headGain;     // 切り口へ向かう明るさの増分          offset 40
    float  floorDim;     // 下端を沈める量                      offset 44
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    const float4 texel = g_Texture.Sample(g_Sampler, input.uv);

    const float2 uv = input.localUv;

    // 断面の照り。上寄り (sheenBias) に山を置く。
    const float d     = abs(uv.y - saturate(sheenBias));
    const float sheen = 1.0f - smoothstep(0.0f, max(sheenWidth, 1.0e-3f), d);

    // 下端の沈み。照りと足し引きすることで «丸い断面» になる。
    const float floorMask = smoothstep(0.55f, 1.0f, uv.y);

    // 進行方向。矩形が切られている前提で «右端 = 切り口» とする。
    const float head = uv.x;

    float3 rgb = fillColor.rgb;
    rgb += sheenColor.rgb * sheen * sheenColor.a;
    rgb *= 1.0f - floorDim * floorMask;
    rgb *= 1.0f + headGain * head * head;

    float4 result = float4(rgb, texel.a * fillColor.a) * UITint(input);
    clip(result.a - 0.002f);
    return result;
}
