// FBZZ Engine
// Material/UITitleBackdrop.hlsl | UI
// タイトルの下地。テクスチャを読まず、極性の 2 色から画面全体の «場» を作る。
//
// WHY 素材を読まないか:
//   Assets/UI/Title/BG_base.png は書き出しの都合でほぼ真っ白のまま入っており、
//   そのまま出すと画面が飛ぶ。シーンでは UIImage の α を 0 にして «無かったこと»
//   にしてあった。1920x1080 の一枚絵を描き直すより、下地に要るもの (奥行きと
//   ± の気配) を手続きで出す方が、色を 1 行変えるだけで作り直せる。
//
// WHY 極を 2 つの隅へ置くか:
//   この作品の芯は ＋ と − の対で、タイトルはそれを最初に見せる場所になる。
//   ロゴは左下、電極は画面の左右にあるので、左下を暖色 (＋)、右上を寒色 (−) で
//   ごく薄く持ち上げると、構図そのものが «2 極のあいだ» になる。
//   グラデーションを 1 つにすると «ただ暗い背景» に戻る。
//
// WHY 走査線を入れるか:
//   一様なグラデーションは «塗り» に見えて、盤面 (磁場グリッド・電極) と同じ
//   «装置の中» の空気にならない。画面ピクセル基準で細い横線を敷くと、下地に
//   解像度が生まれて手前の UI が «その上に置かれている» ように見える。
//
// WHY 周期をピクセルで持つか:
//   UV 基準にすると、同じ .mat を別の大きさの矩形へ当てたときに線の太さが変わる。
//   走査線は «画面の性質» なので、矩形の大きさに依存させない (UICommon の g_Rect)。
#include "UI/UICommon.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 baseColor;        // 下地の色 (a = 全体の不透明度)      offset  0
    float4 plusColor;        // 左下から差す ＋ の色               offset 16
    float4 minusColor;       // 右上から差す − の色                offset 32
    float  glowStrength;     // 2 極の差し込みの強さ               offset 48
    float  glowRadius;       // 差し込みの広さ [0,2]               offset 52
    float  vignette;         // 四隅の落とし込み                   offset 56
    float  scanStrength;     // 走査線の濃さ。0 で線なし           offset 60
    float  scanPeriod;       // 走査線の周期 [px]                  offset 64
    float  grain;            // ざらつき。帯 (バンディング) 消し    offset 68
    float  phase;            // 呼吸と流れの位相 [秒]。script が進める offset 72
    // 画面ごとの覆い具合。1 で下地が全部を隠す。
    //
    // WHY 画面ごとに要るか:
    //   Title と Result は下地の «後ろ» に磁場グリッドと電極が居る。そこを 1 で
    //   塗ると盤面ごと消える。Options と StageSelect は後ろに何も無いので 1 が正しい。
    //   .mat は 1 枚を全画面で共有したい (同じ下地に見せたい) ので、覆い具合だけを
    //   要素ごとの上書きで持つ ─ 色や走査線の設定は共有のまま変えずに済む。
    float  coverage;         //                                    offset 76
    // 隅だけ余分に覆う量。中央は透けたまま、縁だけ締まる。
    float  edgeCoverage;     //                                    offset 80
    float  _pad0;            //                                    offset 84
    float  _pad1;            //                                    offset 88
    float  _pad2;            //                                    offset 92
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

// 2 点の間で滑らかに落ちる差し込み。距離の 2 乗で落とすと «光源» に見えるので、
// smoothstep で «滲み» にする (下地は光っているのではなく、色が寄っている)。
float Bleed(float2 uv, float2 origin, float radius)
{
    const float d = length(uv - origin) / max(radius, 1.0e-3f);
    return 1.0f - smoothstep(0.0f, 1.0f, saturate(d));
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    const float2 uv = input.localUv;

    // 呼吸。2 極が交互にわずかに強くなる ─ 同時に明滅させると «全体が点滅した»
    // になるので、位相を半周ずらして «行き来している» ように見せる。
    const float breath = sin(phase * 0.9f);
    const float plusGain  = 1.0f + 0.18f * breath;
    const float minusGain = 1.0f - 0.18f * breath;

    // 左下 (0,1) が ＋ / 右上 (1,0) が −。localUv は左上が原点。
    const float plus  = Bleed(uv, float2(0.08f, 0.96f), glowRadius) * plusGain;
    const float minus = Bleed(uv, float2(0.94f, 0.06f), glowRadius) * minusGain;

    float3 color = baseColor.rgb
                 + plusColor.rgb  * plus  * glowStrength * plusColor.a
                 + minusColor.rgb * minus * glowStrength * minusColor.a;

    // 走査線。位相でゆっくり流す。止めると «貼ってある模様» に見える。
    const float py   = (uv.y * g_Rect.y) + phase * 6.0f;
    const float scan = 0.5f + 0.5f * cos(py * 6.2831853f / max(scanPeriod, 1.0f));
    color *= 1.0f - scanStrength * scan;

    // 四隅を落とす。中央より隅を暗くしておくと、隅に置いた版権表記やバージョンが
    // «沈んで» 読める側になり、中央のロゴと競合しない。
    const float2 v   = (uv - 0.5f) * 2.0f;
    const float  vig = saturate(dot(v, v) * 0.5f);
    color *= 1.0f - vignette * vig;

    // ざらつき。暗い階調はそのままだと帯が出る。画素位置から作るので、
    // 静止していても «紙の目» のように見える (時間で動かすとノイズが目立つ)。
    const float2 p = input.pos.xy;
    const float  n = frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f) - 0.5f;
    color += n * grain;

    // 覆いは «中央» と «四隅» を別々に持つ。
    //
    // WHY 掛け算にしないか (coverage * (base + vig*edge) にしないか):
    //   中央を透かそうとして coverage を下げると、四隅の額縁まで一緒に薄くなる。
    //   すると «全体をうっすら曇らせる» しか作れず、«中央は盤面が見えていて外周だけ
    //   締まっている» という一番欲しい絵にならない。足し算にすれば独立して決まる。
    const float alpha = saturate(coverage * baseColor.a + vig * edgeCoverage);

    float4 result = float4(color, alpha) * UITint(input);
    clip(result.a - 0.002f);
    return result;
}
