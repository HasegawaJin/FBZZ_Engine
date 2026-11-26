/// @file    RenderScene.hpp
/// @brief   メッシュ描画候補と解決済み資源の不変スナップショット。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Graphics/Renderer/View/Lighting/RenderLightingInput.hpp>
#include <Graphics/Renderer/Visibility/RenderObjectGeometry.hpp>
#include <Graphics/Renderer/Visibility/RenderObjectSelection.hpp>
#include <Graphics/Effects/PostProcess/RenderCustomPostInput.hpp>
#include <Graphics/Renderer/View/Lighting/RenderEnvironment.hpp>
#include <Graphics/Effects/Trails/RenderTrailInput.hpp>
#include <Graphics/Effects/Decals/RenderDecalInput.hpp>
#include <Graphics/Effects/Surfaces/RenderTerrainInput.hpp>
#include <Graphics/Effects/Surfaces/RenderWaterInput.hpp>
#include <Graphics/Effects/Fiber/RenderFiberInput.hpp>
#include <Graphics/Effects/Trails/RenderMeshTrailInput.hpp>
#include <Graphics/Effects/Particles/RenderParticleInput.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fbzz::renderer {

/// @brief マスク書き込み用のグループ。objectIndices は RenderScene::Objects() の添字。
struct RenderMaskGroup {
    math::Vector4 payload; ///< マスクターゲットへ書き込む値。
    bool visibleOnly = true; ///< 可視オブジェクトだけをマスクへ書くかどうか。
    std::vector<uint32_t> objectIndices; ///< Objects() 内の対象オブジェクトの添字列。
};

/// @brief レイ用の LOD0 を解決できなかった LODGroup の Entity と対象レイヤー。
struct RenderRayLodDiagnostic {
    uint32_t sourceIndex = 0; ///< 抽出元 Entity の添字。
    uint32_t sourceGeneration = 0; ///< 同じ添字を再利用した別 Entity と区別する世代。
    uint32_t layerMask = UINT32_MAX; ///< 発生元を含む描画レイヤーのビット集合。
};

enum class RenderRayUnsupportedEffect : uint8_t { PARTICLE, FIBER };

/// @brief レイで未対応のエフェクトの発生元。
/// @note ラスターの可視判定から独立して抽出し、Entity ID はスナップショットの sceneGeneration と組にする。
struct RenderRayEffectSource {
    uint32_t sourceIndex = 0; ///< 抽出元 Entity の添字。
    uint32_t sourceGeneration = 0; ///< 同じ添字を再利用した別 Entity と区別する世代。
    uint32_t layerMask = 0; ///< 発生元を含む描画レイヤーのビット集合。
    RenderRayUnsupportedEffect kind = RenderRayUnsupportedEffect::PARTICLE; ///< レイで未対応の発生源を診断に表示する種別。
};

/// @brief Engine が抽出し、Graphics の描画パスが読むビューの入力。
/// @note GameObject / Component / Model / Mesh / Material のポインターを保持しない。
/// @note 通常 GPU ハンドルは非所有。preparedSkinning の出力だけは ticket が元入力と native lease を保持する。
/// @note 公開後は変更しない。候補のレイヤー・LOD・視錐台・距離は各ビューが評価する。
struct RenderScene {
    std::vector<uint32_t> waterfallObjects; ///< Objects() 内の滝の描画候補を指す添字列。
    std::vector<RenderCustomPostInput> customPost; ///< ビューに追加するカスタムポストプロセスの入力列。
    RenderLightingInput lighting; ///< フレームのライトと影の描画入力。
    RenderEnvironmentInput environment; ///< 空、大気、環境光の描画入力。
    std::vector<RenderTrailInput> trails; ///< 軌跡をリボンとして描くための入力列。
    std::vector<RenderDecalInput> decals; ///< シーン表面に投影するデカールの入力列。
    std::vector<RenderTerrainInput> terrains; ///< 地形パスへ渡す地形の入力列。
    std::vector<RenderWaterInput> water; ///< 水面パスが読む水域の入力列。
    std::vector<RenderFiberInput> fibers; ///< ファイバー描画用の表面入力列。
    std::vector<RenderInstanceInput> instances; ///< インスタンシング描画用にまとめた入力列。
    std::vector<RenderMeshTrailInput> meshTrails; ///< 残像として描くメッシュとその色の入力列。
    std::vector<RenderParticleInput> particles; ///< 描画・GPU 更新に使うパーティクルの入力列。
    /// @note ビューごとの公開結果を識別する。同じフレームでも別の公開結果を区別できる。
    uint64_t snapshotSerial = 0; ///< 同じフレーム内の別の公開結果も区別する識別番号。
    /// @note Entity ID が同じ値に再利用されても、抽出元のシーン世代を区別できる。
    uint64_t sceneGeneration = 0; ///< 抽出元シーンの世代。Entity ID の再利用と区別する。
    uint64_t frameStamp = 0; ///< 抽出結果を作った描画フレームの番号。
    RenderObjectStorage geometry; ///< Objects() が返す描画オブジェクト列の保持先。
    RenderObjectSelection selection; ///< ビューで選択されたオブジェクトの状態。
    std::vector<RenderMeshItem> items; ///< マテリアルスロット単位のメッシュ候補。
    std::vector<RenderRayLodDiagnostic> rayLodDiagnostics; ///< レイ用の LOD0 を取得できなかった候補と発生元。
    std::vector<RenderMaskGroup> maskGroups; ///< 同じマスク値で描くオブジェクトのグループ列。
    uint32_t liveMaskRequests = 0; ///< このフレームで有効なマスク要求の数。
    std::vector<RenderRayEffectSource> rayUnsupportedEffects; ///< レイで描画できないエフェクトの発生元。
    /// @note Engine の正規抽出では静的影候補の ordinal だけを保持する。
    std::vector<uint32_t> shadowBakeMeshObjects; ///< 静的影を焼く際に使う Objects() 内の候補の添字列。

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
