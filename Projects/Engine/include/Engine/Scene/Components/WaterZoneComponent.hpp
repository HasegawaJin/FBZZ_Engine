/// @file    WaterZoneComponent.hpp
/// @brief   接続水域の所属・優先順位・河口と共有メッシュ密度の正本。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Transform.hpp>
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
    EntityRef water;
    int priority = 0;
    void Reflect(IReflector& reflector)
    {
        reflector.RefField("water", water, "WaterComponent");
        reflector.Field("priority", priority);
    }
};

struct WaterZoneRiverMouth {
    EntityRef river;
    bool atStart = false;
    float blendLength = 20.0f; ///< @note 接続端点から川の内側へ向かう距離 [world m]。
    void Reflect(IReflector& reflector)
    {
        reflector.RefField("river", river, "WaterComponent");
        reflector.Field("atStart", atStart);
        reflector.FloatRange("blendLength", blendLength, 0.1f, 10000.0f);
        blendLength = std::isfinite(blendLength) ? (std::clamp)(blendLength, 0.1f, 10000.0f) : 20.0f;
    }
};

struct WaterZoneComponent {
    bool enabled = true;
    int ownerPriority = 0;
    std::vector<WaterZoneMember> members;
    std::vector<WaterZoneRiverMouth> riverMouths;
    float meshCellSize = 2.0f; ///< @note 共通メッシュの目標最大辺長 [world m]。
    /// @note 不変の数値結果と試行署名は保存しない。
    std::shared_ptr<const WaterZoneSnapshot> snapshot;
    /// @brief Fixed-step Zone surface retained independently from LiveQuery preparation.
    std::shared_ptr<const WaterZoneSnapshot> simulationSnapshot;
    /// @brief Owner pose captured atomically with simulationSnapshot.
    Transform simulationOwnerTransform{};
    /// @brief True after the first simulation publication, including an invalid result.
    bool simulationPublished = false;
    std::uint64_t attemptSignature = 0;
    bool attempted = false;
    std::string diagnostic;

    /// @brief Clear published geometry and preparation state after copying this component to another Scene.
    void ResetRuntimeState()
    {
        snapshot.reset();
        simulationSnapshot.reset();
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
        reflector.Tooltip("Connected waters share this object's Water material, waves and buoyancy settings.");
        reflector.Field("ownerPriority", ownerPriority);
        ReflectList(reflector, "members", members, WATER_ZONE_MAX_BODIES - 1);
        ReflectList(reflector, "riverMouths", riverMouths, WATER_ZONE_MAX_MOUTHS);
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
        /// @note 上限を超える保存データは一つ余分な項目を残し、準備時にZone全体を無効として診断する。
        if (requested != values.size()) values.resize((std::min)(requested, maximum + 1));
        for (std::size_t i = 0; i < values.size(); ++i) {
            reflector.BeginObjectElement(i);
            values[i].Reflect(reflector);
            reflector.EndObjectElement();
        }
        const std::size_t removed = reflector.EndObjectList();
        if (removed < values.size()) values.erase(values.begin() + static_cast<std::ptrdiff_t>(removed));
        std::size_t from = 0, to = 0;
        if (reflector.ObjectListMove(from, to) && from < values.size() && to < values.size() && from != to) {
            T moved = std::move(values[from]);
            values.erase(values.begin() + static_cast<std::ptrdiff_t>(from));
            values.insert(values.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
        }
    }
};
} /// @note namespace fbzz::scene
