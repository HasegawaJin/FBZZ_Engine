// FBZZ Engine
// PhysicsMaterial.hpp | fbzz::physics
// 剛体の表面物性 (反発・摩擦・密度) のプリセット定義
#pragma once

#ifdef _WIN32
#  ifdef fbzz_physics_EXPORTS
#    define FBZZ_PHYSICS_API __declspec(dllexport)
#  else
#    define FBZZ_PHYSICS_API __declspec(dllimport)
#  endif
#else
#  define FBZZ_PHYSICS_API
#endif

namespace fbzz::physics
{
    // 剛体コライダーの表面物性を表す値型。
    // WHY: shared_runtime では Engine DLL が Physics DLL 側のプリセット値を参照するため、
    //      MSVC で static const データシンボルも import/export されるようクラス単位で API を付ける。
    struct FBZZ_PHYSICS_API PhysicsMaterial
    {
        float restitution     = 0.3f;   // 反発係数    (0=完全非弾性, 1=完全弾性)
        float staticFriction  = 0.6f;   // 静止摩擦係数
        float dynamicFriction = 0.4f;   // 動摩擦係数
        float density         = 1.0f;   // 密度 (任意単位  例: kg/m^3)

        // ２つのマテリアルが衝突したときの合成値。
        // 反発は小さい方、摩擦は幾何平均を使い、極端な材質が支配しすぎないようにする。
        static float CombineRestitution(const PhysicsMaterial& a, const PhysicsMaterial& b);
        static float CombineFriction(const PhysicsMaterial& a, const PhysicsMaterial& b);
        static float CombineStaticFriction(const PhysicsMaterial& a, const PhysicsMaterial& b);

        // プリセット。Scene 側から値を直接持つため、これは初期値として使う。
        static const PhysicsMaterial Default;   // 汎用
        static const PhysicsMaterial Rubber;    // restitution=0.8, dynamic=0.9
        static const PhysicsMaterial Ice;       // restitution=0.05, dynamic=0.02
        static const PhysicsMaterial Metal;     // restitution=0.4,  dynamic=0.3
        static const PhysicsMaterial Wood;      // restitution=0.2,  dynamic=0.6
        static const PhysicsMaterial Stone;     // restitution=0.1,  dynamic=0.8
    };
} // namespace fbzz::physics
