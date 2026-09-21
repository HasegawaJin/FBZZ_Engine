/// @file    ComputeCall.hpp
/// @brief   コンピュートシェーダーの実行要求データ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note IRenderer::Dispatch に渡す軽量な値型。
/// @note GPU リソースは ResourceHandle で参照し、共有ポインタは持たない。
#pragma once
#include <array>
#include <cstdint>
#include "ResourceHandle.hpp"

namespace fbzz::renderer {

/// @brief コンピュートシェーダーが StructuredBuffer として宣言しうるレジスタ。
/// @note 束縛されなかったスロットへ Texture2D の null を差すとデバッグレイヤーが警告し読み値も
/// @note       未定義になる。どのレジスタがバッファかは HLSL にしか無い情報なので、束縛先を決める
/// @note       バックエンドが引ける形で置いておく。
/// @note LAYOUT: Assets/Shaders/Common/Binding.hlsli の SB_* と一致させること。
inline constexpr std::array<uint32_t, 4> kComputeStructuredBufferSlots = { 14u, 15u, 29u, 30u };

inline constexpr bool IsComputeStructuredBufferSlot(uint32_t slot)
{
    for (const uint32_t declared : kComputeStructuredBufferSlots)
        if (declared == slot) return true;
    return false;
}

struct ComputeCall {
    ResourceHandle<ShaderTag> shader;
    std::array<ResourceHandle<ConstantBufferTag>, 14> constantBuffers = {}; ///< @note b0〜b13
    std::array<ResourceHandle<TextureTag>, 32>        srvInputs       = {}; ///< @note t0〜t31
    /// @brief UAV 出力 (u0〜u7)。
    /// @note 8 本という枠数は撤去済み DX11 SM5.0 の CS UAV 上限の名残 (DX12 は 64 本まで張れる)。
    /// @note       広げるときはシェーダー側 (Binding.hlsli) の時分割も一緒に解くこと。
    std::array<ResourceHandle<TextureTag>, 8>         uavOutputs      = {};
    /// @brief StructuredBuffer SRV (t0〜t31)。添字 = レジスタ番号で、srvInputs と同じ規則。
    /// @note 添字をレジスタ番号に揃えることで、束縛先はシェーダー側の register() をそのまま
    /// @note       書き写すだけになる (固定配置だとレジスタが決まったバッファを渡す口が無かった)。
    /// @note 同じ番号を srvInputs と両方に入れてはならない (後勝ちで静かに壊れる)。
    std::array<ResourceHandle<StructuredBufferTag>, 32> srvBuffers      = {};
    std::array<ResourceHandle<StructuredBufferTag>, 2> uavBuffers       = {}; ///< @note RWStructuredBuffer UAV: u2〜u3
    /// @brief GPU 書き込み可能な頂点バッファ UAV (u4)。コンピュートスキニングの出力先。
    /// @note 出力は「CS が書いて IA が読む」二役なので StructuredBufferTag でなく
    /// @note       BufferTag (頂点バッファ) 側で確保し、バインド口だけここに分けて持つ。
    ResourceHandle<BufferTag>                         uavVertexBuffer  = {};

    uint32_t dispatchX = 1;
    uint32_t dispatchY = 1;
    uint32_t dispatchZ = 1;
};

} /// @note namespace fbzz::renderer
