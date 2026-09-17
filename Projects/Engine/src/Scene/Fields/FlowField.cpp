/// @file    FlowField.cpp
/// @brief   流れのリストの直列化。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Scene/Fields/FlowField.hpp>

#include <cstddef>

namespace fbzz::scene {

void ReflectFlowFieldList(IReflector& r, std::vector<FlowFieldSettings>& forces,
                          const char* key, const char* displayName)
{
    r.BeginField(key, displayName);
    const std::size_t count = r.BeginObjectList(displayName, forces.size());
    /// @note @note 保存キーはリスト自身の分で使い切る。戻さずに要素へ入ると、FlowFieldSettings::Reflect
    ///       が並べる Field が全部この key を指す ── 書けば先勝ちで 1 個目以外が黙って捨てられ、
    ///       読めば全部が既定値のまま残る。どちらもエラーは出ない。
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

} // namespace fbzz::scene
