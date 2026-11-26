/// @file    InstanceSetAsset.hpp
/// @brief   共有配置とメッシュ・材質の LOD 部品を定義する .fzdata アセット。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/Data/DataAsset.hpp>
#include <Graphics/Scene/RenderInstanceInput.hpp>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace fbzz::asset {
struct InstanceMeshPart {
    std::string meshPath, materialPath;
    uint32_t lod = 0; ///< このメッシュ部品を使用する LOD の段番号。0 が最高品質。
};
class InstanceSetAsset final : public DataAsset {
public:
    static constexpr const char* TYPE_NAME = "InstanceSetAsset"; ///< 型レジストリーと保存文書でインスタンス定義を識別する名前。
    const char* GetTypeName() const override { return TYPE_NAME; }
    FBZZ_ENGINE_API void Reflect(scene::IReflector& reflector) override;
    /// @return 編集値が無効なら null。それ以外はアセットが変わるまで同じスナップショットを返す。
    [[nodiscard]] std::shared_ptr<const renderer::InstanceSetData> Snapshot() const { return m_snapshot; }
    [[nodiscard]] std::span<const InstanceMeshPart> Parts() const { return m_parts; }
    [[nodiscard]] std::span<const float> LodHeights() const { return m_lodHeights; }
    /// @note 各部品は同じローカル配置変換を使う。ID は正で重複しない。
    [[nodiscard]] FBZZ_ENGINE_API bool Replace(std::span<const InstanceMeshPart> parts, std::span<const float> lodHeights,
        std::span<const renderer::InstancePlacement> placements);
private:
    FBZZ_ENGINE_API bool Rebuild();
    std::vector<std::string> m_meshPaths, m_materialPaths;
    std::vector<int> m_partLods, m_ids;
    std::vector<float> m_lodHeights{0.2f, 0.08f, 0.02f}; ///< 各 LOD に切り替える画面上の投影高さのしきい値。
    std::vector<math::Vector3> m_positions, m_rotations, m_scales;
    std::vector<InstanceMeshPart> m_parts; ///< 各 LOD と材質枠に対応する制作時のメッシュ部品列。
    std::shared_ptr<const renderer::InstanceSetData> m_snapshot; ///< 描画側へ公開する所有共有の不変インスタンス定義。
    uint64_t m_signature = 0, m_revision = 0; ///< 制作値の変化を照合する内容ハッシュと、公開を更新した版番号。
};
} // namespace fbzz::asset
