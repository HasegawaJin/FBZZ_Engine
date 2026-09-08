// FBZZ Engine
// Material/UITextHero.hlsl | UI
// 見出し用のテキストマテリアル。CLEAR / FAILED / STAGE SELECT / OPTIONS の題字に、
// 外側の光・出現の溶け込み・走査・極の染め分け・色ずれを乗せる。
//
// 前提 (組み込みの UIText.hlsl と同じ):
//   t0 はフォントアトラスで、`.r` がカバレッジ。SDF ではない。
//   g_Rect は 0 (1 ドローに複数グリフを詰めるため矩形が定まらない)。
//   .mat の値は静的で、UIText には要素ごとの上書き (ui.SetMaterialFloat) が無い。
//
// WHY 頂点色を «データ» として読むか:
//   上の制約から、時間も «文字列のどこか» もシェーダーへは届かない。届くのは
//   リッチテキストの <color> が積む頂点色だけ。そこで頂点色を色ではなく
//   グリフごとの制御値として使う (Scripts/UI/UiTextFx.hpp の Encode が書く):
//     r = 出現 (0 = まだ無い, 1 = 置き切った)   … 溶け込みのしきい値
//     g = 1 − 走査 (1 = 走査なし)                   … 光が舐めている強さ (反転)
//     b = 極 (0 = ＋, 0.5 = 中立, 1 = −)           … 左右の染め分け
//     a = 不透明度
//   全体の色と α は g_Color (UIText.color) が持つ。文字の色そのものは .mat の bodyColor。
//   g を反転させてあるのは、タグの無い文字 (頂点色 = 白) が «走査で真っ白» にならず、
//   エディタで開いたときも素の字で見えるように。
//
// WHY 外側の光をアトラスを何度も読んで作るか:
//   アトラスは 1 テクセルの傾斜しか持たないので、しきい値をずらしても太い縁は出ない。
//   周囲 8 点のカバレッジを足せば «ぼけた字» が得られ、それを光として敷ける。
//   タップ数を増やすほど滑らかだが、題字は画面に 1〜2 個なので 8 で足りる。
//
// WHY 出現をノイズで溶かすか:
//   α のフェードは «薄い字が濃くなる» だけで、字が «来た» 感じにならない。
//   字の面をノイズで削り、しきい値を上げていくと、粒が寄り集まって字になる。
//   縁 (しきい値の直下) を熱色にすると、集まりながら冷えていくように見える。
#include "UI/UICommon.hlsli"
#include "UI/UIShading.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 bodyColor;      // 字の色                                    offset  0
    float4 glowColor;      // 外側の光 (a = 強さの上限)                  offset 16
    float4 heatColor;      // 出現の縁と走査の芯                         offset 32
    float4 plusColor;      // 左端へ寄せる極の色                         offset 48
    float4 minusColor;     // 右端へ寄せる極の色                         offset 64
    float4 outlineColor;   // 縁取り (a = 0 で無し)                      offset 80
    float  outlineWidth;   // 縁取りの太さ (傾斜のどこで切るか, 0..1)    offset 96
    float  glowRadius;     // 光の半径 [テクセル]                        offset 100
    float  glowStrength;   // 光の強さ                                   offset 104
    float  tintStrength;   // 極の染めの強さ (0 で無し)                  offset 108
    float  dissolveScale;  // 溶け込みのノイズの細かさ (アトラス UV 倍率) offset 112
    float  splitTexels;    // 色ずれの最大幅 [テクセル]                  offset 116
    float  sheenGain;      // 走査の明るさ                               offset 120
    float  _pad0;          //                                            offset 124
};

