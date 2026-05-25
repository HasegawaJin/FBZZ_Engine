// FBZZ Engine
// ResourceHandle.hpp | fbzz::renderer
// 世代番号付き Renderer リソースハンドル
// ResourceManager が所有する実体を安全に参照するための軽量 ID。
// slot と generation の組み合わせで破棄済み参照を検出する。
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

struct TextureTag {};
struct ShaderTag {};
struct BufferTag {};
struct ConstantBufferTag {};
struct PipelineStateTag {};
struct RenderTargetTag {};

} // namespace fbzz::renderer
