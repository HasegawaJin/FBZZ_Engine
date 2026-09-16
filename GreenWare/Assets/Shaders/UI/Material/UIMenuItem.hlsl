// FBZZ Engine
// Material/UIMenuItem.hlsl | UI
// メニュー項目の選択インジケーター。1 枚の素材から未選択と選択を作り分ける。
//
// 前提となる素材の作り (Assets/UI/Title/Menu_Bar_red.png):
//   芯   … α = 1 の硬い縁を持つ 4x26px の縦棒
//   裾   … 芯の外側へ広がる α <= 0.18 の発光 (ガウス状、σ ≒ 7px)
//   両者のあいだに中間の α が無いため、0.5 でしきい値を取れば芯と裾を分離できる。
//
// WHY テクスチャを差し替えず 1 枚で済ませるか:
//   素材は dim / red / blue の 3 枚あり、芯の形はどれも同じで色と発光の有無だけが違う。
//   差し替えで状態を作ると 0 か 1 しか表現できず、選択が「パッと切り替わる」だけになる。
//   芯の色と裾の強さを別々に動かせるようにすると、カーソル移動に補間が効く。
//
// WHY 未選択で裾を消すか:
//   Reference の未選択バー (Menu_Bar_dim.png) は発光を持たない実線 1 本で、
//   選択との差を「光っているかどうか」で読ませている。裾を残すと差が出ない。
#include "UI/UICommon.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 dimColor;     // 未選択の芯の色                     offset  0
    float4 activeColor;  // 選択の芯の色                       offset 16
    float  selected;     // 0 = 未選択, 1 = 選択 (要素ごとに上書き)  offset 32
    float  glowGain;     // 裾の倍率。1 で素材のまま           offset 36
    float  _pad0;        //                                    offset 40
    float  _pad1;        //                                    offset 44
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    const float a = g_Texture.Sample(g_Sampler, input.uv).a;

    // 芯。素材の縁は硬いので、傾斜を画面 1 ピクセル幅へ正規化してから切る。
    // 固定幅の smoothstep にすると、拡大したときに縁が階段状になる。
    const float width = max(fwidth(a), 1e-4f);
    const float core  = saturate((a - 0.5f) / width + 0.5f);
    // 裾。芯として取ったぶんを引いて二重計上を避ける。
    const float halo  = a * (1.0f - core);

    const float  t     = saturate(selected);
    const float3 rgb   = lerp(dimColor.rgb, activeColor.rgb, t);
    const float  alpha = core * lerp(dimColor.a, activeColor.a, t)
                       + halo * glowGain * t;

    float4 result = float4(rgb, alpha) * UITint(input);
    clip(result.a - 0.002f);
    return result;
}
