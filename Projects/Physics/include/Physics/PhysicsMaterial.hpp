/// @file    PhysicsMaterial.hpp
/// @brief   剛体の表面物性 (反発・摩擦・密度) のプリセット定義。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

#include <cstdint>

#ifdef _WIN32
#  ifdef FBZZPhysics_EXPORTS
#    define FBZZ_PHYSICS_API __declspec(dllexport)
#  else
#    define FBZZ_PHYSICS_API __declspec(dllimport)
#  endif
#else
#  define FBZZ_PHYSICS_API
#endif

namespace fbzz::physics
{
    /// @brief 2 つのマテリアルが接触したときの合成規則。
    /// @note 固定規則だと「氷は触れた相手も滑る (Minimum)」「ゴムは相手が何でも噛む (Maximum)」
    ///       のような演出意図の作り分けができないため、材質側に選ばせる。GeometricMean は
    ///       摩擦の旧既定と一致させるための温存 (選択制にした瞬間に既存シーンの摩擦を変えないため)。
    enum class PhysicsMaterialCombine : uint8_t
    {
        Average = 0,      ///< (a + b) / 2
        GeometricMean,    ///< sqrt(a * b) — 摩擦の既定
        Minimum,          ///< min(a, b)   — 反発の既定
        Multiply,         ///< a * b
        Maximum,          ///< max(a, b)
    };

    /// @brief 剛体コライダーの表面物性を表す値型。
    /// @note shared_runtime では Engine DLL が Physics DLL 側のプリセット値を参照するため、
    ///       MSVC で static const データシンボルも import/export されるようクラス単位で API を付ける。
    struct FBZZ_PHYSICS_API PhysicsMaterial
    {
        float restitution     = 0.3f;   ///< 反発係数    (0=完全非弾性, 1=完全弾性)
        float staticFriction  = 0.6f;   ///< 静止摩擦係数
        float dynamicFriction = 0.4f;   ///< 動摩擦係数
        float density         = 1.0f;   ///< 密度 (kg/m^3 相当。RigidBody の質量自動計算で使う)

        /// @note 合成規則。既定値は選択制にする前の固定規則と一致させてある。
        PhysicsMaterialCombine restitutionCombine = PhysicsMaterialCombine::Minimum;
        PhysicsMaterialCombine frictionCombine    = PhysicsMaterialCombine::GeometricMean;

        /// @brief 2 つのマテリアルが衝突したときの合成値。
        /// @note 規則が食い違う場合は「優先度の高い方」(列挙の並び順) を採用する。平均等の
        ///       折衷は「氷の上でだけ滑らせたい」といった意図を薄めるため、強い指定を勝たせる。
        static float CombineRestitution(const PhysicsMaterial& a, const PhysicsMaterial& b);
        static float CombineFriction(const PhysicsMaterial& a, const PhysicsMaterial& b);
        static float CombineStaticFriction(const PhysicsMaterial& a, const PhysicsMaterial& b);

        /// @brief 指定規則で 2 値を合成する (Editor のプレビュー表示などから直接使う)。
        static float Combine(PhysicsMaterialCombine mode, float a, float b);
        /// @brief 規則が異なる 2 つのマテリアルから、実際に適用される規則を選ぶ。
        static PhysicsMaterialCombine ResolveCombine(PhysicsMaterialCombine a,
                                                     PhysicsMaterialCombine b);

        /// @brief プリセット。Scene 側から値を直接持つため、これは初期値として使う。
        static const PhysicsMaterial Default;   ///< 汎用
        static const PhysicsMaterial Rubber;    ///< restitution=0.8, dynamic=0.9
        static const PhysicsMaterial Ice;       ///< restitution=0.05, dynamic=0.02
        static const PhysicsMaterial Metal;     ///< restitution=0.4,  dynamic=0.3
        static const PhysicsMaterial Wood;      ///< restitution=0.2,  dynamic=0.6
        static const PhysicsMaterial Stone;     ///< restitution=0.1,  dynamic=0.8

        /// @brief プリセットの列挙。Editor の新規作成メニューとスクリプトから同じ表を使う。
        static constexpr int PRESET_COUNT = 6;
        static const char*           PresetName(int index);
        static const PhysicsMaterial* PresetAt(int index);
        /// @brief 名前からプリセットを引く。
        /// @return 未知の名前は nullptr。
        static const PhysicsMaterial* FindPreset(const char* name);
    };
} // namespace fbzz::physics
