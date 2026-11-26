/// @file    TerrainGridComponent.hpp
/// @brief   cellCountX × cellCountZ のグリッドで TerrainComponent エンティティを管理する。
/// @author  Hasegawa Jin
/// @date    2026-06-16

/// @note 複数 Terrain を手動でリンクするとセル数が増えるほどリンク数が二乗で増える。Grid 座標から
/// @note 隣接を自動解決することで、ユーザーはセル追加/削除だけに集中できる。TerrainRenderPass は
/// @note このグリッドを参照してエッジ頂点を隣接 Terrain に合わせる。
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/References/Entity.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <string>
#include <vector>

namespace fbzz::scene {

class Scene;

struct TerrainGridComponent {
    bool enabled   = true; ///< 地形グリッドの有効状態。
    int cellCountX = 4; ///< グリッドの X 方向のセル数。cells の 1 行の長さ。
    int cellCountZ = 4; ///< グリッドの Z 方向のセル数。cells の行数。

    /// @brief 新規セル作成時のデフォルト設定。
    /// @note MapEditorPanel の "Add New Terrain Here" および Inspector の "Apply to All Cells" が参照する。
    int   defaultColumns   = 65; ///< 新規セル内の地形の X 方向の頂点数。
    int   defaultRows      = 65; ///< 新規セル内の地形の Z 方向の頂点数。
    float defaultCellSize  = 2.0f; ///< 新規セル内の地形の格子間隔（ローカル m）。
    int   defaultChunkSize = 32; ///< 新規セルで 1 描画チャンクの辺に含める地形の格子数。

    /// @brief row-major: cells[gz * cellCountX + gx]。ランタイム解決済み EntityID。
    /// @note EntityID は起動ごとに変わり得るため、GUID (cellInstanceIds) を正規データとして保持し
    /// @note ロード後に ResolveFromScene() で変換する。
    std::vector<EntityID> cells; ///< Z 行・X 列の地形エンティティ。未解決または空のセルは INVALID。

    std::vector<std::string> cellInstanceIds; ///< 保存/復元用 GUID 配列。SceneSerializer が読み書きし、Load 後に ResolveFromScene() で解決する。

    const char* GetTypeName() const { return "TerrainGrid"; }

    void Reflect(IReflector& r) {
        r.Field("cellCountX",       cellCountX);
        r.Field("cellCountZ",       cellCountZ);
        r.Field("defaultColumns",   defaultColumns);
        r.Field("defaultRows",      defaultRows);
        r.Field("defaultCellSize",  defaultCellSize);
        r.Field("defaultChunkSize", defaultChunkSize);
        // cells / cellInstanceIds は SceneSerializer の専用コードで処理する。
    }

    /// @brief (gx, gz) のエンティティを返す。
    /// @return 範囲外・空なら INVALID。
    EntityID GetCell(int gx, int gz) const {
        if (gx < 0 || gx >= cellCountX || gz < 0 || gz >= cellCountZ) {
            return EntityID::INVALID;
        }
        const size_t idx = static_cast<size_t>(gz) * static_cast<size_t>(cellCountX)
                         + static_cast<size_t>(gx);
        return idx < cells.size() ? cells[idx] : EntityID::INVALID;
    }

    void SetCell(int gx, int gz, EntityID id) {
        if (gx < 0 || gx >= cellCountX || gz < 0 || gz >= cellCountZ) {
            return;
        }
        EnsureSize();
        cells[static_cast<size_t>(gz) * static_cast<size_t>(cellCountX)
            + static_cast<size_t>(gx)] = id;
    }

    void ClearCell(int gx, int gz) { SetCell(gx, gz, EntityID::INVALID); }

    /// @brief entityId → グリッド座標の逆引き。
    /// @return 見つからなければ false。
    bool TryGetGridPos(EntityID id, int& outGx, int& outGz) const {
        for (int z = 0; z < cellCountZ; ++z)
            for (int x = 0; x < cellCountX; ++x)
                if (GetCell(x, z) == id) { outGx = x; outGz = z; return true; }
        return false;
    }

    void EnsureSize() {
        const size_t need = static_cast<size_t>(cellCountX) * static_cast<size_t>(cellCountZ);
        if (cells.size() < need) {
            cells.resize(need, EntityID::INVALID);
        }
    }

    /// @brief SceneSerializer の Load 後に呼ぶ。cellInstanceIds → cells を解決する。
    FBZZ_ENGINE_API void ResolveFromScene(const Scene& scene);

    /// @brief 読み込み中に既に構築済みの GUID 索引を使ってセル参照を解決する。
    template<typename Resolver>
    void ResolveCellReferences(const Resolver& resolve) {
        EnsureSize();
        const size_t total = static_cast<size_t>(cellCountX) * static_cast<size_t>(cellCountZ);
        for (size_t i = 0; i < total; ++i) {
            cells[i] = i < cellInstanceIds.size() && !cellInstanceIds[i].empty()
                ? resolve(cellInstanceIds[i]) : EntityID::INVALID;
        }
    }

    /// @brief Save 前に呼ぶ。cells → cellInstanceIds を同期する。
    FBZZ_ENGINE_API void SyncInstanceIds(const Scene& scene);
};

}
