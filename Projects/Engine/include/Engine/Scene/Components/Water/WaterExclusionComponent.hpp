/// @file    WaterExclusionComponent.hpp
/// @brief   選択した水域の一部を取り除く、シーン設定の回転付きボックス。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#pragma once

#include <Engine/Scene/References/EntityRef.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fbzz::scene {

/// @brief 対象水面と水中領域の照会から三次元ボックスの内部を除外する。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/water-body-actors-in-unreal-engine#waterbodyexclusionvolume UE Water Body Exclusion Volume.
struct WaterExclusionComponent {
    bool enabled = true;
    bool excludeAll = true;
    math::Vector3 size{4.0f, 4.0f, 4.0f};
    std::vector<EntityRef> waters;
    /// @brief 直近のスナップショット準備結果の実行時診断。
    std::string diagnostic;

    /// @brief 設定済みの範囲と型付き水面参照を Inspector と保存処理に公開する。
    void Reflect(IReflector& reflector)
    {
        reflector.Field("enabled", enabled);
        reflector.Field("size", size);
        reflector.Tooltip("Local box dimensions in metres. The box follows this object's full rotation and signed hierarchy scale; no Collider is required.");
        const auto sanitize = [](float value) {
            return std::isfinite(value) ? (std::clamp)(value, 0.001f, 100000.0f) : 4.0f;
        };
        size = {sanitize(size.x), sanitize(size.y), sanitize(size.z)};
        reflector.Field("excludeAll", excludeAll);
        reflector.Tooltip("When excludeAll is off, only the Water objects listed below are excluded.");
        reflector.RefListField("waters", waters, "WaterComponent");
        reflector.Readonly("status", diagnostic.empty() ? std::string{"Not prepared"} : diagnostic);
    }

    /// @brief 登録済みの表示名を返す。
    [[nodiscard]] const char* GetTypeName() const { return "Water Exclusion"; }
};

}
