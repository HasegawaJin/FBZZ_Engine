/// @file    FluidVolume.hpp
/// @brief   流体に浸かった剛体へ浮力・流れの抵抗を掛ける Volume。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#pragma once
#include <Physics/Volume.hpp>
#include <Math/Vector3.hpp>
#include <functional>
#include <memory>
#include <limits>
#include <unordered_map>

namespace fbzz::physics
{
    /// @brief 剛体ごとの、非トリガーコライダーの体積の合計 [m^3]。
    using BodyVolumeMap = std::unordered_map<const RigidBody*, float>;

    /// @brief 剛体ごとの «流体に浸かる大きさ» = 同じ体積を持つ球の半径 [m]。
    /// @note 球で近似する理由: Volume::Apply が受け取るのは RigidBody だけで、形状は分からない。
    ///       浮力に要るのは «どれだけ沈んだか» の割合なので、体積さえ合っていれば形は球で足りる。
    using BodyRadiusMap = std::unordered_map<const RigidBody*, float>;

    /// @brief 表に載っていない剛体に使う半径 [m]。
    constexpr float DEFAULT_BODY_RADIUS = 0.5f;

    /// @brief 体積表を等価球の半径表 r = cbrt(3V/4pi) へ直す。半径は 0.05〜50 m へクランプする。
    [[nodiscard]] std::shared_ptr<const BodyRadiusMap> MakeBodyRadii(const BodyVolumeMap& volumes);

    /// @brief FluidVolume 1 つぶんの入力。表面と流れは callback で受けるので、Physics は
    /// @brief 水面も格子も高さ場も知らない。
    struct FluidVolumeDesc
    {
        /// @brief 基準面 (center.y) からの表面の高さ [m]。空なら平らな面として扱う。
        /// @param worldX, worldZ ワールド座標 (変位«後»、つまり画面に出ている位置)。
        /// @param time startTime から Tick で進んだ秒数。
        std::function<float(float worldX, float worldZ, float time)> surfaceHeight;
        /// @brief ワールド空間の流速 [m/s]。空なら流れなし。
        std::function<math::Vector3(const math::Vector3& worldPosition)> flowVelocity;

        /// @brief 範囲となる矩形の中心。y が表面の基準面。
        math::Vector3 center = math::Vector3::ZERO;
        float halfX = 0.0f;
        float halfZ = 0.0f;

        /// @brief 完全に沈んだときの上向き加速度 [m/s^2]。
        float buoyancy = 15.0f;
        /// @brief 流体中での速度減衰 [1/s]。流れがあるときは «流体に対する» 速度に掛かる。
        float drag = 2.0f;
        /// @brief 浮力が届く表面からの深さ [m]。
        /// @note 上限を置く理由: 表面は厚みを持たない板なので、置いたままだと
        ///       «表面の真下にある洞窟» の中まで浮力が届いてしまう。
        float depthLimit = 10.0f;
        /// @brief 表面の評価に渡す初期時刻 [秒]。Tick(dt) がここから進める。
        /// @note Physics は Engine の Time を知らないので、時計の起点は呼び手が渡す。
        float startTime = 0.0f;
        /// @brief surfaceHeight の絶対値の上限 [m]。不明なら無限大で早期判定を無効にする。
        float surfaceHeightBound = std::numeric_limits<float>::infinity();

        /// @brief 剛体ごとの等価球半径。複数の FluidVolume で共有できるよう shared_ptr で持つ。
        std::shared_ptr<const BodyRadiusMap> radii;
    };

    /// @brief 矩形 x 深さで切った流体。等価球の水平 4 点で表面と比べ、沈み率ぶんの揚力・
    /// @brief トルク・流れの抵抗を掛ける。
    /// @note 順序 3 で密度ベースの法則 (rho * g * V_sub) とコライダーごとの標本点へ置き換える予定。
    /// @see Docs/design/buoyancy.md
    class FluidVolume final : public Volume
    {
    public:
        explicit FluidVolume(FluidVolumeDesc desc);

        bool Contains(const math::Vector3& position) const override;
        void Apply(RigidBody& body, float dt) override;
        void Tick(float dt) override;

    private:
        /// @brief 表面のワールド Y。
        float SurfaceY(float worldX, float worldZ) const;
        float RadiusOf(const RigidBody& body) const;

        FluidVolumeDesc m_desc;
        float m_maxRadius = DEFAULT_BODY_RADIUS;
        float m_time = 0.0f;
    };
} // namespace fbzz::physics
