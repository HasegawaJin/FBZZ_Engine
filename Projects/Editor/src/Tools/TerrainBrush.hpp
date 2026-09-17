/// @file    TerrainBrush.hpp
/// @brief   Terrain ブラシの純粋カーネル（座標変換・フォールオフ・Sculpt/Paint/Ramp/Hole の適用）。
/// @author  Hasegawa Jin
/// @date    2026-08-14
///
/// @note 同じブラシを人 (TerrainTool) と AI (Command Bus: terrain.sculpt/paint/ramp/hole) の 2 経路が叩く。
///       別実装だと同じ radius/strength でも結果がずれ、«AI が塗った縁だけ出る» 形でしか現れない。
/// @note レイキャスト・ImGui・Undo・dirty フラグ管理は含まない。呼び出し側 (ツール/ディスパッチャ) の責務。
/// @see Docs/design/terrain-layers.md
#pragma once
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>

namespace fbzz::editor {

/// @brief ブラシ中心からの距離に対する減衰カーブ。
enum class TerrainFalloff : std::uint8_t {
    Linear,   ///< 距離に比例して線形減衰
    Smooth,   ///< smoothstep（端が滑らか）
    Gaussian, ///< ガウス曲線（自然な盛り上がり）
};

/// @brief 高さ彫刻の演算種別。
/// @note 保存済み int 値と AI バスの op 名を変えないため、追加は必ず末尾へ置く。
enum class TerrainSculptOp : std::uint8_t {
    Raise,            ///< 高さを上げる
    Lower,            ///< 高さを下げる（基準面より下へ掘れる）
    Smooth,           ///< 周囲 4 近傍と平滑化
    Flatten,          ///< 指定した基準高さへ寄せる
    Stamp,            ///< ブラシ形状を押し付ける（height = max(h, weight)）
    Noise,            ///< fBm 値ノイズを加算する
    ThermalErosion,   ///< 安息角を超えた斜面を崩す
    HydraulicErosion, ///< 水滴が削って運び、低い所へ置く
    Terrace,          ///< 高さを段々に寄せる
};

/// @brief ブラシ形状と操作ごとの設定。radius はワールド単位、strength は 1 秒あたりの最大変化量。
struct TerrainBrush {
    float          radius   = 5.0f;
    float          strength = 0.05f;
    TerrainFalloff falloff  = TerrainFalloff::Smooth;

    float         noiseScale       = 8.0f;  ///< Noise: 最も粗いオクターブの周期 [m]
    int           noiseOctaves     = 4;     ///< Noise: [1, 8]
    std::uint32_t seed             = 1;     ///< Noise / HydraulicErosion の乱数の種
    float         terraceStep      = 2.0f;  ///< Terrace: 段の高さ [m]
    float         terraceSharpness = 0.5f;  ///< Terrace: 0 = 変化なし、1 = 垂直な段
    float         talusDegrees     = 35.0f; ///< ThermalErosion: これより急な斜面を崩す [度]
    int           erosionDroplets  = 48;    ///< HydraulicErosion: 1 回の適用で落とす水滴の数
};

/// @brief ワールド座標を Terrain の heightData が使うローカル座標へ変換する。
/// @note position の減算だけでは親 Transform・回転・スケールを反映できずずれるため、描画と同じ World Matrix を使う。
math::Vector3 ToTerrainLocal(const scene::Transform& transform, const math::Vector3& worldPoint);

/// @brief Terrain ローカル座標を描画空間のワールド座標へ変換する。
math::Vector3 ToTerrainWorld(const scene::Transform& transform, const math::Vector3& localPoint);

/// @brief ブラシ円 (中心 localCenter.xz / 半径 radius) が Terrain の XZ 矩形と重なるか判定する。
/// @note 重ならない Terrain まで undo スナップショットを取ると無駄が大きいため、円と矩形の最近点距離で早期判定する。
bool BrushOverlapsTerrainXZ(const scene::TerrainComponent& terrain,
                            const math::Vector3& localCenter, float radius);

/// @return ブラシ中心からの距離 dist に対するフォールオフウェイト [0, 1]。
float TerrainBrushWeight(const TerrainBrush& brush, float dist);

/// @brief 塗る対象レイヤーを Terrain ごとに解決する。
/// @return この Terrain で塗る層の番号。-1 は «この Terrain には塗らない»。
/// @note Grid 境界をまたぐ Paint で同じ番号が別マテリアルを指しうるため、material path があればそれで解決する。
int ResolvePaintLayerForTerrain(const scene::TerrainComponent& terrain,
                                const std::string&             sourceMaterial,
                                int                            preferredLayer);

/// @brief ブラシを heightData へ適用する。
/// @param hitLocal Terrain ローカル座標。
/// @param flattenTargetWorldHeight Flatten のときだけ使う基準高さ（ローカル Y [m]）。
/// @param dt 押し続けた時間に比例して効く対話ツールの意味論を保つための秒数。
/// @param strokeStep 同じストローク内で何回目の適用か。HydraulicErosion の乱数を回ごとに変える。
/// @note 呼び出し後に TerrainComponent::RequestHeightRebuild() を立てるのは呼び出し側の責務。
/// @see https://history.siggraph.org/learning/the-synthesis-and-rendering-of-eroded-fractal-terrains-by-musgrave-kolb-and-mace/
/// @see https://www.firespark.de/resources/downloads/implementation%20of%20a%20methode%20for%20hydraulic%20erosion.pdf
void ApplyTerrainSculpt(scene::TerrainComponent& terrain,
                        const math::Vector3&     hitLocal,
                        const TerrainBrush&      brush,
                        TerrainSculptOp          op,
                        float                    flattenTargetWorldHeight,
                        float                    dt,
                        std::uint32_t            strokeStep = 0);

/// @brief ブラシでスプラットを layerIndex へ寄せる (terrain_splat::BlendToward)。
/// @param layerIndex Terrain ごとに解決済みの番号 [0, LayerCount)。範囲外なら何もしない。
/// @note 呼び出し後に TerrainComponent::RequestSplatRebuild() を立てるのは呼び出し側の責務。
void ApplyTerrainPaint(scene::TerrainComponent& terrain,
                       const math::Vector3&     hitLocal,
                       const TerrainBrush&      brush,
                       int                      layerIndex,
                       float                    dt);

/// @brief 始点から終点へ線形に傾く坂を作る。線分から radius 以内をフォールオフ付きで寄せる。
/// @param startLocal / endLocal Terrain ローカル座標。y が坂の両端の高さ [m]。
/// @note 1 ストロークに 1 回だけ呼ぶ (dt を掛けない)。strength = 1 で線分上は坂の高さに一致する。
void ApplyTerrainRamp(scene::TerrainComponent& terrain,
                      const math::Vector3&     startLocal,
                      const math::Vector3&     endLocal,
                      const TerrainBrush&      brush);

/// @brief セル中心がブラシ円に入るセルの穴を立てる / 消す。
/// @return 1 セルでも変化したか。
/// @note 呼び出し後に TerrainComponent::RequestHoleRebuild() を立てるのは呼び出し側の責務。
bool ApplyTerrainHole(scene::TerrainComponent& terrain,
                      const math::Vector3&     hitLocal,
                      const TerrainBrush&      brush,
                      bool                     hole);

} // namespace fbzz::editor
