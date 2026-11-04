/// @file    MaterialTransferScope.hpp
/// @brief   材質抽出中の不変転送データ共有スコープ。転送の実装は Graphics が所有する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Graphics/Api.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
namespace fbzz::renderer {
class ResourceManager;
class IRenderer;
class Material;
namespace material_transfer_detail {
class OwnedConstantBufferData;
/// @note Graphics owns TLS; nested stack scopes restore callers and queued transfers retain only immutable bytes.
class MaterialTransferPayloadScope final {
public:
    FBZZ_GRAPHICS_API explicit MaterialTransferPayloadScope(ResourceManager& resources) noexcept;
    FBZZ_GRAPHICS_API ~MaterialTransferPayloadScope();
    MaterialTransferPayloadScope(const MaterialTransferPayloadScope&) = delete;
    MaterialTransferPayloadScope& operator=(const MaterialTransferPayloadScope&) = delete;
private:
    friend class fbzz::renderer::Material;
    struct Entry { uint64_t hash = 0; std::shared_ptr<const OwnedConstantBufferData> payload; };
    /// @note Verify the exact live manager/facade and producer on each lookup before touching source bytes.
    [[nodiscard]] FBZZ_GRAPHICS_API static std::shared_ptr<const OwnedConstantBufferData> CopyOrReuse(
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
