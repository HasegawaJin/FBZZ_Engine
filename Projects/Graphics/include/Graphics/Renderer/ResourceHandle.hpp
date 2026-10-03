/// @file    ResourceHandle.hpp
/// @brief   世代番号付き Renderer リソースハンドル。
/// @author  Hasegawa Jin
/// @date    2026-05-22
/// @note ResourceManager が所有する実体を安全に参照するための軽量 ID。
/// @note slot と generation の組み合わせで破棄済み参照を検出する。
#pragma once
#include <cstdint>
#include <source_location>

namespace fbzz::renderer {

/// @note リソースを «どこから» 作ったか。既定引数で呼び出し位置を拾うために使う。
/// @note 記録側 (ResourceManager) の `__FILE__` を使うと、どの頂点バッファも «同じ 1 行»
/// @note       から生まれたことになりリーク一覧が «誰が漏らしたか» を答えられない。Ensure /
/// @note       Acquire / Init のような «代わりに作る» 関数も、受け取った位置をそのまま下へ渡すこと。
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

/// @note 各タグは `ResourceHandle<T>` の型安全性確保のための空型。
/// @note `ResourceHandle<TextureTag>` と `ResourceHandle<ShaderTag>` は暗黙変換できない。
struct TextureTag {};
struct ShaderTag {};
struct BufferTag {};
struct ConstantBufferTag {};
struct PipelineStateTag {};
struct RenderTargetTag {};
struct StructuredBufferTag {}; ///< @note GPU StructuredBuffer (SRV) — DrawInstanced のインスタンスデータ用
struct AccelerationStructureTag {};

} /// @note namespace fbzz::renderer
