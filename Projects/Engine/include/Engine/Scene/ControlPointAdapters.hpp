/// @file    ControlPointAdapters.hpp
/// @brief   Water と Spline の点参照を数値 authoring snapshot へ変換する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/ControlPointResolver.hpp>
#include <Engine/Scene/ControlPointModes.hpp>
#include <Engine/Scene/ObjectReference.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::asset { struct ClothAsset; }

namespace fbzz::scene {

struct IReflector;
class Scene;
struct SplineComponent;
struct WaterComponent;

inline constexpr std::uint32_t CONTROL_POINT_ADAPTER_VERSION = 1;

/// @brief 水面用の安定 ID 付きバインド行と点ごとの水面属性。
struct WaterControlPointBinding {
    std::uint32_t id = 0;
    ObjectReference target;
    float width = 0.0f;
    float depth = 0.0f;
    float flowSpeed = 0.0f;

    /// @brief 安定 ID 付きバインド行を、順序と属性を保ったまま反射する。
    FBZZ_ENGINE_API void Reflect(IReflector& reflector);
    FBZZ_ENGINE_API bool operator==(const WaterControlPointBinding& other) const;
};

/// @brief スプライン用の安定 ID 付きバインド行。
struct SplineControlPointBinding {
    std::uint32_t id = 0;
    ObjectReference target;

