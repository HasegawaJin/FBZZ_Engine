/// @file    WorldPartitionComponent.hpp
/// @brief   保存対象のワールドマニフェスト、ストリーミング距離、予算と取得元の選択。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Reflection/Reflection.hpp>
#include <Engine/Scene/References/ObjectReference.hpp>
#include <string>

namespace fbzz::scene {
struct WorldPartitionComponent {
    bool enabled = true; ///< このワールドのセルを距離に応じてストリーミングする。
    std::string manifestPath; ///< セルの範囲と近景・遠景アセットを列挙するマニフェストの参照。
    math::Vector3 gridOrigin{}; ///< グリッドのセル座標系の起点（ワールド座標）。
    float cellSize = 256.0f; ///< XZ グリッドの 1 セルの辺長（ワールド m）。
    bool streamInEditor = false; ///< Play 外のエディターでもセルのストリーミングを実行する。
    bool useMainCamera = true; ///< 明示したカメラがない場合に、メインカメラを距離判定の基準にする。
    ObjectReference sourceCamera; ///< 距離判定の位置を供給する明示カメラへの参照。
    math::Vector3 sourcePosition{}; ///< カメラを使えない場合の距離判定の基準点（ワールド座標）。
    float nearDistance = 384.0f; ///< 近景セルを読み込む基準距離（m）。
    float hlodDistance = 1536.0f; ///< 遠景 HLOD セルを読み込む基準距離（m）。
    float hysteresis = 64.0f; ///< セルが境界で頻繁に切り替わらないよう読み込みと退避の距離に加える余裕（m）。
    int maxConcurrentLoads = 2; ///< 同時に実行するセルの読み込みジョブ数の上限。
    int maxPublishesPerFrame = 1; ///< 読み込み済みのセルを 1 フレームにシーンへ反映する件数の上限。
    int maxResidentCells = 256; ///< 同時にシーンへ公開して保持するセル数の上限。
    int maxCellMegabytes = 16; ///< 1 セルの読み込みに許すサイズの上限（MiB）。
    int maxResidentMegabytes = 512; ///< シーンへ保持するセルの合計サイズの上限（MiB）。
    const char* GetTypeName() const { return "World Partition"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("manifestPath", manifestPath);
        r.Field("gridOrigin", gridOrigin);
        r.Field("cellSize", cellSize);
        r.Field("streamInEditor", streamInEditor);
        r.Field("useMainCamera", useMainCamera);
        r.TypedObjectReferenceField("sourceCamera", sourceCamera, "CameraComponent");
        r.Field("sourcePosition", sourcePosition);
        r.Field("nearDistance", nearDistance);
        r.Field("hlodDistance", hlodDistance);
        r.Field("hysteresis", hysteresis);
        r.Field("maxConcurrentLoads", maxConcurrentLoads);
        r.Field("maxPublishesPerFrame", maxPublishesPerFrame);
        r.Field("maxResidentCells", maxResidentCells);
        r.Field("maxCellMegabytes", maxCellMegabytes);
        r.Field("maxResidentMegabytes", maxResidentMegabytes);
    }
};
}
