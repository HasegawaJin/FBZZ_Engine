// FBZZ Engine
// Material/UITitleSheen.hlsl | UI
// ロゴ用。素材の形はそのままに、± の染め分け・艶の走り・外側の光・出現の溶け込み・
// 縁の放電を足す。
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
//
// WHY 縁に放電を走らせるか (2026-09-07):
//   タイトルの背景では電極が火花を散らしている。ロゴだけが «貼った紙» のまま
//   だと、盤面と題字が別の世界に見える。裾 (halo) の中を細かいノイズの筋が
//   走り、ときどき (crackle) 極の色で光れば、題字が同じ装置の一部になる。
//   常時ではなく «ときどき» ─ 常時だと «壊れかけの看板» に見える。
//
// WHY 出現をノイズで溶かすか (UITextHero と同じ):
//   α のフェードは «薄い絵が濃くなる» だけ。面をノイズで削ってしきい値を上げると
//   粒が寄り集まって字になり、縁の熱色が冷えていく。
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
    float  reveal;        // 出現 0..1。script が進める (既定 1)  offset 68
    float  glowStrength;  // 外側の光の強さ。0 で無し             offset 72
    float  crackle;       // 縁の放電の強さ 0..1。script が叩く   offset 76
    float4 glowColor;     // 外側の光の色 (a = 上限)              offset 80
    float  glowRadius;    // 光の半径 [テクセル]                  offset 96
    float  splitTexels;   // 色ずれの最大幅 [テクセル]            offset 100
    float  dissolveScale; // 溶け込みのノイズの細かさ             offset 104
    float  phase;         // 放電の位相 [秒]。script が進める     offset 108
    float4 polePlus;      // 熱色・放電・色ずれに使う ＋ の芯の色  offset 112
    float4 poleMinus;     // 同 − の芯の色                        offset 128
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float Hash21(float2 p)
{
    return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

float ValueNoise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(Hash21(i), Hash21(i + float2(1, 0)), u.x),
                lerp(Hash21(i + float2(0, 1)), Hash21(i + float2(1, 1)), u.x), u.y);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    float2 texSize;
    g_Texture.GetDimensions(texSize.x, texSize.y);
    const float2 texel = 1.0f / max(texSize, 1.0f);
    const float2 uv    = input.localUv;

    const float4 texel0 = g_Texture.Sample(g_Sampler, input.uv);

    // 芯と裾。素材の縁は硬いので、傾斜を画面 1 ピクセル幅へ正規化してから切る
    // (固定幅の smoothstep だと、拡大したときに縁が階段状になる)。
    //
    // NOTE: fwidth はクワッド内の隣接ピクセルを読む。先に discard したレーンが
    //       混ざると結果が未定義になるので、微分は棄却より前で採りきる
    //       (DecalCommon.hlsli の DecalResolve が同じ理由で discard しない)。
    const float width = max(fwidth(texel0.a), 1.0e-4f);
    const float core  = saturate((texel0.a - 0.5f) / width + 0.5f);
    const float halo  = texel0.a * (1.0f - core);

    // 出現の溶け込み。しきい値の直下 (rim) は熱色。
    const float n     = ValueNoise(uv * dissolveScale) * 0.75f + ValueNoise(uv * dissolveScale * 3.1f) * 0.25f;
    const float level = reveal * 1.18f - n;
    const float shown = smoothstep(-0.02f, 0.06f, level);
    const float rim   = (1.0f - smoothstep(0.0f, 0.14f, abs(level - 0.03f))) * (1.0f - step(1.0f, reveal));

    // 左右の染め分け。中央は素材の色のままにしたいので、両端へ向かってだけ寄せる。
    const float lateral = uv.x;                            // 0 = 左, 1 = 右
    const float toPlus  = saturate(1.0f - lateral * 2.0f); // 左半分だけ立つ
    const float toMinus = saturate(lateral * 2.0f - 1.0f); // 右半分だけ立つ
    float3 rgb = texel0.rgb;
    rgb = lerp(rgb, rgb * tintPlus.rgb,  toPlus  * tintStrength * tintPlus.a);
    rgb = lerp(rgb, rgb * tintMinus.rgb, toMinus * tintStrength * tintMinus.a);
    // 極の色そのもの (熱色・放電に使う)。左は ＋、右は −。
    const float3 poleColor = lerp(polePlus.rgb, poleMinus.rgb, lateral);

    // 艶。帯は左から右へ 1 本だけ走る。傾けるのは、水平の帯だと «画面の走査» に
    // 見えて、題字の面を舐めた光に見えないため。
    const float  band = uv.x + (uv.y - 0.5f) * sheenTilt;
    // 位相は 0..1 で一周する。帯の幅ぶん外側から入って外側へ抜けるよう、
    // 進む範囲を [-width, 1+width] へ広げる (端で «湧いて消える» のを避ける)。
    const float  head = lerp(-sheenWidth, 1.0f + sheenWidth, frac(sheenPhase));
    const float  w    = max(sheenWidth, 1.0e-3f);
    const float  d    = saturate(1.0f - abs(band - head) / w);
    // 3 乗で «芯だけ強く» する。線形だと帯の裾まで明るく、幅の広い光の壁になる。
    const float  sheen = d * d * d * core;
    rgb += sheenColor.rgb * sheen * sheenColor.a;

    // 縁の放電。裾の中を細い筋が走り、crackle で極の色に光る。
    // 筋は時間で流れるノイズの «稜線» (abs(n - 0.5) が小さい所)。
    float spark = 0.0f;
    if (crackle > 0.0f) {
        const float2 q  = uv * float2(texSize.x / max(texSize.y, 1.0f), 1.0f) * 9.0f;
        const float  s1 = ValueNoise(q * 2.0f + float2(phase * 3.1f, -phase * 2.3f));
        const float  s2 = ValueNoise(q * 5.0f - float2(phase * 4.7f, phase * 1.9f));
        const float  ridge = 1.0f - smoothstep(0.0f, 0.06f, abs(s1 * 0.7f + s2 * 0.3f - 0.5f));
        spark = ridge * halo * crackle * 2.5f;
        // 芯の縁 (core の傾斜) にも少しだけ乗せる ─ 裾だけ光ると «字から浮いた線» になる。
        spark += ridge * saturate(1.0f - abs(texel0.a - 0.5f) * 4.0f) * crackle * 0.8f;
    }

    // 出現の縁の熱色。極の色 → 白へ。
    rgb = lerp(rgb, lerp(poleColor, float3(1.0f, 0.97f, 0.92f), 0.4f), rim * 0.9f);
    rgb += poleColor * spark;

    // 外側の光。周囲 8 点の α の平均 = ぼけた字。
    float glow = 0.0f;
    if (glowStrength > 0.0f && glowRadius > 0.0f) {
        [unroll] for (int k = 0; k < 8; ++k) {
            const float a = k * 0.78539816f;
            glow += g_Texture.Sample(g_Sampler, input.uv + float2(cos(a), sin(a)) * texel * glowRadius).a;
        }
        glow = saturate(glow * 0.125f + texel0.a * 0.5f);
        glow = glow * glow;
    }

    // 色ずれ。出現の途中と放電で、裾を極の色に割る (芯は割らない ─ 字が読めなくなる)。
    float3 fringe = 0.0f;
    const float split = splitTexels * saturate((1.0f - reveal) * 1.5f + crackle * 0.8f);
    if (split > 0.01f) {
        const float2 dx = float2(texel.x * split, 0.0f);
        const float aR = g_Texture.Sample(g_Sampler, input.uv + dx).a;
        const float aB = g_Texture.Sample(g_Sampler, input.uv - dx).a;
        fringe = polePlus.rgb * saturate(aR - texel0.a) * 0.8f + poleMinus.rgb * saturate(aB - texel0.a) * 0.8f;
    }

    // 裾は色を寄せるだけ。持ち上げるのは «素材が既に持っている» ぶんの範囲に留める。
    //
    // NOTE: core と halo は texel.a から作ってあるので、ここで texel.a を
    //       もう一度掛けない (掛けると縁が二乗ぶん痩せて、字が細る)。
    const float bodyAlpha = (core + halo * (1.0f + edgeGlow)) * shown;
    const float glowAlpha = glow * glowColor.a * glowStrength * (0.5f + 0.5f * sheen + rim + crackle * 0.6f) * shown;
    const float fringeA   = max(fringe.r, max(fringe.g, fringe.b));

    // 本体 over 光。光の色は出現・放電で極の色に寄る。
    const float3 glowRgb = lerp(glowColor.rgb, poleColor, saturate(rim + crackle * 0.7f));
    const float  outA    = saturate(bodyAlpha + glowAlpha * (1.0f - bodyAlpha) + fringeA * (1.0f - bodyAlpha));
    float3 outRgb = rgb * bodyAlpha + glowRgb * glowAlpha * (1.0f - bodyAlpha) + fringe * (1.0f - bodyAlpha);
    outRgb /= max(outA, 1.0e-4f);

    float4 result = float4(outRgb, outA) * UITint(input);
    clip(result.a - 0.002f);
    return result;
}
