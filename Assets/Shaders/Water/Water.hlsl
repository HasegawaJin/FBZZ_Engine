// FBZZ Engine
// Water.hlsl | Water
// Gerstner 波・手続きさざ波・深度吸収・屈折・空反射・接岸泡を合成する水面シェーダー
//
// WHY: 水面は Terrain / Mesh と異なり半透明で、シーン深度と HDR カラーを読む必要がある。
//      専用シェーダーに閉じることで通常マテリアルのテクスチャスロットを圧迫しない。
//
// WHY オーサリング済みテクスチャを使わないか:
//      法線マップに依存すると、水面 1 枚ごとにタイリングとスクロール速度を詰め直す必要があり、
//      大きさの違う水面を並べた瞬間にさざ波の粒度が揃わなくなる。さざ波・泡のムラは
//      ワールド座標で手続き生成し、反射は空連動 IBL キューブマップから引く。
//      これで水面はアセット 0 個で成立し、どのスケールでも同じ細かさになる。
#include "Common/Binding.hlsli"
#include "Common/Random.hlsli"

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 4
// Water SSR はピクセルシェーダー内で走るため、全画面 Compute SSR より低い上限にして水面の面積負荷を抑える。
#define WATER_SSR_MAX_STEPS 16

struct PointLightData
{
    float3 position;
    float  range;
    float3 color;
    float  intensity;
};

struct SpotLightData
{
    float3 position;
    float  range;
    float3 direction;
    float  innerCos;
    float3 color;
    float  outerCos;
    float  intensity;
    float3 _pad;
};

cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float    waterSsrEnabled;
    float    isOrthographic;
    float    _camPad;
};

// Water は半透明 Forward 描画で GBuffer に法線を書かないため、通常の SSR Compute の
// 反射元にはなれない。共通設定だけを受け取り、水面 PS 内でコピー済み深度を追跡する。
// AdvancedGraphicsConstants (b8) — 定義は Common/AdvancedGraphicsConstants.hlsli が持つ。
// WHY 手書きの部分コピーをやめたか: 先頭 8 フィールドだけを写していたため、
//     末尾へ足したフィールド (画面空間 AO / 接触影) が Water からは読めなかった。
#include "Common/AdvancedGraphicsConstants.hlsli"

// WaterCB は C++ の WaterCB と 16 byte 単位で同期する。
// WHAT: Vector4 パックにして、HLSL/C++ 間の暗黙パディング差をなくす。
cbuffer WaterCB : register(CB_OBJECT)
{
    float4x4 g_worldMatrix;
    float4x4 g_wvpMatrix;
    float4   g_shallowColorDepth;    // xyz=浅瀬色, w=浅瀬深度
    float4   g_deepColorDepth;       // xyz=深部色, w=深部深度
    float4   g_surfaceParams;        // x=opacity, y=reflectivity, z=fresnelBias, w=fresnelPower
    float4   g_normalParams;         // w=normalStrength (xyz は旧 normalMap スクロール枠・未使用)
    float4   g_timeParams;           // w=time (xyz は旧 normalMap スクロール枠・未使用)
    float4   g_foamParams;           // x=threshold, y=fade, z=strength, w=foamNoiseScale
    float4   g_refractionFlowParams; // x=refraction, y=flowSpeed, zw=外周フェード幅 (UV 単位, 0 で無効)
    float4   g_waveDir[4];           // xy=direction, z=steepness, w=enabled
    float4   g_waveParams[4];        // x=amplitude, y=wavelength, z=omega, w=k
    float4   g_detailParams;         // x=detailScale, y=detailSpeed, z=detailStrength, w=smoothness
    float4   g_sssParams;            // xyz=透過光の色, w=強度
    float4   g_reflectParams;        // x=skyReflection(0で無効), y=iblMaxMip, z=水面基準Y, w=波高合計
    float4   g_flowParams;           // xy=流れ方向(正規化), z=未使用, w=未使用
};

