/// @file    WaterInteraction.hpp
/// @brief   水中接触の数値イベント・局所濁り・少数の再利用粒子を管理する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {
class Scene;
/// @note 波紋の CPU 近傍と GPU テクスチャで共有する解像度。
inline constexpr uint32_t kWaterRippleTextureSize = 128;
/// @note Scene の構造変更境界で処理する水しぶきを予約する。
FBZZ_ENGINE_API void QueueWaterSplash(Scene& scene, EntityID owner, const math::Vector3& worldPos, float intensity);
struct IReflector;
struct WaterComponent;
struct WaterInteractionSceneStore;

inline constexpr std::size_t WATER_INTERACTION_MAX_PENDING = 32;
inline constexpr std::size_t WATER_INTERACTION_MAX_ACTIVE = 32;
inline constexpr std::size_t WATER_INTERACTION_MAX_PARTICLES = 64;
inline constexpr std::size_t WATER_INTERACTION_MAX_TRACKED_BODIES = 128;
inline constexpr std::size_t WATER_INTERACTION_MAX_OWNERS = 256;
inline constexpr const char* WATER_BUBBLE_MATERIAL =
    "guid:219450eba3664e06b0294f27376b0d3f|Assets/Materials/Water/WaterBubbles.mat";
inline constexpr const char* WATER_SEDIMENT_MATERIAL =
    "guid:8d8c0b099aae44f3b9a30db32f1faf92|Assets/Materials/Water/WaterSediment.mat";

enum class WaterInteractionKind : uint8_t { SURFACE_FOAM = 0, BUBBLES = 1, SEDIMENT = 2 };

/// @brief ワールド空間の制作時インパルス。位置と半径は [m]、速度は [m/s]。
struct WaterInteractionImpulse {
    WaterInteractionKind kind = WaterInteractionKind::BUBBLES;
    math::Vector3 worldPosition{};
    math::Vector3 worldVelocity{};
    float radius = 0.25f;
    float strength = 0.5f;
    float lifetime = 2.0f;
    bool emitSurfaceRipple = true;
};

struct WaterInteractionSettings {
    bool enabled = true;
    bool bubblesEnabled = true;
    bool foamEnabled = true;
    bool sedimentEnabled = true;
    float minimumMotionSpeed = 1.5f;
    float motionInterval = 0.25f;
    float surfaceBand = 0.6f;
    float sedimentMinimumSpeed = 1.0f;
    float sedimentInterval = 0.4f;
    float bubbleLifetime = 2.0f;
    float sedimentLifetime = 5.0f;
    math::Vector3 sedimentScattering = {0.16f, 0.10f, 0.045f};
    std::string bubbleMaterialPath = WATER_BUBBLE_MATERIAL;
    std::string sedimentMaterialPath = WATER_SEDIMENT_MATERIAL;
    FBZZ_ENGINE_API void Reflect(IReflector& reflector);
};

/// @brief 不変の拡散雲データ。密度は散乱係数 [1/m] に掛ける倍率。
struct WaterInteractionPlume {
    WaterInteractionKind kind = WaterInteractionKind::SEDIMENT;
    math::Vector3 worldPosition{};
    math::Vector3 worldVelocity{};
    math::Vector3 scattering{};
    float radius = 0.25f;
    float density = 0.0f;
    float age = 0.0f;
    float lifetime = 2.0f;
};

/// @brief 描画側が保持するのは値のみ。Scene、GameObject、Physics、RHI の参照を含まない。
struct WaterInteractionFrame {
    uint64_t sceneGeneration = 0;
    EntityID owner = EntityID::INVALID;
    uint64_t ownerLifetime = 0;
    uint64_t revision = 0;
    std::vector<WaterInteractionPlume> plumes;
};

struct WaterInteractionEmitterState {
    EntityID id = EntityID::INVALID;
    uint64_t lifetime = 0;
};

/// @brief 水面が所有する一時状態。反射、保存、複製の対象外。
struct WaterInteractionRuntime {
    uint64_t sceneGeneration = 0;
    EntityID owner = EntityID::INVALID;
    uint64_t ownerLifetime = 0;
    uint64_t revision = 0;
    std::vector<WaterInteractionImpulse> pending;
    std::vector<WaterInteractionPlume> active;
    std::array<WaterInteractionEmitterState, 2> emitters{};
    std::shared_ptr<const WaterInteractionFrame> snapshot;
    std::shared_ptr<WaterInteractionSceneStore> sceneStore;
};

/// @return 入力が無効、水面が無効または除外対象、あるいは容量不足なら false。待ち行列は未変更。
/// @note この入口では値だけを記録し、構造変更は Cleanup で行う。
FBZZ_ENGINE_API bool QueueWaterInteraction(Scene& scene, EntityID owner, const WaterInteractionImpulse& impulse);

/// @brief 検証済みの波紋や水しぶきの位置を既存の水面へ投影し、泡を記録する。
FBZZ_ENGINE_API bool QueueWaterSurfaceInteraction(Scene& scene, EntityID owner, math::Vector3 worldPosition, float strength);

/// @brief WaterSystem の剛体走査で所有者と剛体の寿命を追跡し、泡の発生頻度を制限する。
FBZZ_ENGINE_API void SupplyWaterMotionImpulse(Scene& scene, EntityID owner, EntityID body,
    math::Vector3 worldPosition, math::Vector3 worldVelocity, float dt, bool nearSurface,
    bool emitSurfaceRipple = true);

/// @brief 追跡済み剛体の現在の固定ステップ接触を記録する。所有者や Scene の走査は不要。
FBZZ_ENGINE_API void SupplyWaterGroundContact(Scene& scene, EntityID body,
    math::Vector3 worldPoint, math::Vector3 worldNormal, float approachSpeed = 0.0f);

/// @brief 固定ステップの時間を累積する。接触は直列 Cleanup で失効し、追いつき処理中のステップでも有効。
FBZZ_ENGINE_API void BeginWaterInteractionPhysicsStep(Scene& scene, float fixedDt);

/// @brief 通常の描画入力の受け渡し時にランタイム値をコピーする。
FBZZ_ENGINE_API std::shared_ptr<const WaterInteractionFrame> WaterInteractionSnapshot(const WaterComponent& water);

/// @brief 制作データの複製で一時状態を消去し、別の所有者が生成した個体を保持する。
FBZZ_ENGINE_API void ResetWaterInteractionRuntime(WaterComponent& water);

/// @brief Scene の直列な非アクティブ化処理で、生成済みエミッターと一時データを破棄する。
FBZZ_ENGINE_API void ClearWaterInteractions(Scene& scene);
}
