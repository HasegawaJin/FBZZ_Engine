// FBZZ Engine
// Material/UIBattery.hlsl | UI
// 照射バッテリー (企画書 6.3)。電池の輪郭とセルを距離場で描く。
//
// WHY 電池の形をシェーダーで描くか:
//   6.3 は「時間が資源になる」と書いている。読ませたいのは残量そのものではなく
//   「あと何秒ぶん撃てるか」で、連続した帯より区切られたセルの方が数えられる。
//   セルの数と隙間をテクスチャで持つと、刻みを変えるたびに絵の描き直しになる。
//   距離場なら分割数も端子の大きさも .mat のパラメータのままで済む。
//
// WHY UIImage.fillAmount を使わないか:
//   UISystem の fillAmount は矩形と UV を切り落とす実装で、電池の外殻や端子まで
//   一緒に消える。減るのは中身だけなので、充填量はマテリアル側で解釈する。
//
// 軸の向き: fillFromRight = 1 で右詰め。画面中央から外向きに伸びる左右対称の
//   HUD (11 章の左入力 = 左銃) を、同じ .mat 1 枚で両側に使うためのスイッチ。
//   端子は必ず「外側 = 充填の進む先」に付く。
#include "UI/UICommon.hlsli"
#include "UI/UIShading.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 fillColor;      // 残っているセルの色 (a も効く)   offset   0
    float4 trackColor;     // 空になったセルの色               offset  16
    float4 shellColor;     // 外殻と端子の色                   offset  32
    float4 cornerRadius;   // 胴体の角丸 px: 左上/右上/右下/左下 offset 48
    float  fillRatio;      // 残量 [0,1]                       offset  64
    float  fillFromRight;  // 1 で右詰め                       offset  68
    float  shellThickness; // 外殻の線幅 px                    offset  72
    float  bodyInset;      // 外殻と中身の隙間 px              offset  76
    float  terminalWidth;  // 端子の幅 px (0 で端子なし)       offset  80
    float  terminalHeight; // 端子の高さ比 [0,1]               offset  84
    float  cellCount;      // セル分割数 (0 で連続バー)        offset  88
    float  cellGap;        // セル間の隙間 px                  offset  92
    float  headGlowWidth;  // 先端セルの発光幅 px (0 でなし)   offset  96
    float  headGlowGain;   // 先端セルの発光倍率               offset 100
    float  depleted;       // 1 で再点火待ち (中身を薄くする)  offset 104
    float  _pad0;          //                                  offset 108
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    float2 p        = UILocalPixels(input.localUv);
    float2 halfSize = UIHalfSize();

    // 端子を置く向き。右詰めのときは外側が -x になる。
    float dir      = (fillFromRight > 0.5f) ? -1.0f : 1.0f;
    float terminal = clamp(terminalWidth, 0.0f, halfSize.x);

    // ── 輪郭: 胴体 ＋ 端子の和 ────────────────────────────────────────────
    float2 bodyHalf   = float2(max(halfSize.x - terminal * 0.5f, 1.0f), halfSize.y);
    float2 bodyCenter = float2(-dir * terminal * 0.5f, 0.0f);
    float  sdShell    = UI_SdRoundedBox(p - bodyCenter, bodyHalf, cornerRadius);

    if (terminal > 0.5f)
    {
        float2 terminalHalf   = float2(terminal * 0.5f,
                                       max(halfSize.y * saturate(terminalHeight), 1.0f));
        float2 terminalCenter = float2(dir * (halfSize.x - terminal * 0.5f), 0.0f);
        // 端子の角丸は胴体より小さく取る。同じ半径を使うと、細い突起が
        // まるごと丸に潰れて「電池の頭」に見えなくなる。
        float  terminalRadius = min(terminalHalf.x, terminalHalf.y) * 0.6f;
        sdShell = min(sdShell, UI_SdRoundedBox(p - terminalCenter, terminalHalf, terminalRadius));
    }

    // ── 中身の入る空洞 ────────────────────────────────────────────────────
    float  inset      = max(shellThickness + bodyInset, 0.0f);
    float2 cavityHalf = max(bodyHalf - inset, float2(1.0f, 1.0f));
    float  sdCavity   = UI_SdRoundedBox(p - bodyCenter, cavityHalf,
                                        max(cornerRadius - inset, 0.0f));

    // 空洞の内側の端を 0、端子側を 1 とする軸。ピクセルのまま持つと、
    // 先端の締まり方が Canvas の縮尺に依存しなくなる。
    float span   = cavityHalf.x * 2.0f;
    float axisPx = (p.x - bodyCenter.x) * dir + cavityHalf.x;
    float headPx = saturate(fillRatio) * span;
    float filled = UI_Coverage(axisPx - headPx);

    // ── セルの刻み ────────────────────────────────────────────────────────
    // 境界の前後 cellGap/2 を抜いて、独立したブロックの並びに見せる。
    float cellMask = 1.0f;
    if (cellCount >= 1.0f && cellGap > 0.0f)
    {
        float pitch      = span / cellCount;
        float toBoundary = (0.5f - abs(frac(axisPx / pitch) - 0.5f)) * pitch;
        cellMask = UI_Coverage(cellGap * 0.5f - toBoundary);
    }

    // WHY ここで g_Color を掛けないか: UIImage.color は「ウィジェット全体の濃さ」で、
    //     収納中のフェードアウトに使う。セルの明るさ (fillColor) と同じ場所へ掛けると
    //     明るさが二乗になり、色を触るたびにフェードの効き方まで変わる。
    //     役割の違う 2 つは最後に 1 度だけ合成する。
    float4 inner = lerp(trackColor, fillColor, filled);
    inner.a *= UI_Coverage(sdCavity) * cellMask;

    // 先端の発光。満ちきっている / 空のときは先端が存在しないので出さない。
    if (headGlowWidth > 0.0f && fillRatio > 0.001f && fillRatio < 0.999f)
    {
        float glow = saturate(1.0f - abs(axisPx - headPx) / headGlowWidth);
        inner.rgb += fillColor.rgb * glow * glow * headGlowGain;
    }

    // 再点火待ちは「量は戻っているがまだ使えない」状態 (6.3)。
    // 色の明暗だけだと充填中と見分けが付かないので、中身そのものを薄くする。
    inner.a *= lerp(1.0f, 0.35f, saturate(depleted));

    float4 shell = shellColor;
    shell.a *= UI_Border(sdShell, max(shellThickness, 0.0f));

    float4 result = UI_Over(shell, inner);
    result.rgb *= UITint(input).rgb;
    result.a   *= UITint(input).a;
    result.rgb  = UI_Dither(result.rgb, input.pos.xy);

    clip(result.a - 0.002f);
    return result;
}