// ユーザー定義エフェクトパラメータ。MaterialComponent.paramData にマップされる。
// Script から mc->SetParam<float>("rimGlowStrength", val) で動的に変更可能。
// WHY: WaterCB はシステム管理（毎フレーム上書き）だが、このバッファはユーザーが自由に書き換える。
// NOTE: HLSL cbuffer のパッキング規則に従い、_pad で 16B 境界を揃えること。
// 48 bytes (3 x float4 rows). Script: mc->SetParam<float>("specularExponent", 64.0f)
cbuffer MaterialConstants : register(b2)
{
    float  rimGlowStrength;    // Row0: リムグロー強度         default 0.40
    float  minShallowAlpha;    //       浅瀬の最小アルファ     default 0.65
    float  specularStrength;   //       スペキュラー強度       default 0.75
    float  specularExponent;   //       スペキュラー指数       default 80.0
    float3 skyReflectTint;     // Row1: 空反射ベース色 RGB     default (0.45, 0.82, 1.0)
    float  envMapBlend;        //       環境マップ混合率       default 0.35
    float3 rippleRingColor;    // Row2: 波紋リング色 RGB       default (0.88, 0.97, 1.0)
    float  rippleRingStrength; //       波紋リング強度         default 0.72
}

cbuffer LightConstants : register(CB_LIGHT)
{
    float3         lightDir;       float _lightPad;
    float3         lightColor;     float lightIntensity;
    PointLightData pointLights[MAX_POINT_LIGHTS];
    SpotLightData  spotLights[MAX_SPOT_LIGHTS];
    int            pointLightCount;
    int            spotLightCount;
    float2         _lightPad2;
    float3         ambientColor;
    float          _ambientPad;
};

// ShadowConstants (b4) — カスケード配列を含むためレイアウトは 1 か所で定義する。
#include "Common/ShadowConstants.hlsli"

#include "Rendering/Shadow.hlsli"
#include "Rendering/Lighting.hlsli"

// 水面が読むテクスチャは「エンジンが生成するもの」だけ。オーサリング資産は要らない。
Texture2D g_foamMask   : register(t3); // 岸沿いの泡マスク (地形高さから CPU 生成)
Texture2D g_sceneDepth : register(t5); // Water 描画直前の深度コピー
Texture2D g_sceneColor : register(t6); // Water 描画直前の HDR コピー
Texture2D g_rippleTex  : register(t8); // 着水波紋 (CPU 生成)
Texture2D<float> g_shadowMap : register(t9);
TextureCube g_skyReflection : register(TEX_IBL_PREFILTER); // 空連動 IBL の事前フィルタ済みキューブ

// サンプラーのレジスタ割り当ては Binding.hlsli の SAMPLER_* に従う。
// WHY: DX12 は静的サンプラーを Root Signature へ焼き込むため、レジスタごとの意味は
//      全シェーダーで一致していなければならない。Water だけ独自番号を使うと、
//      DX12 では比較サンプラーの位置に通常サンプラーが来て影が壊れる。
SamplerState g_sampler      : register(SAMPLER_DEFAULT);
SamplerState g_samplerClamp : register(SAMPLER_LINEAR_CLAMP); // 環境反射のサンプルも兼ねる
SamplerComparisonState g_shadowSampler : register(SAMPLER_SHADOW);

struct WaterVSInput
{
    float3 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct WaterPSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float2 uv         : TEXCOORD1;
    float3 normal     : TEXCOORD2;
    float3 tangent    : TEXCOORD3;
    float3 binormal   : TEXCOORD4;
    float4 screenPos  : TEXCOORD5;
    float  waveCrest  : TEXCOORD6; // 0=谷, 1=うねりの山。透過光の強さに使う
};

