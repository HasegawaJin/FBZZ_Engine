/// @file    ForceField.cpp
/// @brief   力のリストの直列化と、環境風プリセット。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Scene/Components/ForceField.hpp>

#include <cstddef>

namespace fbzz::scene {

void ReflectForceFieldList(IReflector& r, std::vector<ForceFieldSettings>& forces,
                           const char* key, const char* displayName)
{
    r.BeginField(key, displayName);
    const std::size_t count = r.BeginObjectList(displayName, forces.size());
    // 保存キーはリスト自身の分で使い切る。戻さずに要素へ入ると、ForceFieldSettings::Reflect
    // が並べる Field が全部この key を指してしまう ── 書けば先勝ちで 1 個目以外が黙って
    // 捨てられ、読めば全部が既定値のまま残る。どちらもエラーは出ない。
    r.EndField();
    forces.resize(count);
    for (std::size_t index = 0; index < forces.size(); ++index) {
        r.BeginObjectElement(index);
        forces[index].Reflect(r);
        r.EndObjectElement();
    }
    const std::size_t removeIndex = r.EndObjectList();
    if (removeIndex < forces.size())
        forces.erase(forces.begin() + static_cast<std::ptrdiff_t>(removeIndex));
    r.EndField();
}

std::vector<ForceFieldSettings> MakeAmbientWindForces(
    const math::Vector3& direction, float strength, float turbulence, float pulseFrequency)
{
    std::vector<ForceFieldSettings> forces;

    // 一定方向の風。radius 0 = 減衰なしでシーン全体へ。
    ForceFieldSettings wind;
    wind.fieldType = ForceFieldType::Wind;
    wind.space     = ForceFieldSpace::World;
    wind.direction = direction;
    wind.strength  = strength;
    wind.radius    = 0.0f;
    wind.falloffPower = 1.0f;
    forces.push_back(wind);

    // 乱れ。旧 WindZone の pulseFrequency は «脈動の速さ» で、
    // カールノイズの時間スクロール速度と同じ意味なのでそこへ写す。
    if (turbulence > 0.0f) {
        ForceFieldSettings turb;
        turb.fieldType      = ForceFieldType::Turbulence;
        turb.space          = ForceFieldSpace::World;
        turb.direction      = direction;
        turb.strength       = turbulence;
        turb.radius         = 0.0f;
        turb.falloffPower   = 1.0f;
        turb.noiseFrequency = 0.5f;
        turb.noiseSpeed     = pulseFrequency;
        forces.push_back(turb);
    }
    return forces;
}

} // namespace fbzz::scene
