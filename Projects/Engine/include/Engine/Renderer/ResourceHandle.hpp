/// @file    ResourceHandle.hpp
/// @brief   世代番号付き Renderer リソースハンドル。
/// @author  Hasegawa Jin
/// @date    2026-05-22
///
/// ResourceManager が所有する実体を安全に参照するための軽量 ID。
/// slot と generation の組み合わせで破棄済み参照を検出する。
#pragma once
#include <cstdint>

namespace fbzz::renderer {

template<typename Tag>
struct ResourceHandle {
    uint32_t id = 0;
    uint32_t gen = 0;

    [[nodiscard]] bool IsValid() const { return id != 0; }
    explicit operator bool() const { return IsValid(); }
    bool operator==(const ResourceHandle&) const = default;

    static ResourceHandle Null() { return {}; }
};

// 各タグは ResourceHandle<T> の型安全性確保のための空型。
// ResourceHandle<TextureTag> と ResourceHandle<ShaderTag> は暗黙変換できない。
struct TextureTag {};
struct ShaderTag {};
struct BufferTag {};
struct ConstantBufferTag {};
struct PipelineStateTag {};
struct RenderTargetTag {};
struct MaterialAssetTag {};    // CPU 側 MaterialAsset を AssetManager のスロットプールで管理するハンドル
struct StructuredBufferTag {}; // GPU StructuredBuffer (SRV) — DrawInstanced のインスタンスデータ用

} // namespace fbzz::renderer
