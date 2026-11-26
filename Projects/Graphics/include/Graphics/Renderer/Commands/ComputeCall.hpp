/// @file    ComputeCall.hpp
/// @brief   コンピュートシェーダーの実行要求データ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note IRenderer::Dispatch に渡す軽量な値型。
/// @note GPU リソースは ResourceHandle で参照し、共有ポインタは持たない。
#pragma once
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <array>
#include <cstdint>
#include <vector>
#include "Graphics/Renderer/Resources/ResourceHandle.hpp"

namespace fbzz::renderer {

/// @brief コンピュートシェーダーが StructuredBuffer として宣言しうるレジスタ。
/// @note 束縛されなかったスロットへ Texture2D の null を差すとデバッグレイヤーが警告し読み値も
/// @note       未定義になる。どのレジスタがバッファかは HLSL にしか無い情報なので、束縛先を決める
/// @note       バックエンドが引ける形で置いておく。
inline constexpr std::array<uint32_t, 4> kComputeStructuredBufferSlots = { srv::GPU_PARTICLES, srv::GPU_SPAWN, srv::PUNCTUAL_LIGHTS, srv::CLUSTER_INDICES };

inline constexpr bool IsComputeStructuredBufferSlot(uint32_t slot)
{
    for (const uint32_t declared : kComputeStructuredBufferSlots) {
        if (declared == slot) {
            return true;
        }
    }
    return false;
}

struct ComputeCall {
    ResourceHandle<ShaderTag> shader; ///< 実行するコンピュートシェーダーの非所有ハンドル。
    std::array<ResourceHandle<ConstantBufferTag>, bindingCount::CONSTANT_BUFFERS> constantBuffers = {}; ///< 読み取り定数バッファ。添字は b レジスタ番号。
    std::array<ResourceHandle<TextureTag>, bindingCount::PIXEL_SRVS>        srvInputs       = {}; ///< 読み取りテクスチャ。添字は t レジスタ番号。
    /// @brief UAV 出力 (u0〜u7)。
    /// @note 8 本という枠数は撤去済み DX11 SM5.0 の CS UAV 上限の名残 (DX12 は 64 本まで張れる)。
    /// @note       広げるときはシェーダー側 (Binding.hlsli) の時分割も一緒に解くこと。
    std::array<ResourceHandle<TextureTag>, bindingCount::UAVS>         uavOutputs      = {}; ///< 読み書きテクスチャ。添字は u レジスタ番号。
    /// @brief StructuredBuffer SRV (t0〜t31)。添字 = レジスタ番号で、srvInputs と同じ規則。
    /// @note 添字をレジスタ番号に揃えることで、束縛先はシェーダー側の register() をそのまま
    /// @note       書き写すだけになる (固定配置だとレジスタが決まったバッファを渡す口が無かった)。
    /// @note 同じ番号を srvInputs と両方に入れてはならない (後勝ちで静かに壊れる)。
    std::array<ResourceHandle<StructuredBufferTag>, bindingCount::PIXEL_SRVS> srvBuffers      = {}; ///< 読み取り構造化バッファ。添字は t レジスタ番号。
    std::array<ResourceHandle<StructuredBufferTag>, bindingCount::BUFFER_RANGE> uavBuffers       = {}; ///< 読み書き構造化バッファ。添字 0 は UAV u2。
    /// @brief GPU 書き込み可能な頂点バッファ UAV (u4)。コンピュートスキニングの出力先。
    /// @note 出力は「CS が書いて IA が読む」二役なので StructuredBufferTag でなく
    /// @note       BufferTag (頂点バッファ) 側で確保し、バインド口だけここに分けて持つ。
    ResourceHandle<BufferTag>                         uavVertexBuffer  = {}; ///< u4 に束縛する GPU 書き込み先の頂点バッファ。

    /// @note TLAS shares the t0-t31 namespace; each slot must have exactly one resource type.
    /// @note Initial ray dispatches run on DIRECT only; the backend rejects stale or unbuilt TLAS handles.
    std::array<ResourceHandle<AccelerationStructureTag>, bindingCount::PIXEL_SRVS> accelerationStructures = {}; ///< レイ照会用の TLAS。SRV の同じ番号に別の資源を指定しない。

    uint32_t dispatchX = 1; ///< X 方向のスレッドグループ数。
    uint32_t dispatchY = 1; ///< Y 方向のスレッドグループ数。
    uint32_t dispatchZ = 1; ///< Z 方向のスレッドグループ数。

    /// @note ByteAddressBuffer SRV (t0-t31)。同じ番号の Texture / StructuredBuffer / TLAS とは排他。
    std::array<ResourceHandle<BufferTag>, bindingCount::PIXEL_SRVS> srvRawBuffers = {}; ///< 読み取り raw バッファ。SRV の同じ番号に別の資源を指定しない。
    /// @note GPU table の添字から間接的に読む全 Buffer。スロットを消費せず backend の状態と寿命追跡へ渡す。
    /// @note 初期実装は DIRECT queue 限定。table は各 Buffer の現在の内容版と SRV 添字に一致させること。
    std::vector<ResourceHandle<BufferTag>> indirectReadBuffers; ///< GPU テーブルから間接的に読むバッファの全件。状態と寿命の追跡に使う。
    /// @note DIRECT-only bindless UAV destinations; backend validates every handle and publishes every successful write.
    std::vector<ResourceHandle<BufferTag>> indirectWriteVertexBuffers; ///< DIRECT キューで bindless UAV として書く頂点バッファの全件。
    /// @note Every TextureTag indexed through a GPU material table; consumes no t-slot but requires graph declaration and DIRECT queue tracking.
    std::vector<ResourceHandle<TextureTag>> indirectReadTextures; ///< GPU 材質テーブルから読むテクスチャの全件。グラフ申告と DIRECT キューの状態追跡に使う。
};

} // namespace fbzz::renderer
