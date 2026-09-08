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
// WHY 磁力線を引くか (2026-09-07):
//   2 色の滲みと走査線だけの下地は «暗いグラデーションの Web ページ» に見えた。
//   この作品の背景は «2 極のあいだに張られた場» なので、＋ から − へ向かう
//   双極子の磁力線をそのまま引く。線の族は 2 極を通る円弧になり、位相で族の
//   中をゆっくり滑らせると «場が流れている» ように見える。模様を貼るのでは
//   なく、極の位置から式で出るので、極を動かせば線も付いてくる。
//
// WHY 塵を漂わせるか:
//   静止した下地に手前の UI だけが動くと、UI が «板の上に貼った紙» に見える。
//   奥で細かい塵がごくゆっくり上向きに漂うだけで、下地に «空気の層» ができて
//   UI との前後が付く。数を増やすと «星空» になってしまうので、視界に数十粒。
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
    float  fieldStrength;    // 磁力線の明るさ。0 で線なし          offset 84
    float  fieldDensity;     // 磁力線の本数 (族の分割数)           offset 88
    float  fieldSpeed;       // 磁力線が族の中を滑る速さ            offset 92
    float  moteStrength;     // 塵の明るさ。0 で塵なし              offset 96
    float  moteScale;        // 塵の格子の大きさ [px]。大きいほど疎  offset 100
    float  hazeStrength;     // 低周波の霞。奥行きのムラ            offset 104
    float  _pad0;            //                                    offset 108
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

