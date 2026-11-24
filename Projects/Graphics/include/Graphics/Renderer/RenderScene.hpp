/// @file    RenderScene.hpp
/// @brief   メッシュ描画候補と解決済み資源の不変スナップショット。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Graphics/Renderer/RenderLightingInput.hpp>
#include <Graphics/Renderer/RenderObjectGeometry.hpp>
#include <Graphics/Renderer/RenderObjectSelection.hpp>
#include <Graphics/Effects/RenderCustomPostInput.hpp>
#include <Graphics/Renderer/RenderEnvironment.hpp>
#include <Graphics/Effects/RenderTrailInput.hpp>
#include <Graphics/Effects/RenderDecalInput.hpp>
#include <Graphics/Effects/RenderTerrainInput.hpp>
#include <Graphics/Effects/RenderWaterInput.hpp>
#include <Graphics/Effects/RenderFiberInput.hpp>
#include <Graphics/Effects/RenderMeshTrailInput.hpp>
#include <Graphics/Effects/RenderParticleInput.hpp>
#include <Math/Vector4.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fbzz::renderer {

/// @brief マスク書き込み用のグループ。objectIndices は RenderScene::Objects() の添字。
struct RenderMaskGroup {
    math::Vector4 payload;
    bool visibleOnly = true;
    std::vector<uint32_t> objectIndices;
};

/// @brief レイ用の LOD0 を解決できなかった LODGroup の Entity と対象レイヤー。
struct RenderRayLodDiagnostic {
    uint32_t sourceIndex = 0;
    uint32_t sourceGeneration = 0;
    uint32_t layerMask = UINT32_MAX;
};

enum class RenderRayUnsupportedEffect : uint8_t { PARTICLE, FIBER };

/// @brief レイで未対応のエフェクトの発生元。
/// @note ラスターの可視判定から独立して抽出し、Entity ID はスナップショットの sceneGeneration と組にする。
struct RenderRayEffectSource {
    uint32_t sourceIndex = 0;
    uint32_t sourceGeneration = 0;
    uint32_t layerMask = 0;
    RenderRayUnsupportedEffect kind = RenderRayUnsupportedEffect::PARTICLE;
};

/// @brief Engine が抽出し、Graphics の描画パスが読むビューの入力。
/// @note GameObject / Component / Model / Mesh / Material のポインターを保持しない。
/// @note 通常 GPU ハンドルは非所有。preparedSkinning の出力だけは ticket が元入力と native lease を保持する。
/// @note 公開後は変更しない。候補のレイヤー・LOD・視錐台・距離は各ビューが評価する。
struct RenderScene {
    std::vector<uint32_t> waterfallObjects;
    std::vector<RenderCustomPostInput> customPost;
    RenderLightingInput lighting;
    RenderEnvironmentInput environment;
    std::vector<RenderTrailInput> trails;
    std::vector<RenderDecalInput> decals;
    std::vector<RenderTerrainInput> terrains;
    std::vector<RenderWaterInput> water;
    std::vector<RenderFiberInput> fibers;
    std::vector<RenderInstanceInput> instances;
    std::vector<RenderMeshTrailInput> meshTrails;
    std::vector<RenderParticleInput> particles;
    /// @note ビューごとの公開結果を識別する。同じフレームでも別の公開結果を区別できる。
    uint64_t snapshotSerial = 0;
    /// @note Entity ID が同じ値に再利用されても、抽出元のシーン世代を区別できる。
    uint64_t sceneGeneration = 0;
    uint64_t frameStamp = 0;
    RenderObjectStorage geometry;
    RenderObjectSelection selection;
    std::vector<RenderMeshItem> items;
    std::vector<RenderRayLodDiagnostic> rayLodDiagnostics;
    std::vector<RenderMaskGroup> maskGroups;
    uint32_t liveMaskRequests = 0;
    std::vector<RenderRayEffectSource> rayUnsupportedEffects;
    /// @note Engine の正規抽出では静的影候補の ordinal だけを保持する。
    std::vector<uint32_t> shadowBakeMeshObjects;

    /// @return RenderMeshItem::objectIndex が指す姿勢列。返した参照を使う間はスナップショットを保持する。
    [[nodiscard]] std::span<const RenderObject> Objects() const
    {
        return geometry.Read().objects;
    }

    /// @pre index は Objects() 内の有効な添字。ビュー固有の選択フラグを持つ場合は Objects() と同じ順序と件数。
    [[nodiscard]] bool IsObjectSelected(size_t index) const
    {
        const auto flags = selection.Flags();
        return flags.empty() ? Objects()[index].selected : flags[index] != 0;
    }

    /// @return 元の公開値なら選択添字。手動入力やコピーなら nullopt を返し、従来の選択フラグを使う。
    [[nodiscard]] std::optional<std::span<const uint32_t>> PublishedMeshSelection() const
    {
        return selection.Published();
    }
};

}
