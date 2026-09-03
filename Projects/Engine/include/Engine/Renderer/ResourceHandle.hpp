/// @file    ResourceHandle.hpp
/// @brief   世代番号付き Renderer リソースハンドル。
/// @author  Hasegawa Jin
/// @date    2026-05-22
///
/// ResourceManager が所有する実体を安全に参照するための軽量 ID。
/// slot と generation の組み合わせで破棄済み参照を検出する。
#pragma once
#include <cstdint>
#include <source_location>

namespace fbzz::renderer {

/// リソースを «どこから» 作ったか。既定引数で呼び出し位置を拾うために使う。
///
/// WHY 呼び出し元を持ち回るか: 記録側 (ResourceManager) の __FILE__ を使うと、
///     どの頂点バッファも «同じ 1 行» から生まれたことになり、リーク一覧が
///     «誰が漏らしたか» を答えられなくなる。Ensure / Acquire / Init のような
///     «代わりに作る» 関数も、受け取った位置をそのまま下へ渡すこと。
using Where = std::source_location;

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
