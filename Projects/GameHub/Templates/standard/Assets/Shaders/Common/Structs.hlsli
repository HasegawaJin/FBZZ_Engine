// FBZZ Engine
// Structs.hlsli | Common
// Shared shader input and interpolator structures
#ifndef STRUCTS_HLSLI
#define STRUCTS_HLSLI

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD;
};

// 頂点カラーまで読む版。
//
// WHY VSInput へ足さず別に立てるか:
//   入力レイアウトは VS のリフレクション結果を宣言順に詰めて作られる。COLOR を
//   宣言しなければ従来どおりのオフセットになるので、色を使わないシェーダーは
//   1 行も変えずに済む。renderer::Vertex の色は末尾にあるため、この 2 つの入力は
//   同じ頂点バッファへ同時に噛み合う。
//
// 使い所: MeshBuilder で組んだ手続きメッシュ (斬撃・衝撃波・ビーム) のように、
//         色とアルファを頂点へ持たせて減衰やグラデーションを表現するもの。
struct VSInputColor
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
};

// PSInput に頂点カラーを 1 本足した版。
struct PSInputColor
{
    float4 svPosition : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float3 normal     : TEXCOORD1;
    float3 tangent    : TEXCOORD2;
    float2 uv         : TEXCOORD3;
    float4 color      : COLOR;
};

struct SkinnedVSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float3 tangent     : TANGENT;
    float2 uv          : TEXCOORD;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float3 normal     : TEXCOORD1;
    float3 tangent    : TEXCOORD2;
    float2 uv         : TEXCOORD3;
};

struct GBufferOut
{
    float4 albedoRoughness : SV_Target0;
    float4 normalMetallic  : SV_Target1;
};

struct GBufferData
{
    float3 albedo;
    float  roughness;
    float3 worldNormal;
    float  metallic;
    float3 worldPos;
    float  ao;
};

#endif // STRUCTS_HLSLI