UIPixelInput VSMain(UIVertexInput input)
{
    UIPixelInput output;
    output.pos     = mul(float4(input.pos, 0.0f, 1.0f), g_Ortho);
    output.uv      = input.uv;
    output.localUv = input.uv;
    output.color   = input.color;
    return output;
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

// 傾斜を画面 1 ピクセル幅へ正規化してから閾値を取る (組み込みと同じ)。
float Cut(float coverage, float width, float threshold)
{
    return saturate((coverage - threshold) / width + 0.5f);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    // 頂点色 = 制御値 (ファイル頭)。
    const float reveal = input.color.r;
    const float sheen  = 1.0f - input.color.g;
    const float pol    = input.color.b;
    const float galpha = input.color.a;

    float2 atlasSize;
    g_Texture.GetDimensions(atlasSize.x, atlasSize.y);
    const float2 texel = 1.0f / max(atlasSize, 1.0f);

    // NOTE: fwidth は棄却より前で採る (DecalCommon と同じ理由)。
    const float coverage = g_Texture.Sample(g_Sampler, input.uv).r;
    const float width    = max(fwidth(coverage), 1.0e-4f);

    // 出現の溶け込み。ノイズをしきい値にして、reveal が上がるほど面が増える。
    // 1.18 倍するのは、reveal = 1 で全部の粒がしきい値を越えるように。
    const float n     = ValueNoise(input.uv * dissolveScale) * 0.75f
                      + ValueNoise(input.uv * dissolveScale * 3.1f) * 0.25f;
    const float level = reveal * 1.18f - n;
    const float shown = smoothstep(-0.02f, 0.06f, level);
    const float rim   = (1.0f - smoothstep(0.0f, 0.14f, abs(level - 0.03f))) * (1.0f - step(1.0f, reveal));

    // 色ずれ。出現の途中と走査の芯で r / b を横へずらして読む。
    const float split = splitTexels * saturate((1.0f - reveal) * 1.5f + sheen * 0.6f);
    const float2 dx   = float2(texel.x * split, 0.0f);
    const float covR  = split > 0.01f ? g_Texture.Sample(g_Sampler, input.uv + dx).r : coverage;
    const float covB  = split > 0.01f ? g_Texture.Sample(g_Sampler, input.uv - dx).r : coverage;

    const float body  = Cut(coverage, width, 0.5f) * shown;
    const float bodyR = Cut(covR, width, 0.5f) * shown;
    const float bodyB = Cut(covB, width, 0.5f) * shown;

    // 縁取り: 本体より外側の、カバレッジがまだ立っている帯。
    float outline = 0.0f;
    if (outlineWidth > 0.0f && outlineColor.a > 0.0f)
        outline = saturate(Cut(coverage, width, 0.5f - outlineWidth) - Cut(coverage, width, 0.5f)) * shown;

    // 外側の光。周囲 8 点のカバレッジの平均を «ぼけた字» として使う。
    float glow = 0.0f;
    if (glowStrength > 0.0f && glowRadius > 0.0f) {
        const float r = glowRadius;
        [unroll] for (int k = 0; k < 8; ++k) {
            const float a = k * 0.78539816f;
            const float2 o = float2(cos(a), sin(a)) * texel * r;
            glow += g_Texture.Sample(g_Sampler, input.uv + o).r;
        }
        glow = saturate(glow * 0.125f + coverage * 0.5f);
        glow = glow * glow;   // 裾を絞る。線形だと «字の周りの霧» になる
    }

    // 字の色。左右の極で寄せ、走査の芯は熱色へ。
    float3 rgb = bodyColor.rgb;
    const float toPlus  = saturate(1.0f - pol * 2.0f);
    const float toMinus = saturate(pol * 2.0f - 1.0f);
    rgb = lerp(rgb, plusColor.rgb,  toPlus  * tintStrength * plusColor.a);
    rgb = lerp(rgb, minusColor.rgb, toMinus * tintStrength * minusColor.a);
    const float s3 = sheen * sheen * sheen;
    rgb = lerp(rgb, heatColor.rgb, s3 * sheenGain);
    // 出現の縁は熱色 (集まりながら冷える)。
    rgb = lerp(rgb, heatColor.rgb, rim * 0.9f);

    // 色ずれは r と b の面をそれぞれの縁で置き換える (面の内側は共通)。
    float3 fringe = float3(bodyR, body, bodyB);
    const float bodyA = max(body, max(bodyR, bodyB));
    float3 bodyRgb = rgb * (fringe / max(bodyA, 1.0e-4f));
    // ずれの出ている部分だけ極の色を足す (ずれた縁が «赤と青» に割れる)。
    bodyRgb += (bodyR - body) * plusColor.rgb * 0.8f + (bodyB - body) * minusColor.rgb * 0.8f;

    float4 result = float4(bodyRgb, bodyA * bodyColor.a);
    if (outline > 0.0f)
        result = UI_Over(result, float4(outlineColor.rgb, outlineColor.a * outline * (1.0f - body)));

    // 光は本体の下。出現の縁と走査で強くなる (置き切った後は静かに)。
    const float glowGain = glowStrength * (0.55f + 0.45f * s3 + rim * 1.2f) * shown;
    const float4 glowLayer = float4(lerp(glowColor.rgb, heatColor.rgb, saturate(s3 + rim)),
                                    glow * glowColor.a * glowGain);
    result = UI_Over(result, glowLayer);

    result *= g_Color;
    result.a *= galpha;
    clip(result.a - 0.002f);
    return result;
}
