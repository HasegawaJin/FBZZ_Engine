/// @file UICrosshair.hlsl
/// @brief 画面中央のクロスヘア (4 本の線・中心点・縁取り) を距離場で描く
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 線 1 本ずつを UIImage にしないか:
///   4 本 + 中心点を矩形で並べると、太さを 1px 変えるだけで 10 箇所の position と
///   scale を入れ直すことになる。しかも「中心が 1 点である」ことは、4 つの座標が
///   たまたま揃っている状態でしか成り立たない。距離場なら空き・長さ・太さ・開きは
///   同じ 1 つの式の中の項でしかなく、中心の 1 点は式から出る。
///
/// WHY 縁取りを持つか:
///   12.2 の照準色はプレイヤーの緑で、この作品の背景は草と木で埋まる。同系色の上に
///   細い緑線を置くと輪郭が溶けて狙点が画面から消える。暗い縁を 1px 敷けば、
///   背景の明暗に関わらず線が浮く。矩形を重ねて縁を作ると要素数が倍になるが、
///   距離場なら同じ距離をもう一度しきい値にするだけで済む。
///
/// WHY 開き (spread) をここで解釈するか:
///   照射中に 4 本が外へ開く動きは中心からの距離が伸びるだけで、線の形は変わらない。
///   要素のスケールで代用すると線の太さまで太り、狙点の細さが照射のたびに変わる。
///
/// 単位はすべて Canvas ピクセル (1920x1080 基準)。矩形の中心が画面中心に来る前提で、
/// 図形は中心を原点とするピクセル座標で組み立てる。

#include "UI/UICommon.hlsli"
#include "UI/UIShading.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 tickColor;     // 線と中心点の色 (a も効く)          offset  0
    float4 outlineColor;  // 縁取りの色                         offset 16
    float  gap;           // 中心の空き px (片側)               offset 32
    float  tickLength;    // 線 1 本の長さ px                   offset 36
    float  tickThickness; // 線の太さ px                        offset 40
    float  tickRoundness; // 線の端の丸み [0,1]                 offset 44
    float  dotSize;       // 中心点の直径 px (0 で出さない)     offset 48
    float  spread;        // 4 本が外へ開く量 px                offset 52
    float  outlineWidth;  // 縁取りの太さ px (0 でなし)         offset 56
    float  spriteMode;    // 1 で図形を止めてテクスチャを出す   offset 60
};

// 「この図形は無い」を表す距離。min() の相手にしても必ず負けるだけの大きさ。
static const float kNoShape = 1e6f;

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    // WHY 分岐の外で 1 度だけ取るか: Sample も fwidth も画面微分を使う命令で、
    //     一様でない制御の中では結果が保証されない。スプライトを使わないときに
    //     読むのは UISystem が挿す白 1x1 なので、代償は無い。
    float4 sprite = g_Texture.Sample(g_Sampler, input.uv);

    float2 p = UILocalPixels(input.localUv);

    float halfThickness = max(tickThickness, 0.0f) * 0.5f;
    float halfLength    = max(tickLength, 0.0f) * 0.5f;
    // 中心の空き。ここが「狙っている 1 点」の半径そのもので、照射中はここだけが伸びる。
    float inner    = max(gap, 0.0f) + max(spread, 0.0f);
    float toCenter = inner + halfLength;
    float radius   = halfThickness * saturate(tickRoundness);

    // WHY abs で折り返すか: 上下 (左右) の 2 本は中心対称なので、片側を解けば
    //     もう片側の距離も同じ式で出る。4 本を個別に測ると評価が 4 回になるうえ、
    //     中心の空きが「4 つの数値が揃っている」状態に戻ってしまう。
    float sdVertical   = UI_SdRoundedBox(float2(p.x, abs(p.y) - toCenter),
                                         float2(halfThickness, halfLength), radius);
    float sdHorizontal = UI_SdRoundedBox(float2(abs(p.x) - toCenter, p.y),
                                         float2(halfLength, halfThickness), radius);
    float sdTicks = (halfThickness > 0.0f && halfLength > 0.0f)
                  ? min(sdVertical, sdHorizontal) : kNoShape;

    // 中心点は正円。矩形にすると、線と同じ太さのときだけ十字の交点に見えてしまう。
    float dotRadius = max(dotSize, 0.0f) * 0.5f;
    float sdDot     = (dotRadius > 0.0f) ? UI_SdCircle(p, dotRadius) : kNoShape;

    float sd = min(sdTicks, sdDot);

    float4 ink = tickColor;
    ink.a *= UI_Coverage(sd);

    // 縁は図形を outlineWidth ぶん太らせた形。同じ距離をしきい値にするだけなので、
    // 線が開いても中心点が縮んでも、縁の付き方は勝手に追従する。
    float4 border = outlineColor;
    border.a *= UI_Coverage(sd - max(outlineWidth, 0.0f)) * step(0.001f, outlineWidth);

    // WHY スプライトと図形を重ねないか: 絵の透明部分から手続きの線が透けて、
    //     照準が 2 つあるように見える。形はどちらか一方が持つ。
    float4 result = (spriteMode > 0.5f) ? sprite * tickColor : UI_Over(ink, border);

    // WHY 色を g_Color と分けるか: UIImage.color は「ウィジェット全体の濃さ」で、
    //     収納中のフェードに使う。極性の色まで同じ場所へ載せると、色を触るたびに
    //     フェードの効き方が変わる。役割の違う 2 つは最後に 1 度だけ合成する。
    result.rgb *= g_Color.rgb;
    result.a   *= g_Color.a;
    result.rgb  = UI_Dither(result.rgb, input.pos.xy);

    clip(result.a - 0.002f);
    return result;
}
