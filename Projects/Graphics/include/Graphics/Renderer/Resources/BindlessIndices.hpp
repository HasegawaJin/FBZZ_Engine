/// @file    BindlessIndices.hpp
/// @brief   ドローごとに配る bindless ディスクリプタ添字ブロック (b14) のレイアウト。
/// @author  Hasegawa Jin
/// @date    2026-09-16
/// @note LAYOUT: Assets/Shaders/Common/BindlessIndices.hlsli と **完全に一致**させること。
/// @note       ずらすとシェーダーが別のスロットの添字を読み、無関係なテクスチャが貼られる
/// @note       (コンパイルも実行も通ってしまうので、見た目でしか気づけない)。
/// @note uint を 4 本ずつ束ねてあるのは HLSL の定数バッファが配列要素を 16 バイト境界へ
/// @note       パディングするため。uint indices[32] と書くと 512 バイト消費し、しかも
/// @note       C++ 側と並びが食い違う。
/// @see  Docs/design/bindless.md
#pragma once
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <algorithm>
#include <array>
#include <cstdint>

/// @note INVALID_BINDLESS_INDEX は ITexture.hpp が正本。
#include "Graphics/Renderer/Resources/Textures/ITexture.hpp"

namespace fbzz::renderer {

/// @note 添字ブロックを配る定数バッファレジスタ。b0〜b13 は既存の用途で埋まっている。
/// @note LAYOUT: Assets/Shaders/Common/BindlessIndices.hlsli の register(b14) と一致させること。
inline constexpr uint32_t kBindlessIndicesRegister = cb::BINDLESS_INDICES;

/// @note ルートシグネチャ上のパラメーター番号 (0〜13 は root CBV b0〜b13)。
/// @note かつてここには SRV / UAV のディスクリプタテーブルが居た。全シェーダーが
/// @note       ResourceDescriptorHeap から直接引くようになったので撤去してある。
inline constexpr uint32_t kBindlessIndicesRootParam = bindingCount::CONSTANT_BUFFERS;

/// @note Root signature parameter 15 maps to the one-DWORD shader constant at b15 and ExecuteIndirect CONSTANT writes.
inline constexpr uint32_t kIndirectInstanceOffsetRootParam = kBindlessIndicesRootParam + 1;

/// @note ピクセル/コンピュート側の SRV 添字の本数。旧ピクセル SRV テーブル (t0〜t31) と同じ幅。
inline constexpr uint32_t kBindlessPixelSlotCount = bindingCount::PIXEL_SRVS;

/// @note 頂点側の SRV 添字の本数。旧頂点 SRV テーブルのうち実際に使うのは
/// @note instanceBuffer (t0) と vsBuffers (t14/t15) の 3 本だけ。
inline constexpr uint32_t kBindlessVertexSlotCount = bindingCount::VERTEX_SRVS;

/// @note UAV 添字の本数。旧 UAV テーブル (u0〜u7) と同じ幅。
/// @note 8 という数は撤去済み DX11 SM 5.0 の CS UAV 上限の名残。bindless では上限が無いので、
/// @note       ComputeCall::uavOutputs を広げるときはここと HLSL 側を一緒に増やせばよい。
inline constexpr uint32_t kBindlessUavSlotCount = bindingCount::UAVS;

/// @brief b14 へ配る添字ブロック。
/// @note 16 バイト境界を維持すること (定数バッファのパッキング規則)。
struct BindlessIndicesConstants {
    std::array<uint32_t, kBindlessPixelSlotCount>  pixel{}; ///< ピクセル・コンピュート段の SRV 記述子。配列添字は旧 t レジスタ番号。
    /// @note [0] = instanceBuffer、[1..2] = vsBuffers または vsRawBuffers、[3] = メッシュレットの選別バッファ。
    /// @note 頂点側を別配列にするのは、旧モデルで VS の t0 と PS の t0 が別テーブルを
    /// @note       指していたため。bindless の添字空間は平坦なので、ここで分けないと
    /// @note       instanceBuffer と Albedo が同じ枠を奪い合う。
    std::array<uint32_t, kBindlessVertexSlotCount> vertex{}; ///< 頂点・メッシュ段で参照する SRV の記述子添字。
    std::array<uint32_t, kBindlessUavSlotCount>    uav{}; ///< コンピュート段の UAV 記述子。配列添字は旧 u レジスタ番号。

    /// @brief 全スロットを「無効」で埋める。ドローごとに必ずこの状態から組み直すこと。
    /// @note 前のドローの添字が残ると、束縛していないテクスチャを読んでしまう。
    void Reset()
    {
        pixel.fill(INVALID_BINDLESS_INDEX);
        vertex.fill(INVALID_BINDLESS_INDEX);
        uav.fill(INVALID_BINDLESS_INDEX);
    }

    /// @return ピクセル SRV、頂点 SRV、UAV の全スロットが無効なら true。
    [[nodiscard]] bool IsEmpty() const
    {
        const auto isInvalid = [](uint32_t index) { return index == INVALID_BINDLESS_INDEX; };
        return std::all_of(pixel.begin(), pixel.end(), isInvalid)
            && std::all_of(vertex.begin(), vertex.end(), isInvalid)
            && std::all_of(uav.begin(), uav.end(), isInvalid);
    }
};

static_assert(sizeof(BindlessIndicesConstants) % 16 == 0,
              "BindlessIndicesConstants は 16 バイト境界を維持すること (定数バッファのパッキング規則)");

} // namespace fbzz::renderer
