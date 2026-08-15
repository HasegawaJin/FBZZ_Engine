// FBZZ Engine
// TerrainBrush.hpp | fbzz::editor
// Terrain ブラシの純粋カーネル（座標変換・フォールオフ・Sculpt/Paint の適用）。
//
// WHY 分離するか:
//   同じブラシを叩く経路が 2 つある —— 人が使う TerrainTool (マウス入力) と、
//   AI が使う Command Bus (terrain.sculpt / terrain.paint)。ここを別実装にすると、
//   同じ radius / strength を指定しても「人が塗った結果」と「AI が塗った結果」が
//   食い違う。特に Paint の splat 正規化 (4ch 整数和を常に 255 に保つ端数配分) は
//   書き直せば必ずズレる種類のコードで、ズレは「AI が塗ったところだけ縁が出る」と
//   いう形でしか現れず原因に辿り着けない。入力デバイスに依存しない部分だけを
//   ここへ集約し、TerrainTool も EditorBusDispatcher もこの 1 実装を呼ぶ。
//
// ここに置かないもの: レイキャスト・ImGui・Undo・dirty フラグ管理。
//   それらは呼び出し側 (ツール / ディスパッチャ) の責務。
#pragma once
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>

namespace fbzz::editor {

// ブラシ中心からの距離に対する減衰カーブ。
enum class TerrainFalloff : std::uint8_t {
    Linear,   // 距離に比例して線形減衰
    Smooth,   // smoothstep（端が滑らか）
    Gaussian, // ガウス曲線（自然な盛り上がり）
};

// 高さ彫刻の演算種別。
enum class TerrainSculptOp : std::uint8_t {
    Raise,   // 高さを上げる
    Lower,   // 高さを下げる（基準面より下へ掘れる）
    Smooth,  // 周囲 4 近傍と平滑化
    Flatten, // 指定した基準高さへ寄せる
    Stamp,   // ブラシ形状を押し付ける（height = max(h, weight)）
};

// ブラシ形状。radius はワールド単位、strength は 1 秒あたりの最大変化量。
struct TerrainBrush {
    float          radius   = 5.0f;
    float          strength = 0.05f;
    TerrainFalloff falloff  = TerrainFalloff::Smooth;
};

// ワールド座標を Terrain の heightData が使うローカル座標へ変換する。
// WHY: position の減算だけでは親 Transform・回転・スケールを反映できず、
//      指定位置と実際に編集されるハイトマップ座標がずれるため、描画と同じ World Matrix を使う。
math::Vector3 ToTerrainLocal(const scene::Transform& transform, const math::Vector3& worldPoint);

// Terrain ローカル座標を描画空間のワールド座標へ変換する。
math::Vector3 ToTerrainWorld(const scene::Transform& transform, const math::Vector3& localPoint);

// ブラシ円（中心 localCenter.xz / 半径 radius）が Terrain の XZ 矩形と重なるか判定する。
// WHY: ブラシ範囲に重なる Terrain だけを編集対象にするための早期判定。重ならない Terrain まで
//      undo スナップショット（heightData/splatData の完全コピー）を取ると無駄が大きいため、
//      円と矩形の最近点距離で正確に重なりを求める。
bool BrushOverlapsTerrainXZ(const scene::TerrainComponent& terrain,
                            const math::Vector3& localCenter, float radius);

// ブラシ中心からの距離 dist に対してフォールオフウェイト [0, 1] を返す。
float TerrainBrushWeight(const TerrainBrush& brush, float dist);

// 塗る対象レイヤーを Terrain ごとに解決する。-1 は「この Terrain には塗らない」。
// WHY: Terrain ごとに layerMaterials を持つため、Grid 境界をまたいだ Paint で「同じ index」を
//      塗ると別マテリアルへ誤って塗る可能性がある。material path があればそれで解決する。
int ResolvePaintLayerForTerrain(const scene::TerrainComponent& terrain,
                                const std::string&             sourceMaterial,
                                int                            preferredLayer);

// ブラシを heightData へ適用する。hitLocal は Terrain ローカル座標。
// flattenTargetWorldHeight は Flatten のときだけ使う基準高さ（ワールド単位）。
// dt を掛けるのは「押し続けた時間に比例して効く」という対話ツールの意味論を保つため。
// 呼び出し後に TerrainComponent::RequestHeightRebuild() を立てるのは呼び出し側の責務。
void ApplyTerrainSculpt(scene::TerrainComponent& terrain,
                        const math::Vector3&     hitLocal,
                        const TerrainBrush&      brush,
                        TerrainSculptOp          op,
                        float                    flattenTargetWorldHeight,
                        float                    dt);

// ブラシを splatData へ適用する。layerIndex は Terrain ごとに解決済みの [0, 3]。
// 呼び出し後に TerrainComponent::RequestSplatRebuild() を立てるのは呼び出し側の責務。
void ApplyTerrainPaint(scene::TerrainComponent& terrain,
                       const math::Vector3&     hitLocal,
                       const TerrainBrush&      brush,
                       int                      layerIndex,
                       float                    dt);

} // namespace fbzz::editor
