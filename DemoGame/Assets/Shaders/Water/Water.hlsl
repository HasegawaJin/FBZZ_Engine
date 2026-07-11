// FBZZ Engine
// Water.hlsl | Water
// Gerstner 波・深度グラデーション・泡・屈折・フローマップ・波紋を合成する水面シェーダー
//
// WHY: 水面は Terrain / Mesh と異なり半透明で、シーン深度と HDR カラーを読む必要がある。
//      専用シェーダーに閉じることで通常マテリアルのテクスチャスロットを圧迫しない。
#include "Common/Binding.hlsli"

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 4

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
    float2   _camPad;
};

// Water は半透明 Forward 描画で GBuffer に法線を書かないため、通常の SSR Compute の
// 反射元にはなれない。共通設定だけを受け取り、水面 PS 内でコピー済み深度を追跡する。
cbuffer AdvancedGraphicsConstants : register(CB_ADVANCED_GRAPHICS)
{
    float4 _iblParams;
    float  ssrMaxDistance;
    float  ssrThickness;
    int    ssrSteps;
    float  ssrIntensity;
};

// WaterCB は C++ の WaterCB と 16 byte 単位で同期する。
// WHAT: Vector4 パックにして、HLSL/C++ 間の暗黙パディング差をなくす。
cbuffer WaterCB : register(CB_OBJECT)
{
    float4x4 g_worldMatrix;
    float4x4 g_wvpMatrix;
    float4   g_shallowColorDepth;    // xyz=浅瀬色, w=浅瀬深度
    float4   g_deepColorDepth;       // xyz=深部色, w=深部深度
    float4   g_surfaceParams;        // x=opacity, y=reflectivity, z=fresnelBias, w=fresnelPower
    float4   g_normalMap1Params;     // xy=scroll, z=tiling, w=normalStrength
    float4   g_normalMap2Params;     // xy=scroll, z=tiling, w=time
    float4   g_foamParams;           // x=threshold, y=fade, z=strength, w=tiling
    float4   g_refractionFlowParams; // x=refraction, y=flowSpeed, z=flowTiling, w=enableFlowMap
    float4   g_waveDir[4];           // xy=direction, z=steepness, w=enabled
    float4   g_waveParams[4];        // x=amplitude, y=wavelength, z=omega, w=k
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
};

cbuffer ShadowConstants : register(CB_SHADOW)
{
    float4x4 lightViewProjection;
    float2   shadowMapTexelSize;
    float    shadowBias;
    float    shadowStrength;   // 0=影なし, 1=完全な影
    int      shadowPcfRadius;  // PCF カーネル半径: 0=ハード, 1=3x3, 2=5x5, 3=7x7
    float    _shadowPcfPad[3];
};

#include "Rendering/Shadow.hlsli"

Texture2D g_normalMap1 : register(t0);
Texture2D g_normalMap2 : register(t1);
Texture2D g_foamTex    : register(t2);
Texture2D g_foamMask   : register(t3);
Texture2D g_envTex     : register(t4);
Texture2D g_sceneDepth : register(t5);
Texture2D g_sceneColor : register(t6);
Texture2D g_flowMap    : register(t7);
Texture2D g_rippleTex  : register(t8);
Texture2D<float> g_shadowMap : register(t9);

SamplerState g_sampler      : register(s0);
SamplerState g_samplerClamp : register(s1);
SamplerState g_samplerEnv   : register(s2);
SamplerComparisonState g_shadowSampler : register(s3);

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
    float time = g_normalMap2Params.w;
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
    float3 normal = normalize(cross(tangent, binormal));

    o.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    o.worldPos = worldPos;
    o.uv = v.uv;
    o.normal = normal;
    o.tangent = tangent;
    o.binormal = binormal;
    o.screenPos = o.svPosition;
    return o;
}

float3 SampleWaterNormal(float2 uv, float time)
{
    float enableFlow = g_refractionFlowParams.w;
    float3 waveNormal;

    if (enableFlow > 0.5f)
    {
        // WHAT: Valve 方式の 2 フェーズ flow map。UV リセットの継ぎ目を三角波ブレンドで隠す。
        float2 flow = g_flowMap.Sample(g_samplerClamp, uv * g_refractionFlowParams.z).rg * 2.0f - 1.0f;
        float phase0 = frac(time * g_refractionFlowParams.y);
        float phase1 = frac(time * g_refractionFlowParams.y + 0.5f);
        float blend = abs(2.0f * phase0 - 1.0f);
        float2 uv0 = uv * g_normalMap1Params.z + flow * phase0;
        float2 uv1 = uv * g_normalMap1Params.z + flow * phase1;
        float3 n0 = g_normalMap1.Sample(g_sampler, uv0).rgb * 2.0f - 1.0f;
        float3 n1 = g_normalMap1.Sample(g_sampler, uv1).rgb * 2.0f - 1.0f;
        waveNormal = normalize(lerp(n0, n1, blend));
    }
    else
    {
        float2 uv1 = uv * g_normalMap1Params.z + g_normalMap1Params.xy * time;
        float2 uv2 = uv * g_normalMap2Params.z + g_normalMap2Params.xy * time;
        float3 n1 = g_normalMap1.Sample(g_sampler, uv1).rgb * 2.0f - 1.0f;
        float3 n2 = g_normalMap2.Sample(g_sampler, uv2).rgb * 2.0f - 1.0f;
        waveNormal = normalize(float3(n1.xy + n2.xy, n1.z * n2.z));
    }

    float2 ripple = g_rippleTex.Sample(g_samplerClamp, uv).rg * 2.0f - 1.0f;
    float3 rippleNormal = float3(ripple.xy, sqrt(saturate(1.0f - dot(ripple.xy, ripple.xy))));
    waveNormal = normalize(waveNormal + rippleNormal * 0.5f);
    return normalize(lerp(float3(0.0f, 0.0f, 1.0f), waveNormal, g_normalMap1Params.w));
}

