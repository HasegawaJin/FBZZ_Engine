// FBZZ Engine
// Detail/Detail.hlsl | VS + PS
// Terrain Detail System — Mesh / Billboard GPU Instancing シェーダー
//
// インスタンスデータ (VS t0):
//   StructuredBuffer<DetailInstance> — DrawCall.instanceBuffer 経由で VSSetShaderResources(0) にバインドされる
//   struct DetailInstance { float3 pos; float rotY; float scale; };
//
// テクスチャスロット (PS):
//   t0 = アルベドテクスチャ (TEX_ALBEDO)
//
// 定数バッファ:
//   b0 = CameraConstants (view / proj / viewProjection / cameraPos)
//   b2 = DetailMaterialCB (alphaCutoff / isBillboard)
//
// Mesh モード (isBillboard == 0):
//   頂点バッファから position / normal / uv を受け取り、
//   SV_InstanceID でインスタンスバッファを索引して回転・スケール・ワールド平行移動を適用する。
//
// Billboard モード (isBillboard == 1):
//   position をカメラ右/上ベクトルでスクリーン向きに展開する（Cylindrical Billboard）。
//   法線はカメラ方向に固定。

#include "Common/Binding.hlsli"
#include "Platform/Backend.hlsli"

// ============================================================
// 定数バッファ
// ============================================================

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

// MaterialConstants スロット (b2) を Detail 専用 CB で上書き
#define FBZZ_MATERIAL_CONSTANTS
cbuffer DetailMaterialCB : register(CB_MATERIAL)
{
    float    alphaCutoff;  // アルファ閾値 (0 でアルファテスト無効)
    uint     isBillboard;  // 0=Mesh, 1=Billboard
    uint     hasAlbedoTex; // 1=テクスチャあり, 0=なし (白フォールバック)
    float    _detailPad;
};

// ============================================================
// インスタンスバッファ (VS t0 = DrawCall.instanceBuffer)
// ============================================================

struct DetailInstance
{
    float3 pos;
    float  rotY;
    float  scale;
};
StructuredBuffer<DetailInstance> g_Instances : register(t0);

// ============================================================
// テクスチャ
// ============================================================

Texture2D    gAlbedo  : register(TEX_ALBEDO);
SamplerState gSampler : register(SAMPLER_DEFAULT);

// ============================================================
// 入出力構造体
// ============================================================

struct VsIn
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
};

struct PsIn
{
    float4 svPos  : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

// ============================================================
// VS
// ============================================================

PsIn VSMain(VsIn v, uint instId : SV_InstanceID)
{
    DetailInstance inst = g_Instances[instId];

    // Y 軸回転行列 (row-major)
    float s = sin(inst.rotY);
    float c = cos(inst.rotY);
    float3x3 rot = float3x3(
         c, 0.0f, s,
         0.0f, 1.0f, 0.0f,
        -s, 0.0f, c
    );

    float3 localPos = v.position * inst.scale;

    [branch]
    if (isBillboard)
    {
        // Cylindrical Billboard: カメラ右/上ベクトルで頂点をスクリーン向きに展開する。
        // WHY: 遠景の草・花を安価に描画するため、カメラに常に正対するクワッドを使う。
        //      Cylindrical (Y 軸固定) にすることで地面と浮いた見た目を避ける。
        float3 right = float3(view[0][0], view[1][0], view[2][0]);
        float3 up    = float3(0.0f, 1.0f, 0.0f); // Y 固定 (Cylindrical)
        localPos = right * v.position.x * inst.scale
                 + up    * v.position.y * inst.scale;
    }
    else
    {
        localPos = mul(rot, localPos);
    }

    float3 worldPos = localPos + inst.pos;

    PsIn o;
    o.svPos    = mul(float4(worldPos, 1.0f), viewProjection);
    o.worldPos = worldPos;

    [branch]
    if (isBillboard)
        o.normal = -normalize(worldPos - cameraPos);
    else
        o.normal = normalize(mul(rot, v.normal));

    o.uv = v.uv;
    return o;
}

// ============================================================
// PS
// ============================================================

float4 PSMain(PsIn p) : SV_Target0
{
    float4 col = hasAlbedoTex
        ? gAlbedo.Sample(gSampler, p.uv)
        : float4(0.8f, 0.8f, 0.8f, 1.0f); // テクスチャ未設定時はグレーフォールバック

    // アルファテスト — alphaCutoff > 0 のときのみ適用
    if (alphaCutoff > 0.0f)
        clip(col.a - alphaCutoff);

    return col;
}
