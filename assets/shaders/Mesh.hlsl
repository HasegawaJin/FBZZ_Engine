// FBZZ Engine
// Mesh.hlsl | fbzz::renderer
// Lambert 拡散 + Blinn-Phong 鏡面反射による簡易ライティング

cbuffer CameraConstants : register(b0) {
    float4x4 viewProjection;
    float3   cameraPos;
    float    _pad;
};

cbuffer ObjectConstants : register(b1) {
    float4x4 world;
};

cbuffer MaterialConstants : register(b2) {
    float4 albedo;
    float  metallic;
    float  roughness;
    float  hasAlbedoTex;  // 1.0 = テクスチャあり、0.0 = 単色
    float  _pad2;
};

cbuffer LightConstants : register(b3) {
    float3 lightDir;
    float  _pad3;
    float3 lightColor;
    float  lightIntensity;
};

Texture2D    albedoTex  : register(t0);
SamplerState texSampler : register(s0);

struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

struct PSInput {
    float4 position  : SV_POSITION;
    float3 worldPos  : TEXCOORD0;
    float3 normal    : TEXCOORD1;
    float2 uv        : TEXCOORD2;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    float4 wp       = mul(float4(input.position, 1.0f), world);
    output.worldPos = wp.xyz;
    output.position = mul(wp, viewProjection);
    // 非一様スケールへの対応は Step 6 以降。現状は world の上位 3x3 で変換する
    output.normal   = normalize(mul(input.normal, (float3x3)world));
    output.uv       = input.uv;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    // テクスチャがあれば albedo に乗算、なければ albedo 単色を使う
    float4 baseColor = albedo;
    if (hasAlbedoTex > 0.5f)
        baseColor *= albedoTex.Sample(texSampler, input.uv);

    float3 N       = normalize(input.normal);
    float3 L       = normalize(-lightDir);
    float3 V       = normalize(cameraPos - input.worldPos);
    float3 H       = normalize(L + V);

    // Lambert 拡散
    float  NdotL   = max(dot(N, L), 0.0f);
    float3 diffuse = baseColor.rgb * lightColor * lightIntensity * NdotL;

    // Blinn-Phong 鏡面反射 (roughness が高いほど鈍い)
    float  shininess = max(lerp(128.0f, 2.0f, roughness), 2.0f);
    float  NdotH     = max(dot(N, H), 0.0f);
    float3 specular  = lightColor * lightIntensity * pow(NdotH, shininess) * (1.0f - roughness) * 0.5f;

    float3 ambient   = baseColor.rgb * 0.08f;

    return float4(ambient + diffuse + specular, baseColor.a);
}
