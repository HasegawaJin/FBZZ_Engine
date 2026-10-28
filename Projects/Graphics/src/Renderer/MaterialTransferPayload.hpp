/// @file    MaterialTransferPayload.hpp
/// @brief   Non-installed exact immutable CPU payload sharing during one material extraction.
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once
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
[[nodiscard]] bool IsRegisteredOwnedTransferProducer(IRenderer& renderer);

/// @note Graphics owns TLS; nested stack scopes restore callers and queued transfers retain only immutable bytes.
class MaterialTransferPayloadScope final {
public:
    FBZZ_MATERIAL_TRANSFER_API explicit MaterialTransferPayloadScope(ResourceManager& resources) noexcept;
    FBZZ_MATERIAL_TRANSFER_API ~MaterialTransferPayloadScope();
    MaterialTransferPayloadScope(const MaterialTransferPayloadScope&) = delete;
    MaterialTransferPayloadScope& operator=(const MaterialTransferPayloadScope&) = delete;
private:
    friend class fbzz::renderer::Material;
    struct Entry { uint64_t hash = 0; std::shared_ptr<const OwnedConstantBufferData> payload; };
    /// @note Verify the exact live manager/facade and producer on each lookup before touching source bytes.
    [[nodiscard]] static std::shared_ptr<const OwnedConstantBufferData> CopyOrReuse(
        ResourceManager& resources, std::span<const uint8_t> bytes);
    static constexpr size_t MAX_ENTRIES = 64;
    static constexpr size_t MAX_PAYLOAD_BYTES = 4096;
    static constexpr size_t MAX_SCOPE_BYTES = 64 * 1024;
    static constexpr uint8_t MAX_DEPTH = 4;
    std::array<Entry, MAX_ENTRIES> m_entries{};
    MaterialTransferPayloadScope* m_previous = nullptr;
    ResourceManager* m_resources = nullptr;
    IRenderer* m_facade = nullptr;
    uint64_t m_resourceInstance = 0;
    size_t m_count = 0, m_ownedBytes = 0;
    uint8_t m_depth = 1;
    bool m_eligible = false, m_busy = false;
};

} /// @note namespace material_transfer_detail
} /// @note namespace fbzz::renderer
