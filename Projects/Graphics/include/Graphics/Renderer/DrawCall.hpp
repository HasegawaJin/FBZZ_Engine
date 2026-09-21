/// @file    DrawCall.hpp
/// @brief   レンダラーへ渡す描画要求データ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note 頂点・インデックス・シェーダー・定数バッファを ResourceHandle で参照する。
/// @note RenderSystem が生成し、IRenderer が ResourceManager で実体へ解決する。
#pragma once
#include <array>
#include <cstdint>
#include "RenderLayer.hpp"
#include "RenderState.hpp"
#include "ResourceHandle.hpp"

namespace fbzz::renderer {

/// @brief DrawCall::psBuffers が占めるピクセルシェーダー SRV の先頭レジスタ。
/// @note LAYOUT: Assets/Shaders/Common/Binding.hlsli の SB_PUNCTUAL_LIGHTS / SB_CLUSTER_INDICES
/// @note       と完全に一致させること。ずらすと HLSL 側が別スロットを読んで黙って壊れる。
inline constexpr uint32_t kPsBufferBaseSlot = 29;

struct DrawCall {
    ResourceHandle<BufferTag> vertexBuffer;
    ResourceHandle<BufferTag> indexBuffer;
    ResourceHandle<ShaderTag> shader;
    ResourceHandle<PipelineStateTag> pipelineState;

    /// @brief 定数バッファ (b0〜b13)。両バックエンドで共通契約とし、DX12 は root CBV へ Submit 時
    /// @note        の GPU VA を記録する。
    /// @note LAYOUT (Constants.hlsli と同期): [0]=CameraConstants(b0) [1]=ObjectConstants(b1)
    /// @note       [2]=MaterialConstants(b2) [3]=LightConstants(b3) [4]=ShadowConstants(b4)
    /// @note       [5]=PostProcConstants(b5) [6]=AtmosphereConstants(b6) [7]=SkinningConstants(b7)
    std::array<ResourceHandle<ConstantBufferTag>, 14> constantBuffers = {};

    /// @brief テクスチャ (t0〜t31)。
    /// @note LAYOUT (Constants.hlsli と同期): [0]=Albedo [1]=Normal [5]=HDR [7]=Depth
    /// @note       [8]=ShadowMap [10]=Bloom。Advanced Graphics が t16〜t24 を使うため 32 スロット保持。
    std::array<ResourceHandle<TextureTag>, 32> textures = {};

    uint32_t indexCount  = 0;
    /// @note indexCount=0 かつ vertexCount>0 でインデックスなし描画 (フルスクリーントライアングル等)。
    uint32_t vertexCount = 0;
    uint32_t startIndex  = 0;
    uint32_t baseVertex  = 0;

    /// @brief GPU Instancing。instanceBuffer が有効なら instanceCount 1 以上で Instanced Draw。
    /// @note instanceBuffer は VS の t0 に StructuredBuffer<T> としてバインドされ、
    /// @note       HLSL 側で SV_InstanceID でインデックスしてインスタンスデータを取得する。
    uint32_t instanceCount = 1;
    ResourceHandle<StructuredBufferTag> instanceBuffer;

    /// @brief VS-readable StructuredBuffer (t14〜t15)。GPU パーティクル等の頂点データを渡す。
    /// @note SV_VertexID ベースの描画は頂点バッファを持たず、StructuredBuffer から取り出す。
    /// @note       t14 を使うのはテクスチャ SRV (t0〜t13) と重複しないため。
    std::array<ResourceHandle<StructuredBufferTag>, 2> vsBuffers = {};

    /// @brief PS-readable StructuredBuffer (t29〜t30)。クラスタライティングのライト配列と
    /// @note        インデックスリスト。
    /// @note vsBuffers は頂点シェーダーにしか束縛できない (DX12 の root param 15 は
    /// @note       SHADER_VISIBILITY_VERTEX) ため専用スロットを設ける。DX12 側はピクセル SRV
    /// @note       テーブル (t0〜t31) の空き 2 枠へ差し込むだけでルートシグネチャの変更は不要。
    std::array<ResourceHandle<StructuredBufferTag>, 2> psBuffers = {};

    RenderLayer layer = RenderLayer::OPAQUE_LAYER;
    PrimitiveTopology topology = PrimitiveTopology::TRIANGLE_LIST;
};

} /// @note namespace fbzz::renderer
