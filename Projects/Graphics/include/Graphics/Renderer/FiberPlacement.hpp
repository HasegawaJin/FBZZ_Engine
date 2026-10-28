/// @file    FiberPlacement.hpp
/// @brief   UV に依存しない表面ペイントと決定的なチャンク単位の草配置。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Graphics/Renderer/FiberGeometry.hpp>
#include <array>
#include <functional>
#include <map>

namespace fbzz::renderer {
inline constexpr uint32_t FIBER_MAX_PAINT_CELLS = 1048576;
inline constexpr uint32_t FIBER_MAX_PLACEMENT_BLADES = 2000000;

struct FiberPaintValue {
    float density = 1.0f;
    float length = 1.0f;
    bool operator==(const FiberPaintValue&) const = default;
};
/// @note セルは元三角形番号と重心座標の格子に結合する。位置・UV・GPU 資源を保存しない。
struct FiberPaintData {
    uint64_t meshSignature = 0;
    float cellSize = 0.5f;
    FiberPaintValue defaults;
    std::map<uint64_t, FiberPaintValue> cells;
    bool operator==(const FiberPaintData&) const = default;
};
enum class FiberPaintMode { DENSITY, LENGTH, SMOOTH };
struct FiberPaintStroke {
    math::Vector3 from{}, to{};
    float radius = 2.0f;
    float target = 1.0f;
    float softness = 0.35f;
    float maxSlope = 55.0f;
    FiberPaintMode mode = FiberPaintMode::DENSITY;
};
struct FiberPlacementOptions {
    float density = 200.0f;
    float width = 0.015f;
    float chunkSize = 4.0f;
    float maxSlope = 55.0f;
    uint32_t seed = 1;
    uint32_t maxBlades = 1000000;
    math::Matrix4 world = math::Matrix4::Identity();
};
/// @note タイルの原点・辺は Mesh のローカル空間。境界タイルは三角形、内部タイルは平行四辺形。
struct FiberPlacementTile {
    uint64_t key = 0;
    uint32_t triangle = 0;
    uint32_t divisions = 1;
    math::Vector3 origin{}, edgeU{}, edgeV{}, normal{};
    math::Vector2 uv{}, uvU{}, uvV{};
    bool half = false;
};
struct FiberPlacementChunk {
    std::array<int, 3> cell{};
    std::vector<FiberPlacementTile> tiles;
    std::vector<FiberBladeRoot> roots;
    math::Vector3 center{};
    float radius = 0;
    uint64_t contentSignature = 0;
    uint64_t revision = 0;
};
/// @note 根元は派生キャッシュ。ペイント変更時は内容署名が変わったチャンクだけ再生成する。
struct FiberPlacementCache {
    uint64_t layoutSignature = 0;
    uint64_t optionsSignature = 0;
    uint64_t revision = 0;
    uint32_t bladeCount = 0;
    std::vector<FiberPlacementChunk> chunks;
};

/// @return 不正または CPU 三角形なしなら 0。UV は署名へ含めない。
[[nodiscard]] uint64_t FiberMeshSignature(const Mesh& mesh);
/// @return 失敗時 out は未変更。重複 UV、UV なしを受理する。
[[nodiscard]] bool CreateFiberPaintData(const Mesh& mesh, float cellSize, float density, FiberPaintData& out);
[[nodiscard]] bool ValidateFiberPaintData(const FiberPaintData& data);
/// @note world は有限かつ可逆なアフィン行列。密度はワールド面積 [m²] あたり、幅はワールド [m]。
/// @return 不正入力・トポロジー不一致・総本数超過は false。失敗時 cache は未変更。
[[nodiscard]] bool UpdateFiberPlacement(const Mesh& mesh, const FiberPaintData& paint,
    const FiberPlacementOptions& options, FiberPlacementCache& cache);
/// @note ブラシはワールド [m] の線分。density は目標値への max/min 適用で、反復回数に依存しない。
/// @param visible 指定時はセル中心と元三角形番号で可視性を判定する。空なら可視性制限なし。
/// @return 失敗時 data は未変更。changed は実際に変化したセル数。
[[nodiscard]] bool PaintFiberSurface(const Mesh& mesh, const math::Matrix4& world,
    const FiberPaintStroke& stroke, FiberPaintData& data, uint32_t& changed,
    const std::function<bool(const math::Vector3&, uint32_t)>& visible = {});
/// @note ray の方向は正規化済みワールド空間。distance は最も近い正の交差 [m] で更新する。
[[nodiscard]] bool RaycastFiberSurface(const Mesh& mesh, const math::Matrix4& world,
    const math::Vector3& origin, const math::Vector3& direction, float& distance, uint32_t& triangle);
} /// @note namespace fbzz::renderer
