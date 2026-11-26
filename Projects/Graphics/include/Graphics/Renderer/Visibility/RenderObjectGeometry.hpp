/// @file    RenderObjectGeometry.hpp
/// @brief   描画候補の姿勢と、その配列について検証した空間差分。
/// @author  Hasegawa Jin
/// @date    2026-11-23
#pragma once
#include <Graphics/Renderer/Geometry/RenderGeometryInput.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace fbzz::renderer {

/// @note version は形状の公開版。baseVersion・シーン世代・件数が一致した場合だけ添字による差分を適用できる。
struct RenderSpatialUpdate {
    uint64_t version = 0;
    uint64_t baseVersion = 0;
    size_t objectCount = 0;
    bool fullReplacement = true;
    std::vector<uint32_t> changedObjectOrdinals;
};

/// @note 差分の添字はこの姿勢列を指す。構成・順序・レンダラーの寿命が変われば全件を再公開する。
struct RenderObjectGeometry {
    std::vector<RenderObject> objects;
    std::optional<RenderSpatialUpdate> spatialUpdate;
};

/// @brief 抽出中は姿勢と差分を直接所有し、複数ビューへ公開するときだけ同じ不変値を参照する。
class RenderObjectStorage {
public:
    [[nodiscard]] const RenderObjectGeometry& Read() const { return m_shared ? *m_shared : m_local; }
    [[nodiscard]] bool IsShared() const { return static_cast<bool>(m_shared); }
    [[nodiscard]] const RenderSpatialUpdate* SpatialUpdate() const
    {
        const auto& update = Read().spatialUpdate;
        return update ? &*update : nullptr;
    }

    /// @note 書き込み前に以前の空間差分を失効させる。共有中なら、このビューの姿勢だけを複製する。
    std::vector<RenderObject>& EditObjects()
    {
        if (m_shared) {
            ReplaceObjects(m_shared->objects);
        }
        m_local.spatialUpdate.reset();
        return m_local.objects;
    }
    void SetSpatialUpdate(std::optional<RenderSpatialUpdate> update)
    {
        if (m_shared) {
            ReplaceObjects(m_shared->objects);
        }
        m_local.spatialUpdate = std::move(update);
    }
    void ReplaceObjects(std::vector<RenderObject> objects)
    {
        m_local = {std::move(objects), std::nullopt};
        m_shared.reset();
    }
    void UseShared(std::shared_ptr<const RenderObjectGeometry> geometry)
    {
        m_local = {};
        m_shared = std::move(geometry);
    }

private:
    RenderObjectGeometry m_local;
    std::shared_ptr<const RenderObjectGeometry> m_shared;
};

}
