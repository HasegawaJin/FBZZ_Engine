// FBZZ Engine
// ResourceHandle.hpp | fbzz::renderer
// Generation checked renderer resource handles
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
