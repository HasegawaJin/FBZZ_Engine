/// @file    DrawCall.hpp
/// @brief   レンダラーへ渡す描画要求データ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note 頂点・インデックス・シェーダー・定数バッファを ResourceHandle で参照する。
/// @note RenderSystem が生成し、IRenderer が ResourceManager で実体へ解決する。
#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <memory>
#include "RenderLayer.hpp"
#include "RenderState.hpp"
#include "ResourceHandle.hpp"

namespace fbzz::renderer {
class PreparedSkinningTicket;

/// @brief DrawCall::psBuffers が占めるピクセルシェーダー SRV の先頭レジスタ。
/// @note LAYOUT: Assets/Shaders/Common/Binding.hlsli の SB_PUNCTUAL_LIGHTS / SB_CLUSTER_INDICES
/// @note       と完全に一致させること。ずらすと HLSL 側が別スロットを読んで黙って壊れる。
inline constexpr uint32_t kPsBufferBaseSlot = 29;
/// @note VS粒子のt14/t15とは添字空間を分けた、追加のPS-only数値表。
inline constexpr uint32_t kAuxiliaryPsBufferBaseSlot = 14;

struct DrawCall {
    /// @note 原 snapshot の出力 lease。通常 handle の退役後も捕捉済み native 頂点を保持する。
    std::shared_ptr<const PreparedSkinningTicket> preparedSkinning;
    uint32_t preparedSkinningRow = 0;
    /// @note Original immutable mesh identity used only by automatic batching of distinct deformed outputs.
    ResourceHandle<BufferTag> deformationSource;
    /// @note Every raw vertex buffer indexed by instance rows; DIRECT queue validation and transitions cover the whole list.
    std::vector<ResourceHandle<BufferTag>> indirectReadBuffers;
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
    /// @note 定数の bindless 添字から読むテクスチャ。スロットを消費せず状態遷移と寿命追跡へ渡す。
    std::array<ResourceHandle<TextureTag>, 2> indirectReadTextures{};

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

    /// @brief VS/MS-readable raw Buffer SRV (t14〜t15)。Mesh Shader が既存 VB/IB を読む用途にも使う。
    /// @note 同じ slot の vsBuffers と同時指定しない。両方を指定した場合、DX12 は描画を記録しない。
    std::array<ResourceHandle<BufferTag>, 2> vsRawBuffers = {};

    /// @brief PS-readable StructuredBuffer (t29〜t30)。クラスタライティングのライト配列と
    /// @note        インデックスリスト。
    /// @note vsBuffers / vsRawBuffers は VS/MS の t14/t15 専用で、DX12 側はピクセル SRV
    /// @note       テーブル (t0〜t31) の空き 2 枠へ差し込むだけでルートシグネチャの変更は不要。
    std::array<ResourceHandle<StructuredBufferTag>, 2> psBuffers = {};
    /// @note 同じ番号のtexturesと同時に設定しない。従来のクラスタ表t29/t30は維持する。
    std::array<ResourceHandle<StructuredBufferTag>, 2> auxiliaryPsBuffers = {};

    /// @brief GPU が生成した間接描画引数。各要素は 20B/24B DRAW_INDEXED または 12B DISPATCH_MESH。
    /// @note 全 command は同じ DrawCall の shader、root binding、render target、topology を共有する。
    /// @note 引数バッファは ExecuteIndirect の記録から GPU fence 完了まで生存させる。
    ResourceHandle<StructuredBufferTag> indirectArguments;
    /// @brief indirectArguments 内の byte offset。ExecuteIndirect の引数形式に合わせ 4 byte 境界であること。
    uint64_t indirectArgumentOffset = 0;
    /// @brief 引数バッファから実行する共通 binding の command 数。
    uint32_t indirectCommandCount = 1;
    /// @brief 各 indexed indirect command の先頭 DWORD を b15 root constant として渡す。
    /// @note true は 24 byte argument record 専用で、Mesh Shader dispatch と同時に指定できない。
    bool indirectInstanceOffset = false;

    /// @brief Mesh Shader の直接 dispatch または間接 dispatch 形式を選ぶ threadgroup X 数。
    /// @note indirectArguments が無効で X > 0 の場合は direct Mesh Shader dispatch。
    /// @note indirectArguments が有効なら X > 0 は DISPATCH_MESH、X = 0 は DRAW_INDEXED を選ぶ。
    /// @note Y/Z は indirect dispatch では引数データから取得し、直接 dispatch のときだけ使う。
    uint32_t meshGroupCountX = 0;
    uint32_t meshGroupCountY = 0;
    uint32_t meshGroupCountZ = 0;

    RenderLayer layer = RenderLayer::OPAQUE_LAYER;
    PrimitiveTopology topology = PrimitiveTopology::TRIANGLE_LIST;
};

} /// @note namespace fbzz::renderer