float LinearizeDepth(float rawDepth)
{
    return (nearZ * farZ) / max(farZ - rawDepth * (farZ - nearZ), 0.0001f);
}

float3 SoftWaterTonemap(float3 color)
{
    // WHAT: 強い specular / foam を緩やかに圧縮し、白飛びした板のような水面を避ける。
    // WHY: Water は HDR 上で反射と泡を加算するため、最後に軽い肩を作ると見た目が安定する。
    return color / (1.0f + color * 0.18f);
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
    float stepLength = ssrMaxDistance / max((float)ssrSteps, 1.0f);

    [loop]
    for (int step = 0; step < ssrSteps; ++step)
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
    float time = g_normalMap2Params.w;
    float2 screenUV = p.screenPos.xy / p.screenPos.w * float2(0.5f, -0.5f) + 0.5f;

    float3 tangentNormal = SampleWaterNormal(p.uv, time);
    float3x3 tbn = float3x3(normalize(p.tangent), normalize(p.binormal), normalize(p.normal));
    float3 N = normalize(mul(tangentNormal, tbn));
    float3 V = normalize(cameraPos - p.worldPos);
    float NdotV = saturate(dot(N, V));

    float fresnelTerm = pow(saturate(1.0f - NdotV), g_surfaceParams.w);
    float fresnel = saturate(g_surfaceParams.z + (1.0f - g_surfaceParams.z) * fresnelTerm);
    fresnel *= g_surfaceParams.y;

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
    float3 refractColor = g_sceneColor.Sample(g_samplerClamp, refrUV).rgb;
    // sceneColor が未コピーの場合 refractColor ≈ (0,0,0) になるため、
    // シーンの輝度がゼロのときは waterColor を透過色として代用し、
    // 設定した浅瀬/深部カラーが常に視覚に反映されるようにする。
    float sceneAvail = saturate(dot(refractColor, float3(1.0f, 1.0f, 1.0f))) * (1.0f - backgroundMask);
    float3 baseRefract = lerp(waterColor, refractColor * absorptionTint, sceneAvail);
    waterColor = lerp(baseRefract, waterColor, saturate(depthFactor * 0.65f));

    float3 envSample = g_envTex.Sample(g_samplerEnv, screenUV + tangentNormal.xy * 0.015f).rgb;
    float envAvailable = saturate(dot(envSample, float3(1.0f, 1.0f, 1.0f)));
    float3 reflectColor = lerp(skyReflectTint, envSample, envMapBlend * envAvailable);
    float4 ssrReflection = TraceWaterSSR(p.worldPos, N);
    reflectColor = lerp(reflectColor, ssrReflection.rgb, ssrReflection.a);
    float reflectionWeight = saturate(fresnel) * lerp(1.0f, 0.45f, backgroundMask);
    float3 color = lerp(waterColor, reflectColor, reflectionWeight);

    float3 L = normalize(-lightDir);
    // WHAT: Water は半透明なので影を強く乗算せず、直射光と浅い水面の明るさを中心に抑える。
    // WHY: 完全な黒影にすると水面下の屈折色まで不自然に消えるため、shadow は「太陽光の弱まり」として扱う。
    float shadow = ComputeShadow(g_shadowMap, g_shadowSampler, p.worldPos,
        lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    color *= lerp(0.72f, 1.0f, shadow);

    float3 H = normalize(L + V);
    float NdotH = saturate(dot(N, H));
    float specular = pow(NdotH, max(specularExponent, 1.0f)) * lightIntensity * shadow;
    float sparkle = pow(saturate(dot(reflect(-V, N), L)), max(specularExponent * 0.45f, 1.0f));
    color += lightColor * (specular * specularStrength + sparkle * specularStrength * 0.18f) * lerp(0.65f, 1.0f, shadow);

    float rim = pow(1.0f - NdotV, 3.0f) * rimGlowStrength;
    color += lerp(waterColor, skyReflectTint, 0.35f) * rim;

    float foamMaskVal = g_foamMask.Sample(g_samplerClamp, p.uv).r;
    float foamTexVal = g_foamTex.Sample(g_sampler, p.uv * g_foamParams.w + time * 0.03f).r;
    float foam = smoothstep(0.05f, 1.0f, foamMaskVal * foamTexVal) * g_foamParams.z * lerp(0.80f, 1.0f, shadow);
    float3 foamColor = lerp(float3(0.72f, 0.88f, 0.92f), float3(1.0f, 1.0f, 1.0f), saturate(foamTexVal));
    color = lerp(color, foamColor, saturate(foam));

    float2 rippleRG = g_rippleTex.Sample(g_samplerClamp, p.uv).rg * 2.0f - 1.0f;
    float rippleRing = saturate(length(rippleRG) * rippleRingStrength);
    color = lerp(color, rippleRingColor, rippleRing);
    color = SoftWaterTonemap(max(color, 0.0f));

    float alpha = g_surfaceParams.x * lerp(minShallowAlpha, 1.0f, depthFactor);
    alpha = max(alpha, backgroundMask * 0.92f);
    alpha = saturate(max(alpha, max(foam * 0.9f, rippleRing * 0.95f)));
    return float4(color, alpha);
}
