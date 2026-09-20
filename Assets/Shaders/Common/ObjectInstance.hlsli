// @file    ObjectInstance.hlsli
// @brief   インスタンシング変種が VS で読む per-instance データ。
// @author  Hasegawa Jin
// @date    2026-09-20
//
// @note FBZZ_INSTANCED を定義したシェーダーだけがこのファイルの中身を得る。定義していなければ
//       何も宣言しないので、本体と変種で同じ include を書いておける。
// @see Docs/design/gpu-instancing.md
#ifndef FBZZ_COMMON_OBJECT_INSTANCE_HLSLI
#define FBZZ_COMMON_OBJECT_INSTANCE_HLSLI

#include "Binding.hlsli"
#include "BindlessIndices.hlsli"

#ifdef FBZZ_INSTANCED

// @brief インスタンス 1 個ぶんの可変データ。
// @note LAYOUT: Projects/Engine/include/Engine/Scene/Systems/RenderPasses/InstanceBatch.hpp の
//       PerInstanceData と一致させること (128 バイト)。
// @note objectParams (LOD ディザ) はここに無い。PS が b1 から読み、束ねるのは遷移していない
//       物体だけという契約のため (Docs/design/gpu-instancing.md §2.1)。
struct ObjectInstance
{
    float4x4 world;
    float4x4 worldInvTranspose;
};

// @note DrawCall::instanceBuffer が VS_SB_INSTANCE_SLOT へ束縛される。
FBZZ_VS_SBUFFER(ObjectInstance, gObjectInstances, VS_SB_INSTANCE_SLOT);

// @brief モーションベクターパスの読み方。2 枠目が worldInvTranspose でなく prevWorld。
// @note b1 の 2 枠目をパスごとに別の意味で使う規約 (Constants.hlsli の FBZZ_OBJECT_CONSTANTS)
//       と同じものを、per-instance 側にもそのまま持ち込む。
// @note gObjectInstances と同じ 128 バイトの並びを別の名前で読むだけで、指す先は同じバッファ。
//       束ねる側 (InstanceBatcher) は中身を解釈せず 2 本の行列をそのまま運ぶ。
struct ObjectInstanceMotion
{
    float4x4 world;
    float4x4 prevWorld;
};

FBZZ_VS_SBUFFER(ObjectInstanceMotion, gObjectInstancesMotion, VS_SB_INSTANCE_SLOT);

#endif // FBZZ_INSTANCED

#endif // FBZZ_COMMON_OBJECT_INSTANCE_HLSLI