    /// @brief 安定 ID 付きバインド行を、順序を保ったまま反射する。
    FBZZ_ENGINE_API void Reflect(IReflector& reflector);
    FBZZ_ENGINE_API bool operator==(const SplineControlPointBinding& other) const;
};

/// @brief 静的ベイクに保存する所有者ローカル空間の数値スプライン点。
struct SplineControlPointValue {
    std::uint32_t id = 0;
    math::Vector3 localPosition = math::Vector3::ZERO;
    bool operator==(const SplineControlPointValue& other) const = default;
};

/// @brief 入力元の選択、ベイクの妥当性、数値制約に関するアダプターの結果。
enum class ControlPointAdapterStatus : std::uint8_t {
    SUCCESS,
    INVALID_SOURCE_MODE,
    INVALID_RUNTIME_MODE,
    INVALID_NUMERIC_DATA,
    INVALID_LAKE_PLANE,
    MISSING_BAKE,
    STALE_BAKE,
    RESOLUTION_FAILED,
};

/// @brief 平面形状の生成前に水面の入力行を解決した結果。
struct WaterControlPointSnapshot {
    std::vector<WaterSplinePoint> points;
    WaterSurfaceShape surfaceShape = WaterSurfaceShape::RECTANGLE;
    float lakePlaneHeight = 0.0f;
    ControlPointAdapterStatus status = ControlPointAdapterStatus::SUCCESS;
    ControlPointResolveStatus resolveStatus = ControlPointResolveStatus::SUCCESS;
    std::uint64_t inputSignature = 0;
    bool bakeStale = false;
    bool valid = false;
};

/// @brief 追従点のサンプリング前にスプラインの入力行を解決した結果。
struct SplineControlPointSnapshot {
    std::vector<SplineControlPointValue> points;
    /// @brief 解決した点列に適用する開曲線または閉曲線の扱い。
    bool closed = false;
    ControlPointAdapterStatus status = ControlPointAdapterStatus::SUCCESS;
    ControlPointResolveStatus resolveStatus = ControlPointResolveStatus::SUCCESS;
    std::uint64_t inputSignature = 0;
    bool bakeStale = false;
    bool valid = false;
};

/// @brief 水面のバインド行を順に反射し、上限超過を示す番兵行を保持する。
FBZZ_ENGINE_API void ReflectWaterControlPointBindings(IReflector& reflector,
    std::vector<WaterControlPointBinding>& bindings);

/// @brief スプラインのバインド行を順に反射し、上限超過を示す番兵行を保持する。
FBZZ_ENGINE_API void ReflectSplineControlPointBindings(IReflector& reflector,
    std::vector<SplineControlPointBinding>& bindings);

/// @brief 安定 ID 付きの数値スプラインベイク点を反射する。
FBZZ_ENGINE_API void ReflectSplineControlPointValues(IReflector& reflector,
    const char* fieldName, std::vector<SplineControlPointValue>& points);

/// @brief 既存のスプライン点スキーマで水面のベイク行を反射する。
FBZZ_ENGINE_API void ReflectWaterControlPointValues(IReflector& reflector,
    const char* fieldName, std::vector<WaterSplinePoint>& points);

/// @brief 上限付きの所有者ローカル空間の湖面除外輪郭を反射する。
FBZZ_ENGINE_API void ReflectWaterLakeIslandPoints(IReflector& reflector,
    std::vector<WaterLakeIslandPoint>& points);

/// @brief 布のソルバー粒子の識別情報と三角形トポロジーをハッシュする。位置と材質は入力に含めない。
/// @param asset 格子から布を生成する場合は null。
/// @param generatedSegments 生成する布の格子分割数。
/// @return 位置または材質だけの編集では変わらない非ゼロの署名。
[[nodiscard]] FBZZ_ENGINE_API std::uint64_t ClothTopologySignature(
    const asset::ClothAsset* asset, std::uint32_t generatedSegments);

/// @brief 接続候補について現在の布のトポロジーと粒子数を返す。
/// @param asset 現在のインポート済みアセット。格子生成の場合は null。
/// @param generatedSegments 現在の格子分割数。
/// @param particle 接続候補のソルバー粒子の添字。
/// @param signature 成功時のみトポロジーの署名を格納する。
/// @param particleCount 現在のソルバー粒子数を格納する。
/// @return トポロジーが無効か、particle が現在の範囲外なら false。
[[nodiscard]] FBZZ_ENGINE_API bool ResolveClothTopologyForParticle(const asset::ClothAsset* asset,
    std::uint32_t generatedSegments, int particle, std::uint64_t& signature,
    std::uint32_t& particleCount);

/// @brief コンポーネントのランタイムモードにかかわらず、制作時の入力値を解決する。
/// @note signatureRuntimeMode を使うと、ベイク処理が確定予定のモードの署名を採取できる。
[[nodiscard]] FBZZ_ENGINE_API WaterControlPointSnapshot ResolveWaterControlPointAuthoring(
    Scene& scene, EntityID ownerId, WaterComponent& water,
    ControlPointRuntimeMode signatureRuntimeMode);

/// @brief 静的ベイクが選択されている場合はその保存値を含め、現在の水面ランタイム値を解決する。
/// @note RUNTIME は静的な制作時の参照先を解決しないため、Scene のアクティブ化時にベイクを検証する。
[[nodiscard]] FBZZ_ENGINE_API WaterControlPointSnapshot ResolveWaterControlPoints(
    Scene& scene, EntityID ownerId, WaterComponent& water,
    ControlPointReadContext context = ControlPointReadContext::AUTHORING);

/// @brief コンポーネントのランタイムモードにかかわらず、制作時の入力値を解決する。
/// @note signatureRuntimeMode を使うと、ベイク処理が確定予定のモードの署名を採取できる。
[[nodiscard]] FBZZ_ENGINE_API SplineControlPointSnapshot ResolveSplinePointAuthoring(
    Scene& scene, EntityID ownerId, SplineComponent& spline,
    ControlPointRuntimeMode signatureRuntimeMode);

/// @brief 静的ベイクが選択されている場合はその保存値を含め、現在のスプラインランタイム値を解決する。
/// @note RUNTIME は静的な制作時の参照先を解決しないため、Scene のアクティブ化時にベイクを検証する。
[[nodiscard]] FBZZ_ENGINE_API SplineControlPointSnapshot ResolveSplinePoints(
    Scene& scene, EntityID ownerId, SplineComponent& spline,
    ControlPointReadContext context = ControlPointReadContext::AUTHORING);

/// @brief 水面やスプラインの静的ベイクに欠落、不正、古い入力があればランタイム開始前に拒否する。
/// @param scene 現在の数値制作データを各ベイクと照合する対象。
/// @return LIVE 以外の制御点ポリシーが保存済みスナップショットで実行できない場合は false。
[[nodiscard]] FBZZ_ENGINE_API bool ValidateStaticControlPointBakes(Scene& scene);

}
