// FBZZ Engine
// Material/UIMenuBand.hlsl | UI
// メニュー行の «選択帯»。カーソルの乗った行の背後に、左から右へ開く帯を敷く。
//
// 素材 (Assets/Textures/UI/T_UI_MenuBand.png) の作り:
//   A … 帯の形。左端が 1、右へ向かって落ち、上下の縁は数 px でぼける
//   R … 細かい紙目。帯の中の «ざらつき» に使う
//   形は素材、動き (開く・斜線が流れる・決定で光る) はここ、と分けてある。
//
// WHY 芯 (UIMenuItem) と別に帯を持つか:
//   芯の 4px と文字の色だけで «選ばれている» を見せると、目が行の左端にしか
//   留まらず、行全体が押せる領域だと伝わらない。帯は当たり判定の形そのもの
//   (行の矩形) を薄く見せる。芯は «どの行か» を、帯は «どこまでが行か» を言う。
//
// WHY 左から開くか:
//   カーソルが乗った瞬間に帯が全面に «出現» すると、行が点滅したように見える。
//   芯のある左端から開いていけば、光が芯から «滲み出た» ように読める。
//   閉じるときは逆に右から消える (開いた順に戻す) のではなく、全体が薄れる ─
//   閉じる方向を作ると、隣の行へ移ったときに 2 本の帯が «追いかけ合って» 見える。
//
// WHY 斜線を流すか:
//   選ばれている間ずっと静止している帯は «塗った矩形» になる。細い斜線が芯から
//   離れる向きへごく遅く流れていると、帯が «通電している» ように見える。
//   斜線は素材に焼かず式で引く。素材だと矩形の大きさで太さが変わる。
#include "UI/UICommon.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 bandColor;     // 帯の色 (a = 全体の強さ)。極の色を入れる     offset  0
    float4 flashColor;    // 決定の瞬間の色 (a = 強さ)                 offset 16
    float  selected;      // 0 = 閉じている, 1 = 開き切った (要素ごとに上書き) offset 32
    float  flash;         // 決定の光。script が 1 から 0 へ減衰させる     offset 36
    float  phase;         // 斜線の流れと脈動の位相 [秒]                 offset 40
    float  hatchStrength; // 斜線の濃さ。0 で斜線なし                    offset 44
    float  hatchPeriod;   // 斜線の間隔 [px]                             offset 48
    float  edgeWidth;     // 左端の明るい縁の幅 [px]                     offset 52
    float  pulse;         // 開いている間の脈動の振幅。0 で止まる         offset 56
    float  _pad0;         //                                            offset 60
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    const float4 texel = g_Texture.Sample(g_Sampler, input.uv);
    const float2 uv    = input.localUv;
    const float2 px    = uv * g_Rect.xy;   // 矩形内のピクセル座標 (左上原点)

    const float open = saturate(selected);

    // 開き。帯の先頭が左端から右端の少し外まで走る。先頭は柔らかく (硬い縁だと
    // «ワイプ» に見える)。開き切ると先頭は矩形の外に居るので、全面が乗る。
    const float head  = lerp(-0.15f, 1.25f, open);
    const float reach = 1.0f - smoothstep(head - 0.25f, head, uv.x);

    // 斜線。芯から離れる向き (右) へ流す。45 度で引くと «工事現場の縞» になるので
    // 寝かせる。線の芯だけ細く立てて、面の明るさはほとんど変えない。
    const float hp    = max(hatchPeriod, 2.0f);
    const float diag  = (px.x - px.y * 1.8f) / hp - phase * 0.9f;
    const float hatch = pow(0.5f + 0.5f * cos(diag * 6.2831853f), 6.0f) * hatchStrength;

    // 左端の縁。芯 (UIMenuItem) の裾と重なる場所なので、ここだけ少し明るい。
    const float edge = 1.0f - smoothstep(0.0f, max(edgeWidth, 1.0f), px.x);

    // 脈動。開いている間だけ、ゆっくり呼吸する。速いと «警告灯» になる。
    const float breathe = 1.0f + pulse * sin(phase * 2.4f) * open;

    // 帯そのもの。素材の形 × 開き × (1 + 縁 + 斜線)。
    const float shape = texel.a * reach * open;
    float3 rgb   = bandColor.rgb * (1.0f + edge * 0.8f + hatch) * breathe;
    float  alpha = shape * bandColor.a * (1.0f + texel.r * 0.15f);

    // 決定の光。帯全体が一度白へ寄り、縁から外へ抜ける。
    // 開きとは独立に足す (閉じかけの行を押しても光る)。
    const float f = saturate(flash);
    rgb   = lerp(rgb, flashColor.rgb, f * flashColor.a);
    alpha = saturate(alpha + texel.a * f * flashColor.a * 0.9f);

    float4 result = float4(rgb, alpha) * UITint(input);
    clip(result.a - 0.002f);
    return result;
}
