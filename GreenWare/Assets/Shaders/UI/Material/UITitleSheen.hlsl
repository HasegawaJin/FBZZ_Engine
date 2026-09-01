// FBZZ Engine
// Material/UITitleSheen.hlsl | UI
// ロゴ用。素材の形はそのままに、± の染め分けと «艶» の走りだけを足す。
//
// WHY 素材を描き直さないか:
//   ロゴの字面は決まっていて、直したいのは «質感» の方。PNG に艶を焼き込むと
//   1 枚の絵として固定され、動かないぶん «印刷された紙» に見える。形は素材、
//   光り方はマテリアル、と分ければロゴを差し替えても質感は残る。
//
// WHY 左右で色を分けるか:
//   この作品は ＋ と − の対で出来ている。ロゴの左端をわずかに暖色、右端を
//   わずかに寒色へ寄せると、題字そのものが «2 極のあいだに張られたもの» になる。
//   はっきり塗り分けると «2 色のロゴ» になってしまうので、寄せるのは数 % だけ。
//
// WHY 艶を «帯» で走らせるか:
//   金属やガラスの題字が生きて見えるのは、光源との角度で 1 本のハイライトが
//   舐めるように動くから。全体を明滅させると «点滅する看板» になる。
//   一定周期で 1 回だけ横切る帯なら、動いていない時間の方が長く、
//   «ときどき光を拾う» という静かな見え方になる。
//
// WHY 芯と裾を分けるか (UIMenuItem と同じ判断):
//   ロゴ素材は縁の外へ薄い α を持つ。芯にだけ艶を乗せ、裾は色を寄せるだけに
//   すると、光が字の «面» を走って見える。裾まで一緒に光らせると、
//   字の周りの空気が光る = 白飛びの原因になる。
#include "UI/UICommon.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 tintPlus;      // 左端へ寄せる色                       offset  0
    float4 tintMinus;     // 右端へ寄せる色                       offset 16
    float4 sheenColor;    // 艶の色 (a = 倍率)                    offset 32
    float  tintStrength;  // 染め分けの強さ。0 で素材のまま       offset 48
    float  sheenWidth;    // 艶の帯の幅 [0,1]                     offset 52
    float  sheenTilt;     // 帯の傾き。0 で垂直、1 で 45 度        offset 56
    float  sheenPhase;    // 帯の位置 [0,1]。script が進める       offset 60
    float  edgeGlow;      // 裾の持ち上げ。0 で素材のまま         offset 64
    float  _pad0;         //                                      offset 68
    float  _pad1;         //                                      offset 72
    float  _pad2;         //                                      offset 76
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    const float4 texel = g_Texture.Sample(g_Sampler, input.uv);

    // 芯と裾。素材の縁は硬いので、傾斜を画面 1 ピクセル幅へ正規化してから切る
    // (固定幅の smoothstep だと、拡大したときに縁が階段状になる)。
    //
    // NOTE: fwidth はクワッド内の隣接ピクセルを読む。先に discard したレーンが
    //       混ざると結果が未定義になるので、微分は棄却より前で採りきる
    //       (DecalCommon.hlsli の DecalResolve が同じ理由で discard しない)。
    const float width = max(fwidth(texel.a), 1.0e-4f);
    const float core  = saturate((texel.a - 0.5f) / width + 0.5f);
    const float halo  = texel.a * (1.0f - core);

    // 左右の染め分け。中央は素材の色のままにしたいので、両端へ向かってだけ寄せる。
    const float lateral = input.localUv.x;                 // 0 = 左, 1 = 右
    const float toPlus  = saturate(1.0f - lateral * 2.0f); // 左半分だけ立つ
    const float toMinus = saturate(lateral * 2.0f - 1.0f); // 右半分だけ立つ
    float3 rgb = texel.rgb;
    rgb = lerp(rgb, rgb * tintPlus.rgb,  toPlus  * tintStrength * tintPlus.a);
    rgb = lerp(rgb, rgb * tintMinus.rgb, toMinus * tintStrength * tintMinus.a);

    // 艶。帯は左から右へ 1 本だけ走る。傾けるのは、水平の帯だと «画面の走査» に
    // 見えて、題字の面を舐めた光に見えないため。
    const float2 uv   = input.localUv;
    const float  band = uv.x + (uv.y - 0.5f) * sheenTilt;
    // 位相は 0..1 で一周する。帯の幅ぶん外側から入って外側へ抜けるよう、
    // 進む範囲を [-width, 1+width] へ広げる (端で «湧いて消える» のを避ける)。
    const float  head = lerp(-sheenWidth, 1.0f + sheenWidth, frac(sheenPhase));
    const float  w    = max(sheenWidth, 1.0e-3f);
    const float  d    = saturate(1.0f - abs(band - head) / w);
    // 3 乗で «芯だけ強く» する。線形だと帯の裾まで明るく、幅の広い光の壁になる。
    const float  sheen = d * d * d * core;

    rgb += sheenColor.rgb * sheen * sheenColor.a;

    // 裾は色を寄せるだけ。持ち上げるのは «素材が既に持っている» ぶんの範囲に留める。
    //
    // NOTE: core と halo は texel.a から作ってあるので、ここで texel.a を
    //       もう一度掛けない (掛けると縁が二乗ぶん痩せて、字が細る)。
    const float alpha = core + halo * (1.0f + edgeGlow);

    float4 result = float4(rgb, alpha) * UITint(input);
    clip(result.a - 0.002f);
    return result;
}
