/// @file    WaterZoneComponent.hpp
/// @brief   接続水域の所属・優先順位・河口と共有メッシュ密度の正本。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Engine/Scene/References/EntityRef.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Engine/Scene/Hierarchy/Transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::scene {
struct WaterZoneSnapshot;
inline constexpr std::size_t WATER_ZONE_MAX_BODIES = 32;
inline constexpr std::size_t WATER_ZONE_MAX_MOUTHS = 32;

struct WaterZoneMember {
    EntityRef water; ///< この Zone の共有水面に追加する WaterComponent の参照。
    int priority = 0; ///< 水面が重なる箇所でこの水面を選ぶための優先順位。
    void Reflect(IReflector& reflector)
    {
        reflector.RefField("water", water, "WaterComponent");
        reflector.Field("priority", priority);
    }
};

struct WaterZoneRiverMouth {
    EntityRef river; ///< Zone に接続するスプライン川の WaterComponent の参照。
    bool atStart = false; ///< 接続する川の端点を先頭にする。false は末尾。
    float blendLength = 20.0f; ///< 接続端点から川の内側へ向かう距離 [world m]。
    EntityRef receiver; ///< 未指定は Zone の親水域。支流では同じ Zone の下流側の水域を指定できる。
    void Reflect(IReflector& reflector)
    {
        reflector.RefField("river", river, "WaterComponent");
        reflector.Field("atStart", atStart);
        reflector.FloatRange("blendLength", blendLength, 0.1f, 10000.0f);
        reflector.RefField("receiver", receiver, "WaterComponent");
        blendLength = std::isfinite(blendLength) ? (std::clamp)(blendLength, 0.1f, 10000.0f) : 20.0f;
    }
};

struct WaterZoneComponent {
    bool enabled = true; ///< 所属する水面の共通形状・波・浮力をこの Zone で準備する。
    int ownerPriority = 0; ///< 所有者自身の水面を、重なる他の所属水面と比較する優先順位。
    std::vector<WaterZoneMember> members; ///< 所有者自身を除いて、共通水面へ追加する水面と優先順位の一覧。
    std::vector<WaterZoneRiverMouth> riverMouths; ///< 接続端点と受け側水面ごとの河口設定。
    bool blendMaterials = false; ///< 河口の接続区間で川と受け側水域の材質を混ぜる。
    float meshCellSize = 2.0f; ///< 共通メッシュの目標最大辺長 [world m]。
    std::shared_ptr<const WaterZoneSnapshot> snapshot; ///< 不変の数値結果と試行署名は保存しない。
    std::vector<EntityID> materialSources; ///< ライブスナップショットの材質源として使う水面のエンティティ列。
    std::shared_ptr<const WaterZoneSnapshot> simulationSnapshot; ///< LiveQuery の準備とは独立に保持する、固定ステップの Zone 水面。
    std::vector<EntityID> simulationMaterialSources; ///< 固定ステップで公開したシミュレーション水面に対応する材質源。
    Transform simulationOwnerTransform{}; ///< simulationSnapshot と同時に取得する所有者の姿勢。
    bool simulationPublished = false; ///< 最初のシミュレーション公開後は true。結果が無効な場合も含む。
    std::uint64_t attemptSignature = 0; ///< 直近に Zone の形状生成を試みた入力の署名。
    bool attempted = false; ///< 現在の attemptSignature に対して準備を試行済み。
    std::string diagnostic; ///< 直近の準備が完了しなかった理由を Inspector に表示する文字列。

    /// @brief このコンポーネントを別のシーンへコピーした後、公開済みの形状と準備状態を破棄する。
    void ResetRuntimeState()
    {
        snapshot.reset();
        materialSources.clear();
        simulationSnapshot.reset();
        simulationMaterialSources.clear();
        simulationOwnerTransform = Transform{};
        simulationPublished = false;
        attemptSignature = 0;
        attempted = false;
        diagnostic.clear();
    }

    const char* GetTypeName() const { return "Water Zone"; }
    void Reflect(IReflector& reflector)
    {
        reflector.Field("enabled", enabled);
        reflector.Tooltip("Connected waters share this object's waves and buoyancy settings.");
        reflector.Field("ownerPriority", ownerPriority);
        ReflectList(reflector, "members", members, WATER_ZONE_MAX_BODIES - 1);
        ReflectList(reflector, "riverMouths", riverMouths, WATER_ZONE_MAX_MOUTHS);
        reflector.Field("blendMaterials", blendMaterials);
        reflector.Tooltip("Blend each river's material into its receiver across the mouth length in world meters.");
        reflector.FloatRange("meshCellSize", meshCellSize, 0.05f, 100.0f);
        meshCellSize = std::isfinite(meshCellSize) ? (std::clamp)(meshCellSize, 0.05f, 100.0f) : 2.0f;
        reflector.Readonly("ready", static_cast<bool>(snapshot));
        reflector.Readonly("status", diagnostic.empty()
            ? std::string(snapshot ? "Ready" : "Not prepared") : diagnostic);
    }

private:
    template<typename T>
    static void ReflectList(IReflector& reflector, const char* name, std::vector<T>& values, std::size_t maximum)
    {
        const std::size_t requested = reflector.BeginObjectList(name, values.size());
        // 上限を超える保存データは一つ余分な項目を残し、準備時にZone全体を無効として診断する。
        if (requested != values.size()) {
            values.resize((std::min)(requested, maximum + 1));
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            reflector.BeginObjectElement(i);
            values[i].Reflect(reflector);
            reflector.EndObjectElement();
        }
        const std::size_t removed = reflector.EndObjectList();
        if (removed < values.size()) {
            values.erase(values.begin() + static_cast<std::ptrdiff_t>(removed));
        }
        std::size_t from = 0, to = 0;
        if (reflector.ObjectListMove(from, to) && from < values.size() && to < values.size() && from != to) {
            T moved = std::move(values[from]);
            values.erase(values.begin() + static_cast<std::ptrdiff_t>(from));
            values.insert(values.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
        }
    }
};
}
