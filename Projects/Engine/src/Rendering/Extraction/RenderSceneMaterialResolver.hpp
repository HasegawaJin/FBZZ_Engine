/// @file    RenderSceneMaterialResolver.hpp
/// @brief   ビューの抽出中だけ共有するマテリアルの能力・パラメーター・同期済みバインディング。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include "../Passes/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Components/Geometry/MaterialComponent.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <Graphics/Renderer/Shaders/ShaderDescriptor.hpp>
#include <array>
#include <optional>
#include <unordered_map>

namespace fbzz::scene::render_extraction_detail {

struct GBufferCopyField {
    size_t variableIndex = SIZE_MAX;
    uint32_t sourceOffset = 0, destinationOffset = 0, bytes = 0;
    bool writable = false;
};

/// @note 参照位置だけを保持し、使用時は現在の layout とコピー元のバイト範囲を照合する。
struct GBufferCopyPlan {
    std::array<GBufferCopyField, 14> fields{};

    explicit GBufferCopyPlan(const renderer::ShaderDescriptor& descriptor);

    bool Matches(const renderer::ShaderDescriptor& prepared, const renderer::ShaderDescriptor& current) const;

    void Copy(const std::vector<uint8_t>& source, std::array<uint8_t, 96>& output) const;
};

/// @note 一回の抽出だけで能力照会を共有し、次の抽出では変更・削除を再確認する。
class RenderMaterialResolver {
public:
    explicit RenderMaterialResolver(const renderer::ResourceManager* resources = nullptr);

    renderer::GeometryMaterialInput ReadSlotCapabilities(const MaterialSlot& slot);

    /// @note スロットのアセットロード後に呼び、同じ公開版の材質と設定から描画状態を得る。
    void ApplySlotState(const MaterialSlot& slot, renderer::RenderMaterial& output);

    /// @return 抽出内の既定パラメーター。未準備または対象外のスロットは nullptr。
    const PreparedMaterialParams* Prepare(MaterialComponent& component, size_t slotIndex,
        renderer::ResourceManager& resources, bool skinned);

    /// @note 上書きのないアセット既定値だけを、同じ抽出内で共有する。
    /// @note 通常の物体は別のサブメッシュが異なる投影サイズで同じスロットを同期できるため、テクスチャ付きの共有を許可しない。
    bool CopySynchronized(const MaterialSlot& slot, renderer::ResourceManager& resources, bool skinned,
        float screenPixels, renderer::RenderMaterial& output, bool shareTexturedDefaults = false) const;

    void RememberSynchronized(const MaterialSlot& slot, renderer::ResourceManager& resources, bool skinned,
        float screenPixels, const renderer::RenderMaterial& material, bool shareTexturedDefaults = false);

    bool CopyPacked(const MaterialSlot& slot, const renderer::Material& source, renderer::RenderMaterial& output,
        const renderer::ResourceManager& resources, const GBufferCopyPlan*& copyPlan,
        const renderer::ShaderDescriptor*& layout);

    void RememberPacked(const MaterialSlot& slot, const renderer::Material& source, const renderer::RenderMaterial& output,
        uint64_t shaderVersion);

private:
    renderer::GeometryMaterialInput ExtractCapabilities(const MaterialSlotAuthoring& slot,
        const asset::MaterialAsset::AuthoringSnapshotPtr& snapshot);

    /// @note 先行スロットのコールバックがアセットの公開版を変えずにシェーダーを再ロードできる。
    void RefreshCapabilitiesEpoch();

    static bool CanShareSynchronization(const MaterialSlot& slot);

    struct SynchronizedMaterial {
        renderer::RenderMaterial material;
        uint64_t shaderVersion = 0, resetVersion = 0;
        uint32_t textureRequest = 0;
        bool skinned = false;
    };
    struct PackedVariant {
        renderer::ResourceHandle<renderer::ShaderTag> shader;
        uint64_t shaderVersion = 0;
        std::vector<uint8_t> sourceBytes;
        std::array<uint8_t, 96> params{};
        bool direct = false;
    };
    struct CachedMaterial {
        PreparedMaterialParams prepared;
        std::optional<GBufferCopyPlan> copyPlan;
        std::vector<PackedVariant> packedVariants;
        std::vector<SynchronizedMaterial> synchronized;
        bool hasTextureRequests = false;
    };
    const renderer::ResourceManager* m_resources = nullptr;
    uint64_t m_resourceOwner = 0, m_resetVersion = 0, m_shaderVersion = 0;
    int m_assetGeneration = -1;
    std::unordered_map<std::string, bool> m_shaderCapabilities;
    std::unordered_map<asset::MaterialAsset::AuthoringSnapshotPtr, renderer::GeometryMaterialInput> m_geometryDefaults; ///< 追加ローブまたはブレンドにスロット固有の上書きがある場合は共有しない。
    std::unordered_map<asset::MaterialAsset::AuthoringSnapshotPtr, CachedMaterial> m_prepared; ///< 同一ハンドルへの差し替えを区別し、コールバックの間も既定値を所有する。
};

}
