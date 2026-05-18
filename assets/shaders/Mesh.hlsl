// FBZZ Engine
// Mesh.hlsl | fbzz::renderer
// Lambert 拡散 + Blinn-Phong 鏡面反射による簡易ライティング
// cbuffer レイアウトは Constants.hlsli に合わせること

// b0: CameraConstants (Constants.hlsli 準拠)
// Mesh.hlsl は viewProjection と cameraPos のみ使用するが、
// オフセットを合わせるため未使用フィールドも宣言する。
cbuffer CameraConstants : register(b0) {
    float4x4 _view;
    float4x4 _projection;
    float4x4 viewProjection;       // offset 128
    float4x4 _invViewProjection;
    float3   cameraPos;            // offset 256
    float    _nearZ;
    float    _farZ;
    float3   _camPad;
};

// b1: ObjectConstants (Constants.hlsli 準拠)
cbuffer ObjectConstants : register(b1) {
    float4x4 world;
    // worldInvTranspose は Mesh.hlsl では未使用 (Step 6 以降で対応)
};

// b2: MaterialConstants (Constants.hlsli 準拠)
cbuffer MaterialConstants : register(b2) {
    float4 albedo;
    float  metallic;
    float  roughness;
    float  _emissiveScale;
    uint   textureMask;  // bit0=albedo
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
    float3 tangent  : TANGENT;
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
    output.normal   = normalize(mul(input.normal, (float3x3)world));
    output.uv       = input.uv;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float4 baseColor = albedo;
    if (textureMask & 1u)
        baseColor *= albedoTex.Sample(texSampler, input.uv);

    float3 N       = normalize(input.normal);
    float3 L       = normalize(-lightDir);
    float3 V       = normalize(cameraPos - input.worldPos);
    float3 H       = normalize(L + V);

    float  NdotL   = max(dot(N, L), 0.0f);
    float3 diffuse = baseColor.rgb * lightColor * lightIntensity * NdotL;

    float  shininess = max(lerp(128.0f, 2.0f, roughness), 2.0f);
    float  NdotH     = max(dot(N, H), 0.0f);
    float3 specular  = lightColor * lightIntensity * pow(NdotH, shininess) * (1.0f - roughness) * 0.5f;

    float3 ambient   = baseColor.rgb * 0.08f;

    return float4(ambient + diffuse + specular, baseColor.a);
}
