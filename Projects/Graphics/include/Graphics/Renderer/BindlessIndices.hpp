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
#include <array>
#include <cstdint>

/// @note INVALID_BINDLESS_INDEX は ITexture.hpp が正本。
#include "ITexture.hpp"

namespace fbzz::renderer {

/// @note 添字ブロックを配る定数バッファレジスタ。b0〜b13 は既存の用途で埋まっている。
/// @note LAYOUT: Assets/Shaders/Common/BindlessIndices.hlsli の register(b14) と一致させること。
inline constexpr uint32_t kBindlessIndicesRegister = 14;

/// @note ルートシグネチャ上のパラメーター番号 (0〜13 は root CBV b0〜b13)。
/// @note かつてここには SRV / UAV のディスクリプタテーブルが居た。全シェーダーが
/// @note       ResourceDescriptorHeap から直接引くようになったので撤去してある。
inline constexpr uint32_t kBindlessIndicesRootParam = 14;

/// @note ピクセル/コンピュート側の SRV 添字の本数。旧ピクセル SRV テーブル (t0〜t31) と同じ幅。
inline constexpr uint32_t kBindlessPixelSlotCount = 32;

/// @note 頂点側の SRV 添字の本数。旧頂点 SRV テーブルのうち実際に使うのは
/// @note instanceBuffer (t0) と vsBuffers (t14/t15) の 3 本だけ。
inline constexpr uint32_t kBindlessVertexSlotCount = 4;

/// @note UAV 添字の本数。旧 UAV テーブル (u0〜u7) と同じ幅。
/// @note 8 という数は撤去済み DX11 SM 5.0 の CS UAV 上限の名残。bindless では上限が無いので、
/// @note       ComputeCall::uavOutputs を広げるときはここと HLSL 側を一緒に増やせばよい。
inline constexpr uint32_t kBindlessUavSlotCount = 8;

/// @brief b14 へ配る添字ブロック。
/// @note 16 バイト境界を維持すること (定数バッファのパッキング規則)。
struct BindlessIndicesConstants {
    /// @note 旧 DrawCall::textures[0..31] と psBuffers (t29/t30) に対応する添字。
    std::array<uint32_t, kBindlessPixelSlotCount>  pixel{};
    /// @note [0] = instanceBuffer、[1] = vsBuffers[0]、[2] = vsBuffers[1]、[3] = 予約。
    /// @note 頂点側を別配列にするのは、旧モデルで VS の t0 と PS の t0 が別テーブルを
    /// @note       指していたため。bindless の添字空間は平坦なので、ここで分けないと
    /// @note       instanceBuffer と Albedo が同じ枠を奪い合う。
    std::array<uint32_t, kBindlessVertexSlotCount> vertex{};
    /// @note 旧 ComputeCall::uavOutputs[0..7] に対応する UAV 添字。
    std::array<uint32_t, kBindlessUavSlotCount>    uav{};

    /// @brief 全スロットを「無効」で埋める。ドローごとに必ずこの状態から組み直すこと。
    /// @note 前のドローの添字が残ると、束縛していないテクスチャを読んでしまう。
    void Reset()
    {
        pixel.fill(INVALID_BINDLESS_INDEX);
        vertex.fill(INVALID_BINDLESS_INDEX);
        uav.fill(INVALID_BINDLESS_INDEX);
    }
};

static_assert(sizeof(BindlessIndicesConstants) % 16 == 0,
              "BindlessIndicesConstants は 16 バイト境界を維持すること (定数バッファのパッキング規則)");

} /// @note namespace fbzz::renderer
