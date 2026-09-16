/// @file    ComputeCall.hpp
/// @brief   コンピュートシェーダーの実行要求データ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// IRenderer::Dispatch に渡す軽量な値型。
/// GPU リソースは ResourceHandle で参照し、共有ポインタは持たない。
#pragma once
#include <array>
#include <cstdint>
#include "ResourceHandle.hpp"

namespace fbzz::renderer {

// コンピュートシェーダーが StructuredBuffer として宣言しうるレジスタ。
//
// WHY 一覧を C++ 側へ写すか: DX12 の null ディスクリプタは、シェーダーが宣言した次元と
//     一致していなければならない。束縛されなかったスロットへ Texture2D の null を差すと
//     デバッグレイヤーが警告し、読み値も未定義になる。どのレジスタがバッファかは
//     HLSL にしか無い情報なので、束縛先を決めるバックエンドが引ける形で置いておく。
// LAYOUT: Assets/Shaders/Common/Binding.hlsli の SB_* と一致させること。
inline constexpr std::array<uint32_t, 4> kComputeStructuredBufferSlots = { 14u, 15u, 29u, 30u };

inline constexpr bool IsComputeStructuredBufferSlot(uint32_t slot)
{
    for (const uint32_t declared : kComputeStructuredBufferSlots)
        if (declared == slot) return true;
    return false;
}

struct ComputeCall {
    ResourceHandle<ShaderTag> shader;
    std::array<ResourceHandle<ConstantBufferTag>, 14> constantBuffers = {}; // b0〜b13
    std::array<ResourceHandle<TextureTag>, 32>        srvInputs       = {}; // t0〜t31
    // u0〜u7。枠数の由来は撤去済みの DX11 SM5.0 の CS UAV 上限で、DX12 は 64 本まで張れる。
    // 広げるときはシェーダー側 (Binding.hlsli) の時分割も一緒に解くこと。
    std::array<ResourceHandle<TextureTag>, 8>         uavOutputs      = {};
    // StructuredBuffer SRV。添字 = レジスタ番号で、srvInputs と同じ規則。
    //
    // WHY 添字 = レジスタ番号にするか: 以前は「先頭 2 本を t14/t15 へ」という固定配置で、
    //      クラスタライトリスト (t29/t30) のようにレジスタが決まっているバッファを
    //      CS へ渡す口が無かった。srvInputs と規則を揃えておけば、束縛先はシェーダー側の
    //      register() をそのまま書き写すだけになる。
    // NOTE: 同じ番号を srvInputs と両方に入れてはならない (後勝ちで静かに壊れる)。
    std::array<ResourceHandle<StructuredBufferTag>, 32> srvBuffers      = {}; // t0〜t31
    std::array<ResourceHandle<StructuredBufferTag>, 2> uavBuffers       = {}; // RWStructuredBuffer UAV: u2〜u3
    // GPU 書き込み可能な頂点バッファ UAV: u4。コンピュートスキニングの出力先。
    // WHY: 出力は「CS が書いて IA が読む」二役なので StructuredBufferTag ではなく
    //      BufferTag (頂点バッファ) 側で確保する。バインド口だけここに分けて持つ。
    ResourceHandle<BufferTag>                         uavVertexBuffer  = {}; // u4

    uint32_t dispatchX = 1;
    uint32_t dispatchY = 1;
    uint32_t dispatchZ = 1;
};

} // namespace fbzz::renderer
