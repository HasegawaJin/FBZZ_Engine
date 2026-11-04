/// @file    MaterialTransferPayload.hpp
/// @brief   Non-installed exact immutable CPU payload sharing during one material extraction.
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once
#include <Graphics/Renderer/MaterialTransferScope.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#if defined(_WIN32) && defined(FBZZGraphics_EXPORTS)
#define FBZZ_MATERIAL_TRANSFER_API __declspec(dllexport)
#elif defined(_WIN32)
#define FBZZ_MATERIAL_TRANSFER_API __declspec(dllimport)
#else
#define FBZZ_MATERIAL_TRANSFER_API
#endif

namespace fbzz::renderer {
class IRenderer;
class Material;
class ResourceManager;

namespace material_transfer_detail {

/// @note Construction deep-copies input; no API exposes a writable alias or any GPU/resource owner.
class OwnedConstantBufferData final {
public:
    explicit OwnedConstantBufferData(std::span<const uint8_t> bytes) : m_bytes(bytes.begin(), bytes.end()) {}
    OwnedConstantBufferData(const OwnedConstantBufferData&) = delete;
    OwnedConstantBufferData& operator=(const OwnedConstantBufferData&) = delete;
    [[nodiscard]] std::span<const uint8_t> Bytes() const { return m_bytes; }
private:
    const std::vector<uint8_t> m_bytes;
};

/// @note Uses only the existing exact-facade registry; unknown/busy/worker callers retain legacy raw updates.
[[nodiscard]] FBZZ_GRAPHICS_API bool IsRegisteredOwnedTransferProducer(IRenderer& renderer);

} /// @note namespace material_transfer_detail
} /// @note namespace fbzz::renderer