float Hash21(float2 p)
{
    return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

// 値ノイズ (格子の 4 隅の乱数を滑らかに補間)。霞用なので 2 オクターブで足りる。
float ValueNoise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    const float a = Hash21(i);
    const float b = Hash21(i + float2(1.0f, 0.0f));
    const float c = Hash21(i + float2(0.0f, 1.0f));
    const float d = Hash21(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// 双極子の流線関数。2 極を通る円弧の族になる。
//
// WHY ポテンシャルではなく流線関数か: ポテンシャルの等高線は極を «囲む» 輪で、
//     磁石の絵として読めない。流線 (角度の差) の等高線が磁力線の形になる。
float Stream(float2 p, float2 plusAt, float2 minusAt)
{
    const float2 a = p - plusAt;
    const float2 b = p - minusAt;
    return atan2(a.y, a.x) - atan2(b.y, b.x);
}

// 磁力線 1 本ぶんの明るさ。族の位置 s [0,1) を位相で滑らせ、0 付近に細い線を置く。
float FieldLines(float2 uv, float2 aspect, float2 plusAt, float2 minusAt, float density, float t)
{
    const float2 p = uv * aspect;
    const float  s = Stream(p, plusAt * aspect, minusAt * aspect) / 6.2831853f;
    // 族の中を滑らせる。線が «流れる» のは ＋ から − へ向かう向きなので、
    // 時間で族の番号を減らす側へ進める (増やすと − から ＋ へ逆流して見える)。
    // NOTE: atan2 の差は 2 極を結ぶ線分をまたぐと 2π 飛ぶ。density を整数に丸めて
    //       おけば、飛びは線の番号がちょうど整数ぶんずれるだけで frac には出ない。
    const float  k = s * floor(density) - t;
    // 幅は画面ピクセルで一定にしたい。fwidth で傾斜を正規化してから線を切る。
    const float  w = max(fwidth(k), 1.0e-4f);
    const float  d = abs(frac(k) - 0.5f);   // 0 (線の芯) 〜 0.5 (線と線の中間)
    // 飛びの直上は fwidth が跳ね上がる。そこを線と誤認すると 2 極を結ぶ太い筋が出る。
    const float  stroke = (1.0f - smoothstep(0.0f, w * 1.6f, d)) * step(w, 0.25f);
    // 極の近くは線が密になって潰れるので、極から離れるほど見せる。
    const float  nearPlus  = length(p - plusAt  * aspect);
    const float  nearMinus = length(p - minusAt * aspect);
    const float  open = smoothstep(0.05f, 0.45f, min(nearPlus, nearMinus));
    // 線の族の «向き» で明暗を付ける。同じ明るさで全部引くと方眼に見える。
    const float  ripple = 0.55f + 0.45f * sin(s * 6.2831853f * 2.0f + t * 0.7f);
    return stroke * open * ripple;
}

// 漂う塵。画面をセルに切り、セルごとに 1 粒だけ置く。粒は上へ向かってごく遅く流れ、
// 明滅の位相もセルごとにずらす (全部が同じ拍で瞬くと «点滅する星空» になる)。
float Motes(float2 px, float cell, float t)
{
    const float2 flow = float2(0.0f, t * 9.0f);           // 上向き (px/s)
    const float2 q    = (px + flow) / max(cell, 8.0f);
    const float2 id   = floor(q);
    const float2 f    = frac(q);
    float sum = 0.0f;
    // 隣のセルの粒が境界をまたいで欠けないように 3x3 を見る。
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        const float2 g   = float2(x, y);
        const float2 cid = id + g;
        const float  h0  = Hash21(cid);
        const float  h1  = Hash21(cid + 17.31f);
        const float  h2  = Hash21(cid + 41.07f);
        // 粒の居場所はセルの中で少し揺れる (真っ直ぐ上がると «雨» に見える)。
        const float2 at  = g + float2(0.25f + 0.5f * h0 + 0.08f * sin(t * 0.6f + h2 * 6.28f),
                                      0.25f + 0.5f * h1);
        const float  r   = 0.035f + 0.045f * h2;
        const float  d   = length(f - at);
        const float  spot = 1.0f - smoothstep(r * 0.4f, r, d);
        // 3 粒に 1 粒だけ見せる。全セルに置くと格子が透けて見える。
        const float  keep = step(0.66f, h0 + h1 * 0.5f);
        const float  twinkle = 0.55f + 0.45f * sin(t * (0.8f + h1) + h2 * 6.28f);
        sum += spot * keep * twinkle;
    }
    return saturate(sum);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    const float2 uv = input.localUv;
    // 矩形の縦横比。磁力線と霞は «画面の形» で描かないと、横長の画面で円弧が潰れる。
    const float2 aspect = float2(g_Rect.x / max(g_Rect.y, 1.0f), 1.0f);

    // 呼吸。2 極が交互にわずかに強くなる ─ 同時に明滅させると «全体が点滅した»
    // になるので、位相を半周ずらして «行き来している» ように見せる。
    const float breath = sin(phase * 0.9f);
    const float plusGain  = 1.0f + 0.18f * breath;
    const float minusGain = 1.0f - 0.18f * breath;

    // 左下 (0,1) が ＋ / 右上 (1,0) が −。localUv は左上が原点。
    const float2 plusAt  = float2(0.08f, 0.96f);
    const float2 minusAt = float2(0.94f, 0.06f);
    const float plus  = Bleed(uv, plusAt,  glowRadius) * plusGain;
    const float minus = Bleed(uv, minusAt, glowRadius) * minusGain;

    float3 color = baseColor.rgb
                 + plusColor.rgb  * plus  * glowStrength * plusColor.a
                 + minusColor.rgb * minus * glowStrength * minusColor.a;
    // 線と塵は、下地が透けている画面 (Title / Result の coverage 0.1) でも見えるように
    // 不透明度を自分で持ち上げる。覆いに掛けると、透けた画面では 1 割しか残らない。
    float extraAlpha = 0.0f;

    // 霞。低周波のムラを 2 層ずらして流す。下地に «奥行きの厚み» を出すためで、
    // 明るくはしない (色を少し持ち上げるだけ)。
    if (hazeStrength > 0.0f) {
        const float2 hp = uv * aspect * 2.2f;
        const float  h  = ValueNoise(hp + float2(phase * 0.020f, -phase * 0.012f)) * 0.65f
                        + ValueNoise(hp * 2.1f - float2(phase * 0.031f, phase * 0.017f)) * 0.35f;
        const float  haze = (h - 0.5f) * hazeStrength;
        color += (plusColor.rgb * plusColor.a * plus + minusColor.rgb * minusColor.a * minus + 0.35f) * haze;
    }

    // 磁力線。＋ の色から − の色へ、線の位置で染める (真ん中は白っぽく抜ける)。
    if (fieldStrength > 0.0f) {
        const float  lines = FieldLines(uv, aspect, plusAt, minusAt,
                                        max(fieldDensity, 1.0f), phase * fieldSpeed);
        const float  toMinus = saturate(dot(uv - plusAt, minusAt - plusAt)
                                        / max(dot(minusAt - plusAt, minusAt - plusAt), 1.0e-4f));
        const float3 lineColor = lerp(plusColor.rgb * plusColor.a, minusColor.rgb * minusColor.a, toMinus);
        // 中央の線は白へ寄せる。2 色だけだと «赤と青の縞» になり、場に見えない。
        const float  mid = 1.0f - abs(toMinus * 2.0f - 1.0f);
        color += lerp(lineColor, float3(0.82f, 0.86f, 0.92f), mid * 0.6f) * lines * fieldStrength;
        extraAlpha += lines * saturate(fieldStrength * 3.0f) * 0.6f;
    }

    // 塵。画面ピクセル基準。矩形の大きさを変えても粒の大きさは変わらない。
    if (moteStrength > 0.0f) {
        const float2 px = uv * g_Rect.xy;
        const float  m  = Motes(px, moteScale, phase);
        // 奥の層をもう 1 枚、半分の大きさ・半分の速さで敷いて視差を付ける。
        const float  back = Motes(px * 0.5f + 311.0f, moteScale, phase * 0.5f);
        color += float3(0.75f, 0.80f, 0.88f) * (m + back * 0.45f) * moteStrength;
        extraAlpha += (m + back * 0.45f) * saturate(moteStrength) * 0.5f;
    }

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
    const float  n = Hash21(p) - 0.5f;
    color += n * grain;

    // 覆いは «中央» と «四隅» を別々に持つ。
    //
    // WHY 掛け算にしないか (coverage * (base + vig*edge) にしないか):
    //   中央を透かそうとして coverage を下げると、四隅の額縁まで一緒に薄くなる。
    //   すると «全体をうっすら曇らせる» しか作れず、«中央は盤面が見えていて外周だけ
    //   締まっている» という一番欲しい絵にならない。足し算にすれば独立して決まる。
    const float alpha = saturate(coverage * baseColor.a + vig * edgeCoverage + extraAlpha);

    float4 result = float4(color, alpha) * UITint(input);
    clip(result.a - 0.002f);
    return result;
}