float3 GerstnerDisplace(float4 dirData, float4 params, float3 pos, float time, inout float3 tangent, inout float3 binormal)
{
    // WHAT: deep-water Gerstner 波を 1 本評価し、同時に解析微分で TBN を更新する。
    // WHY: CPU 頂点へ法線・接線を持たせず、波変位後の正しい法線を GPU で復元するため。
    if (dirData.w <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    float2 D = normalize(dirData.xy);
    float  Q = saturate(dirData.z);
    float  A = params.x;
    float  k = params.w;
    float  omega = params.z;
    float  phi = k * dot(D, pos.xz) - omega * time;
    float  s = sin(phi);
    float  c = cos(phi);

    tangent.x  -= Q * D.x * D.x * k * A * s;
    tangent.y  += D.x * k * A * c;
    tangent.z  -= Q * D.x * D.y * k * A * s;
    binormal.x -= Q * D.x * D.y * k * A * s;
    binormal.y += D.y * k * A * c;
    binormal.z -= Q * D.y * D.y * k * A * s;

    return float3(Q * A * D.x * c, A * s, Q * A * D.y * c);
}

WaterPSInput VSMain(WaterVSInput v)
{
    WaterPSInput o;
    float time = g_timeParams.w;
    float3 worldPos = mul(float4(v.position, 1.0f), g_worldMatrix).xyz;
    float3 tangent = float3(1.0f, 0.0f, 0.0f);
    float3 binormal = float3(0.0f, 0.0f, 1.0f);
    float3 disp = float3(0.0f, 0.0f, 0.0f);

    [unroll]
    for (int i = 0; i < 4; ++i)
        disp += GerstnerDisplace(g_waveDir[i], g_waveParams[i], worldPos, time, tangent, binormal);

    worldPos += disp;
    tangent = normalize(tangent);
    binormal = normalize(binormal);
    // tangent = ∂P/∂x, binormal = ∂P/∂z。法線は cross(binormal, tangent) で +Y を向く。
    // WHY: 逆順 cross(tangent, binormal) は平坦な水面で (0,-1,0) を返す。以前はこれで法線が
    //      真下を向き、NdotV が常に 0 → フレネル飽和・スペキュラ消失・影の向き反転を起こしていた。
    float3 normal = normalize(cross(binormal, tangent));

    o.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    o.worldPos = worldPos;
    o.uv = v.uv;
    o.normal = normal;
    o.tangent = tangent;
    o.binormal = binormal;
    o.screenPos = o.svPosition;
    // 変位そのものから山の高さを取る。親 Transform があっても基準面がずれない。
    o.waveCrest = saturate(disp.y / max(g_reflectParams.w, 0.01f));
    return o;
}

// 値ノイズと解析勾配を同時に返す (xy=∂/∂p, z=値)。
// WHY: 法線を得るのに近傍を 3 回サンプルする必要がなく、1 セルぶんの計算で勾配が出る。
float3 WaterNoiseD(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u  = f * f * (3.0f - 2.0f * f);
    float2 du = 6.0f * f * (1.0f - f);

    float a = Hash2D(i);
    float b = Hash2D(i + float2(1.0f, 0.0f));
    float c = Hash2D(i + float2(0.0f, 1.0f));
    float d = Hash2D(i + float2(1.0f, 1.0f));

    float k1 = b - a;
    float k2 = c - a;
    float k3 = a - b - c + d;
    return float3(du * (float2(k1, k2) + k3 * u.yx),
                  a + k1 * u.x + k2 * u.y + k3 * u.x * u.y);
}

// オクターブごとの固有ドリフト。同じ向きに揃うと縞が流れて見えるため方向をばらす。
#define WATER_DETAIL_OCTAVES 5
static const float2 kWaterDrift[WATER_DETAIL_OCTAVES] = {
    float2( 0.31f,  0.17f), float2(-0.23f,  0.41f),
    float2( 0.47f, -0.29f), float2(-0.37f, -0.13f),
    float2( 0.11f,  0.53f)
};
// 振幅 0.55^i (i = 0..4) の総和。途中で打ち切っても正規化がぶれないよう定数で持つ。
static const float kWaterDetailNorm = 2.110381f;

// さざ波の高さ勾配 (∂h/∂x, ∂h/∂z) をワールド XZ で積む。
// WHY ワールド座標: UV で評価すると extent の違う水面どうしでさざ波の細かさが揃わない。
//
// @param footprint このピクセルが覆うワールド距離 [m]。遠いほど・浅い角度ほど大きい。
// @ret   xy = 勾配, z = Nyquist で落とした細部の割合 (0 = 全部残った, 1 = 全部落ちた)。
float3 WaterDetailGradient(float2 worldXZ, float time, float footprint)
{
    float scale = max(g_detailParams.x, 1.0e-4f);
    float speed = g_detailParams.y;
    float2 flow = g_flowParams.xy * (speed * time);

    // オクターブごとに座標を回し、値ノイズの格子が縞として残らないようにする。
    float2x2 rot = float2x2(1.0f, 0.0f, 0.0f, 1.0f);
    const float2x2 step = float2x2(0.80f, -0.60f, 0.60f, 0.80f);

    float2 grad = float2(0.0f, 0.0f);
    float amp = 1.0f, freq = scale, kept = 0.0f;

    [loop]
    for (int i = 0; i < WATER_DETAIL_OCTAVES; ++i)
    {
        // 1 ピクセルに 1 周期以上入るオクターブは、平均すれば «ざらつき» しか残らない。
        // WHY 落とすか: 残すとカメラが動くたびに遠景の水面が総毛立って明滅する。
        //      周期がピクセルの 2 倍を切ったところから滑らかに寝かせ、遠くの水面は
        //      «細部が消えて鏡に近づく» という実際の見え方へ収束させる。
        // freq は単調増加なので、ここで潰れた先のオクターブはすべて潰れている。
        float fade = saturate(1.0f - footprint * freq * 2.0f);
        if (fade <= 0.0f) break;

        float2 q = mul(rot, worldXZ) * freq + (flow + kWaterDrift[i] * (time * speed)) * freq;
        float3 n = WaterNoiseD(q);
        // 勾配は回した座標系で出るので、rot の逆 (= 転置) を掛けて元の軸へ戻す。
        grad += mul(n.xy, rot) * (amp * freq * fade);
        kept += amp * fade;
        rot   = mul(step, rot);
        amp  *= 0.55f;
        freq *= 2.07f;
    }

    const float invNorm = 1.0f / kWaterDetailNorm;
    return float3(grad * invNorm, saturate(1.0f - kept * invNorm));
}

// 接空間法線。tangent = +X(world), binormal = +Z(world) なので xy がそのまま world XZ に対応する。
// @ret xyz = 接空間法線, w = 落とした細部量 (specular AA で粗さへ回す)。
float4 SampleWaterNormal(float2 worldXZ, float2 uv, float time, float footprint)
{
    float3 detail = WaterDetailGradient(worldXZ, time, footprint);
    float2 grad = detail.xy * max(g_detailParams.z, 0.0f);
    float3 waveNormal = normalize(float3(-grad.x, -grad.y, 1.0f));

    float2 ripple = g_rippleTex.Sample(g_samplerClamp, uv).rg * 2.0f - 1.0f;
    float3 rippleNormal = float3(ripple.xy, sqrt(saturate(1.0f - dot(ripple.xy, ripple.xy))));
    waveNormal = normalize(waveNormal + rippleNormal * 0.5f);
    return float4(normalize(lerp(float3(0.0f, 0.0f, 1.0f), waveNormal, g_normalParams.w)), detail.z);
}

// 水面メッシュ外周のフェード係数。矩形の縁でアルファを落とす。
//
// WHY 必要か: 水面はゼロ厚のシートなので、矩形の縁では «板の切り口» のような直線で
//     シーンが切り替わる。水に厚みが無いことがその 1 本の線で分かってしまう。
//     縁へ向かって消していけば «岸に向かって薄くなる水» として読めるようになる。
// WHY 幅を UV で受け取るか: CPU 側が «メートル / extent» を軸ごとに割って渡す。
//     こうすると extent が違う軸でも、縦横で帯の実寸が揃う。
// 上限 0.15: 小さな水面で幅を大きく取ると、中央まで薄まって水が消える。
float WaterEdgeFade(float2 uv, float2 widthUV)
{
    float2 w = min(max(widthUV, 0.0f), 0.15f);
    float2 d = min(uv, 1.0f - uv); // 最も近い縁までの距離 [0, 0.5]
    float fx = w.x > 1.0e-5f ? smoothstep(0.0f, w.x, d.x) : 1.0f;
    float fy = w.y > 1.0e-5f ? smoothstep(0.0f, w.y, d.y) : 1.0f;
    return fx * fy;
}

float LinearizeDepth(float rawDepth)
{
    // 平行投影では深度が既に線形。透視用の逆数式を通すと視空間 Z が狂い、
    // 水深フェード・屈折・水面 SSR が一斉にずれる。
    if (isOrthographic > 0.5f) return nearZ + rawDepth * (farZ - nearZ);
    return (nearZ * farZ) / max(farZ - rawDepth * (farZ - nearZ), 0.0001f);
}

// 水面専用 SSR。Water 描画直前の sceneDepth / sceneColor を使うため、現在の水面を
// 読み戻す競合を起こさず、既に描画済みの不透明・半透明オブジェクトを反射できる。
float4 TraceWaterSSR(float3 worldPos, float3 normal)
{
    if (waterSsrEnabled < 0.5f || ssrIntensity <= 0.0f || ssrSteps <= 0)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    float3 incident = normalize(worldPos - cameraPos);
    float3 rayDirVS = normalize(mul(float4(reflect(incident, normal), 0.0f), view).xyz);
    float3 rayPosVS = mul(float4(worldPos, 1.0f), view).xyz + rayDirVS * max(ssrThickness, 0.02f);
    // WHY: RenderSettings の SSR 品質をそのまま水面 PS に流すと、広い水面で step 数×ピクセル数の負荷が跳ねる。
    int waterSsrSteps = min(ssrSteps, WATER_SSR_MAX_STEPS);
    float stepLength = ssrMaxDistance / max((float)waterSsrSteps, 1.0f);

    [loop]
    for (int step = 0; step < waterSsrSteps; ++step)
    {
        rayPosVS += rayDirVS * stepLength;
        if (rayPosVS.z <= nearZ || rayPosVS.z >= farZ)
            break;

        float4 clip = mul(float4(rayPosVS, 1.0f), projection);
        if (clip.w <= 0.0f)
            break;

        float2 rayUV = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
        if (any(rayUV <= 0.0f) || any(rayUV >= 1.0f))
            break;

        float sceneRawDepth = g_sceneDepth.SampleLevel(g_samplerClamp, rayUV, 0).r;
        if (sceneRawDepth >= 0.9999f)
            continue;

        float sceneViewDepth = LinearizeDepth(sceneRawDepth);
        float depthDelta = rayPosVS.z - sceneViewDepth;
        float hitThickness = max(ssrThickness, stepLength * abs(rayDirVS.z));
        if (depthDelta >= 0.0f && depthDelta <= hitThickness)
        {
            // 画面端では不安定なヒットを環境反射へ滑らかにフォールバックする。
            float2 edgeDistance = min(rayUV, 1.0f - rayUV);
            float confidence = saturate(min(edgeDistance.x, edgeDistance.y) * 12.0f);
            return float4(g_sceneColor.SampleLevel(g_samplerClamp, rayUV, 0).rgb,
                          confidence * saturate(ssrIntensity));
        }
    }
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
}

float4 PSMain(WaterPSInput p) : SV_Target0
{
    float time = g_timeParams.w;
    float2 screenUV = p.screenPos.xy / p.screenPos.w * float2(0.5f, -0.5f) + 0.5f;

    // 1 ピクセルが覆うワールド距離。さざ波の LOD と specular AA の両方がこれを基準にする。
    float2 footprintDX = ddx(p.worldPos.xz);
    float2 footprintDY = ddy(p.worldPos.xz);
    float  footprint   = max(length(footprintDX), length(footprintDY));

    float4 normalSample  = SampleWaterNormal(p.worldPos.xz, p.uv, time, footprint);
    float3 tangentNormal = normalSample.xyz;
    float  lostDetail    = normalSample.w;

    float3x3 tbn = float3x3(normalize(p.tangent), normalize(p.binormal), normalize(p.normal));
    float3 N = normalize(mul(tangentNormal, tbn));
    float3 V = normalize(cameraPos - p.worldPos);
    // 水面下から見上げているときは面の裏側を見ている (PSO は両面描画)。
    // WHY 幾何法線で判定するか: さざ波を含む N で判定すると、浅い角度のピクセルだけが
    //     ばらばらに反転して斑になる。向きの決定は滑らかな頂点法線に任せる。
    if (dot(p.normal, V) < 0.0f) N = -N;
    float NdotV = saturate(dot(N, V));

    // Schlick フレネル。g_surfaceParams.z を水の F0 (実測 0.02 前後) として扱う。
    // WHY: 以前の bias + (1-bias)*pow は grazing 角以外でも下駄を履かせていたため、
    //      真上から見た水面まで一定量の反射が乗って「板に空が映っている」見え方になっていた。
    float f0 = saturate(g_surfaceParams.z);
    float fresnel = f0 + (1.0f - f0) * pow(saturate(1.0f - NdotV), max(g_surfaceParams.w, 1.0f));
    fresnel = saturate(fresnel * g_surfaceParams.y * 2.0f);

    float rawSceneDepth = g_sceneDepth.Sample(g_samplerClamp, screenUV).r;
    // WHAT: 深度が far plane に張り付く場所は、Terrain / Mesh が存在しない背景ピクセルとして扱う。
    // WHY: 背景の skydome 色を屈折色として読むと、水面が空そのものに溶けてしまう。
    //      Unity の Ocean 的な見え方に寄せるため、背景ピクセルは「底が見えない深い水」として描く。
    float backgroundMask = step(0.9999f, rawSceneDepth);
    float linearSceneDepth = LinearizeDepth(rawSceneDepth);
    float linearSurfDepth = max(p.screenPos.w, 0.0001f);
    float waterDepth = lerp(max(0.0f, linearSceneDepth - linearSurfDepth), g_deepColorDepth.w, backgroundMask);
    float depthFactor = smoothstep(0.0f, 1.0f, saturate(waterDepth / max(g_deepColorDepth.w, 0.0001f)));

    float shallowFactor = smoothstep(0.0f, 1.0f, saturate(waterDepth / max(g_shallowColorDepth.w, 0.0001f)));
    float3 waterColor = lerp(g_shallowColorDepth.xyz, g_deepColorDepth.xyz, depthFactor);
    float3 absorptionTint = lerp(float3(1.0f, 1.0f, 1.0f), g_deepColorDepth.xyz, saturate(depthFactor * 0.45f));

    // WHAT: Water 直前の HDR スナップショットを、水面法線でずらした screen UV から読む。
    // WHY: 現在描画中の HDR RT を直接読むと DX11 の read/write 競合になるため、コピー済み sceneColor を参照する。

    // WHAT: スクリーンスペース屈折は水面法線で HDR カラー参照 UV をずらす。
    // WHY: 水底ジオメトリを再描画せず、透明水面らしい歪みを安価に得る。
    // sceneColor は Water 描画直前にコピーされた HDR で、同一 RT の read/write 競合を避ける。
    float refractionMask = shallowFactor * (1.0f - backgroundMask);
    float2 refrOffset = tangentNormal.xy * g_refractionFlowParams.x * (1.0f - saturate(fresnel)) * refractionMask;
    float2 refrUV = saturate(screenUV + refrOffset);

    // WHAT: 屈折先のピクセルが水面より手前にある（= カメラと水面の間に物体がある）場合は屈折させない。
    // WHY: そのまま UV をずらすと手前オブジェクトのシルエットが水中に滲み出す典型的なアーティファクトになる。
    //      屈折先の深度が水面より手前なら、その物体は水中ではないのでオフセットを破棄し素の screenUV を使う。
    float refrRawDepth    = g_sceneDepth.Sample(g_samplerClamp, refrUV).r;
    float refrLinearDepth = LinearizeDepth(refrRawDepth);
    if (refrLinearDepth < linearSurfDepth)
        refrUV = screenUV;
    float3 refractColor = g_sceneColor.Sample(g_samplerClamp, refrUV).rgb;
    // 背景 (底が見えない) ピクセルだけ水色そのものへ倒し、それ以外は素直に透過色を使う。
    // WHY: 以前は refractColor の輝度がゼロに近いほど waterColor へ寄せていたため、
    //      水中の暗い岩や影の部分が水色に塗り潰されて沈んだ物体が見えなくなっていた。
    float3 baseRefract = lerp(refractColor * absorptionTint, waterColor, backgroundMask);
    waterColor = lerp(baseRefract, waterColor, saturate(depthFactor * 0.65f));

    // 空反射は空連動 IBL の事前フィルタ済みキューブから引く (専用の環境テクスチャは不要)。
    // smoothness が低いほど粗い mip を引き、ざらついた水面では反射がぼける。
    float3 R = reflect(-V, N);
    // 落とした細部はサブピクセルの法線ばらつきそのものなので、粗さへ移す (specular AA)。
    // WHY: 移さないと «消えた細部» が反射とハイライトからだけ抜け落ち、遠景の水面が
    //      磨いた金属板になる。粗さに戻せば «細かい波で反射がぼける» 側に着地する。
    float  roughness = saturate(1.0f - g_detailParams.w);
    float  roughnessAA = saturate(roughness + lostDetail * (1.0f - roughness) * 0.70f);
    float3 reflectColor = skyReflectTint;
    [branch]
    if (g_reflectParams.x > 0.001f)
    {
        float3 sky = g_skyReflection.SampleLevel(g_samplerClamp, R,
                                                 roughnessAA * (float)max(iblMaxMipLevel, 0)).rgb;
        reflectColor = lerp(skyReflectTint, sky, saturate(g_reflectParams.x));
    }

    // 反射ウェイト(フレネル)を先に求め、SSR は寄与が実際に見えるピクセルだけトレースする。
    // WHY: TraceWaterSSR は上限付きでも複数回レイマーチする WaterForward の主コスト。水面を見下ろす
    //      (NdotV 大 → 低フレネル) ピクセルは反射がほぼ見えないため、レイマーチを丸ごと省いても
    //      結果はほぼ不変。背景ピクセルはヒット候補が薄く長い空走査になりやすいため環境反射へフォールバックする。
    //      逆に浅い角度(高フレネル・反射が目立つ)では従来どおりトレースする。
    //      SSR は SampleLevel(明示 LOD) を使うため分岐内でも勾配の問題は起きない。
    float reflectionWeight = saturate(fresnel) * lerp(1.0f, 0.45f, backgroundMask);
    [branch]
    if (reflectionWeight > 0.04f && backgroundMask < 0.5f)
    {
        float4 ssrReflection = TraceWaterSSR(p.worldPos, N);
        reflectColor = lerp(reflectColor, ssrReflection.rgb, ssrReflection.a);
    }
    float3 color = lerp(waterColor, reflectColor, reflectionWeight);

    float3 L = normalize(-lightDir);
    // WHAT: Water は半透明なので影を強く乗算せず、直射光と浅い水面の明るさを中心に抑える。
    // WHY: 完全な黒影にすると水面下の屈折色まで不自然に消えるため、shadow は「太陽光の弱まり」として扱う。
    float shadow = ComputeShadow(g_shadowMap, g_shadowSampler, p.worldPos,
        lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    // Forward の画面空間 AO / 接触影。Deferred では b8 が 0 なので素通りする。
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
    color *= lerp(0.72f, 1.0f, shadow);

    // 太陽のきらめき。smoothness から指数を決め、Blinn-Phong の正規化係数を上限付きで掛ける。
    // WHY: 正規化しないと鏡面に近い水面でもハイライトが lightIntensity 止まりで沈む。逆に
    //      無制限に正規化すると 1 ピクセルだけ数百の輝度が出て Bloom がちらつく。上限で挟む。
    float3 H = normalize(L + V);
    float NdotH = saturate(dot(N, H));
    // 指数は粗さ (specular AA 込み) から引く。遠景でハイライトが 1 ピクセルに縮んで
    // 這うのを防ぐため、細部が消えたぶんだけ山を広く・低くする。
    float specPower = exp2(saturate(1.0f - roughnessAA) * 10.0f + 2.0f); // 4 .. 4096
    float specNorm  = min((specPower + 8.0f) * 0.125f, 24.0f);
    float specular  = pow(NdotH, specPower) * specNorm * max(lightIntensity, 0.0f) * shadow;
    color += lightColor * specular * specularStrength * lerp(0.65f, 1.0f, shadow);

    // 波の背面から透ける光 (subsurface)。うねりの山ほど強く、太陽を背にしたとき最大になる。
    // WHY: 海面が「光を通す液体」に見えるかはここで決まる。反射とスペキュラだけだと
    //      金属板のような硬い水面になり、Unity の Ocean との差が最も出る部分。
    float sss = pow(saturate(dot(V, -L)), 4.0f) * p.waveCrest * saturate(g_sssParams.w);
    color += g_sssParams.rgb * lightColor * sss * lerp(0.4f, 1.0f, shadow);

    // Forward+ / Deferred+ の局所光。水面は透明材質なので、局所光の影は共通影とは分離し、
    // 水色の拡散寄与だけを加算する。Deferred 経路でも Water は HDR へ直接描くためここで評価する。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        color += Lighting_Lambert_Direct(N, ps.L, waterColor,
            ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END

    float rim = pow(1.0f - NdotV, 3.0f) * rimGlowStrength;
    color += lerp(waterColor, skyReflectTint, 0.35f) * rim;

    // WHAT: 波打ち際（地形と水面が交差する浅瀬）に発生する接岸泡。
    // WHY: g_foamParams.x(threshold)/.y(fade) はこれまでシェーダー内で未使用だった。
    //      waterDepth が threshold より浅いほど泡を強くし、岸辺に沿った白い帯を作ることで
    //      Unity の Ocean 的な接岸表現に寄せる。テクスチャ泡と max 合成してムラを残す。
    float foamThreshold = g_foamParams.x;
    float foamFade      = max(g_foamParams.y, 0.0001f);
    float shoreFoam     = (1.0f - smoothstep(foamThreshold, foamThreshold + foamFade, waterDepth)) * (1.0f - backgroundMask);

    float foamMaskVal = g_foamMask.Sample(g_samplerClamp, p.uv).r;
    // 泡のムラも手続きノイズで作る。ワールド座標なので水面の大きさに依らず粒が揃う。
    float2 foamP = p.worldPos.xz * max(g_foamParams.w, 1.0e-4f)
                 + g_flowParams.xy * (time * 0.35f);
    float foamTexVal = saturate(WaterNoiseD(foamP).z * 1.6f);
    // うねりの頂点で崩れる白波。岸が無い外洋でも波が «立って» 見えるかはここで決まる。
    float crestFoam = smoothstep(0.45f, 0.90f, p.waveCrest) * 0.45f;
    float foamAmount = max(max(foamMaskVal, shoreFoam), crestFoam) * foamTexVal;
    float foam = smoothstep(0.05f, 1.0f, foamAmount) * g_foamParams.z * lerp(0.80f, 1.0f, shadow);
    float3 foamColor = lerp(float3(0.72f, 0.88f, 0.92f), float3(1.0f, 1.0f, 1.0f), saturate(foamTexVal));
    color = lerp(color, foamColor, saturate(foam));

    float2 rippleRG = g_rippleTex.Sample(g_samplerClamp, p.uv).rg * 2.0f - 1.0f;
    float rippleRing = saturate(length(rippleRG) * rippleRingStrength);
    color = lerp(color, rippleRingColor, rippleRing);
    // NOTE: 以前はここで色を事前圧縮 (color/(1+color*0.18)) していたが、水面は HDR バッファへ
    //       ブレンド描画され、露出・ACES トーンマップは Composite パスが一括で行う。事前圧縮すると
    //       スペキュラ／きらめき／太陽反射が Bloom に乗らず平坦になるため、リニア HDR のまま出力する。
    color = max(color, 0.0f);

    float alpha = g_surfaceParams.x * lerp(minShallowAlpha, 1.0f, depthFactor);
    alpha = max(alpha, backgroundMask * 0.92f);
    alpha = saturate(max(alpha, max(foam * 0.9f, rippleRing * 0.95f)));

    // 外周フェードは «最後に» 掛ける。上の max 群より前だと、泡や背景マスクが
    // 縁のアルファを持ち上げ直してしまい、切り口の線がそのまま残る。
    alpha *= WaterEdgeFade(p.uv, g_refractionFlowParams.zw);
    // WHY 完全な透明を捨てるか: 水面は深度を書く (自分自身の重なりを解決するため)。
    //     フェードで見えなくなった縁がそのまま深度を書くと、その裏にある半透明や
    //     デカールを «見えない板» が遮る。絵に出ない画素は深度も残さない。
    clip(alpha - 0.003f);
    return float4(color, alpha);
}
