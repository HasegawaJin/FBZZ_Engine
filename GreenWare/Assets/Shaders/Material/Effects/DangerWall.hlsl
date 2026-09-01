/// @file    DangerWall.hlsl
/// @brief   直進攻撃の通り道を «立てて» 見せる壁。床の帯だけでは高さが読めない
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// PSO: SOLID_NOCULL + ADDITIVE + DEPTH_READ (depth_write = false)
///
/// 頂点は DangerWallComponent が毎フレーム組む 2 枚の板。
/// uv.x = 始点から終点への進み [0,1]、uv.y = 床から天までの高さ [0,1]。
///
/// WHY 床の帯では足りないか:
///   ボス 1 は全高 6m・全幅 9m で、10m の距離で画面高さの 71% を占める (boss.md)。
///   突進とビームは «その距離» で来るので、足元のデカールが本体の陰に入る。
///   通り道を垂直に立てれば、ボスの体で床が隠れていても回廊が見える。
///
/// WHY 面を塗り潰さないか:
///   通り道は «見えなければならない» が «向こう側が見えなくなっては困る» 場所でもある。
///   避ける先はたいてい壁の向こうにあるので、塗ると避ける判断材料を消してしまう。
///   縦の桟と上下の縁だけを出して、面はほぼ素通しにする。
///
/// WHY 加算か:
///   半透明の面を 2 枚重ねると、交差する角度で濃さが倍になって «濃い所が危ない» と
///   読み違える。加算なら重なっても «光が増える» だけで、危険度の誤読にならない。
///
/// WHY cbuffer の名前を変えてはいけないか:
///   ShaderDescriptor は PS バイトコードを GetConstantBufferByName("MaterialConstants")
///   で引いて変数表を作る。名前が違うと MaterialInstance::Set* が黙って捨てられる。

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 危険の色。DecalBossTelegraph の frameColor と揃える。
    float4 albedo;

    /// 予兆の進み [0,1]。1 で着弾。
    float  progress;
    /// 全体の明るさ (HDR)。Bloom のしきい値を越える値にしないと滲まない。
    float  intensity;
    /// 縦の桟の本数。
    float  ribCount;
    /// 桟 1 本の幅 [0,1]。
    float  ribWidth;

    /// 桟が流れる位相 [周]。呼び出し側が進める。
    float  ribScroll;
    /// 上下の縁の太さ [0,1]。
    float  edgeWidth;
    /// 天へ向かって薄くなる量 [0,1]。1 で上端が完全に消える。
    float  heightFade;
    float  _dangerPad0;
};

static const float kDangerTau = 6.28318530718f;

/// 三角波の帯。矩形波だと縁が階段になり、sin だと «幅» を直接指定できない。
float DangerRib(float coord, float count, float width, float aa)
{
    if (count <= 0.0f) return 0.0f;
    const float phase = abs(frac(coord * count) - 0.5f) * 2.0f;
    const float half  = saturate(width);
    return 1.0f - smoothstep(half, half + aa, phase);
}

PSInput VSMain(VSInput v)
{
    PSInput o;
    const float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.worldPos   = worldPos.xyz;
    o.svPosition = mul(worldPos, viewProjection);
    o.normal     = normalize(mul(v.normal, (float3x3)worldInvTranspose));
    o.tangent    = normalize(mul(v.tangent, (float3x3)world));
    o.uv         = v.uv;
    return o;
}

float4 PSMain(PSInput p) : SV_Target0
{
    const float along  = saturate(p.uv.x);
    const float height = saturate(p.uv.y);
    const float aa     = max(fwidth(along) * 2.0f, 1.0e-3f);

    // 上下の縁。床との接地線と «天井» を出すと、壁が空間に立っていることが分かる。
    const float edge = max(1.0f - smoothstep(0.0f, max(edgeWidth, 1.0e-3f), height),
                           1.0f - smoothstep(0.0f, max(edgeWidth, 1.0e-3f), 1.0f - height));

    // 縦の桟。流れる向きが «どちらから来るか» を言う。
    const float rib = DangerRib(along - ribScroll, ribCount, ribWidth, aa);

    // 満ちの先端。床のデカールと同じ時計を壁でも見せる。
    const float p01   = saturate(progress);
    const float front = 1.0f - smoothstep(0.0f, 0.05f, abs(along - p01));

    // 上へ行くほど薄く。天井まで同じ濃さだと «箱» に見えて、床の危険と繋がらない。
    const float fade = lerp(1.0f, 1.0f - saturate(heightFade), height);

    // 進むほど濃く。出た瞬間と直前が同じだと «あと何秒» が壁から読めない。
    const float gain = lerp(0.45f, 1.0f, p01);

    const float mask = saturate(edge * 0.9f + rib * 0.55f + front * 1.4f) * fade * gain;

    const float3 color = albedo.rgb * (mask * max(intensity, 0.0f));
    // 加算なので alpha は «どれだけ足すか»。0 を返すと SrcBlend=SRC_ALPHA で消える。
    return float4(color, saturate(mask * albedo.a));
}
