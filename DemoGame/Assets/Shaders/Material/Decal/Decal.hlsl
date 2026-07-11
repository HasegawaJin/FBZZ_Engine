// FBZZ Engine
// Material/Decal/Decal.hlsl | Deferred Decal
//
// フルスクリーントライアングルを 1 draw 発行し、深度バッファからワールド座標を
// 復元してデカール OBB に投影する。
// OBB 外 / 空ピクセル (depth == 1.0) のフラグメントは discard する。
//
// cbuffer:
//   b0  CameraConstants  (invViewProjection)
//   b2  DecalConstants   (invDecalWorld, albedoTint, emissive, normalStrength, alpha)
//
// Texture:
//   t0  albedo      (オプション。なければ albedoTint をそのまま使用)
//   t1  normal      (オプション。normalStrength で強度を制御)
//   t3  emissive    (オプション)
//   t7  sceneDepth
//   t13 decalMask   (bit3 有効時のみ。白ピクセル = 除外オブジェクト → discard)

#include "Common/Binding.hlsli"
#include "Common/Space.hlsli"
#include "Platform/DX11.hlsli"

Texture2D    texAlbedo    : register(TEX_ALBEDO);
Texture2D    texNormal    : register(TEX_NORMAL);
Texture2D    texEmissive  : register(TEX_EMISSIVE);
Texture2D    texDepth     : register(TEX_DEPTH);
Texture2D    texDecalMask : register(TEX_DECAL_MASK);
SamplerState sampDefault  : register(SAMPLER_DEFAULT);

// b0: カメラ定数 (invViewProjection のみ使用)
cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float3   _camPad;
};

// b2: デカール専用定数 (MaterialConstants の代わりに使用)
cbuffer DecalConstants : register(CB_MATERIAL)
{
    float4x4 invDecalWorld;   // ワールド→デカールローカル空間の変換行列
    float4   albedoTint;      // RGBA アルベドカラー
    float3   emissiveColor;   // エミッシブカラー
    float    emissiveScale;   // エミッシブ強度
    float    normalStrength;  // 法線マップ強度 [0, 1]
    float    alpha;           // フェードアルファ (ライフタイム込み)
    uint     textureMask;     // bit0=albedo  bit1=normal  bit2=emissive  bit3=decalMask
    float    _pad;
    float3   decalTangent;    // Decal local +X in world space. Normal-map T axis.
    float    _pad1;
    float3   decalBitangent;  // Decal local +Z in world space. Projected UV V axis.
    float    _pad2;
    float3   decalNormal;     // Decal projection-plane normal in world space.
    float    _pad3;
};

// b3: The HDR decal pass keeps the existing composite path and only needs
// the main directional light for normal-map shading.
cbuffer LightConstants : register(CB_LIGHT)
{
    float3 lightDir;
    float  _lightPad;
    float3 lightColor;
    float  lightIntensity;
};

struct FSTriOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

// 頂点バッファなし — SV_VertexID からフルスクリーントライアングルを生成
FSTriOut VSMain(uint id : SV_VertexID)
{
    FSTriOut o;
    o.uv         = float2((id & 1u) ? 2.0f : 0.0f,
                          (id & 2u) ? 2.0f : 0.0f);
    o.svPosition = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float3 DecodeDecalNormal(float3 normalSample)
{
    float3 tangentNormal = normalSample * 2.0f - 1.0f;
    float3 T = normalize(decalTangent);
    float3 B = normalize(decalBitangent);
    float3 N = normalize(decalNormal);
    return normalize(T * tangentNormal.x + B * tangentNormal.y + N * tangentNormal.z);
}

float ComputeDirectionalLightRatio(float3 baseNormal, float3 mappedNormal)
{
    // WHY: Decals are composited into HDR after DeferredLighting, so this pass
    // cannot update GBuffer normals. Apply only the relative lighting delta so
    // a flat normal map keeps the previous decal brightness.
    float3 L = normalize(-lightDir);
    float baseLight = 0.25f + saturate(dot(normalize(baseNormal), L)) * max(lightIntensity, 0.0f);
    float mappedLight = 0.25f + saturate(dot(normalize(mappedNormal), L)) * max(lightIntensity, 0.0f);
    return clamp(mappedLight / max(baseLight, 0.001f), 0.25f, 2.0f);
}

float4 PSMain(FSTriOut p) : SV_Target
{
    float2 uv = p.uv;

    // 空 / 背景ピクセルをスキップ
    float ndcDepth = texDepth.Sample(sampDefault, uv).r;
    if (ndcDepth >= 1.0f)
        discard;

    // receiverLayerMask で除外されたオブジェクトのピクセルをスキップ
    if ((textureMask & 8u) && texDecalMask.Sample(sampDefault, uv).r > 0.5f)
        discard;

    // 深度からワールド座標を復元
    float3 worldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);

    // デカールローカル空間へ変換
    float3 localPos = mul(float4(worldPos, 1.0f), invDecalWorld).xyz;

    // OBB 外 ([-0.5, 0.5]^3) はデカール投影しない
    if (any(abs(localPos) > 0.5f))
        discard;

    // ローカル XZ 平面に投影して UV 算出 (ローカル -Y 方向が投影軸)
    float2 decalUV = localPos.xz + 0.5f;

    // --- Albedo (テクスチャがなければ albedoTint をそのまま使用) ---
    float4 albedoSample = (textureMask & 1u)
        ? texAlbedo.Sample(sampDefault, decalUV) * albedoTint
        : albedoTint;

    float finalAlpha = albedoSample.a * alpha;
    if (finalAlpha < 0.001f)
        discard;

    float3 decalColor = albedoSample.rgb;
    if ((textureMask & 2u) && normalStrength > 0.0f)
    {
        float3 baseNormal = normalize(decalNormal);
        float3 normalSample = texNormal.Sample(sampDefault, decalUV).rgb;
        float3 mappedNormal = DecodeDecalNormal(normalSample);
        float normalRatio = ComputeDirectionalLightRatio(baseNormal, mappedNormal);
        decalColor *= lerp(1.0f, normalRatio, saturate(normalStrength));
    }

    // --- Emissive (テクスチャがなければ emissiveColor * emissiveScale のみ) ---
    float3 emissive = (textureMask & 4u)
        ? texEmissive.Sample(sampDefault, decalUV).rgb * emissiveColor * emissiveScale
        : emissiveColor * emissiveScale;

    return float4(decalColor + emissive, finalAlpha);
}
