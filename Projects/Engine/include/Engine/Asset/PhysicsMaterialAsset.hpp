/// @file    PhysicsMaterialAsset.hpp
/// @brief   .physmat 物理マテリアルアセットのランタイム表現と TOML 入出力 API。
/// @author  Hasegawa Jin
/// @date    2026-08-16
///
/// 設計意図 (WHY):
/// physics::PhysicsMaterial は以前からコライダーへ「インライン値」で持たせていた。
/// そのため氷の床を 20 枚置くと同じ 4 つの数値を 20 回打つことになり、後から
/// 「やっぱり少し滑りすぎ」と思っても 20 箇所を直して回る必要があった。
/// .mat と同じく共有アセットにすることで、材質の調整が 1 ファイルの編集で全箇所へ届く。
///
/// Physics 側の値型をそのまま内包し、アセット側で値を再定義しない。
/// WHY: 物理挙動の正本は Physics モジュールにあるべきで、Engine 側に平行した定義を作ると
/// フィールドを足したときに片方だけ更新されるズレが必ず起きる。
#pragma once
#include <Engine/Scene/Script.hpp>   // IReflector (Inspector / TOML 共用の反射)
#include <Physics/PhysicsMaterial.hpp>
#include <string>
#include <string_view>

namespace fbzz::asset {

// .physmat の内容を保持する共有物理マテリアル。
struct PhysicsMaterialAsset {
    physics::PhysicsMaterial material = physics::PhysicsMaterial::Default;

    // 由来プリセット名 (空 = 手動編集)。Inspector の「どこから作ったか」表示だけに使い、
    // 挙動には影響しない。
    std::string presetName;

    const char* GetTypeName() const { return "Physics Material"; }

    // Inspector と TOML 入出力で同じ反射を使う。
    // WHY: 物理マテリアルはフィールドが少なく特殊な UI も要らないため、
    //      専用の描画コードと専用のシリアライザを二重に持つ理由がない。
    void Reflect(scene::IReflector& r)
    {
        r.FloatRange("restitution", material.restitution, 0.0f, 1.0f);
        r.FloatRange("staticFriction", material.staticFriction, 0.0f, 2.0f);
        r.FloatRange("dynamicFriction", material.dynamicFriction, 0.0f, 2.0f);
        r.FloatRange("density", material.density, 0.001f, 25000.0f);

        static constexpr const char* kCombineLabels[] = {
            "Average", "Geometric Mean", "Minimum", "Multiply", "Maximum"
        };
        int restitutionCombine = static_cast<int>(material.restitutionCombine);
        r.Enum("restitutionCombine", restitutionCombine, kCombineLabels);
        material.restitutionCombine =
            static_cast<physics::PhysicsMaterialCombine>(restitutionCombine);

        int frictionCombine = static_cast<int>(material.frictionCombine);
        r.Enum("frictionCombine", frictionCombine, kCombineLabels);
        material.frictionCombine =
            static_cast<physics::PhysicsMaterialCombine>(frictionCombine);

        r.Field("presetName", presetName);
    }
};

// TOML から .physmat を読み込む。破損・未存在時は false を返し、例外は使わない。
[[nodiscard]] bool LoadPhysicsMaterialAssetFromFile(std::string_view path,
                                                    PhysicsMaterialAsset& outAsset);

// .physmat を TOML へ保存する。Editor からの Save と新規作成で共有する。
[[nodiscard]] bool SavePhysicsMaterialAssetToFile(std::string_view path,
                                                   const PhysicsMaterialAsset& asset);

} // namespace fbzz::asset
