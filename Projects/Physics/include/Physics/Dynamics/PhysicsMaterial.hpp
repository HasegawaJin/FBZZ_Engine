/// @file    PhysicsMaterial.hpp
/// @brief   剛体の表面物性 (反発・摩擦・密度) のプリセット定義。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Physics/Api.hpp>

#include <cstdint>

namespace fbzz::physics
{
    /// @brief 2 つのマテリアルが接触したときの合成規則。
    /// @note 固定規則だと「氷は触れた相手も滑る (Minimum)」「ゴムは相手が何でも噛む (Maximum)」
    /// @note のような演出意図の作り分けができないため、材質側に選ばせる。GeometricMean は
    /// @note 摩擦の旧既定と一致させるための温存 (選択制にした瞬間に既存シーンの摩擦を変えないため)。
    enum class PhysicsMaterialCombine : uint8_t
    {
        Average = 0,      ///< @note (a + b) / 2
        GeometricMean,    ///< @note sqrt(a * b) — 摩擦の既定
        Minimum,          ///< @note min(a, b)   — 反発の既定
        Multiply,         ///< @note a * b
        Maximum,          ///< @note max(a, b)
    };

    /// @brief 剛体コライダーの表面物性を表す値型。
    /// @note DLL 間で同じプリセット値を参照するため、共有データと外部メソッドを明示的に公開する。
    struct PhysicsMaterial
    {
        float restitution     = 0.3f;   ///< @note 反発係数    (0=完全非弾性, 1=完全弾性)
        float staticFriction  = 0.6f;   ///< @note 静止摩擦係数
        float dynamicFriction = 0.4f;   ///< @note 動摩擦係数
        float density         = 1.0f;   ///< @note 密度 (kg/m^3 相当。RigidBody の質量自動計算で使う)

        PhysicsMaterialCombine restitutionCombine = PhysicsMaterialCombine::Minimum; ///< 合成規則。既定値は選択制にする前の固定規則と一致させてある。
        PhysicsMaterialCombine frictionCombine    = PhysicsMaterialCombine::GeometricMean; ///< 2 形状の摩擦係数からペアの摩擦係数を作る規則。

        /// @brief 2 つのマテリアルが衝突したときの合成値。
        /// @note 規則が食い違う場合は「優先度の高い方」(列挙の並び順) を採用する。平均等の
        /// @note 折衷は「氷の上でだけ滑らせたい」といった意図を薄めるため、強い指定を勝たせる。
        FBZZ_PHYSICS_API static float CombineRestitution(const PhysicsMaterial& a, const PhysicsMaterial& b);
        FBZZ_PHYSICS_API static float CombineFriction(const PhysicsMaterial& a, const PhysicsMaterial& b);
        FBZZ_PHYSICS_API static float CombineStaticFriction(const PhysicsMaterial& a, const PhysicsMaterial& b);

        /// @brief 指定規則で 2 値を合成する (Editor のプレビュー表示などから直接使う)。
        FBZZ_PHYSICS_API static float Combine(PhysicsMaterialCombine mode, float a, float b);
        /// @brief 規則が異なる 2 つのマテリアルから、実際に適用される規則を選ぶ。
        FBZZ_PHYSICS_API static PhysicsMaterialCombine ResolveCombine(PhysicsMaterialCombine a,
                                                     PhysicsMaterialCombine b);

        /// @brief プリセット。Scene 側から値を直接持つため、これは初期値として使う。
        FBZZ_PHYSICS_API static const PhysicsMaterial Default;   ///< @note 汎用
        FBZZ_PHYSICS_API static const PhysicsMaterial Rubber; ///< ゴムの接触特性を表す組み込み材質。
        FBZZ_PHYSICS_API static const PhysicsMaterial Ice; ///< 氷の接触特性を表す組み込み材質。
        FBZZ_PHYSICS_API static const PhysicsMaterial Metal; ///< 金属の接触特性を表す組み込み材質。
        FBZZ_PHYSICS_API static const PhysicsMaterial Wood; ///< 木材の接触特性を表す組み込み材質。
        FBZZ_PHYSICS_API static const PhysicsMaterial Stone; ///< 石の接触特性を表す組み込み材質。

        /// @brief プリセットの列挙。Editor の新規作成メニューとスクリプトから同じ表を使う。
        static constexpr int PRESET_COUNT = 6; ///< 組み込み材質プリセットの件数。
        FBZZ_PHYSICS_API static const char*           PresetName(int index);
        FBZZ_PHYSICS_API static const PhysicsMaterial* PresetAt(int index);
        /// @brief 名前からプリセットを引く。
        /// @return 未知の名前は nullptr。
        FBZZ_PHYSICS_API static const PhysicsMaterial* FindPreset(const char* name);
    };
} /// @note namespace fbzz::physics
